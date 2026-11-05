/* <sys/time.h> — SecOS libc (uptime-based; no RTC wall clock). SPDX: MIT */
#ifndef _SYS_TIME_H
#define _SYS_TIME_H
#include <time.h>
struct timeval  { long tv_sec; long tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };
int gettimeofday(struct timeval* tv, void* tz);   /* [M39] seconds+usec since boot */

/* fd_set exists for prototypes only: there is no select() (FD_SET is left
 * undefined on purpose so ports keep their non-select code paths). */
#ifndef _SECOS_FD_SET_T
#define _SECOS_FD_SET_T
typedef struct { unsigned long fds_bits[1024 / (8 * sizeof(unsigned long))]; } fd_set;
#endif
#endif
