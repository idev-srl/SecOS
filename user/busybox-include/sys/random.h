/* SecOS BusyBox overlay: <sys/random.h>. */
#ifndef _BB_SYS_RANDOM_H
#define _BB_SYS_RANDOM_H
#include <sys/types.h>
#define GRND_NONBLOCK 1
#define GRND_RANDOM   2
ssize_t getrandom(void* buf, size_t len, unsigned flags);
#endif
