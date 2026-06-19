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
#include "terminal.h"

extern int  keyboard_has_char(void);
extern char keyboard_getchar(void);

#define TTY_LINE 256
static char g_line[TTY_LINE];   /* assembled line (incl. trailing '\n') */
static int  g_len = 0;          /* bytes assembled and ready to deliver  */
static int  g_pos = 0;          /* bytes already delivered to the reader */

static char tty_getchar_blocking(void) {
    while (!keyboard_has_char()) __asm__ volatile ("sti; hlt");
    return keyboard_getchar();
}

/* Cooked line read. Returns >0 bytes (up to len), 0 on EOF (Ctrl-D on an empty
 * line), or -1 when the line was interrupted (Ctrl-C). */
int tty_read(void* buf, int len) {
    if (len <= 0) return -1;
    char* out = (char*)buf;

    /* Deliver leftover from a line that didn't fit the previous buffer. */
    if (g_pos < g_len) {
        int n = 0;
        while (g_pos < g_len && n < len) out[n++] = g_line[g_pos++];
        return n;
    }

    /* Assemble a fresh line. */
    int n = 0;
    for (;;) {
        char c = tty_getchar_blocking();
        if (c == '\n' || c == '\r') {
            terminal_putchar('\n');
            if (n < TTY_LINE) g_line[n++] = '\n';   /* keep the newline (POSIX) */
            break;
        } else if (c == '\b' || c == 0x7f) {        /* backspace / DEL */
            if (n > 0) { n--; terminal_writestring("\b \b"); }
        } else if (c == 0x04) {                      /* Ctrl-D */
            if (n == 0) return 0;                    /* EOF on an empty line */
            break;                                   /* else deliver the partial line */
        } else if (c == 0x03) {                      /* Ctrl-C */
            terminal_writestring("^C\n");
            return -1;                               /* interrupt the read */
        } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f) {
            if (n < TTY_LINE - 1) { g_line[n++] = c; terminal_putchar(c); }
        }
    }

    g_len = n; g_pos = 0;
    int m = 0;
    while (g_pos < g_len && m < len) out[m++] = g_line[g_pos++];
    return m;
}
