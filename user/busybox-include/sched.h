/* SecOS BusyBox overlay: <sched.h>. SPDX-License-Identifier: MIT */
#ifndef _BB_SCHED_H
#define _BB_SCHED_H
#include <sys/types.h>
#include <time.h>
#include "libsecos.h"
struct sched_param { int sched_priority; };
#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2
#define SCHED_BATCH 3
#define SCHED_IDLE  5
#define CLONE_VM      0x00000100
#define CLONE_NEWNS   0x00020000
#define CLONE_NEWUTS  0x04000000
#define CLONE_NEWIPC  0x08000000
#define CLONE_NEWUSER 0x10000000
#define CLONE_NEWPID  0x20000000
#define CLONE_NEWNET  0x40000000
/* 1024 CPUs, as glibc/musl (the kernel supports up to 256). */
#define CPU_SETSIZE 1024
typedef struct { unsigned long bits[CPU_SETSIZE / 64]; } cpu_set_t;
#define CPU_ZERO(s)  do { for (int __i = 0; __i < CPU_SETSIZE / 64; __i++) (s)->bits[__i] = 0; } while (0)
#define CPU_SET(c, s) ((s)->bits[(c) / 64] |= (1UL << ((c) % 64)))
#define CPU_CLR(c, s) ((s)->bits[(c) / 64] &= ~(1UL << ((c) % 64)))
#define CPU_ISSET(c, s) (((s)->bits[(c) / 64] >> ((c) % 64)) & 1)
#define CPU_COUNT(s) __cpu_count(s)
static inline int __cpu_count(const cpu_set_t* s) {
    int n = 0;
    for (int i = 0; i < CPU_SETSIZE / 64; i++) n += __builtin_popcountl(s->bits[i]);
    return n;
}
int sched_getparam(pid_t p, struct sched_param* sp);
int sched_setparam(pid_t p, const struct sched_param* sp);
int sched_getscheduler(pid_t p);
int sched_setscheduler(pid_t p, int pol, const struct sched_param* sp);
int sched_get_priority_max(int pol);
int sched_get_priority_min(int pol);
int sched_rr_get_interval(pid_t p, struct timespec* t);
int sched_getaffinity(pid_t p, size_t sz, cpu_set_t* s);
int sched_setaffinity(pid_t p, size_t sz, const cpu_set_t* s);
int unshare(int flags);
#endif
