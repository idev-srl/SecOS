/* SecOS BusyBox overlay: <sys/param.h>. SPDX-License-Identifier: MIT */
#ifndef _BB_SYS_PARAM_H
#define _BB_SYS_PARAM_H
#include <limits.h>
#define MAXPATHLEN PATH_MAX
#define MAXHOSTNAMELEN 64
#define MAXSYMLINKS 8
#define NOFILE 32
#define NGROUPS NGROUPS_MAX
#define HZ 100
#define DEV_BSIZE 512
#define NBBY 8
#ifndef howmany
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#endif
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define powerof2(x) ((((x) - 1) & (x)) == 0)
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#include <endian.h>
#endif
