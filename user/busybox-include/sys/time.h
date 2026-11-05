/* SecOS BusyBox overlay: <sys/time.h> = SecOS libc + interval timers and the
 * utimes family (busybox_compat.c). SPDX-License-Identifier: MIT */
#ifndef _BB_SYS_TIME_H
#define _BB_SYS_TIME_H
#include <time.h>
#include_next <sys/time.h>
struct itimerval { struct timeval it_interval, it_value; };
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2
int setitimer(int which, const struct itimerval* nv, struct itimerval* ov);
int getitimer(int which, struct itimerval* v);
int sbb_utimes(const char* path, const struct timeval tv[2]);
int lutimes(const char* path, const struct timeval tv[2]);
int futimes(int fd, const struct timeval tv[2]);
int adjtime(const struct timeval* d, struct timeval* old);
#endif
