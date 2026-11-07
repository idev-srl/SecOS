/*
 * SecOS Kernel - Orderly power-off / reboot / halt (SYS_REBOOT)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * SYS_REBOOT(cmd) runs in the calling process's syscall and is restartable,
 * like the other blocking syscalls (rip rewound + block, re-run on wake). It
 * walks through three phases kept in the caller's PCB:
 *
 *   1. TERM  : SIGTERM every other user process, give them POWER_GRACE_TICKS
 *              to exit (the caller sleeps, re-checking every POWER_POLL_TICKS).
 *   2. KILL  : SIGKILL whatever is left, wait up to POWER_KILL_TICKS for the
 *              scheduler to tear them down.
 *   3. FINAL : sync every mounted filesystem (vfs_sync_all), unmount everything
 *              but "/", then ACPI poweroff / reset, or halt the CPU.
 *
 * Privilege: only signed code runs at all; a confined (CAP_ENFORCE) manifest
 * additionally needs CAP_POWER. Every step is logged as [POWER] on debugcon.
 */
#include <stdint.h>
#include "process.h"
#include "sched.h"
#include "signal.h"
#include "debugcon.h"
#include "terminal.h"
#include "power.h"
#include "../fs/vfs.h"

extern void process_foreach(void (*cb)(process_t*, void*), void* user);
extern uint64_t timer_get_ticks(void);
extern void acpi_poweroff(void);
extern void acpi_reboot(void);

#define POWER_GRACE_TICKS 1000   /* SIGTERM grace (timer ticks = ms) */
#define POWER_KILL_TICKS   500   /* wait for SIGKILLed processes to go away */
#define POWER_POLL_TICKS    20

enum { PWR_IDLE = 0, PWR_TERM, PWR_KILL };

struct pwr_ctx { uint32_t self; int sig; int alive; };
static void pwr_cb(process_t* p, void* u) {
    struct pwr_ctx* c = (struct pwr_ctx*)u;
    if (p->pid == 0 || p->pid == c->self || p->state == PROC_ZOMBIE) return;
    c->alive++;
}
/* Count the other live user processes; optionally signal them (outside the
 * process-table lock: signal_post may look up a parent). */
static int pwr_others(uint32_t self, int sig) {
    struct pwr_ctx c = { self, sig, 0 };
    process_foreach(pwr_cb, &c);
    if (sig && c.alive) {
        extern long ksys_kill(int, int);
        ksys_kill(-1, sig);                  /* every process except the caller */
    }
    return c.alive;
}

static const char* cmd_name(int cmd) {
    switch (cmd) {
    case SECOS_REBOOT_POWEROFF: return "poweroff";
    case SECOS_REBOOT_RESTART:  return "reboot";
    case SECOS_REBOOT_HALT:     return "halt";
    default:                    return "?";
    }
}

/* [M51-C6] Only one caller runs the final phase. Two processes asking for a
 * reboot at once (each walks its own phases in its PCB) would otherwise sync
 * and unmount every filesystem twice at the same time on two CPUs. The loser
 * parks its CPU with interrupts on (it keeps answering TLB shootdowns the
 * winner's unmounts may send); the winner powers the machine off. */
static volatile int g_power_final_owner;

static void power_final(int cmd) {
    if (__atomic_exchange_n(&g_power_final_owner, 1, __ATOMIC_ACQ_REL)) {
        debugcon_writestring("[POWER] final phase already running on another CPU\n");
        for (;;) __asm__ volatile("sti; hlt");
    }
    debugcon_writestring("[POWER] syncing filesystems\n");
    int n = vfs_sync_all();
    debugcon_writestring("[POWER] synced "); debugcon_print_hex((uint64_t)n);
    debugcon_writestring(" filesystem(s); unmounting\n");
    int u = vfs_unmount_all();
    debugcon_writestring("[POWER] unmounted "); debugcon_print_hex((uint64_t)u);
    debugcon_writestring(" mount(s)\n");
    __asm__ volatile("cli");
    if (cmd == SECOS_REBOOT_RESTART) {
        debugcon_writestring("[POWER] restarting\n");
        terminal_writestring("\nRebooting...\n");
        acpi_reboot();                       /* does not return */
    } else if (cmd == SECOS_REBOOT_POWEROFF) {
        debugcon_writestring("[POWER] powering off\n");
        terminal_writestring("\nPowering off...\n");
        acpi_poweroff();                     /* returns only if ACPI S5 failed */
        debugcon_writestring("[POWER] ACPI poweroff unavailable; halting\n");
        terminal_writestring("ACPI poweroff unavailable; halting.\n");
    } else {
        debugcon_writestring("[POWER] system halted\n");
        terminal_writestring("\nSystem halted.\n");
    }
    for (;;) __asm__ volatile("cli; hlt");
}

/* Returns <0 on error, or 1 when the caller must block (rip rewound by the
 * trap layer, sleep_until armed). Never returns once the final phase starts. */
long power_syscall_step(process_t* cur, int cmd) {
    if (cmd != SECOS_REBOOT_POWEROFF && cmd != SECOS_REBOOT_RESTART && cmd != SECOS_REBOOT_HALT)
        return -22;                                          /* -EINVAL */
    if (!cur) power_final(cmd);
    uint64_t now = timer_get_ticks();

    if (cur->power_phase == PWR_IDLE) {
        debugcon_writestring("[POWER] "); debugcon_writestring(cmd_name(cmd));
        debugcon_writestring(" requested by pid="); debugcon_print_hex(cur->pid);
        debugcon_writestring("; SIGTERM to all processes\n");
        signal_set_foreground_pgid(cur->pgid);   /* nobody else owns the console now */
        cur->power_phase = PWR_TERM;
        cur->power_deadline = now + POWER_GRACE_TICKS;
        if (pwr_others(cur->pid, SIGTERM) == 0) cur->power_deadline = now;
    }
    if (cur->power_phase == PWR_TERM) {
        int alive = pwr_others(cur->pid, 0);
        if (alive && now < cur->power_deadline) goto wait;
        if (alive) {
            debugcon_writestring("[POWER] grace expired; SIGKILL to "); debugcon_print_hex((uint64_t)alive);
            debugcon_writestring(" process(es)\n");
            pwr_others(cur->pid, SIGKILL);
        } else {
            debugcon_writestring("[POWER] all processes exited\n");
        }
        cur->power_phase = PWR_KILL;
        cur->power_deadline = now + POWER_KILL_TICKS;
    }
    if (cur->power_phase == PWR_KILL) {
        int alive = pwr_others(cur->pid, 0);
        if (alive && now < cur->power_deadline) goto wait;
        if (alive) {
            debugcon_writestring("[POWER] "); debugcon_print_hex((uint64_t)alive);
            debugcon_writestring(" process(es) still alive; continuing\n");
        }
        power_final(cmd);                                    /* NORETURN */
    }
    return -22;
wait:
    cur->sleep_until = now + POWER_POLL_TICKS;               /* woken by the timer tick */
    return 1;
}
