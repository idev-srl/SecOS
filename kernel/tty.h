/*
 * SecOS Kernel - Console TTY line discipline (termios)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * The termios ABI shared with user/include/termios.h. Flag values and c_cc
 * indices are Linux's, so ported programs that hard-code them still work; the
 * struct itself is SecOS's own (no c_line, NCCS = 20, speeds in c_cflag).
 */
#ifndef TTY_H
#define TTY_H
#include <stdint.h>

#define T_NCCS 20
struct tty_termios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_cc[T_NCCS];
};
struct tty_winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

/* c_iflag */
#define T_IGNBRK  0000001
#define T_BRKINT  0000002
#define T_IGNPAR  0000004
#define T_PARMRK  0000010
#define T_INPCK   0000020
#define T_ISTRIP  0000040
#define T_INLCR   0000100
#define T_IGNCR   0000200
#define T_ICRNL   0000400
#define T_IXON    0002000
#define T_IXANY   0004000
#define T_IXOFF   0010000
#define T_IMAXBEL 0020000
#define T_IUTF8   0040000
/* c_oflag */
#define T_OPOST   0000001
#define T_ONLCR   0000004
/* c_cflag */
#define T_B38400  0000017
#define T_CS8     0000060
#define T_CREAD   0000200
#define T_HUPCL   0002000
/* c_lflag */
#define T_ISIG    0000001
#define T_ICANON  0000002
#define T_ECHO    0000010
#define T_ECHOE   0000020
#define T_ECHOK   0000040
#define T_ECHONL  0000100
#define T_NOFLSH  0000200
#define T_TOSTOP  0000400
#define T_ECHOCTL 0001000
#define T_ECHOPRT 0002000
#define T_ECHOKE  0004000
#define T_IEXTEN  0100000
/* c_cc indices */
#define T_VINTR    0
#define T_VQUIT    1
#define T_VERASE   2
#define T_VKILL    3
#define T_VEOF     4
#define T_VTIME    5
#define T_VMIN     6
#define T_VSWTC    7
#define T_VSTART   8
#define T_VSTOP    9
#define T_VSUSP    10
#define T_VEOL     11
#define T_VREPRINT 12
#define T_VDISCARD 13
#define T_VWERASE  14
#define T_VLNEXT   15
#define T_VEOL2    16

/* ioctl requests (Linux numbers) */
#define TTY_TCGETS     0x5401
#define TTY_TCSETS     0x5402
#define TTY_TCSETSW    0x5403
#define TTY_TCSETSF    0x5404
#define TTY_TCSBRK     0x5409
#define TTY_TCXONC     0x540A
#define TTY_TCFLSH     0x540B
#define TTY_TIOCSCTTY  0x540E
#define TTY_TIOCGPGRP  0x540F
#define TTY_TIOCSPGRP  0x5410
#define TTY_TIOCOUTQ   0x5411
#define TTY_TIOCGWINSZ 0x5413
#define TTY_TIOCSWINSZ 0x5414
#define TTY_FIONREAD   0x541B
#define TTY_TIOCNOTTY  0x5422
#define TTY_FIONBIO    0x5421
#define TTY_TIOCGSID   0x5429
#define TTY_TIOCGPTN   0x80045430   /* pty master: slave index (unsigned int) */
#define TTY_TIOCSPTLCK 0x40045431   /* pty master: lock (!=0) / unlock (0) the slave */
#define TTY_TIOCGPTLCK 0x80045439   /* pty master: lock state */

/* ---- line discipline instance ------------------------------------------------
 * One struct tty per terminal: the console (tty_console()) and the slave side of
 * every pseudo-terminal (kernel/pty.c). The same n_tty-style code (termios input
 * and output processing, canonical editing, ISIG, VMIN/VTIME, job control)
 * serves all of them; only the byte source and sink differ:
 *   console: input pulled from the keyboard/serial ring, output to the VT;
 *   pty:     input pushed by master writes, output into the master's ring.
 */
#define TTY_LINE 1024
#define TQ_SIZE  4096
struct tty;
struct tty_sig { uint32_t id; int sig; int to_pid; };
#define TTY_PSIG_MAX 8
/* Output sink: `n` bytes; `onlcr` asks for NL -> CR NL (OPOST|ONLCR). */
typedef void (*tty_out_fn)(struct tty* t, const char* s, int n, int onlcr);

struct tty {
    struct tty_termios tio;
    struct tty_winsize ws;          /* pty: the size; console: TIOCSWINSZ override */
    uint8_t  line[TTY_LINE];        /* canonical line being edited */
    int      n, lnext;
    uint16_t q[TQ_SIZE];            /* read queue (TQ_* flags, see tty.c) */
    int      qh, qt;
    volatile int flush_req;         /* ISIG from the IRQ: flush before the next op */
    volatile int intr_noproc;       /* console VINTR with no foreground group */
    uint64_t last_rx, vtime_start;  /* VTIME bookkeeping */
    uint32_t vtime_pid;
    tty_out_fn out;
    int      is_console;
    /* Job control. The console's foreground group is signal.c's
     * foreground_pgid (the keyboard IRQ targets it); a pty keeps its own. */
    uint32_t fg_pgrp;               /* foreground process group (0 = none) */
    uint32_t session;               /* session this is the controlling tty of (0 = none) */
    int      hungup;                /* pty master gone: read drains then EIO, write EIO */
    /* Signals raised while a pty lock is held, posted once it is dropped. */
    int      psig_n;
    struct tty_sig psig[TTY_PSIG_MAX];
    void*    owner;                 /* pty_t* for a pty slave, NULL for the console */
};

/* tty_ldisc_read modes */
#define TTY_RD_SPIN     0           /* kernel caller: wait in place for input */
#define TTY_RD_BLOCK    1           /* syscall: return -2 with the wait armed */
#define TTY_RD_NONBLOCK 2           /* O_NONBLOCK: -EAGAIN */

struct process;
struct tty* tty_console(void);
void tty_instance_init(struct tty* t, tty_out_fn out, void* owner); /* pty defaults */
void tty_input_byte(struct tty* t, uint8_t c);       /* one byte through input processing */
int  tty_input_room(struct tty* t);                  /* bytes input can still accept */
int  tty_ldisc_read(struct tty* t, void* buf, int len, int mode);
int  tty_ldisc_readable(struct tty* t);              /* bytes a read() could take now */
int  tty_ldisc_ioctl(struct tty* t, uint64_t cmd, uint64_t arg); /* -515: not a ldisc ioctl */
/* [M50] The same on a kernel buffer of tty_ldisc_ioctl_payload() bytes, for a
 * caller that holds a spinlock (pty.c): it copies the payload in before and out
 * after the lock. */
int  tty_ldisc_ioctl_kbuf(struct tty* t, uint64_t cmd, void* kbuf);
unsigned tty_ldisc_ioctl_payload(uint64_t cmd, int* in);
int  tty_onlcr(struct tty* t);                       /* OPOST && ONLCR */
uint32_t tty_fg(struct tty* t);                      /* foreground process group */
void tty_set_fg(struct tty* t, uint32_t pgrp);
void tty_emit_sig(struct tty* t, uint32_t id, int sig, int to_pid);
int  tty_take_sigs(struct tty* t, struct tty_sig* out); /* under the lock: move them out */
void tty_post_sigs(const struct tty_sig* s, int n);   /* then post with no lock held */
/* Job control for a pty: is `cur` a background member of this terminal's
 * session? Returns 0 (go ahead) or -EINTR (sig posted to its group) / -EIO
 * (sig blocked or ignored; TTOU: ignored => 0 per POSIX). */
int  tty_bg_check(struct tty* t, struct process* cur, int sig);

void tty_init(void);
int  tty_read(void* buf, int len);           /* spins (kernel callers) */
int  tty_read_try(void* buf, int len);       /* -2: caller blocks (wait_tty) */
int  tty_read_nonblock(void* buf, int len);  /* -EAGAIN when empty */
int  tty_write(const void* buf, int len);    /* termios output processing */
int  tty_ioctl(uint64_t cmd, uint64_t arg);  /* -515: not handled here */
int  tty_isig_for(char c);                   /* IRQ: signal for this byte, or 0 */
void tty_note_isig(int sig);
void tty_wake_readers(uint64_t now);
void tty_get_termios(struct tty_termios* out);
void tty_reset(void);                        /* boot defaults, queues flushed */

#endif
