/* <time.h> — SecOS libc (minimal: ticks-based monotonic time). SPDX: MIT */
#ifndef _TIME_H
#define _TIME_H
#include <stddef.h>
typedef long time_t;
typedef long clock_t;
#define CLOCKS_PER_SEC 1000
time_t time(time_t* t);     /* seconds since boot (uptime; no RTC wall clock) */
clock_t clock(void);        /* ms since boot */
#endif
