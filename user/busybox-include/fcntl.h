/* SecOS BusyBox overlay: <fcntl.h> = SecOS libc + POSIX open flags (Linux
 * values; the kernel honours O_CREAT/O_TRUNC/O_APPEND, the rest are accepted
 * and emulated or ignored in sbb_open). SPDX-License-Identifier: MIT */
#ifndef _BB_FCNTL_H
#define _BB_FCNTL_H
#include <sys/types.h>
#include_next <fcntl.h>
#include <sys/stat.h>
#define O_ACCMODE   0x3
#ifndef O_EXCL
#define O_EXCL      0x80
#endif
#ifndef O_NOCTTY
#define O_NOCTTY    0x100
#endif
#define O_NDELAY    O_NONBLOCK
#define O_DSYNC     0x1000
#define O_SYNC      0x101000
#define O_RSYNC     O_SYNC
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000
#define O_NOATIME   0x40000
#define O_LARGEFILE 0
#define O_DIRECT    0x4000
#define O_PATH      0x200000
#define O_TMPFILE   0x410000
#define O_ASYNC     0x2000
#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7
#define F_SETOWN 8
#define F_GETOWN 9
#define F_DUPFD_CLOEXEC 1030
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_FOLLOW   0x400
#define AT_EACCESS          0x200
#define AT_EMPTY_PATH       0x1000
#define POSIX_FADV_NORMAL     0
#define POSIX_FADV_RANDOM     1
#define POSIX_FADV_SEQUENTIAL 2
#define POSIX_FADV_WILLNEED   3
#define POSIX_FADV_DONTNEED   4
#define POSIX_FADV_NOREUSE    5
struct flock { short l_type, l_whence; off_t l_start, l_len; pid_t l_pid; };
int sbb_open(const char* path, int flags, ...);
int openat(int dirfd, const char* path, int flags, ...);
int creat(const char* path, mode_t mode);
int posix_fadvise(int fd, off_t off, off_t len, int advice);
int posix_fallocate(int fd, off_t off, off_t len);
#endif
