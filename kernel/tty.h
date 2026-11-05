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

#endif
