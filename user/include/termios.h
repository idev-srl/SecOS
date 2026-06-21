/* SecOS - [M39] <termios.h> minimal terminal control (over SYS_IOCTL).
 * Layout matches the kernel's struct secos_termios. SPDX-License-Identifier: MIT */
#ifndef _TERMIOS_H
#define _TERMIOS_H

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
#define NCCS 20

struct termios {
    tcflag_t c_iflag;   /* input modes */
    tcflag_t c_oflag;   /* output modes */
    tcflag_t c_cflag;   /* control modes */
    tcflag_t c_lflag;   /* local modes */
    cc_t     c_cc[NCCS]; /* control characters (VMIN=4, VTIME=5) */
};

/* c_lflag bits (subset) */
#define ISIG   0x0001
#define ICANON 0x0002
#define ECHO   0x0008

/* tcsetattr 'how' (accepted, no queue distinction) */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* control chars */
#define VMIN  4
#define VTIME 5

/* ioctl requests */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGWINSZ 0x5413
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410

struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };

int  tcgetattr(int fd, struct termios* t);
int  tcsetattr(int fd, int how, const struct termios* t);
void cfmakeraw(struct termios* t);
int  tcgetpgrp(int fd);
int  tcsetpgrp(int fd, int pgrp);
int  ioctl(int fd, unsigned long request, void* arg);

#endif
