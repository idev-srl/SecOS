/* <utime.h> — SecOS libc. SPDX-License-Identifier: MIT */
#ifndef _UTIME_H
#define _UTIME_H
#include <time.h>
struct utimbuf { time_t actime; time_t modtime; };
int utime(const char* path, const struct utimbuf* times);   /* NULL = now */
#endif
