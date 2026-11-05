/* SecOS BusyBox overlay: <sys/ioctl.h> (Linux request numbers; the kernel
 * implements TCGETS/TCSETS*, TIOCGWINSZ, TIOCG/SPGRP; others fail ENOTTY).
 * SPDX-License-Identifier: MIT */
#ifndef _BB_SYS_IOCTL_H
#define _BB_SYS_IOCTL_H
#include <termios.h>
#define TCSBRK      0x5409
#define TCXONC      0x540A
#define TCFLSH      0x540B
#define TIOCEXCL    0x540C
#define TIOCNXCL    0x540D
#define TIOCSCTTY   0x540E
#define TIOCOUTQ    0x5411
#define TIOCSTI     0x5412
#define TIOCSWINSZ  0x5414
#define TIOCMGET    0x5415
#define TIOCMBIS    0x5416
#define TIOCMBIC    0x5417
#define TIOCMSET    0x5418
#define TIOCGSOFTCAR 0x5419
#define TIOCSSOFTCAR 0x541A
#define FIONREAD    0x541B
#define TIOCINQ     FIONREAD
#define TIOCLINUX   0x541C
#define TIOCCONS    0x541D
#define FIONBIO     0x5421
#define TIOCNOTTY   0x5422
#define TIOCSETD    0x5423
#define TIOCGETD    0x5424
#define TIOCSBRK    0x5427
#define TIOCCBRK    0x5428
#define TIOCGSID    0x5429
#define TIOCGPTN    0x80045430
#define TIOCSPTLCK  0x40045431
#define FIONCLEX    0x5450
#define FIOCLEX     0x5451
#define FIOASYNC    0x5452
#define TIOCGSERIAL 0x541E
#define TIOCSSERIAL 0x541F
#define TIOCVHANGUP 0x5437
#define SIOCGIFNAME    0x8910
#define SIOCGIFCONF    0x8912
#define SIOCGIFFLAGS   0x8913
#define SIOCSIFFLAGS   0x8914
#define SIOCGIFADDR    0x8915
#define SIOCSIFADDR    0x8916
#define SIOCGIFDSTADDR 0x8917
#define SIOCSIFDSTADDR 0x8918
#define SIOCGIFBRDADDR 0x8919
#define SIOCSIFBRDADDR 0x891a
#define SIOCGIFNETMASK 0x891b
#define SIOCSIFNETMASK 0x891c
#define SIOCGIFMETRIC  0x891d
#define SIOCSIFMETRIC  0x891e
#define SIOCGIFMTU     0x8921
#define SIOCSIFMTU     0x8922
#define SIOCSIFNAME    0x8923
#define SIOCSIFHWADDR  0x8924
#define SIOCGIFHWADDR  0x8927
#define SIOCGIFINDEX   0x8933
#define SIOCSIFHWBROADCAST 0x8937
#define SIOCGIFTXQLEN  0x8942
#define SIOCSIFTXQLEN  0x8943
#define SIOCGIFMAP     0x8970
#define SIOCSIFMAP     0x8971
#define SIOCADDRT      0x890B
#define SIOCDELRT      0x890C
#define SIOCGARP       0x8954
#define SIOCSARP       0x8955
#define SIOCDARP       0x8953
#define SIOCDEVPRIVATE 0x89F0
#define SIOCPROTOPRIVATE 0x89E0
#define TIOCPKT_DATA       0
#define TIOCM_LE   0x001
#define TIOCM_DTR  0x002
#define TIOCM_RTS  0x004
#define TIOCM_CTS  0x020
#define TIOCM_CAR  0x040
#define TIOCM_RNG  0x080
#define TIOCM_DSR  0x100
#define N_TTY 0
#define _IOC(d, t, n, s) (((d) << 30) | ((s) << 16) | ((t) << 8) | (n))
#define _IO(t, n)        _IOC(0U, (t), (n), 0)
#define _IOR(t, n, ty)   _IOC(2U, (t), (n), sizeof(ty))
#define _IOW(t, n, ty)   _IOC(1U, (t), (n), sizeof(ty))
#define _IOWR(t, n, ty)  _IOC(3U, (t), (n), sizeof(ty))
int sbb_ioctl(int fd, unsigned long request, ...);
#endif
