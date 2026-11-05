/* <time.h> — SecOS libc. Wall clock = kernel CLOCK_REALTIME (CMOS RTC anchored
 * to the TSC); all broken-down time is UTC (no time-zone database).
 * SPDX-License-Identifier: MIT */
#ifndef _TIME_H
#define _TIME_H
#include <stddef.h>
typedef long time_t;
typedef long clock_t;
typedef int  clockid_t;
#define CLOCKS_PER_SEC 1000            /* clock(): process CPU time in ms */
#ifndef _STRUCT_TIMESPEC
#define _STRUCT_TIMESPEC
struct timespec { long tv_sec; long tv_nsec; };
#endif
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;                    /* always 0 (UTC) */
    const char* tm_zone;               /* "UTC" */
};
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
#define TIMER_ABSTIME            1
#define TIME_UTC                 1

time_t  time(time_t* t);               /* seconds since the Unix epoch */
clock_t clock(void);                   /* process CPU time, CLOCKS_PER_SEC units */
double  difftime(time_t a, time_t b);
int     clock_gettime(clockid_t clk, struct timespec* ts);
int     clock_settime(clockid_t clk, const struct timespec* ts);  /* REALTIME only */
int     clock_getres(clockid_t clk, struct timespec* res);
int     nanosleep(const struct timespec* req, struct timespec* rem);
int     clock_nanosleep(clockid_t clk, int flags, const struct timespec* req, struct timespec* rem);
int     timespec_get(struct timespec* ts, int base);
struct tm* gmtime(const time_t* t);
struct tm* gmtime_r(const time_t* t, struct tm* r);
struct tm* localtime(const time_t* t);              /* == gmtime (UTC) */
struct tm* localtime_r(const time_t* t, struct tm* r);
time_t  mktime(struct tm* tm);                      /* == timegm (UTC) */
time_t  timegm(struct tm* tm);
size_t  strftime(char* s, size_t max, const char* fmt, const struct tm* tm);
char*   asctime(const struct tm* tm);
char*   asctime_r(const struct tm* tm, char* buf);
char*   ctime(const time_t* t);
char*   ctime_r(const time_t* t, char* buf);
void    tzset(void);
extern long  timezone;
extern int   daylight;
extern char* tzname[2];
#endif
