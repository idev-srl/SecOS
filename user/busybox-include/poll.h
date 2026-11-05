/* SecOS BusyBox overlay: <poll.h>. No poll syscall yet: the shim reports
 * files/pipes/console as ready (TODO(integration): use SYS_POLL).
 * SPDX-License-Identifier: MIT */
#ifndef _BB_POLL_H
#define _BB_POLL_H
#define POLLIN     0x001
#define POLLPRI    0x002
#define POLLOUT    0x004
#define POLLERR    0x008
#define POLLHUP    0x010
#define POLLNVAL   0x020
#define POLLRDNORM 0x040
#define POLLRDBAND 0x080
#define POLLWRNORM 0x100
#define POLLWRBAND 0x200
typedef unsigned long nfds_t;
struct pollfd { int fd; short events; short revents; };
int poll(struct pollfd* fds, nfds_t n, int timeout_ms);
#endif
