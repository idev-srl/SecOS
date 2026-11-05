/* SecOS BusyBox overlay: <sys/timex.h>. */
#ifndef _BB_SYS_TIMEX_H
#define _BB_SYS_TIMEX_H
#include <sys/time.h>
struct timex { unsigned modes; long offset, freq, maxerror, esterror; int status;
               long constant, precision, tolerance; struct timeval time; long tick; };
int adjtimex(struct timex* t);
#endif
