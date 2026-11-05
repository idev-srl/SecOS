/* SecOS BusyBox overlay: <sys/mman.h> (PROT/MAP values from libsecos.h).
 * SPDX-License-Identifier: MIT */
#ifndef _BB_SYS_MMAN_H
#define _BB_SYS_MMAN_H
#include <sys/types.h>
#include "libsecos.h"
#define MAP_SHARED    0x01
#define MAP_FIXED     0x10
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_POPULATE  0x8000
#define MAP_FAILED    ((void*)-1)
#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4
#define MCL_CURRENT   1
#define MCL_FUTURE    2
#define MADV_NORMAL   0
#define MADV_DONTNEED 4
void* sbb_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off);
int   msync(void* addr, size_t len, int flags);
int   mlock(const void* addr, size_t len);
int   munlock(const void* addr, size_t len);
int   mlockall(int flags);
int   munlockall(void);
int   madvise(void* addr, size_t len, int advice);
void* mremap(void* old, size_t olen, size_t nlen, int flags, ...);
#endif
