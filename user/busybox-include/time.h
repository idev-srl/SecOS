/* SecOS BusyBox overlay: <time.h> = SecOS libc (wall clock, UTC calendar
 * functions) + the few declarations BusyBox uses that libc does not have
 * (implemented in user/busybox_compat.c). SPDX-License-Identifier: MIT */
#ifndef _BB_TIME_H
#define _BB_TIME_H
#include_next <time.h>
#include <sys/types.h>
struct itimerspec { struct timespec it_interval, it_value; };
time_t     timelocal(struct tm* tm);
char*      strptime(const char* s, const char* fmt, struct tm* tm);
int        stime(const time_t* t);
#endif
