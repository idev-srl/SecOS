/* SecOS BusyBox overlay: <sys/uio.h>. */
#ifndef _BB_SYS_UIO_H
#define _BB_SYS_UIO_H
#include <sys/types.h>
struct iovec { void* iov_base; size_t iov_len; };
ssize_t readv(int fd, const struct iovec* v, int n);
ssize_t writev(int fd, const struct iovec* v, int n);
#endif
