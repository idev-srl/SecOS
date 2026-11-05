/* <sys/uio.h> — SecOS libc: readv/writev (a read()/write() per segment).
 * SPDX-License-Identifier: MIT */
#ifndef _SYS_UIO_H
#define _SYS_UIO_H
#include <stddef.h>
struct iovec { void* iov_base; size_t iov_len; };
long readv(int fd, const struct iovec* iov, int cnt);
long writev(int fd, const struct iovec* iov, int cnt);
#endif
