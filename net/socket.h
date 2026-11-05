/*
 * socket.h — [M24] kernel-side socket layer prototypes.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Sockets are referenced by a socket index (0..MAX_SOCK-1) stored in a process
 * fd entry as `is_sock = index + 1`. All calls are non-blocking and return
 * -errno (-EAGAIN = would block; kernel/syscall_trap.c sleeps + re-runs the
 * syscall for a blocking fd).
 */
#ifndef NET_SOCKET_H
#define NET_SOCKET_H
#include <stdint.h>

/* poll() revents bits (Linux values). */
#define SOCK_POLLIN   0x001
#define SOCK_POLLOUT  0x004
#define SOCK_POLLERR  0x008
#define SOCK_POLLHUP  0x010
#define SOCK_POLLNVAL 0x020

int  socket_create(int type);                     /* index, or -errno */
void socket_ref(int idx);                         /* fd dup / fork */
void socket_unref(int idx);                       /* fd close; frees on the last ref */
int  socket_bind(int idx, uint16_t port);
int  socket_connect(int idx, uint32_t ip, uint16_t port);
int  socket_listen(int idx, int backlog);
int  socket_accept(int idx);                      /* new index, or -EAGAIN */
int  socket_send(int idx, const void* buf, int len);
int  socket_sendto(int idx, const void* buf, int len, uint32_t ip, uint16_t port);
int  socket_recv(int idx, void* buf, int len, uint32_t* src_ip, uint16_t* src_port);
int  socket_poll(uint32_t pid, int idx);          /* revents, never blocks */
int  socket_name(int idx, int peer, uint32_t* ip, uint16_t* port);

#endif /* NET_SOCKET_H */
