/* <sys/select.h> — SecOS libc: select()/pselect() implemented over poll().
 * SPDX-License-Identifier: MIT */
#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H
#include <sys/time.h>
#ifndef FD_SETSIZE
#define FD_SETSIZE 128
#endif
#ifndef _SECOS_FD_SET_T
#define _SECOS_FD_SET_T
typedef struct { unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))]; } fd_set;
#endif
#define __FDW(fd) ((fd) / (8 * (int)sizeof(unsigned long)))
#define __FDB(fd) (1UL << ((fd) % (8 * (int)sizeof(unsigned long))))
#define FD_ZERO(s)     do { for (unsigned __i = 0; __i < sizeof((s)->fds_bits) / sizeof((s)->fds_bits[0]); __i++) (s)->fds_bits[__i] = 0; } while (0)
#define FD_SET(fd, s)  ((s)->fds_bits[__FDW(fd)] |= __FDB(fd))
#define FD_CLR(fd, s)  ((s)->fds_bits[__FDW(fd)] &= ~__FDB(fd))
#define FD_ISSET(fd, s) (((s)->fds_bits[__FDW(fd)] & __FDB(fd)) != 0)
int select(int nfds, fd_set* r, fd_set* w, fd_set* e, struct timeval* tmo);
int pselect(int nfds, fd_set* r, fd_set* w, fd_set* e, const struct timespec* tmo, const unsigned long* sigmask);
#endif
