/*
 * unix_sock.h — [U10] AF_UNIX sockets (kernel/unix_sock.c), used by the
 * Linux-shaped socket calls in kernel/lx_net.c. Every buffer passed here is a
 * kernel buffer; -errno on failure, -EAGAIN = would block (retry when the
 * object in *wait_on is woken).
 * SPDX-License-Identifier: MIT
 */
#ifndef UNIX_SOCK_H
#define UNIX_SOCK_H
#include <stdint.h>

struct process;
struct anonfd;

struct lx_sockaddr_un { uint16_t sun_family; char sun_path[108]; };
struct lx_ucred { int32_t pid; uint32_t uid, gid; };

typedef struct usock usock_t;

usock_t* unix_of(const void* ino);           /* the socket behind an fd inode, or NULL */
int  unix_is_sock(const void* ino);
struct anonfd* unix_anon(usock_t* u);
int  unix_type(usock_t* u);                  /* 1 = SOCK_STREAM, 2 = SOCK_DGRAM */

int  unix_create(struct process* c, int type, usock_t** out);
int  unix_socketpair(struct process* c, int type, usock_t** a, usock_t** b);
int  unix_bind(struct process* c, usock_t* u, const struct lx_sockaddr_un* a, uint32_t alen);
int  unix_listen(struct process* c, usock_t* u, int backlog);
int  unix_connect(struct process* c, usock_t* u, const struct lx_sockaddr_un* a, uint32_t alen, void** wait_on);
int  unix_disconnect(usock_t* u);
int  unix_accept(usock_t* u, usock_t** out);
void unix_drop(usock_t* u);                  /* an accepted socket that got no fd */
int  unix_send(struct process* c, usock_t* u, const void* kbuf, int len, int flags,
               const struct lx_sockaddr_un* dst, uint32_t dlen, void** wait_on);
int  unix_recv(usock_t* u, void* kbuf, int len, int flags, struct lx_sockaddr_un* from,
               uint32_t* fromlen, uint32_t* full);
int  unix_shutdown(usock_t* u, int how);
int  unix_getname(usock_t* u, int peer, struct lx_sockaddr_un* out, uint32_t* len);
int  unix_poll(usock_t* u);
int  unix_nread(usock_t* u);
int  unix_setopt(usock_t* u, int level, int name, const void* val, uint32_t len);
int  unix_getopt(usock_t* u, int level, int name, void* val, uint32_t* len);
uint64_t unix_timeo(usock_t* u, int send);

#endif /* UNIX_SOCK_H */
