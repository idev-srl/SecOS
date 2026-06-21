/* <sys/time.h> — SecOS libc (uptime-based; no RTC wall clock). SPDX: MIT */
#ifndef _SYS_TIME_H
#define _SYS_TIME_H
#include <time.h>
struct timeval  { long tv_sec; long tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };
int gettimeofday(struct timeval* tv, void* tz);   /* [M39] seconds+usec since boot */
#endif
