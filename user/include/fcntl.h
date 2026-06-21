/* <fcntl.h> — SecOS libc. SPDX-License-Identifier: MIT */
#ifndef _FCNTL_H
#define _FCNTL_H
#include "libsecos.h"   /* O_RDONLY/O_WRONLY/O_RDWR + open() */
/* SecOS has no creation flags yet; define them as no-ops for source compat. */
#ifndef O_CREAT
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400
#endif
#define O_NONBLOCK 0x800
#define O_CLOEXEC  0x80000

/* [M39] fcntl commands (subset a shell needs). F_DUPFD via dup(); F_GETFL/F_SETFL
 * track the open flags; F_GETFD/F_SETFD/F_SETLK accepted as no-ops. */
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1
int fcntl(int fd, int cmd, ...);
#endif
