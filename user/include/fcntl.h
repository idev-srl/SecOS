/* <fcntl.h> — SecOS libc. SPDX-License-Identifier: MIT */
#ifndef _FCNTL_H
#define _FCNTL_H
#include "libsecos.h"   /* O_RDONLY/O_WRONLY/O_RDWR + open() */
/* Creation flags honoured by SYS_OPEN (O_CREAT/O_TRUNC/O_APPEND/O_EXCL). */
#ifndef O_CREAT
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400
#endif
#define O_NOCTTY   0x100
#ifndef O_EXCL
#define O_EXCL   0x80     /* with O_CREAT: fail with EEXIST if it exists */
#endif
#define O_NONBLOCK 0x800
#define O_CLOEXEC  0x80000

/* [M39] fcntl commands (subset a shell needs). F_DUPFD via dup(); F_GETFL/F_SETFL
 * track the open flags; F_GETFD/F_SETFD carry FD_CLOEXEC (honoured by execve);
 * the locking commands are accepted as no-ops. */
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1
#define F_DUPFD_CLOEXEC 1030
#ifndef F_GETLK
#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7
#endif
int pipe2(int fds[2], int flags);
int fcntl(int fd, int cmd, ...);
#endif
