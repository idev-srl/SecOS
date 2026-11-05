/* SecOS Dropbear port: poll() over SYS_POLL (78). TODO(integration): move to libc.
 * SPDX-License-Identifier: MIT */
#ifndef _DB_POLL_H
#define _DB_POLL_H
struct pollfd { int fd; short events; short revents; };
typedef unsigned long nfds_t;
#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020
int poll(struct pollfd* fds, nfds_t n, int timeout_ms);
#endif
