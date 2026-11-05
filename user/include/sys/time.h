/* <sys/time.h> — SecOS libc (real Unix time via CLOCK_REALTIME). SPDX: MIT */
#ifndef _SYS_TIME_H
#define _SYS_TIME_H
#include <time.h>
#ifndef _STRUCT_TIMEVAL
#define _STRUCT_TIMEVAL
struct timeval  { long tv_sec; long tv_usec; };
#endif
struct timezone { int tz_minuteswest; int tz_dsttime; };
int gettimeofday(struct timeval* tv, void* tz);           /* seconds+usec since the epoch */
int settimeofday(const struct timeval* tv, const struct timezone* tz);   /* CAP_TIME */
#define timerisset(t)    ((t)->tv_sec || (t)->tv_usec)
#define timerclear(t)    ((t)->tv_sec = (t)->tv_usec = 0)
#define timercmp(a, b, CMP) (((a)->tv_sec == (b)->tv_sec) ? ((a)->tv_usec CMP (b)->tv_usec) : ((a)->tv_sec CMP (b)->tv_sec))
#define timeradd(a, b, r) do { (r)->tv_sec = (a)->tv_sec + (b)->tv_sec; (r)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
    if ((r)->tv_usec >= 1000000) { (r)->tv_sec++; (r)->tv_usec -= 1000000; } } while (0)
#define timersub(a, b, r) do { (r)->tv_sec = (a)->tv_sec - (b)->tv_sec; (r)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
    if ((r)->tv_usec < 0) { (r)->tv_sec--; (r)->tv_usec += 1000000; } } while (0)
/* Like glibc: <sys/time.h> provides select() and fd_set. */
#include <sys/select.h>
#endif
