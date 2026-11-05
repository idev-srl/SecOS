/* SecOS BusyBox overlay: <sys/time.h>. SPDX-License-Identifier: MIT */
#ifndef _BB_SYS_TIME_H
#define _BB_SYS_TIME_H
#include <time.h>
#include_next <sys/time.h>
#include <sys/select.h>
struct itimerval { struct timeval it_interval, it_value; };
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2
#define timerisset(t) ((t)->tv_sec || (t)->tv_usec)
#define timerclear(t) ((t)->tv_sec = (t)->tv_usec = 0)
#define timercmp(a, b, CMP) (((a)->tv_sec == (b)->tv_sec) ? \
    ((a)->tv_usec CMP (b)->tv_usec) : ((a)->tv_sec CMP (b)->tv_sec))
#define timeradd(a, b, r) do { (r)->tv_sec = (a)->tv_sec + (b)->tv_sec; \
    (r)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
    if ((r)->tv_usec >= 1000000) { (r)->tv_sec++; (r)->tv_usec -= 1000000; } } while (0)
#define timersub(a, b, r) do { (r)->tv_sec = (a)->tv_sec - (b)->tv_sec; \
    (r)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
    if ((r)->tv_usec < 0) { (r)->tv_sec--; (r)->tv_usec += 1000000; } } while (0)
int setitimer(int which, const struct itimerval* nv, struct itimerval* ov);
int getitimer(int which, struct itimerval* v);
int settimeofday(const struct timeval* tv, const struct timezone* tz);
int sbb_utimes(const char* path, const struct timeval tv[2]);
int lutimes(const char* path, const struct timeval tv[2]);
int futimes(int fd, const struct timeval tv[2]);
int adjtime(const struct timeval* d, struct timeval* old);
#endif
