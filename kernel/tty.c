/*
 * SecOS Kernel - TTY line discipline (termios)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * One line discipline, many terminals. Every terminal is a struct tty
 * (kernel/tty.h): the console (framebuffer/VGA output through drivers/vt.c,
 * input from the PS/2 + USB HID ring and COM1) and the slave side of every
 * pseudo-terminal (kernel/pty.c). The console serves ring-3 stdin (fd 0 when
 * not redirected, any dup of it) and /dev/console (and /dev/tty for processes
 * whose controlling terminal is the console).
 *
 * termios is honoured the way Linux's n_tty does it, for the flags programs
 * actually use:
 *   c_iflag  ISTRIP INLCR IGNCR ICRNL IUTF8
 *   c_oflag  OPOST ONLCR (applied to writes)
 *   c_lflag  ISIG ICANON ECHO ECHOE ECHOK ECHONL ECHOCTL ECHOKE NOFLSH IEXTEN TOSTOP
 *   c_cc     VINTR VQUIT VERASE VKILL VEOF VEOL VEOL2 VSUSP VWERASE VREPRINT
 *            VLNEXT VMIN VTIME
 * Canonical mode assembles and edits a line (read() returns at most one line;
 * VEOF on an empty line is EOF). Non-canonical mode hands bytes over as they
 * arrive with the POSIX VMIN/VTIME rules. With ISIG, VINTR/VQUIT/VSUSP raise
 * SIGINT/SIGQUIT/SIGTSTP for the terminal's foreground process group. On the
 * console that happens from the PS/2 IRQ asynchronously (drivers/keyboard.c
 * asks tty_isig_for()), and here for bytes that come from serial / USB, which
 * are only seen when a reader polls. On a pty it happens when the master
 * writes the byte.
 *
 * Console input is pulled from the keyboard ring when a reader (or FIONREAD)
 * looks, and echoed then. A blocked console reader is woken by the timer tick
 * (tty_wake_readers) when input is pending, or every TTY_POLL_TICKS so it can
 * re-poll the USB keyboard and re-check a VTIME deadline. Blocked pty readers
 * and writers are woken by the other side (sched_wake_pty) and by the same
 * periodic tick (VTIME, and a safety net for a wakeup racing the block).
 *
 * Job control: only the foreground process group may read; a background
 * reader gets SIGTTIN (or EIO when SIGTTIN is blocked/ignored). On a pty that
 * is the reader's controlling terminal, writes with TOSTOP and termios/pgrp
 * changes from the background raise SIGTTOU.
 *
 * Locking: the console is not locked (as before). A pty's struct tty is only
 * touched under its pty lock (kernel/pty.c); signals raised meanwhile are
 * queued (tty_emit_sig) and posted by tty_post_sigs() after the lock is
 * dropped, because signal posting takes the process-table lock.
 */
#include <stdint.h>
#include <stdbool.h>
#include "terminal.h"
#include "process.h"
#include "signal.h"
#include "tty.h"
#include "../mm/user_copy.h"
#include "fbdev.h"      // [M40] KDSETMODE console handoff, KDSKBLAYOUT
#include "keyboard.h"   // [M40] keymap selection
#include "debugcon.h"
#include "cred.h"
#include "../fs/vfs.h"
#include "klock.h"   /* [M50] cpu_wait_irq */

extern int  keyboard_getc_raw(void);
extern bool keyboard_input_pending_noirq(void);
extern process_t* sched_get_current(void);
extern uint64_t timer_get_ticks(void);
extern uint32_t timer_get_frequency(void);
extern void vt_set_reply_hook(void (*hook)(const char* s, int n));

#define TTY_EINTR (-4)          /* -EINTR: libc maps it to errno=EINTR */
#define TTY_EIO   (-5)          /* -EIO: background read with SIGTTIN blocked/ignored, hangup */
#define TTY_EAGAIN (-11)
#define TTY_WOULDBLOCK (-2)     /* no input yet: the syscall layer blocks the reader */

/* Read queue entries carry flags: TQ_EOL marks the last byte of a canonical
 * line; a TQ_EOF entry is a zero-length line (VEOF on an empty line) and makes
 * one read() return 0. */
#define TQ_EOL 0x100
#define TQ_EOF 0x200

/* ---- the console instance ----------------------------------------------------- */
static void console_out(struct tty* t, const char* s, int n, int onlcr) {
    (void)t;
    terminal_write_tty(s, (size_t)n, onlcr);
}
static struct tty g_con;
static int g_con_init;

static void tio_defaults(struct tty_termios* tio) {
    for (unsigned i = 0; i < sizeof(*tio); i++) ((uint8_t*)tio)[i] = 0;
    tio->c_iflag = T_ICRNL | T_IUTF8;
    tio->c_oflag = T_OPOST | T_ONLCR;
    tio->c_cflag = T_B38400 | T_CS8 | T_CREAD | T_HUPCL;
    tio->c_lflag = T_ISIG | T_ICANON | T_ECHO | T_ECHOE | T_ECHOK | T_ECHOCTL | T_ECHOKE | T_IEXTEN;
    tio->c_cc[T_VINTR] = 0x03;  tio->c_cc[T_VQUIT] = 0x1C; tio->c_cc[T_VERASE] = 0x7F;
    tio->c_cc[T_VKILL] = 0x15;  tio->c_cc[T_VEOF] = 0x04;  tio->c_cc[T_VTIME] = 0;
    tio->c_cc[T_VMIN] = 1;      tio->c_cc[T_VSTART] = 0x11; tio->c_cc[T_VSTOP] = 0x13;
    tio->c_cc[T_VSUSP] = 0x1A;  tio->c_cc[T_VEOL] = 0;     tio->c_cc[T_VREPRINT] = 0x12;
    tio->c_cc[T_VDISCARD] = 0x0F; tio->c_cc[T_VWERASE] = 0x17; tio->c_cc[T_VLNEXT] = 0x16;
    tio->c_cc[T_VEOL2] = 0;
}

static void flush_input(struct tty* t);

void tty_instance_init(struct tty* t, tty_out_fn out, void* owner) {
    for (unsigned i = 0; i < sizeof(*t); i++) ((uint8_t*)t)[i] = 0;
    tio_defaults(&t->tio);
    t->out = out;
    t->owner = owner;
    t->ws.ws_row = 24; t->ws.ws_col = 80;
}

struct tty* tty_console(void) {
    if (!g_con_init) {
        tty_instance_init(&g_con, console_out, 0);
        g_con.is_console = 1;
        g_con.ws.ws_row = 0; g_con.ws.ws_col = 0;   /* 0 = the real console size */
        g_con_init = 1;
    }
    return &g_con;
}

#define LF(t, f) ((t)->tio.c_lflag & (f))
#define IF(t, f) ((t)->tio.c_iflag & (f))
/* A control character slot matches c (0 = disabled, _POSIX_VDISABLE). */
static inline int cc_is(struct tty* t, int idx, uint8_t c) {
    return t->tio.c_cc[idx] != 0 && t->tio.c_cc[idx] == c;
}

/* ---- queues ----------------------------------------------------------------- */
static int q_count(struct tty* t) { return (t->qt - t->qh + TQ_SIZE) % TQ_SIZE; }
static int q_space(struct tty* t) { return TQ_SIZE - 1 - q_count(t); }
static void q_push(struct tty* t, uint16_t v) {
    if (q_space(t) > 0) { t->q[t->qt] = v; t->qt = (t->qt + 1) % TQ_SIZE; }
}
static void q_clear(struct tty* t) { t->qh = t->qt = 0; }

/* ---- job control / signals ---------------------------------------------------- */
uint32_t tty_fg(struct tty* t) {
    return t->is_console ? signal_get_foreground_pgid() : t->fg_pgrp;
}
void tty_set_fg(struct tty* t, uint32_t pgrp) {
    if (t->is_console) signal_set_foreground_pgid(pgrp);
    else t->fg_pgrp = pgrp;
}

/* Raise `sig` for process group `id` (or process `id` with to_pid). The
 * console posts at once (it is not locked, and a reader spinning in place must
 * see its own SIGINT); a pty queues it (tty_take_sigs / tty_post_sigs). */
static void post_now(uint32_t id, int sig, int to_pid) {
    if (!id) return;
    if (to_pid) {
        process_t* p = process_get_by_pid(id);   // [M50-B1] held until posted
        if (p) { signal_post(p, sig); process_put(p); }
    } else {
        signal_post_pgid(id, sig);
    }
}
void tty_emit_sig(struct tty* t, uint32_t id, int sig, int to_pid) {
    if (!id) return;
    if (t->is_console) { post_now(id, sig, to_pid); return; }
    if (t->psig_n < TTY_PSIG_MAX) {
        t->psig[t->psig_n].id = id; t->psig[t->psig_n].sig = sig;
        t->psig[t->psig_n].to_pid = to_pid; t->psig_n++;
    }
}
/* Move the queued signals out (under the pty lock) ... */
int tty_take_sigs(struct tty* t, struct tty_sig* out) {
    int n = t->psig_n;
    for (int i = 0; i < n; i++) out[i] = t->psig[i];
    t->psig_n = 0;
    return n;
}
/* ... and post them once the lock is dropped. */
void tty_post_sigs(const struct tty_sig* s, int n) {
    for (int i = 0; i < n; i++) post_now(s[i].id, s[i].sig, s[i].to_pid);
}

static int sig_blocked_or_ignored(process_t* p, int sig) {
    if ((p->sig_blocked >> sig) & 1) return 1;
    return p->sig_handler[sig] == SIG_IGN;
}

/* Background access to a pty that is the caller's controlling terminal. */
int tty_bg_check(struct tty* t, process_t* cur, int sig) {
    if (!cur || cur->pid == 0 || t->is_console) return 0;
    if ((struct tty*)cur->ctty != t || t->session == 0 || t->session != cur->sid) return 0;
    uint32_t fg = t->fg_pgrp;
    if (!fg || cur->pgid == fg) return 0;
    if (sig_blocked_or_ignored(cur, sig)) return sig == SIGTTIN ? TTY_EIO : 0;
    tty_emit_sig(t, cur->pgid, sig, 0);
    return TTY_EINTR;
}

/* ---- output ------------------------------------------------------------------ */
int tty_onlcr(struct tty* t) {
    return (t->tio.c_oflag & T_OPOST) && (t->tio.c_oflag & T_ONLCR);
}
static void echo_n(struct tty* t, const char* s, int n) { t->out(t, s, n, tty_onlcr(t)); }
static void echo_c(struct tty* t, char c) { echo_n(t, &c, 1); }
/* Echo one input byte, control characters as ^X with ECHOCTL. */
static void echo_byte(struct tty* t, uint8_t c) {
    if (LF(t, T_ECHOCTL) && (c < 0x20 || c == 0x7F) && c != '\t' && c != '\n' && c != '\r') {
        char b[2] = { '^', (char)(c == 0x7F ? '?' : (c + '@')) };
        echo_n(t, b, 2);
    } else {
        echo_c(t, (char)c);
    }
}
/* Columns the echo of byte c took (for erase). UTF-8 continuation bytes: 0. */
static int echo_width(struct tty* t, uint8_t c) {
    if (c == '\t') return 1;
    if (c < 0x20 || c == 0x7F) return LF(t, T_ECHOCTL) ? 2 : 0;
    if (IF(t, T_IUTF8) && (c & 0xC0) == 0x80) return 0;
    return 1;
}

/* Console write path (fd 1/2 and /dev/console): termios output processing. */
int tty_write(const void* buf, int len) {
    if (len <= 0) return 0;
    struct tty* t = tty_console();
    terminal_write_tty((const char*)buf, (size_t)len, tty_onlcr(t));
    return len;
}

/* DSR/DA replies from the emulator are typed into the console input queue
 * (left unterminated in canonical mode: a reply is not a line). */
static void tty_reply(const char* s, int n) {
    struct tty* t = tty_console();
    for (int i = 0; i < n; i++) q_push(t, (uint8_t)s[i]);
}

/* ---- input processing -------------------------------------------------------- */
static void flush_input(struct tty* t) { t->n = 0; t->lnext = 0; q_clear(t); }

static void canon_commit(struct tty* t, int eof_marker) {
    if (eof_marker && t->n == 0) { q_push(t, TQ_EOF); return; }
    for (int i = 0; i < t->n; i++) q_push(t, (uint16_t)(t->line[i] | (i == t->n - 1 ? TQ_EOL : 0)));
    t->n = 0;
}

/* Erase one character (a whole UTF-8 sequence with IUTF8) from the line. */
static void canon_erase_one(struct tty* t, int do_echo) {
    if (t->n <= 0) return;
    int w = 0;
    for (;;) {
        uint8_t c = t->line[--t->n];
        w += echo_width(t, c);
        if (!(IF(t, T_IUTF8) && (c & 0xC0) == 0x80) || t->n == 0) break;
    }
    if (do_echo && LF(t, T_ECHO)) {
        if (LF(t, T_ECHOE)) { for (int i = 0; i < w; i++) echo_n(t, "\b \b", 3); }
        else echo_byte(t, t->tio.c_cc[T_VERASE]);
    }
}

static int isig_sig(struct tty* t, uint8_t c) {
    if (!LF(t, T_ISIG)) return 0;
    if (cc_is(t, T_VINTR, c)) return SIGINT;
    if (cc_is(t, T_VQUIT, c)) return SIGQUIT;
    if (cc_is(t, T_VSUSP, c)) return SIGTSTP;
    return 0;
}

/* Called from the PS/2 IRQ: is this byte a signal character right now? */
int tty_isig_for(char c) {
    return isig_sig(tty_console(), (uint8_t)c);
}
/* The IRQ posted `sig` for a signal character: flush pending input (unless
 * NOFLSH) at the next console operation. */
void tty_note_isig(int sig) {
    (void)sig;
    struct tty* t = tty_console();
    if (!(t->tio.c_lflag & T_NOFLSH)) t->flush_req = 1;
}

static void process_byte(struct tty* t, uint8_t c) {
    if (IF(t, T_ISTRIP)) c &= 0x7F;
    if (t->lnext) {                                   /* literal next */
        t->lnext = 0;
        if (LF(t, T_ICANON)) {
            if (LF(t, T_ECHO) && LF(t, T_ECHOCTL)) echo_n(t, "\b\b", 2);
            if (t->n < TTY_LINE - 1) { t->line[t->n++] = c; if (LF(t, T_ECHO)) echo_byte(t, c); }
        } else {
            q_push(t, c);
        }
        return;
    }
    if (c == '\r') {
        if (IF(t, T_IGNCR)) return;
        if (IF(t, T_ICRNL)) c = '\n';
    } else if (c == '\n' && IF(t, T_INLCR)) {
        c = '\r';
    }

    int sig = isig_sig(t, c);
    if (sig) {
        if (!LF(t, T_NOFLSH)) flush_input(t);
        if (LF(t, T_ECHO)) echo_byte(t, c);
        uint32_t fg = tty_fg(t);
        if (fg) tty_emit_sig(t, fg, sig, 0);
        else if (sig == SIGINT && t->is_console) {
            if (LF(t, T_ECHO)) echo_c(t, '\n');
            t->intr_noproc = 1;
        }
        return;
    }

    if (!LF(t, T_ICANON)) {
        q_push(t, c);
        t->last_rx = timer_get_ticks();
        if (LF(t, T_ECHO)) echo_byte(t, c);
        else if (c == '\n' && LF(t, T_ECHONL)) echo_c(t, '\n');
        return;
    }

    /* Canonical editing. */
    if (LF(t, T_IEXTEN) && cc_is(t, T_VLNEXT, c)) {
        t->lnext = 1;
        if (LF(t, T_ECHO) && LF(t, T_ECHOCTL)) echo_n(t, "^\b", 2);
        return;
    }
    if (cc_is(t, T_VERASE, c) || c == 0x08 || c == 0x7F) {   /* both BS and DEL erase */
        canon_erase_one(t, 1);
        return;
    }
    if (cc_is(t, T_VKILL, c)) {
        if (LF(t, T_ECHO) && !LF(t, T_ECHOKE) && LF(t, T_ECHOK)) {
            echo_byte(t, c); echo_c(t, '\n'); t->n = 0;
        } else {
            while (t->n > 0) canon_erase_one(t, 1);
        }
        return;
    }
    if (LF(t, T_IEXTEN) && cc_is(t, T_VWERASE, c)) {
        while (t->n > 0 && (t->line[t->n - 1] == ' ' || t->line[t->n - 1] == '\t')) canon_erase_one(t, 1);
        while (t->n > 0 && t->line[t->n - 1] != ' ' && t->line[t->n - 1] != '\t') canon_erase_one(t, 1);
        return;
    }
    if (LF(t, T_IEXTEN) && cc_is(t, T_VREPRINT, c)) {
        if (LF(t, T_ECHO)) {
            echo_byte(t, c); echo_c(t, '\n');
            for (int i = 0; i < t->n; i++) echo_byte(t, t->line[i]);
        }
        return;
    }
    if (cc_is(t, T_VEOF, c)) {
        canon_commit(t, 1);                            /* VEOF itself is not stored */
        return;
    }
    if (c == '\n' || cc_is(t, T_VEOL, c) || (LF(t, T_IEXTEN) && cc_is(t, T_VEOL2, c))) {
        if (LF(t, T_ECHO) || (c == '\n' && LF(t, T_ECHONL))) echo_byte(t, c);
        if (t->n < TTY_LINE) t->line[t->n++] = c;
        else t->line[TTY_LINE - 1] = c;                /* full: the terminator still ends it */
        canon_commit(t, 0);
        return;
    }
    if (t->n < TTY_LINE - 1) {                         /* keep room for the terminator */
        t->line[t->n++] = c;
        if (LF(t, T_ECHO)) echo_byte(t, c);
    }
}

void tty_input_byte(struct tty* t, uint8_t c) {
    if (t->flush_req) { t->flush_req = 0; flush_input(t); }
    process_byte(t, c);
}

/* How many more input bytes this terminal can take without dropping any: the
 * pending canonical line still has to fit in the read queue when it is
 * committed. In canonical mode a full line keeps accepting (the bytes are
 * discarded, as n_tty does) so a writer never deadlocks on a line nobody ends. */
int tty_input_room(struct tty* t) {
    int room = q_space(t) - t->n - 1;
    if (LF(t, T_ICANON) && t->n >= TTY_LINE - 1 && room > 0) return room;
    return room > 0 ? room : 0;
}

/* Drain the raw console input sources into the discipline. In non-canonical
 * mode a lone ESC waits a few ms for the rest of an escape sequence: COM1
 * delivers bytes one at a time, and a reader that sees ESC alone (readline,
 * through FIONREAD) would take it as a bare Escape key instead of the start of
 * ESC [ A. PS/2 and USB queue whole sequences atomically. */
static void pull_input(struct tty* t) {
    if (t->flush_req) { t->flush_req = 0; flush_input(t); }
    if (!t->is_console) return;                        /* a pty is pushed by its master */
    for (;;) {
        int c = keyboard_getc_raw();
        if (c < 0) break;
        process_byte(t, (uint8_t)c);
        if (c == 0x1B && !LF(t, T_ICANON) && !keyboard_input_pending_noirq()) {
            uint64_t t0 = timer_get_ticks();
            uint32_t hz = timer_get_frequency(); if (!hz) hz = 1000;
            uint64_t wait = hz / 100 + 1;              /* ~10 ms */
            while (!keyboard_input_pending_noirq() && timer_get_ticks() - t0 < wait)
                cpu_wait_irq();
        }
    }
}

/* Bytes a read() can take now (FIONREAD semantics: in canonical mode only
 * complete lines count). */
static int readable_count(struct tty* t) {
    int n = q_count(t);
    if (!LF(t, T_ICANON)) return n;
    int last = -1;
    for (int i = 0; i < n; i++) {
        uint16_t v = t->q[(t->qh + i) % TQ_SIZE];
        if (v & (TQ_EOL | TQ_EOF)) last = i;
    }
    return last + 1;
}
/* poll(POLLIN): data, or a pending EOF marker, would satisfy a read now. */
int tty_ldisc_readable(struct tty* t) {
    pull_input(t);
    int n = readable_count(t);
    if (n == 0 && LF(t, T_ICANON) && q_count(t) > 0 && (t->q[t->qh] & TQ_EOF)) return 1;
    if (n == 0 && !LF(t, T_ICANON) && t->tio.c_cc[T_VMIN] == 0) return 1;  /* never waits */
    return n;
}

/* ---- read -------------------------------------------------------------------- */
/* Copy from the read queue. Canonical: stop after one line (and swallow an EOF
 * marker, returning 0 if it is first). */
static int take(struct tty* t, uint8_t* out, int len) {
    int m = 0;
    if (LF(t, T_ICANON)) {
        if (q_count(t) > 0 && (t->q[t->qh] & TQ_EOF)) { t->qh = (t->qh + 1) % TQ_SIZE; return 0; }
        while (m < len && q_count(t) > 0) {
            uint16_t v = t->q[t->qh];
            if (v & TQ_EOF) break;
            t->qh = (t->qh + 1) % TQ_SIZE;
            out[m++] = (uint8_t)v;
            if (v & TQ_EOL) break;
        }
        return m;
    }
    while (m < len && q_count(t) > 0) {
        uint16_t v = t->q[t->qh];
        t->qh = (t->qh + 1) % TQ_SIZE;
        if (v & TQ_EOF) continue;                       /* stale canonical EOF */
        out[m++] = (uint8_t)v;
    }
    return m;
}

/* Wait for more input. Console: block in the scheduler (TTY_RD_BLOCK, the
 * timer tick wakes it) or spin. Pty: always hand back WOULDBLOCK with
 * wait_pty armed (the master's write wakes it). Returns 0 to retry, or an
 * error/WOULDBLOCK to hand back. When interruption is due (signal pending, the
 * line cancelled) the partially typed line stays, so the re-issued read()
 * continues it (bash treats a non-EINTR read error on stdin as EOF). */
static int wait_input(struct tty* t, process_t* cur, int mode) {
    if (cur && signal_pending(cur)) return TTY_EINTR;   /* [M30] EINTR */
    if (mode == TTY_RD_NONBLOCK) return TTY_EAGAIN;
    if (!t->is_console) {
        if (mode != TTY_RD_BLOCK || !cur || cur->pid == 0) return TTY_EAGAIN;
        cur->wait_pty = t->owner;                       /* sched_wake_pty(owner) */
        return TTY_WOULDBLOCK;
    }
    if (mode == TTY_RD_BLOCK && cur && cur->pid != 0) {
        cur->wait_tty = 1;                              /* armed; tty_wake_readers() */
        return TTY_WOULDBLOCK;
    }
    cpu_wait_irq();
    return 0;
}

/* Returns >0 bytes (up to len), 0 on EOF (VEOF on an empty line, or a VMIN=0
 * timeout), -EINTR when a signal is pending or the line was cancelled, -EIO
 * for a background reader that cannot be stopped or a hung-up pty, -EAGAIN
 * (nonblocking), -2 when the caller should block (TTY_RD_BLOCK). */
int tty_ldisc_read(struct tty* t, void* buf, int len, int mode) {
    if (len <= 0) return -1;
    uint8_t* out = (uint8_t*)buf;
    process_t* cur = sched_get_current();

    /* Job control: only the foreground process group may read. A background
     * reader gets SIGTTIN (its whole group stops, so a shell sees the job as
     * Stopped) and its read restarts once it is resumed in the foreground; with
     * SIGTTIN blocked/ignored it gets EIO (POSIX). The console keeps its
     * historical rule (any process outside the foreground group); a pty
     * applies it to members of the session it controls. */
    if (t->is_console) {
        uint32_t fg = signal_get_foreground_pgid();
        if (cur && fg && cur->pgid != fg) {
            if (!signal_ttin_acts(cur)) return TTY_EIO;
            signal_post_pgid(cur->pgid, SIGTTIN);
            return TTY_EINTR;
        }
    } else {
        int r = tty_bg_check(t, cur, SIGTTIN);
        if (r) return r;
    }

    for (;;) {
        pull_input(t);
        if (t->intr_noproc) { t->intr_noproc = 0; return TTY_EINTR; }   /* Ctrl-C, no fg group */
        if (t->hungup) {                                /* pty master gone: drain, then EIO */
            int any = LF(t, T_ICANON) ? (readable_count(t) > 0 || (q_count(t) > 0 && (t->q[t->qh] & TQ_EOF)))
                                      : q_count(t) > 0;
            if (!any) return TTY_EIO;
            return take(t, out, len);
        }
        if (LF(t, T_ICANON)) {
            if (readable_count(t) > 0 || (q_count(t) > 0 && (t->q[t->qh] & TQ_EOF))) {
                return take(t, out, len);
            }
        } else {
            int vmin = t->tio.c_cc[T_VMIN], vtime = t->tio.c_cc[T_VTIME];
            int avail = q_count(t);
            uint64_t now = timer_get_ticks();
            uint32_t hz = timer_get_frequency(); if (!hz) hz = 1000;
            uint64_t tlen = (uint64_t)vtime * hz / 10;    /* VTIME is in 1/10 s */
            int need = vmin < len ? vmin : len;
            if (vmin == 0 && vtime == 0) return take(t, out, len);        /* pure poll */
            if (vmin > 0 && avail >= need) { t->vtime_pid = 0; return take(t, out, len); }
            if (vmin > 0 && vtime > 0 && avail > 0 && now - t->last_rx >= tlen)
                return take(t, out, len);                                 /* inter-byte timeout */
            if (vmin == 0) {
                if (avail > 0) { t->vtime_pid = 0; return take(t, out, len); }
                uint32_t pid = cur ? cur->pid : 0;
                if (t->vtime_pid != pid + 1) { t->vtime_pid = pid + 1; t->vtime_start = now; }
                if (now - t->vtime_start >= tlen) { t->vtime_pid = 0; return 0; }   /* timeout */
            }
        }
        int w = wait_input(t, cur, mode);
        if (w != 0) {
            if (w != TTY_WOULDBLOCK) t->vtime_pid = 0;
            return w;
        }
    }
}

/* Console reads (kernel callers spin; SYS_READ on fd 0 and /dev/console blocks
 * in the scheduler: the INT 0x80 path only preempts ring 3, so a reader
 * spinning in ring 0 starved every other process). */
int tty_read(void* buf, int len) { return tty_ldisc_read(tty_console(), buf, len, TTY_RD_SPIN); }
int tty_read_try(void* buf, int len) { return tty_ldisc_read(tty_console(), buf, len, TTY_RD_BLOCK); }
int tty_read_nonblock(void* buf, int len) { return tty_ldisc_read(tty_console(), buf, len, TTY_RD_NONBLOCK); }

/* Timer tick: wake blocked console readers when PS/2/serial input is waiting,
 * and every TTY_POLL_TICKS regardless so the reader re-polls the USB HID
 * keyboard (which must not be polled from an ISR) and re-checks VTIME. Blocked
 * pty readers/writers are re-run on the same period: their VTIME deadlines
 * need it, and it bounds a wakeup lost between "nothing to do" and the block. */
#define TTY_POLL_TICKS 8
static void tty_wake_cb(process_t* p, void* u) {
    int periodic = *(int*)u;
    if (p->state != PROC_BLOCKED) return;
    if (p->wait_tty) { p->wait_tty = 0; p->state = PROC_READY; }
    else if (periodic && p->wait_pty) { p->wait_pty = 0; p->state = PROC_READY; }
}
void tty_wake_readers(uint64_t now) {
    extern void process_foreach(void (*cb)(process_t*, void*), void* user);
    int periodic = (now % TTY_POLL_TICKS) == 0;
    if (!periodic && !keyboard_input_pending_noirq()) return;
    process_foreach(tty_wake_cb, &periodic);
}

/* ---- ioctl ------------------------------------------------------------------- */
/* termios + window size + queue ioctls. Job-control and session ioctls
 * (TIOCGPGRP/TIOCSPGRP/TIOCSCTTY/TIOCNOTTY/TIOCGSID) are handled by the
 * caller (syscall.c for the console, pty.c for a pty) next to the process
 * checks. Returns 0 / a negative errno; ENOIOCTLCMD (-515) if not here. */
static int k_copy_to(void* u, const void* k, unsigned n) { return copy_to_user(u, k, n) ? -14 : 0; }
static int k_copy_from(void* k, const void* u, unsigned n) { return copy_from_user(k, u, n) ? -14 : 0; }
/* [M50] `arg` is a user pointer, or (kb) a kernel buffer the caller copies
 * in before / out after its spinlock: no user copy may run under one. */
static int x_copy_to(int kb, void* u, const void* k, unsigned n) {
    if (!kb) return k_copy_to(u, k, n);
    for (unsigned i = 0; i < n; i++) ((uint8_t*)u)[i] = ((const uint8_t*)k)[i];
    return 0;
}
static int x_copy_from(int kb, void* k, const void* u, unsigned n) {
    if (!kb) return k_copy_from(k, u, n);
    for (unsigned i = 0; i < n; i++) ((uint8_t*)k)[i] = ((const uint8_t*)u)[i];
    return 0;
}

void tty_init(void) {
    tty_console();
    vt_set_reply_hook(tty_reply);
}

static void winsize_get(struct tty* t, struct tty_winsize* w) {
    if (!t->is_console) { *w = t->ws; return; }
    int cols, rows; terminal_get_size(&cols, &rows);
    w->ws_row = t->ws.ws_row ? t->ws.ws_row : (uint16_t)rows;
    w->ws_col = t->ws.ws_col ? t->ws.ws_col : (uint16_t)cols;
    w->ws_xpixel = (uint16_t)(cols * 8); w->ws_ypixel = (uint16_t)(rows * 16);
}

/* Bytes of the user payload of ldisc request `cmd` (0 = none); *in = 1 when
 * the kernel reads it (set requests), 0 when it writes it (get requests). */
unsigned tty_ldisc_ioctl_payload(uint64_t cmd, int* in) {
    switch (cmd) {
    case TTY_TCGETS:   *in = 0; return sizeof(struct tty_termios);
    case TTY_TCSETS: case TTY_TCSETSW: case TTY_TCSETSF:
                       *in = 1; return sizeof(struct tty_termios);
    case TTY_TIOCGWINSZ: *in = 0; return sizeof(struct tty_winsize);
    case TTY_TIOCSWINSZ: *in = 1; return sizeof(struct tty_winsize);
    case TTY_FIONREAD: case TTY_TIOCOUTQ: *in = 0; return sizeof(int);
    default:           *in = 0; return 0;
    }
}

static int ldisc_ioctl(struct tty* t, uint64_t cmd, uint64_t arg, int kb) {
    switch (cmd) {
    case TTY_TCGETS:
        return x_copy_to(kb, (void*)arg, &t->tio, sizeof(t->tio));
    case TTY_TCSETS: case TTY_TCSETSW: case TTY_TCSETSF: {
        struct tty_termios nt;
        int r = x_copy_from(kb, &nt, (void*)arg, sizeof(nt));
        if (r) return r;
        int was_canon = (t->tio.c_lflag & T_ICANON) != 0;
        int now_canon = (nt.c_lflag & T_ICANON) != 0;
        if (cmd == TTY_TCSETSF) flush_input(t);        /* TCSAFLUSH: drop unread input */
        t->tio = nt;
        if (was_canon && !now_canon) {
            /* Leaving canonical mode: the partially typed line becomes readable
             * as raw bytes (n_tty does the same). */
            for (int i = 0; i < t->n; i++) q_push(t, t->line[i]);
            t->n = 0;
            for (int i = 0; i < q_count(t); i++) {
                int k = (t->qh + i) % TQ_SIZE;
                if (t->q[k] & TQ_EOF) t->q[k] = TQ_EOF;   /* dropped by a raw take() */
            }
        } else if (!was_canon && now_canon && q_count(t) > 0) {
            /* Entering canonical mode with raw bytes pending: make them one
             * readable line instead of stranding them. */
            int k = (t->qt - 1 + TQ_SIZE) % TQ_SIZE;
            t->q[k] |= TQ_EOL;
        }
        t->vtime_pid = 0;
        return 0;
    }
    case TTY_TIOCGWINSZ: {
        struct tty_winsize w;
        winsize_get(t, &w);
        return x_copy_to(kb, (void*)arg, &w, sizeof(w));
    }
    case TTY_TIOCSWINSZ: {
        struct tty_winsize w, old;
        int r = x_copy_from(kb, &w, (void*)arg, sizeof(w));
        if (r) return r;
        winsize_get(t, &old);
        if (t->is_console) {
            /* Setting the real size (readline re-sets what it read) clears the override. */
            int cols, rows; terminal_get_size(&cols, &rows);
            t->ws.ws_row = (w.ws_row == rows) ? 0 : w.ws_row;
            t->ws.ws_col = (w.ws_col == cols) ? 0 : w.ws_col;
        } else {
            t->ws = w;
        }
        if (w.ws_row != old.ws_row || w.ws_col != old.ws_col) {   /* Linux: only on a change */
            uint32_t fg = tty_fg(t);
            if (fg) tty_emit_sig(t, fg, SIGWINCH, 0);
        }
        return 0;
    }
    case TTY_FIONREAD: {
        pull_input(t);
        int n = readable_count(t);
        return x_copy_to(kb, (void*)arg, &n, sizeof(n));
    }
    case TTY_TIOCOUTQ: {
        int n = 0;
        return x_copy_to(kb, (void*)arg, &n, sizeof(n));
    }
    case TTY_TCFLSH:
        if (arg == 0 || arg == 2) { pull_input(t); flush_input(t); }   /* TCIFLUSH / TCIOFLUSH */
        return 0;
    case TTY_TCSBRK: case TTY_TCXONC:
        return 0;
    default:
        return -515;
    }
}

int tty_ldisc_ioctl(struct tty* t, uint64_t cmd, uint64_t arg) { return ldisc_ioctl(t, cmd, arg, 0); }
int tty_ldisc_ioctl_kbuf(struct tty* t, uint64_t cmd, void* kbuf) {
    return ldisc_ioctl(t, cmd, (uint64_t)kbuf, 1);
}

/* [M40] Console-only requests: graphics handoff (KDSETMODE/KDGETMODE, see
 * drivers/fbdev.c) and the keyboard layout (KDSKBLAYOUT/KDGKBLAYOUT). */
/* [M43] Global console state (layout, graphics mode): root, or the user the
 * console belongs to (login chowns /dev/console to whoever logs in there). */
static int console_owner_ok(void) {
    extern process_t* sched_get_current(void);
    extern vfs_inode_t* vfs_lookup(const char*);
    process_t* c = sched_get_current();
    if (cred_is_root(c)) return 1;
    vfs_inode_t* con = vfs_lookup("/dev/console");
    return con && con->uid == c->cred.euid;
}
static int console_kd_ioctl(uint64_t cmd, uint64_t arg) {
    if ((cmd == KDSKBLAYOUT || cmd == KDSETMODE) && !console_owner_ok()) return -1;   /* -EPERM */
    if (cmd == KDSKBLAYOUT) {
        char name[16];
        int i = 0;
        for (; i < 15; i++) {
            if (copy_from_user(&name[i], (const char*)arg + i, 1)) return -14;
            if (!name[i]) break;
        }
        name[i] = 0;
        if (keyboard_set_keymap(name) != 0) return -22;          /* unknown layout */
        debugcon_writestring("[KBD] keymap ");
        debugcon_writestring(keyboard_keymap_name());
        debugcon_writestring("\n");
        return 0;
    }
    if (cmd == KDGKBLAYOUT) {
        char name[16] = { 0 };
        const char* n = keyboard_keymap_name();
        for (int i = 0; n[i] && i < 15; i++) name[i] = n[i];
        return k_copy_to((void*)arg, name, sizeof(name));
    }
    return vtcon_ioctl(cmd, arg);                                /* -515 if not KD* */
}

/* Console ioctl entry (termios, window size, queues, console mode, keymap). */
int tty_ioctl(uint64_t cmd, uint64_t arg) {
    int r = console_kd_ioctl(cmd, arg);
    if (r != -515) return r;
    return tty_ldisc_ioctl(tty_console(), cmd, arg);
}

/* Back to the boot defaults (cooked, echo, ISIG) with empty queues: the
 * kernel shell calls it when a foreground program it ran has finished. */
void tty_reset(void) {
    struct tty* t = tty_console();
    tio_defaults(&t->tio);
    flush_input(t);
    t->flush_req = 0; t->intr_noproc = 0; t->vtime_pid = 0;
}

/* Current console termios (for the self-test / debugging). */
void tty_get_termios(struct tty_termios* out) { *out = tty_console()->tio; }
