/* SecOS Dropbear port: select() emulated over poll() in user/dropbear_compat.c.
 * TODO(integration): move to libc.  SPDX-License-Identifier: MIT */
#ifndef _DB_SYS_SELECT_H
#define _DB_SYS_SELECT_H
#include <sys/time.h>
#define FD_SETSIZE 128
typedef struct { unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))]; } fd_set;
#define DB_NFDBITS_ (8 * (int)sizeof(unsigned long))
#define FD_ZERO(s)    do { unsigned db_i_; for (db_i_ = 0; db_i_ < sizeof((s)->fds_bits)/sizeof((s)->fds_bits[0]); db_i_++) (s)->fds_bits[db_i_] = 0; } while (0)
#define FD_SET(d,s)   ((s)->fds_bits[(d)/DB_NFDBITS_] |=  (1UL << ((d) % DB_NFDBITS_)))
#define FD_CLR(d,s)   ((s)->fds_bits[(d)/DB_NFDBITS_] &= ~(1UL << ((d) % DB_NFDBITS_)))
#define FD_ISSET(d,s) (((s)->fds_bits[(d)/DB_NFDBITS_] >> ((d) % DB_NFDBITS_)) & 1UL)
int select(int nfds, fd_set* r, fd_set* w, fd_set* e, struct timeval* tv);
#endif
