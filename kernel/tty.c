/*
 * SecOS Kernel - Console TTY line discipline
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M25] Cooked-mode line discipline for ring-3 stdin (fd 0) and the /dev/tty,
 * /dev/console read path. It line-buffers input, echoes typed characters, and
 * supports backspace, Ctrl-D (EOF) and Ctrl-C (interrupt the current line).
 *
 * Blocking model: like the socket layer, we spin on `sti; hlt` so the keyboard
 * IRQ (and the timer-driven USB-HID poll) can fill the shared keyboard buffer
 * while we wait — the INT 0x80 gate is an interrupt gate (IF cleared on entry),
 * so we must re-enable interrupts to ever wake. Only the foreground process
 * reads stdin at a time (the shell blocks in SYS_WAIT while a child runs), so a
 * single static line buffer is sufficient.
 *
 * NOTE: Ctrl-C here only interrupts an in-progress *read* (returns -1). Turning
 * it into an asynchronous kill of a compute-bound foreground process needs a
 * real signal-delivery mechanism, which is deferred (see docs/devlog/M25.md).
 */
#include <stdint.h>
#include <stdbool.h>
#include "terminal.h"
#include "process.h"
#include "signal.h"

extern int  keyboard_has_char(void);
extern char keyboard_getchar(void);
extern process_t* sched_get_current(void);

#define TTY_LINE 256
static char g_line[TTY_LINE];   /* assembled line (incl. trailing '\n') */
static int  g_len = 0;          /* bytes assembled and ready to deliver  */
static int  g_pos = 0;          /* bytes already delivered to the reader */
static int  g_asm = 0;          /* bytes of the line being typed (not yet complete) */

#define TTY_EINTR (-4)          /* -EINTR: libc maps it to errno=EINTR */
#define TTY_EIO   (-5)          /* -EIO: background read with SIGTTIN blocked/ignored */

#define TTY_WOULDBLOCK (-2)     /* no input yet: the syscall layer blocks the reader */

/* When set (tty_read_try), a reader with no input pending gives the CPU away
 * instead of spinning: the syscall layer blocks it (wait_tty) and the timer tick
 * wakes it when input arrives. The INT 0x80 path only preempts ring 3, so a
 * reader spinning here in ring 0 starved every other process: background jobs
 * made no progress while bash sat at its prompt. */
static int g_tty_may_block = 0;

/* Returns 0 and stores a char in *out, -1 if interrupted by a pending signal
 * (so the read() returns EINTR and the signal can be delivered) [M30], or -2
 * when the caller should block (tty_read_try only). */
static int tty_getchar_blocking(char* out) {
    while (!keyboard_has_char()) {
        process_t* cur = sched_get_current();
        if (cur && signal_pending(cur)) return -1;     /* [M30] EINTR */
        if (g_tty_may_block && cur && cur->pid != 0) {
            cur->wait_tty = 1;                         /* armed; tty_wake_readers() */
            return -2;
        }
        __asm__ volatile ("sti; hlt");
    }
    *out = keyboard_getchar();
    return 0;
}

/* Cooked line read. Returns >0 bytes (up to len), 0 on EOF (Ctrl-D on an empty
 * line), or -EINTR when a signal is pending or the line was cancelled (Ctrl-C).
 * A signal interruption keeps the partially typed line (g_asm) so the re-issued
 * read() continues it. It used to return -1 (errno EPERM) and drop the partial
 * line: bash treats a non-EINTR read error on stdin as EOF, so a SIGCHLD from a
 * background job landing while the user typed made the login shell exit. */
int tty_read(void* buf, int len) {
    if (len <= 0) return -1;
    char* out = (char*)buf;

    /* Job control: only the foreground process group may read the console. A
     * background reader gets SIGTTIN (its whole group stops, so a shell sees the
     * job as Stopped) and its read restarts once it is resumed in the
     * foreground; with SIGTTIN blocked/ignored it gets EIO (POSIX). */
    {
        process_t* cur = sched_get_current();
        uint32_t fg = signal_get_foreground_pgid();
        if (cur && fg && cur->pgid != fg) {
            if (!signal_ttin_acts(cur)) return TTY_EIO;
            signal_post_pgid(cur->pgid, SIGTTIN);
            return TTY_EINTR;
        }
    }

    /* Deliver leftover from a line that didn't fit the previous buffer. */
    if (g_pos < g_len) {
        int n = 0;
        while (g_pos < g_len && n < len) out[n++] = g_line[g_pos++];
        return n;
    }

    /* Assemble a line (resuming one interrupted by a signal). */
    int n = g_asm;
    for (;;) {
        char c;
        int gr = tty_getchar_blocking(&c);
        if (gr != 0) {                                   /* [M30] interrupted / would block */
            g_asm = n;                                   /* resume on re-read */
            return gr == -2 ? TTY_WOULDBLOCK : TTY_EINTR;
        }
        if (c == '\n' || c == '\r') {
            terminal_putchar('\n');
            if (n < TTY_LINE) g_line[n++] = '\n';   /* keep the newline (POSIX) */
            break;
        } else if (c == '\b' || c == 0x7f) {        /* backspace / DEL */
            if (n > 0) { n--; terminal_writestring("\b \b"); }
        } else if (c == 0x04) {                      /* Ctrl-D */
            if (n == 0) { g_asm = 0; return 0; }     /* EOF on an empty line */
            break;                                   /* else deliver the partial line */
        } else if (c == 0x03) {                      /* Ctrl-C */
            terminal_writestring("^C\n");
            g_asm = 0;                               /* the line is discarded */
            return TTY_EINTR;                        /* interrupt the read */
        } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f) {
            if (n < TTY_LINE - 1) { g_line[n++] = c; terminal_putchar(c); }
        }
    }

    g_len = n; g_pos = 0; g_asm = 0;
    int m = 0;
    while (g_pos < g_len && m < len) out[m++] = g_line[g_pos++];
    return m;
}

/* Console read() entry used by SYS_READ on fd 0: like tty_read(), but returns
 * TTY_WOULDBLOCK (-2) with current->wait_tty armed instead of spinning in ring 0
 * when no input is pending; syscall_trap.c then blocks the caller and re-runs
 * the read once tty_wake_readers() sees input. */
int tty_read_try(void* buf, int len) {
    g_tty_may_block = 1;
    int r = tty_read(buf, len);
    g_tty_may_block = 0;
    return r;
}

/* Timer tick: wake blocked console readers when PS/2/serial input is waiting,
 * and every TTY_POLL_TICKS regardless so the reader re-polls the USB HID
 * keyboard (which must not be polled from an ISR). */
#define TTY_POLL_TICKS 8
static void tty_wake_cb(process_t* p, void* u) {
    (void)u;
    if (p->state == PROC_BLOCKED && p->wait_tty) { p->wait_tty = 0; p->state = PROC_READY; }
}
void tty_wake_readers(uint64_t now) {
    extern bool keyboard_input_pending_noirq(void);
    extern void process_foreach(void (*cb)(process_t*, void*), void* user);
    if ((now % TTY_POLL_TICKS) != 0 && !keyboard_input_pending_noirq()) return;
    process_foreach(tty_wake_cb, 0);
}
