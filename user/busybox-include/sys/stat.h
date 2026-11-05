/* SecOS BusyBox overlay: <sys/stat.h>. struct stat itself is the SecOS ABI
 * one (libsecos.h, pinned to kernel/secos_abi.h). SPDX-License-Identifier: MIT */
#ifndef _BB_SYS_STAT_H
#define _BB_SYS_STAT_H
#include <sys/types.h>
#include <time.h>
#include_next <sys/stat.h>
#ifndef S_IFIFO
#define S_IFIFO  0x1000
#endif
#ifndef S_IFSOCK
#define S_IFSOCK 0xC000
#endif
#define S_ISFIFO(m)  (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m)  (((m) & S_IFMT) == S_IFSOCK)
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_IREAD  S_IRUSR
#define S_IWRITE S_IWUSR
#define S_IEXEC  S_IXUSR
#define ACCESSPERMS (S_IRWXU|S_IRWXG|S_IRWXO)
#define ALLPERMS (S_ISUID|S_ISGID|S_ISVTX|S_IRWXU|S_IRWXG|S_IRWXO)
#define DEFFILEMODE (S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH)
#define UTIME_NOW  ((1L << 30) - 1L)
#define UTIME_OMIT ((1L << 30) - 2L)
#define st_atim_sec st_atime
int    fstat(int fd, struct stat* st);
int    fstatat(int dirfd, const char* path, struct stat* st, int flags);
int    fchmod(int fd, mode_t mode);
int    fchmodat(int dirfd, const char* path, mode_t mode, int flags);
int    mkdirat(int dirfd, const char* path, mode_t mode);
int    mkfifo(const char* path, mode_t mode);
int    mknod(const char* path, mode_t mode, dev_t dev);
int    mknodat(int dirfd, const char* path, mode_t mode, dev_t dev);
int    utimensat(int dirfd, const char* path, const struct timespec ts[2], int flags);
int    futimens(int fd, const struct timespec ts[2]);
#endif
