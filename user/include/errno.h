/* <errno.h> — SecOS libc. SPDX-License-Identifier: MIT */
#ifndef _ERRNO_H
#define _ERRNO_H
extern int errno;
#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define EBADF    9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define EMFILE  24
#define ENOTTY  25
#define ENOSPC  28
#define EPIPE   32
#define ERANGE  34
#define ENOSYS  38
#define ENXIO    6
#define E2BIG    7
#define EBUSY   16
#define EXDEV   18
#define ENFILE  23
#define ESPIPE  29
#define EROFS   30
#define EDOM    33
#define ENAMETOOLONG 36
#define ENOTEMPTY 39
#define ELOOP   40
#define EWOULDBLOCK EAGAIN
#define EOPNOTSUPP 95
#define ETIMEDOUT 110
#endif
