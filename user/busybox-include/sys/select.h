/* SecOS BusyBox overlay: <sys/select.h> (select() emulated over poll()).
 * SPDX-License-Identifier: MIT */
#ifndef _BB_SYS_SELECT_H
#define _BB_SYS_SELECT_H
#include <sys/types.h>
#define FD_SETSIZE 64
typedef struct { unsigned long fds_bits[FD_SETSIZE / 64]; } fd_set;
#define FD_ZERO(s)    do { (s)->fds_bits[0] = 0; } while (0)
#define FD_SET(d, s)  ((s)->fds_bits[(d) / 64] |= (1UL << ((d) % 64)))
#define FD_CLR(d, s)  ((s)->fds_bits[(d) / 64] &= ~(1UL << ((d) % 64)))
#define FD_ISSET(d, s) (((s)->fds_bits[(d) / 64] >> ((d) % 64)) & 1)
struct timeval;
struct timespec;
int select(int n, fd_set* r, fd_set* w, fd_set* e, struct timeval* tv);
int pselect(int n, fd_set* r, fd_set* w, fd_set* e, const struct timespec* ts, const void* mask);
#endif
