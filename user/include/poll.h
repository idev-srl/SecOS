/* <poll.h> — SecOS libc (SYS_POLL 78). Works on pipes, the console TTY,
 * regular files (always ready) and sockets (libc socket fds, >= 64).
 * SPDX-License-Identifier: MIT */
#ifndef _POLL_H
#define _POLL_H
#include <time.h>
typedef unsigned long nfds_t;
struct pollfd { int fd; short events; short revents; };
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
int poll(struct pollfd* fds, nfds_t nfds, int timeout_ms);
/* ppoll: the signal mask is swapped around poll() (not atomically). */
int ppoll(struct pollfd* fds, nfds_t nfds, const struct timespec* tmo, const unsigned long* sigmask);
#endif
