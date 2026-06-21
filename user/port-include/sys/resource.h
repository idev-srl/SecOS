#ifndef _PORT_SYS_RESOURCE_H
#define _PORT_SYS_RESOURCE_H
typedef unsigned long rlim_t;
#define RLIM_INFINITY (~0UL)
#define RLIMIT_CPU 0
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA 2
#define RLIMIT_STACK 3
#define RLIMIT_CORE 4
#define RLIMIT_NOFILE 7
#define RLIMIT_AS 9
#define RLIM_NLIMITS 16
struct rlimit { rlim_t rlim_cur; rlim_t rlim_max; };
struct rusage { long ru_utime, ru_stime; long ru_maxrss; };
int getrlimit(int, struct rlimit*);
int setrlimit(int, const struct rlimit*);
int getrusage(int, struct rusage*);
#endif
