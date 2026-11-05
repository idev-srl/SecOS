/*
 * SecOS Kernel - Console TTY line discipline (termios)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * One terminal: the console (framebuffer/VGA output through drivers/vt.c,
 * input from the PS/2 + USB HID ring and COM1). It serves ring-3 stdin (fd 0
 * when not redirected, any dup of it) and /dev/tty, /dev/console.
 *
 * termios is honoured the way Linux's n_tty does it, for the flags programs
 * actually use:
 *   c_iflag  ISTRIP INLCR IGNCR ICRNL IUTF8
 *   c_oflag  OPOST ONLCR (applied to console writes)
 *   c_lflag  ISIG ICANON ECHO ECHOE ECHOK ECHONL ECHOCTL ECHOKE NOFLSH IEXTEN
 *   c_cc     VINTR VQUIT VERASE VKILL VEOF VEOL VEOL2 VSUSP VWERASE VREPRINT
 *            VLNEXT VMIN VTIME
 * Canonical mode assembles and edits a line (read() returns at most one line;
 * VEOF on an empty line is EOF). Non-canonical mode hands bytes over as they
 * arrive with the POSIX VMIN/VTIME rules. With ISIG, VINTR/VQUIT/VSUSP raise
 * SIGINT/SIGQUIT/SIGTSTP for the foreground process group: from the PS/2 IRQ
 * asynchronously (drivers/keyboard.c asks tty_isig_for()), and here for bytes
 * that come from serial / USB, which are only seen when a reader polls.
 *
 * Input is pulled from the keyboard ring when a reader (or FIONREAD) looks,
 * and echoed then. A blocked reader is woken by the timer tick
 * (tty_wake_readers) when input is pending, or every TTY_POLL_TICKS so it can
 * re-poll the USB keyboard and re-check a VTIME deadline.
 *
 * Job control: only the foreground process group may read; a background
 * reader gets SIGTTIN (or EIO when SIGTTIN is blocked/ignored).
 */
#include <stdint.h>
#include <stdbool.h>
#include "terminal.h"
#include "process.h"
#include "signal.h"
#include "tty.h"
#include "../mm/user_copy.h"

extern int  keyboard_getc_raw(void);
extern bool keyboard_input_pending_noirq(void);
extern process_t* sched_get_current(void);
extern uint64_t timer_get_ticks(void);
extern uint32_t timer_get_frequency(void);
extern void vt_set_reply_hook(void (*hook)(const char* s, int n));
extern void debugcon_putchar(char c);

/* ---- settings --------------------------------------------------------------- */
static struct tty_termios g_tio;
static int g_tio_init;
static uint16_t g_ws_row, g_ws_col;     /* TIOCSWINSZ override (0 = real console size) */

static void tio_defaults(void) {
    for (unsigned i = 0; i < sizeof(g_tio); i++) ((uint8_t*)&g_tio)[i] = 0;
    g_tio.c_iflag = T_ICRNL | T_IUTF8;
    g_tio.c_oflag = T_OPOST | T_ONLCR;
    g_tio.c_cflag = T_B38400 | T_CS8 | T_CREAD | T_HUPCL;
    g_tio.c_lflag = T_ISIG | T_ICANON | T_ECHO | T_ECHOE | T_ECHOK | T_ECHOCTL | T_ECHOKE | T_IEXTEN;
    g_tio.c_cc[T_VINTR] = 0x03;  g_tio.c_cc[T_VQUIT] = 0x1C; g_tio.c_cc[T_VERASE] = 0x7F;
    g_tio.c_cc[T_VKILL] = 0x15;  g_tio.c_cc[T_VEOF] = 0x04;  g_tio.c_cc[T_VTIME] = 0;
    g_tio.c_cc[T_VMIN] = 1;      g_tio.c_cc[T_VSTART] = 0x11; g_tio.c_cc[T_VSTOP] = 0x13;
    g_tio.c_cc[T_VSUSP] = 0x1A;  g_tio.c_cc[T_VEOL] = 0;     g_tio.c_cc[T_VREPRINT] = 0x12;
    g_tio.c_cc[T_VDISCARD] = 0x0F; g_tio.c_cc[T_VWERASE] = 0x17; g_tio.c_cc[T_VLNEXT] = 0x16;
    g_tio.c_cc[T_VEOL2] = 0;
    g_tio_init = 1;
}
static inline void tio_ensure(void) { if (!g_tio_init) tio_defaults(); }
#define LF(f) (g_tio.c_lflag & (f))
#define IF(f) (g_tio.c_iflag & (f))
/* A control character slot matches c (0 = disabled, _POSIX_VDISABLE). */
static inline int cc_is(int idx, uint8_t c) { return g_tio.c_cc[idx] != 0 && g_tio.c_cc[idx] == c; }

/* ---- queues ----------------------------------------------------------------- */
/* Canonical line being edited. */
#define TTY_LINE 1024
static uint8_t g_line[TTY_LINE];
static int g_n;
static int g_lnext;                       /* VLNEXT: next byte is literal */
/* Read queue: bytes a read() can take. Entries carry flags: TQ_EOL marks the
 * last byte of a canonical line; a TQ_EOF entry is a zero-length line (VEOF on
 * an empty line) and makes one read() return 0. */
#define TQ_SIZE 4096
#define TQ_EOL 0x100
#define TQ_EOF 0x200
static uint16_t g_q[TQ_SIZE];
static int g_qh, g_qt;                    /* head (read), tail (write) */
static int q_count(void) { return (g_qt - g_qh + TQ_SIZE) % TQ_SIZE; }
static int q_space(void) { return TQ_SIZE - 1 - q_count(); }
static void q_push(uint16_t v) { if (q_space() > 0) { g_q[g_qt] = v; g_qt = (g_qt + 1) % TQ_SIZE; } }
static void q_clear(void) { g_qh = g_qt = 0; }

static volatile int g_flush_req;          /* ISIG from the IRQ: flush before the next op */
static volatile int g_intr_noproc;        /* VINTR with no foreground group: interrupt the read */
static uint64_t g_last_rx;                /* tick of the last byte queued (VTIME inter-byte) */
static uint64_t g_vtime_start;            /* VMIN=0,VTIME>0: when the read began */
static uint32_t g_vtime_pid;

#define TTY_EINTR (-4)          /* -EINTR: libc maps it to errno=EINTR */
#define TTY_EIO   (-5)          /* -EIO: background read with SIGTTIN blocked/ignored */
#define TTY_EAGAIN (-11)
#define TTY_WOULDBLOCK (-2)     /* no input yet: the syscall layer blocks the reader */

/* ---- output ------------------------------------------------------------------ */
static int out_onlcr(void) {
    tio_ensure();
    return (g_tio.c_oflag & T_OPOST) && (g_tio.c_oflag & T_ONLCR);
}
static void echo_n(const char* s, int n) { terminal_write_tty(s, (size_t)n, out_onlcr()); }
static void echo_c(char c) { echo_n(&c, 1); }
/* Echo one input byte, control characters as ^X with ECHOCTL. */
static void echo_byte(uint8_t c) {
    if (LF(T_ECHOCTL) && (c < 0x20 || c == 0x7F) && c != '\t' && c != '\n' && c != '\r') {
        char b[2] = { '^', (char)(c == 0x7F ? '?' : (c + '@')) };
        echo_n(b, 2);
    } else {
        echo_c((char)c);
    }
}
/* Columns the echo of byte c took (for erase). UTF-8 continuation bytes: 0. */
static int echo_width(uint8_t c) {
    if (c == '\t') return 1;
    if (c < 0x20 || c == 0x7F) return LF(T_ECHOCTL) ? 2 : 0;
    if (IF(T_IUTF8) && (c & 0xC0) == 0x80) return 0;
    return 1;
}

/* Console write path (fd 1/2 and /dev/tty): termios output processing. */
int tty_write(const void* buf, int len) {
    if (len <= 0) return 0;
    terminal_write_tty((const char*)buf, (size_t)len, out_onlcr());
    return len;
}

/* DSR/DA replies from the emulator are typed into the input queue. */
static void tty_reply(const char* s, int n) {
    for (int i = 0; i < n; i++) { q_push((uint8_t)s[i]); }
    if (g_tio.c_lflag & T_ICANON) { /* a reply is not a line: leave it unterminated */ }
}

/* ---- input processing -------------------------------------------------------- */
static void flush_input(void) { g_n = 0; g_lnext = 0; q_clear(); }

static void canon_commit(int eof_marker) {
    if (eof_marker && g_n == 0) { q_push(TQ_EOF); return; }
    for (int i = 0; i < g_n; i++) q_push((uint16_t)(g_line[i] | (i == g_n - 1 ? TQ_EOL : 0)));
    g_n = 0;
}

/* Erase one character (a whole UTF-8 sequence with IUTF8) from the line. */
static void canon_erase_one(int do_echo) {
    if (g_n <= 0) return;
    int w = 0;
    for (;;) {
        uint8_t c = g_line[--g_n];
        w += echo_width(c);
        if (!(IF(T_IUTF8) && (c & 0xC0) == 0x80) || g_n == 0) break;
    }
    if (do_echo && LF(T_ECHO)) {
        if (LF(T_ECHOE)) { for (int i = 0; i < w; i++) echo_n("\b \b", 3); }
        else echo_byte(g_tio.c_cc[T_VERASE]);
    }
}

static int isig_sig(uint8_t c) {
    if (!LF(T_ISIG)) return 0;
    if (cc_is(T_VINTR, c)) return SIGINT;
    if (cc_is(T_VQUIT, c)) return SIGQUIT;
    if (cc_is(T_VSUSP, c)) return SIGTSTP;
    return 0;
}

/* Called from the PS/2 IRQ: is this byte a signal character right now? */
int tty_isig_for(char c) {
    tio_ensure();
    return isig_sig((uint8_t)c);
}
/* The IRQ posted `sig` for a signal character: flush pending input (unless
 * NOFLSH) at the next TTY operation. */
void tty_note_isig(int sig) {
    (void)sig;
    if (!(g_tio.c_lflag & T_NOFLSH)) g_flush_req = 1;
}

static void process_byte(uint8_t c) {
    if (IF(T_ISTRIP)) c &= 0x7F;
    if (g_lnext) {                                    /* literal next */
        g_lnext = 0;
        if (LF(T_ICANON)) {
            if (LF(T_ECHO) && LF(T_ECHOCTL)) echo_n("\b\b", 2);
            if (g_n < TTY_LINE - 1) { g_line[g_n++] = c; if (LF(T_ECHO)) echo_byte(c); }
        } else {
            q_push(c);
        }
        return;
    }
    if (c == '\r') {
        if (IF(T_IGNCR)) return;
        if (IF(T_ICRNL)) c = '\n';
    } else if (c == '\n' && IF(T_INLCR)) {
        c = '\r';
    }

    int sig = isig_sig(c);
    if (sig) {
        if (!LF(T_NOFLSH)) flush_input();
        if (LF(T_ECHO)) echo_byte(c);
        uint32_t fg = signal_get_foreground_pgid();
        if (fg) signal_post_pgid(fg, sig);
        else if (sig == SIGINT) { if (LF(T_ECHO)) echo_c('\n'); g_intr_noproc = 1; }
        return;
    }

    if (!LF(T_ICANON)) {
        q_push(c);
        g_last_rx = timer_get_ticks();
        if (LF(T_ECHO)) echo_byte(c);
        else if (c == '\n' && LF(T_ECHONL)) echo_c('\n');
        return;
    }

    /* Canonical editing. */
    if (LF(T_IEXTEN) && cc_is(T_VLNEXT, c)) {
        g_lnext = 1;
        if (LF(T_ECHO) && LF(T_ECHOCTL)) echo_n("^\b", 2);
        return;
    }
    if (cc_is(T_VERASE, c) || c == 0x08 || c == 0x7F) {   /* both BS and DEL erase */
        canon_erase_one(1);
        return;
    }
    if (cc_is(T_VKILL, c)) {
        if (LF(T_ECHO) && !LF(T_ECHOKE) && LF(T_ECHOK)) {
            echo_byte(c); echo_c('\n'); g_n = 0;
        } else {
            while (g_n > 0) canon_erase_one(1);
        }
        return;
    }
    if (LF(T_IEXTEN) && cc_is(T_VWERASE, c)) {
        while (g_n > 0 && (g_line[g_n - 1] == ' ' || g_line[g_n - 1] == '\t')) canon_erase_one(1);
        while (g_n > 0 && g_line[g_n - 1] != ' ' && g_line[g_n - 1] != '\t') canon_erase_one(1);
        return;
    }
    if (LF(T_IEXTEN) && cc_is(T_VREPRINT, c)) {
        if (LF(T_ECHO)) {
            echo_byte(c); echo_c('\n');
            for (int i = 0; i < g_n; i++) echo_byte(g_line[i]);
        }
        return;
    }
    if (cc_is(T_VEOF, c)) {
        canon_commit(1);                               /* VEOF itself is not stored */
        return;
    }
    if (c == '\n' || cc_is(T_VEOL, c) || (LF(T_IEXTEN) && cc_is(T_VEOL2, c))) {
        if (LF(T_ECHO) || (c == '\n' && LF(T_ECHONL))) echo_byte(c);
        if (g_n < TTY_LINE) g_line[g_n++] = c;
        else g_line[TTY_LINE - 1] = c;                 /* full: the terminator still ends it */
        canon_commit(0);
        return;
    }
    if (g_n < TTY_LINE - 1) {                          /* keep room for the terminator */
        g_line[g_n++] = c;
        if (LF(T_ECHO)) echo_byte(c);
    }
}

/* Drain the raw input sources into the discipline. In non-canonical mode a
 * lone ESC waits a few ms for the rest of an escape sequence: COM1 delivers
 * bytes one at a time, and a reader that sees ESC alone (readline, through
 * FIONREAD) would take it as a bare Escape key instead of the start of
 * ESC [ A. PS/2 and USB queue whole sequences atomically. */
static void pull_input(void) {
    tio_ensure();
    if (g_flush_req) { g_flush_req = 0; flush_input(); }
    for (;;) {
        int c = keyboard_getc_raw();
        if (c < 0) break;
        process_byte((uint8_t)c);
        if (c == 0x1B && !LF(T_ICANON) && !keyboard_input_pending_noirq()) {
            uint64_t t0 = timer_get_ticks();
            uint32_t hz = timer_get_frequency(); if (!hz) hz = 1000;
            uint64_t wait = hz / 100 + 1;              /* ~10 ms */
            while (!keyboard_input_pending_noirq() && timer_get_ticks() - t0 < wait)
                __asm__ volatile ("sti; hlt");
        }
    }
}

/* Bytes a read() can take now (FIONREAD semantics: in canonical mode only
 * complete lines count). */
static int readable_count(void) {
    int n = q_count();
    if (!LF(T_ICANON)) return n;
    int last = -1;
    for (int i = 0; i < n; i++) {
        uint16_t v = g_q[(g_qh + i) % TQ_SIZE];
        if (v & (TQ_EOL | TQ_EOF)) last = i;
    }
    return last + 1;
}

/* ---- read -------------------------------------------------------------------- */
/* When set (tty_read_try), a reader with no input pending gives the CPU away
 * instead of spinning: the syscall layer blocks it (wait_tty) and the timer tick
 * wakes it when input arrives. The INT 0x80 path only preempts ring 3, so a
 * reader spinning here in ring 0 starved every other process: background jobs
 * made no progress while bash sat at its prompt. */
static int g_tty_may_block = 0;
static int g_nonblock = 0;

/* Copy from the read queue. Canonical: stop after one line (and swallow an EOF
 * marker, returning 0 if it is first). */
static int take(uint8_t* out, int len) {
    int m = 0;
    if (LF(T_ICANON)) {
        if (q_count() > 0 && (g_q[g_qh] & TQ_EOF)) { g_qh = (g_qh + 1) % TQ_SIZE; return 0; }
        while (m < len && q_count() > 0) {
            uint16_t v = g_q[g_qh];
            if (v & TQ_EOF) break;
            g_qh = (g_qh + 1) % TQ_SIZE;
            out[m++] = (uint8_t)v;
            if (v & TQ_EOL) break;
        }
        return m;
    }
    while (m < len && q_count() > 0) {
        uint16_t v = g_q[g_qh];
        g_qh = (g_qh + 1) % TQ_SIZE;
        if (v & TQ_EOF) continue;                       /* stale canonical EOF */
        out[m++] = (uint8_t)v;
    }
    return m;
}

/* Wait for more input: block in the scheduler (tty_read_try) or spin. Returns
 * 0 to retry, or an error/WOULDBLOCK to hand back. */
static int wait_input(process_t* cur) {
    if (cur && signal_pending(cur)) return TTY_EINTR;   /* [M30] EINTR */
    if (g_nonblock) return TTY_EAGAIN;
    if (g_tty_may_block && cur && cur->pid != 0) {
        cur->wait_tty = 1;                              /* armed; tty_wake_readers() */
        return TTY_WOULDBLOCK;
    }
    __asm__ volatile ("sti; hlt");
    return 0;
}

/* Returns >0 bytes (up to len), 0 on EOF (VEOF on an empty line, or a VMIN=0
 * timeout), -EINTR when a signal is pending or the line was cancelled, -2 when
 * the caller should block (tty_read_try only). A signal interruption keeps
 * the partially typed line so the re-issued read() continues it (bash treats a
 * non-EINTR read error on stdin as EOF). */
int tty_read(void* buf, int len) {
    if (len <= 0) return -1;
    uint8_t* out = (uint8_t*)buf;
    process_t* cur = sched_get_current();

    /* Job control: only the foreground process group may read the console. A
     * background reader gets SIGTTIN (its whole group stops, so a shell sees the
     * job as Stopped) and its read restarts once it is resumed in the
     * foreground; with SIGTTIN blocked/ignored it gets EIO (POSIX). */
    {
        uint32_t fg = signal_get_foreground_pgid();
        if (cur && fg && cur->pgid != fg) {
            if (!signal_ttin_acts(cur)) return TTY_EIO;
            signal_post_pgid(cur->pgid, SIGTTIN);
            return TTY_EINTR;
        }
    }

    for (;;) {
        pull_input();
        if (g_intr_noproc) { g_intr_noproc = 0; return TTY_EINTR; }   /* Ctrl-C, no fg group */
        if (LF(T_ICANON)) {
            if (readable_count() > 0 || (q_count() > 0 && (g_q[g_qh] & TQ_EOF))) {
                return take(out, len);
            }
        } else {
            int vmin = g_tio.c_cc[T_VMIN], vtime = g_tio.c_cc[T_VTIME];
            int avail = q_count();
            uint64_t now = timer_get_ticks();
            uint32_t hz = timer_get_frequency(); if (!hz) hz = 1000;
            uint64_t tlen = (uint64_t)vtime * hz / 10;    /* VTIME is in 1/10 s */
            int need = vmin < len ? vmin : len;
            if (vmin == 0 && vtime == 0) return take(out, len);           /* pure poll */
            if (vmin > 0 && avail >= need) { g_vtime_pid = 0; return take(out, len); }
            if (vmin > 0 && vtime > 0 && avail > 0 && now - g_last_rx >= tlen)
                return take(out, len);                                    /* inter-byte timeout */
            if (vmin == 0) {
                if (avail > 0) { g_vtime_pid = 0; return take(out, len); }
                uint32_t pid = cur ? cur->pid : 0;
                if (g_vtime_pid != pid + 1) { g_vtime_pid = pid + 1; g_vtime_start = now; }
                if (now - g_vtime_start >= tlen) { g_vtime_pid = 0; return 0; }   /* timeout */
            }
        }
        int w = wait_input(cur);
        if (w != 0) {
            if (w != TTY_WOULDBLOCK) g_vtime_pid = 0;
            return w;
        }
    }
}

/* Console read() entry used by SYS_READ on fd 0 and /dev/tty: like tty_read(),
 * but returns TTY_WOULDBLOCK (-2) with current->wait_tty armed instead of
 * spinning in ring 0 when no input is pending; syscall_trap.c then blocks the
 * caller and re-runs the read once tty_wake_readers() sees input. */
int tty_read_try(void* buf, int len) {
    g_tty_may_block = 1;
    int r = tty_read(buf, len);
    g_tty_may_block = 0;
    return r;
}
/* O_NONBLOCK read: -EAGAIN instead of waiting. */
int tty_read_nonblock(void* buf, int len) {
    g_nonblock = 1;
    int r = tty_read(buf, len);
    g_nonblock = 0;
    return r;
}

/* Timer tick: wake blocked console readers when PS/2/serial input is waiting,
 * and every TTY_POLL_TICKS regardless so the reader re-polls the USB HID
 * keyboard (which must not be polled from an ISR) and re-checks VTIME. */
#define TTY_POLL_TICKS 8
static void tty_wake_cb(process_t* p, void* u) {
    (void)u;
    if (p->state == PROC_BLOCKED && p->wait_tty) { p->wait_tty = 0; p->state = PROC_READY; }
}
void tty_wake_readers(uint64_t now) {
    extern void process_foreach(void (*cb)(process_t*, void*), void* user);
    if ((now % TTY_POLL_TICKS) != 0 && !keyboard_input_pending_noirq()) return;
    process_foreach(tty_wake_cb, 0);
}

/* ---- ioctl ------------------------------------------------------------------- */
/* termios + window size + queue ioctls on the console. Job-control ioctls
 * (TIOCGPGRP/TIOCSPGRP) stay in syscall.c next to the process-group logic.
 * Returns 0 / a negative errno; ENOIOCTLCMD (-515) if not a tty ioctl here. */
static int k_copy_to(void* u, const void* k, unsigned n) { return copy_to_user(u, k, n) ? -14 : 0; }
static int k_copy_from(void* k, const void* u, unsigned n) { return copy_from_user(k, u, n) ? -14 : 0; }

void tty_init(void) {
    tio_ensure();
    vt_set_reply_hook(tty_reply);
}

int tty_ioctl(uint64_t cmd, uint64_t arg) {
    tio_ensure();
    switch (cmd) {
    case TTY_TCGETS:
        return k_copy_to((void*)arg, &g_tio, sizeof(g_tio));
    case TTY_TCSETS: case TTY_TCSETSW: case TTY_TCSETSF: {
        struct tty_termios t;
        int r = k_copy_from(&t, (void*)arg, sizeof(t));
        if (r) return r;
        int was_canon = (g_tio.c_lflag & T_ICANON) != 0;
        int now_canon = (t.c_lflag & T_ICANON) != 0;
        if (cmd == TTY_TCSETSF) flush_input();         /* TCSAFLUSH: drop unread input */
        g_tio = t;
        if (was_canon && !now_canon) {
            /* Leaving canonical mode: the partially typed line becomes readable
             * as raw bytes (n_tty does the same). */
            for (int i = 0; i < g_n; i++) q_push(g_line[i]);
            g_n = 0;
            for (int i = 0; i < q_count(); i++) {
                int k = (g_qh + i) % TQ_SIZE;
                if (g_q[k] & TQ_EOF) g_q[k] = TQ_EOF;   /* dropped by a raw take() */
            }
        } else if (!was_canon && now_canon && q_count() > 0) {
            /* Entering canonical mode with raw bytes pending: make them one
             * readable line instead of stranding them. */
            int k = (g_qt - 1 + TQ_SIZE) % TQ_SIZE;
            g_q[k] |= TQ_EOL;
        }
        g_vtime_pid = 0;
        return 0;
    }
    case TTY_TIOCGWINSZ: {
        struct tty_winsize w;
        int cols, rows; terminal_get_size(&cols, &rows);
        w.ws_row = g_ws_row ? g_ws_row : (uint16_t)rows;
        w.ws_col = g_ws_col ? g_ws_col : (uint16_t)cols;
        w.ws_xpixel = (uint16_t)(cols * 8); w.ws_ypixel = (uint16_t)(rows * 16);
        return k_copy_to((void*)arg, &w, sizeof(w));
    }
    case TTY_TIOCSWINSZ: {
        struct tty_winsize w;
        int r = k_copy_from(&w, (void*)arg, sizeof(w));
        if (r) return r;
        g_ws_row = w.ws_row; g_ws_col = w.ws_col;
        uint32_t fg = signal_get_foreground_pgid();
        if (fg) signal_post_pgid(fg, SIGWINCH);
        return 0;
    }
    case TTY_FIONREAD: {
        pull_input();
        int n = readable_count();
        return k_copy_to((void*)arg, &n, sizeof(n));
    }
    case TTY_TIOCOUTQ: {
        int n = 0;
        return k_copy_to((void*)arg, &n, sizeof(n));
    }
    case TTY_TCFLSH:
        if (arg == 0 || arg == 2) { pull_input(); flush_input(); }   /* TCIFLUSH / TCIOFLUSH */
        return 0;
    case TTY_TCSBRK: case TTY_TCXONC: case TTY_TIOCSCTTY: case TTY_TIOCNOTTY:
        return 0;
    default:
        return -515;
    }
}

/* Back to the boot defaults (cooked, echo, ISIG) with empty queues: the
 * kernel shell calls it when a foreground program it ran has finished. */
void tty_reset(void) {
    tio_defaults();
    flush_input();
    g_flush_req = 0; g_intr_noproc = 0; g_vtime_pid = 0;
}

/* Current termios (for the self-test / debugging). */
void tty_get_termios(struct tty_termios* out) { tio_ensure(); *out = g_tio; }
