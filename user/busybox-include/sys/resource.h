/* SecOS BusyBox overlay: <sys/resource.h>. SPDX-License-Identifier: MIT */
#ifndef _BB_SYS_RESOURCE_H
#define _BB_SYS_RESOURCE_H
#include <sys/time.h>
typedef unsigned long rlim_t;
#define RLIM_INFINITY (~0UL)
#define RLIM_SAVED_MAX RLIM_INFINITY
#define RLIM_SAVED_CUR RLIM_INFINITY
#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_RSS     5
#define RLIMIT_NPROC   6
#define RLIMIT_NOFILE  7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS      9
#define RLIMIT_LOCKS   10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE 12
#define RLIMIT_NICE    13
#define RLIMIT_RTPRIO  14
#define RLIMIT_RTTIME  15
#define RLIMIT_NLIMITS 16
#define RLIM_NLIMITS   16
struct rlimit { rlim_t rlim_cur; rlim_t rlim_max; };
#define RUSAGE_SELF      0
#define RUSAGE_CHILDREN (-1)
struct rusage {
    struct timeval ru_utime, ru_stime;
    long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap,
         ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw;
};
#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2
int getrlimit(int r, struct rlimit* l);
int setrlimit(int r, const struct rlimit* l);
int prlimit(pid_t pid, int r, const struct rlimit* n, struct rlimit* o);
int getrusage(int who, struct rusage* u);
int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int prio);
#endif
