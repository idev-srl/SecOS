/* <sys/stat.h> — SecOS libc. SPDX-License-Identifier: MIT */
#ifndef _SYS_STAT_H
#define _SYS_STAT_H
#include "libsecos.h"   /* struct stat + stat/lstat/chmod + S_IF* macros */
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#endif
