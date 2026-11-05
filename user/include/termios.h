/* SecOS - <termios.h> terminal control (over SYS_IOCTL).
 * Layout matches the kernel's struct tty_termios (kernel/tty.h); flag values
 * and c_cc indices are Linux's. SPDX-License-Identifier: MIT */
#ifndef _TERMIOS_H
#define _TERMIOS_H

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;
#define NCCS 20

struct termios {
    tcflag_t c_iflag;   /* input modes */
    tcflag_t c_oflag;   /* output modes */
    tcflag_t c_cflag;   /* control modes (speed in CBAUD) */
    tcflag_t c_lflag;   /* local modes */
    cc_t     c_cc[NCCS]; /* control characters */
};

/* c_cc indices */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16
#define _POSIX_VDISABLE '\0'

/* c_iflag */
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IUCLC   0001000
#define IXON    0002000
#define IXANY   0004000
#define IXOFF   0010000
#define IMAXBEL 0020000
#define IUTF8   0040000

/* c_oflag */
#define OPOST   0000001
#define OLCUC   0000002
#define ONLCR   0000004
#define OCRNL   0000010
#define ONOCR   0000020
#define ONLRET  0000040
#define OFILL   0000100
#define OFDEL   0000200
#define NLDLY   0000400
#define CRDLY   0003000
#define TABDLY  0014000
#define TAB0    0000000
#define TAB3    0014000
#define XTABS   0014000
#define BSDLY   0020000
#define VTDLY   0040000
#define FFDLY   0100000

/* c_cflag */
#define CBAUD   0010017
#define B0      0000000
#define B50     0000001
#define B75     0000002
#define B110    0000003
#define B134    0000004
#define B150    0000005
#define B200    0000006
#define B300    0000007
#define B600    0000010
#define B1200   0000011
#define B1800   0000012
#define B2400   0000013
#define B4800   0000014
#define B9600   0000015
#define B19200  0000016
#define B38400  0000017
#define B57600  0010001
#define B115200 0010002
#define B230400 0010003
#define CSIZE   0000060
#define CS5     0000000
#define CS6     0000020
#define CS7     0000040
#define CS8     0000060
#define CSTOPB  0000100
#define CREAD   0000200
#define PARENB  0000400
#define PARODD  0001000
#define HUPCL   0002000
#define CLOCAL  0004000
#define CRTSCTS 020000000000

/* c_lflag */
#define ISIG    0000001
#define ICANON  0000002
#define XCASE   0000004
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define ECHOCTL 0001000
#define ECHOPRT 0002000
#define ECHOKE  0004000
#define FLUSHO  0010000
#define PENDIN  0040000
#define IEXTEN  0100000
#define EXTPROC 0200000

/* tcsetattr 'how' */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2
/* tcflush queue selector */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2
/* tcflow actions */
#define TCOOFF 0
#define TCOON  1
#define TCIOFF 2
#define TCION  3

/* ioctl requests */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TCSBRK     0x5409
#define TCXONC     0x540A
#define TCFLSH     0x540B
#define TIOCSCTTY  0x540E
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCOUTQ   0x5411
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD   0x541B
#define TIOCINQ    FIONREAD
#define TIOCNOTTY  0x5422
#define TIOCGSID   0x5429

struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };

int  tcgetattr(int fd, struct termios* t);
int  tcsetattr(int fd, int how, const struct termios* t);
void cfmakeraw(struct termios* t);
speed_t cfgetospeed(const struct termios* t);
speed_t cfgetispeed(const struct termios* t);
int  cfsetospeed(struct termios* t, speed_t s);
int  cfsetispeed(struct termios* t, speed_t s);
int  cfsetspeed(struct termios* t, speed_t s);
int  tcflush(int fd, int queue);
int  tcdrain(int fd);
int  tcflow(int fd, int action);
int  tcsendbreak(int fd, int duration);
int  tcgetpgrp(int fd);
int  tcsetpgrp(int fd, int pgrp);
int  tcgetsid(int fd);
int  ioctl(int fd, unsigned long request, void* arg);

#endif
