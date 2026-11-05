/* SecOS BusyBox overlay: <sys/times.h>. */
#ifndef _BB_SYS_TIMES_H
#define _BB_SYS_TIMES_H
#include <time.h>
struct tms { clock_t tms_utime, tms_stime, tms_cutime, tms_cstime; };
clock_t times(struct tms* t);
#endif
