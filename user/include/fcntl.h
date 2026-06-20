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
#endif
