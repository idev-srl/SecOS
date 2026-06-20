/*
 * SecOS Kernel - Scheduler
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M7] Cooperative ring-3 scheduling via SYS_YIELD.
 * [M8] Preemptive (timer-driven) scheduling + process lifecycle (exit/reap)
 *      with a kernel idle task as the always-available fallback.
 */
#include "sched.h"
#include "terminal.h"
#include "debugcon.h"
#include "vmm.h"
#include "percpu.h"

// [M29] Scheduler state is per-CPU: `current`, `idle_task` and the preemption
// quantum live in this CPU's cpu_t. The macros keep the single-core code below
// unchanged while making each core operate on its own task set. `this_cpu()`
// returns CPU 0 before SMP is up, so the pre-SMP path is identical.
#define current     (this_cpu()->current)
#define idle_task   (this_cpu()->idle_task)
#define slice_left  (this_cpu()->slice_left)

// [M8] Preemption quantum: switch the running user task every N timer ticks.
#define SCHED_QUANTUM_TICKS 3

extern void process_foreach(void (*cb)(process_t*, void*), void* user);
extern void tss_set_kernel_stack(uint64_t stack);
extern void arch_iret_to_tf(trapframe_t* tf) __attribute__((noreturn));
extern int  process_destroy(process_t* p);

// ---- process selection (round-robin over runnable user processes) ----
// [M29] Only consider processes pinned to THIS CPU (cpu_affinity), so each core
// schedules its own task set and no two cores ever run the same process.
struct pick_ctx { process_t* after; int passed; process_t* cand; uint32_t affinity; };
static void pick_scan_cb(process_t* p, void* user) {
    struct pick_ctx* c = (struct pick_ctx*)user;
    if (p->cpu_affinity != c->affinity) return;       // not ours
    if (p == c->after) { c->passed = 1; return; }
    if (!c->passed && c->after != NULL) return;       // not yet past 'after'
    if (p->state == PROC_NEW || p->state == PROC_READY) { if (!c->cand) c->cand = p; }
}
static process_t* pick_user(process_t* after) {
    uint32_t aff = this_cpu()->index;
    struct pick_ctx c = { after, after == NULL ? 1 : 0, NULL, aff };
    process_foreach(pick_scan_cb, &c);
    if (!c.cand) { // wrap around from the beginning
        struct pick_ctx c2 = { NULL, 1, NULL, aff };
        process_foreach(pick_scan_cb, &c2);
        return c2.cand;
    }
    return c.cand;
}

void sched_init(void) {
    // [M29] Ensure the BSP (CPU 0) exists before any per-CPU access. Its real
    // LAPIC ID is fixed up after ACPI/LAPIC bring-up via smp_set_bsp_lapic_id().
    if (smp_cpu_count() == 0) smp_register_cpu(0);
    cpu_by_index(0)->online = 1;     // the boot CPU is always online
    current = NULL; idle_task = NULL;
}
process_t* sched_get_current(void) { return current; }
void sched_set_current(process_t* p) { current = p; }
void sched_set_idle(process_t* idle) { idle_task = idle; }
int sched_add_process(process_t* p) { (void)p; return 0; }

// ---- low-level switch: load target space + kernel stack, iretq into it ----
static void switch_to(process_t* next) __attribute__((noreturn));
static void switch_to(process_t* next) {
    current = next;
    next->state = PROC_RUNNING;
    // [M29] Observability: a user task running on an application processor proves
    // multicore scheduling. (idle tasks have pid 0 and never log here.)
    { cpu_t* cpu = this_cpu();
      if (cpu->index != 0 && next->pid != 0) {
        uint64_t df = debugcon_line_lock();   // keep the line atomic across cores
        debugcon_writestring("[SMP] cpu="); debugcon_print_hex(cpu->index);
        debugcon_writestring(" run pid="); debugcon_print_hex(next->pid);
        debugcon_writestring("\n");
        debugcon_line_unlock(df);
      } }
    vmm_switch_space(next->space);
    tss_set_kernel_stack(next->kstack_top);
    slice_left = SCHED_QUANTUM_TICKS;
    arch_iret_to_tf(next->tf);
    // not reached
}

static inline void save_tf(process_t* p, trapframe_t* tf) {
    const uint8_t* s = (const uint8_t*)tf;
    uint8_t* d = (uint8_t*)p->tf;
    for (int i = 0; i < (int)sizeof(trapframe_t); i++) d[i] = s[i];
}

/* [M28-2] EOI the scheduler tick to whichever controller owns it (LAPIC in APIC
 * mode, else the 8259). The preempt path EOIs here because switch_to() does not
 * return, so the ISR stub's trailing irq_eoi never runs. */
extern void irq_eoi(void);

// ---- [M8] preemptive tick ----
void sched_on_timer_tick(trapframe_t* tf) {
    if (current) current->cpu_ticks++;
    // [M17] Wake any sleepers whose deadline elapsed (independent of preemption).
    { extern uint64_t timer_get_ticks(void); sched_wake_sleepers(timer_get_ticks()); }
    if (!idle_task) return;            // preemption only active once a scheduler is armed
    if (!current) return;              // not yet running the scheduler

    int from_user = ((tf->cs & 3) == 3);

    if (current == idle_task) {
        // Idle is always safe to preempt: hand the CPU to a runnable user task.
        // Save idle's interrupted context so that returning to it (after the
        // user task exits) resumes where it was — this lets the interactive
        // shell run as the idle task and get control back after `run`.
        process_t* next = pick_user(NULL);
        if (next) { if (idle_task->tf) save_tf(idle_task, tf); irq_eoi(); switch_to(next); }
        return;
    }

    // A user task is current. Only preempt when it was interrupted in ring-3
    // (never mid-syscall in ring-0), and only when its quantum has expired.
    if (!from_user) return;
    if (slice_left > 1) { slice_left--; return; }

    process_t* next = pick_user(current);
    if (!next) next = idle_task;       // nobody else runnable → go idle
    if (next == current) { slice_left = SCHED_QUANTUM_TICKS; return; }

    if (current->state == PROC_RUNNING) current->state = PROC_READY;
    save_tf(current, tf);
    debugcon_writestring("[SCHED] preempt ");
    debugcon_print_hex(current->pid);
    debugcon_writestring(" -> ");
    debugcon_print_hex(next->pid);
    debugcon_writestring("\n");
    irq_eoi();
    switch_to(next);
}

// ---- [M7] cooperative yield from SYS_YIELD ----
void sched_yield_from_syscall(trapframe_t* tf) {
    if (!current) return;
    if (current->tf) { save_tf(current, tf); current->tf->rax = 0; } // yield returns 0

    process_t* next = pick_user(current);
    if (!next && idle_task) next = idle_task;
    if (!next || next == current) return;             // nothing else: keep running

    debugcon_writestring("[SCHED] switch ");
    debugcon_print_hex(current->pid);
    debugcon_writestring(" -> ");
    debugcon_print_hex(next->pid);
    debugcon_writestring("\n");
    if (current->state == PROC_RUNNING) current->state = PROC_READY;
    switch_to(next);
}

// ---- legacy cooperative entry (kept for compatibility) ----
void sched_yield(void) {
    if (!current) return;
    process_t* next = pick_user(current);
    if (!next && idle_task) next = idle_task;
    if (next && next != current) {
        if (current->state == PROC_RUNNING) current->state = PROC_READY;
        switch_to(next);
    }
}

// ---- [M8] exit + reaping ----
struct alive_ctx { int n; };
static void alive_cb(process_t* p, void* u) {
    struct alive_ctx* c = (struct alive_ctx*)u;
    if (p == idle_task) return;
    if (p->state != PROC_ZOMBIE) c->n++;
}
int sched_count_alive_user(void) {
    struct alive_ctx c = { 0 };
    process_foreach(alive_cb, &c);
    return c.n;
}

void sched_reap_zombies(void) {
    // [M29] Reap only zombies pinned to THIS CPU, and only from a context that has
    // already switched off the zombie's kernel stack (the idle loop / a later
    // task). process_reap_one() detaches each zombie from the table under the
    // proc lock before we free it, so no other core can race on it.
    extern process_t* process_reap_one(uint32_t affinity);
    uint32_t aff = this_cpu()->index;
    process_t* z;
    while ((z = process_reap_one(aff)) != NULL) {
        process_destroy(z);             // frees space/tables/kstack/tf (zombie != current)
    }
}

// [M30] Notify the parent of a child's exit/stop via SIGCHLD.
static void notify_parent_sigchld(process_t* child) {
    extern process_t* process_find_by_pid(uint32_t);
    extern int signal_post(process_t*, int);
    if (!child || child->ppid == 0) return;
    process_t* parent = process_find_by_pid(child->ppid);
    if (parent) signal_post(parent, 17 /*SIGCHLD*/);
}

void sched_exit_current(trapframe_t* tf) {
    (void)tf;
    if (!current || current == idle_task) return;
    debugcon_writestring("[SCHED] exit ");
    debugcon_print_hex(current->pid);
    debugcon_writestring("\n");
    // [M16] exit_code was set by the SYS_EXIT handler from the user's status.
    current->state = PROC_ZOMBIE;
    notify_parent_sigchld(current);                       // [M30] SIGCHLD to parent
    sched_wake_waitpid(current->pid, current->exit_code); // [M16] wake a waiter

    process_t* next = pick_user(current);
    if (!next) next = idle_task;       // last one out → idle reaps and reports
    // We are running on the exiting task's kernel stack, so it cannot reap
    // itself; the next scheduling context (often idle) calls sched_reap_zombies.
    switch_to(next);
}

// [M15] Terminate the current process after an unrecoverable ring-3 fault and
// switch to the next runnable task — the same mechanism as a SYS_EXIT, but
// driven by the exception handler. NEVER returns. If there is no killable
// current (idle/none), the fault is unrecoverable and we halt (a kernel bug).
void sched_kill_current(int reason) {
    if (!current || current == idle_task) {
        debugcon_writestring("[KILL] no killable process -> halt\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    debugcon_writestring("[KILL] pid=");
    debugcon_print_hex(current->pid);
    debugcon_writestring(" reason=");
    debugcon_print_hex((uint64_t)reason);
    debugcon_writestring("\n");
    // Encode the fault as a signal-style status (128 + vector), à la POSIX.
    current->exit_code = 128 + (reason & 0x7F);
    current->state = PROC_ZOMBIE;
    notify_parent_sigchld(current);                       // [M30] SIGCHLD to parent
    sched_wake_waitpid(current->pid, current->exit_code); // [M16] wake a waiter

    process_t* next = pick_user(current);
    if (!next) next = idle_task;       // idle reaps the zombie and carries on
    switch_to(next);                   // NORETURN
}

// [M30] Stop the current process (SIGSTOP/SIGTSTP): save its ring-3 context,
// mark it PROC_STOPPED (the scheduler skips it until SIGCONT flips it READY),
// notify the parent, and switch away. Mirrors sched_block_current. NORETURN.
void sched_stop_current(trapframe_t* tf) {
    if (!current || current == idle_task) {
        for (;;) __asm__ volatile("cli; hlt");
    }
    debugcon_writestring("[STOP] pid=");
    debugcon_print_hex(current->pid);
    debugcon_writestring("\n");
    save_tf(current, tf);
    current->state = PROC_STOPPED;
    notify_parent_sigchld(current);
    process_t* next = pick_user(current);
    if (!next) next = idle_task;
    switch_to(next);                   // NORETURN
}

// [M16/M17] Block the current process: save its mid-syscall trapframe and switch
// away. The caller must have armed a wait condition (wait_pid / sleep_until /
// recv_chan) and rewound rip so the syscall re-runs on wake. Returns only in the
// (impossible) case that there is no current/idle-only — defensive.
void sched_block_current(trapframe_t* tf) {
    if (!current || current == idle_task) return;
    save_tf(current, tf);
    current->state = PROC_BLOCKED;
    process_t* next = pick_user(current);
    if (!next) next = idle_task;
    switch_to(next);                   // NORETURN in practice
}

// [M16] Wake any process blocked in SYS_WAIT on 'pid', delivering 'code'.
struct wake_wp_ctx { uint32_t pid; int code; };
static void wake_wp_cb(process_t* p, void* u) {
    struct wake_wp_ctx* w = (struct wake_wp_ctx*)u;
    if (p->state == PROC_BLOCKED && p->wait_pid >= 0 && (uint32_t)p->wait_pid == w->pid) {
        p->wait_result = w->code;
        p->wait_ready  = 1;
        p->wait_pid    = -1;
        p->state       = PROC_READY;
    }
}
void sched_wake_waitpid(uint32_t pid, int code) {
    struct wake_wp_ctx w = { pid, code };
    process_foreach(wake_wp_cb, &w);
}

// [M17] Wake processes whose SYS_SLEEP deadline has elapsed.
static void wake_sleep_cb(process_t* p, void* u) {
    uint64_t now = *(uint64_t*)u;
    if (p->state == PROC_BLOCKED && p->sleep_until != 0 && now >= p->sleep_until) {
        p->state = PROC_READY; // sleep_until cleared when the syscall re-runs
    }
}
void sched_wake_sleepers(uint64_t now) {
    process_foreach(wake_sleep_cb, &now);
}

// [M17] Wake processes blocked receiving on IPC channel 'chan'.
static void wake_chan_cb(process_t* p, void* u) {
    int chan = *(int*)u;
    if (p->state == PROC_BLOCKED && p->recv_chan == chan) {
        p->recv_chan = -1;
        p->state = PROC_READY;
    }
}
void sched_wake_chan(int chan) {
    process_foreach(wake_chan_cb, &chan);
}

// [M25] Wake processes blocked on pipe 'pp' (either a reader waiting for data or
// a writer waiting for space). The woken task re-runs its read/write syscall and
// re-checks the condition, so a spurious wake just re-blocks.
static void wake_pipe_cb(process_t* p, void* u) {
    if (p->state == PROC_BLOCKED && p->wait_pipe == u) {
        p->wait_pipe = 0;
        p->state = PROC_READY;
    }
}
void sched_wake_pipe(void* pp) {
    process_foreach(wake_pipe_cb, pp);
}
