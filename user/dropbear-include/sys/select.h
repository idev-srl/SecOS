/* SecOS Dropbear port: select() emulated over poll() in user/dropbear_compat.c.
 * Shares the libc's guards: if the libc already declared fd_set (sys/time.h's
 * prototype-only one, or a real <sys/select.h>) that type is used, and
 * FD_SETSIZE follows its size. TODO(integration): move to libc.
 * SPDX-License-Identifier: MIT */
#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H
#include <sys/time.h>
#ifndef _SECOS_FD_SET_T
#define _SECOS_FD_SET_T
typedef struct { unsigned long fds_bits[128 / (8 * sizeof(unsigned long))]; } fd_set;
#endif
#ifndef FD_SETSIZE
#define FD_SETSIZE ((int)(8 * sizeof(fd_set)))
#endif
#define DB_NFDBITS_ (8 * (int)sizeof(unsigned long))
#ifndef FD_ZERO
#define FD_ZERO(s)    do { unsigned db_i_; for (db_i_ = 0; db_i_ < sizeof((s)->fds_bits)/sizeof((s)->fds_bits[0]); db_i_++) (s)->fds_bits[db_i_] = 0; } while (0)
#define FD_SET(d,s)   ((s)->fds_bits[(d)/DB_NFDBITS_] |=  (1UL << ((d) % DB_NFDBITS_)))
#define FD_CLR(d,s)   ((s)->fds_bits[(d)/DB_NFDBITS_] &= ~(1UL << ((d) % DB_NFDBITS_)))
#define FD_ISSET(d,s) (((s)->fds_bits[(d)/DB_NFDBITS_] >> ((d) % DB_NFDBITS_)) & 1UL)
#endif
int select(int nfds, fd_set* r, fd_set* w, fd_set* e, struct timeval* tv);
#endif
