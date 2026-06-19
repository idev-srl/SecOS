/*
 * socket.h — [M24] kernel-side socket layer prototypes.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef NET_SOCKET_H
#define NET_SOCKET_H
#include <stdint.h>

int  socket_create(uint32_t pid, int type);
int  socket_close(uint32_t pid, int fd);
void socket_owner_cleanup(uint32_t pid);
int  socket_bind(uint32_t pid, int fd, uint16_t port);
int  socket_connect(uint32_t pid, int fd, uint32_t ip, uint16_t port);
int  socket_listen(uint32_t pid, int fd, int backlog);
int  socket_accept(uint32_t pid, int fd);
int  socket_send(uint32_t pid, int fd, const void* buf, int len);
int  socket_sendto(uint32_t pid, int fd, const void* buf, int len, uint32_t ip, uint16_t port);
int  socket_recv(uint32_t pid, int fd, void* buf, int len, uint32_t* src_ip, uint16_t* src_port);

#endif /* NET_SOCKET_H */
