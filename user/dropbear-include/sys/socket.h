/* SecOS Dropbear port: POSIX <sys/socket.h> over the SecOS socket syscalls
 * (implemented in user/dropbear_compat.c). IPv4 TCP/UDP only.
 * TODO(integration): move to libc.  SPDX-License-Identifier: MIT */
#ifndef _DB_SYS_SOCKET_H
#define _DB_SYS_SOCKET_H
#include <sys/types.h>
typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;
struct sockaddr { sa_family_t sa_family; char sa_data[14]; };
struct sockaddr_storage { sa_family_t ss_family; char __ss_pad[126]; } __attribute__((aligned(8)));
#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_LOCAL  AF_UNIX
#define AF_INET   2
#define AF_INET6  10
#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX   AF_UNIX
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6
#ifndef SOCK_STREAM
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#endif
#define SOCK_RAW 3
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC  0x80000
#define SOL_SOCKET    1
#define SO_DEBUG      1
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_BROADCAST  6
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_KEEPALIVE  9
#define SO_PRIORITY   12
#define SO_LINGER     13
#define SO_REUSEPORT  15
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2
#define MSG_PEEK     0x2
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000
#define SOMAXCONN 16
struct linger { int l_onoff; int l_linger; };
int db_socket(int domain, int type, int protocol);
int db_bind(int fd, const struct sockaddr* sa, socklen_t len);
int db_listen(int fd, int backlog);
int db_accept(int fd, struct sockaddr* sa, socklen_t* len);
int db_connect(int fd, const struct sockaddr* sa, socklen_t len);
ssize_t db_send(int fd, const void* buf, size_t len, int flags);
ssize_t db_recv(int fd, void* buf, size_t len, int flags);
ssize_t db_sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr* sa, socklen_t sl);
ssize_t db_recvfrom(int fd, void* buf, size_t len, int flags, struct sockaddr* sa, socklen_t* sl);
int db_shutdown(int fd, int how);
int db_setsockopt(int fd, int level, int opt, const void* val, socklen_t len);
int db_getsockopt(int fd, int level, int opt, void* val, socklen_t* len);
int db_getsockname(int fd, struct sockaddr* sa, socklen_t* len);
int db_getpeername(int fd, struct sockaddr* sa, socklen_t* len);
int db_socketpair(int domain, int type, int protocol, int sv[2]);
/* Function-like so struct members with these names are untouched. The
 * non-POSIX libsecos prototypes are renamed away in user/dropbear_port.h. */
#define socket(d,t,p)            db_socket(d,t,p)
#define bind(f,a,l)              db_bind(f,a,l)
#define listen(f,b)              db_listen(f,b)
#define accept(f,a,l)            db_accept(f,a,l)
#define connect(f,a,l)           db_connect(f,a,l)
#define send(f,b,l,fl)           db_send(f,b,l,fl)
#define recv(f,b,l,fl)           db_recv(f,b,l,fl)
#define sendto(f,b,l,fl,a,sl)    db_sendto(f,b,l,fl,a,sl)
#define recvfrom(f,b,l,fl,a,sl)  db_recvfrom(f,b,l,fl,a,sl)
#define shutdown(f,h)            db_shutdown(f,h)
#define setsockopt(f,l,o,v,n)    db_setsockopt(f,l,o,v,n)
#define getsockopt(f,l,o,v,n)    db_getsockopt(f,l,o,v,n)
#define getsockname(f,a,l)       db_getsockname(f,a,l)
#define getpeername(f,a,l)       db_getpeername(f,a,l)
#define socketpair(d,t,p,s)      db_socketpair(d,t,p,s)
#endif
