/* SecOS BusyBox overlay: <time.h> = SecOS libc + struct tm / timespec and the
 * calendar functions (implemented in user/busybox_compat.c, UTC only).
 * SPDX-License-Identifier: MIT */
#ifndef _BB_TIME_H
#define _BB_TIME_H
#include_next <time.h>
#include <sys/types.h>
#ifndef _STRUCT_TIMESPEC
#define _STRUCT_TIMESPEC
struct timespec { time_t tv_sec; long tv_nsec; };
#endif
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char* tm_zone;
};
struct itimerspec { struct timespec it_interval, it_value; };
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
#define TIMER_ABSTIME 1
extern char* tzname[2];
extern long timezone;
extern int daylight;
struct tm* gmtime(const time_t* t);
struct tm* gmtime_r(const time_t* t, struct tm* out);
struct tm* localtime(const time_t* t);
struct tm* localtime_r(const time_t* t, struct tm* out);
time_t     mktime(struct tm* tm);
time_t     timegm(struct tm* tm);
time_t     timelocal(struct tm* tm);
double     difftime(time_t a, time_t b);
char*      asctime(const struct tm* tm);
char*      asctime_r(const struct tm* tm, char* buf);
char*      ctime(const time_t* t);
char*      ctime_r(const time_t* t, char* buf);
size_t     strftime(char* s, size_t max, const char* fmt, const struct tm* tm);
char*      strptime(const char* s, const char* fmt, struct tm* tm);
void       tzset(void);
int        stime(const time_t* t);
int        clock_gettime(clockid_t id, struct timespec* ts);
int        clock_settime(clockid_t id, const struct timespec* ts);
int        clock_getres(clockid_t id, struct timespec* ts);
int        clock_nanosleep(clockid_t id, int flags, const struct timespec* req, struct timespec* rem);
int        nanosleep(const struct timespec* req, struct timespec* rem);
#endif
