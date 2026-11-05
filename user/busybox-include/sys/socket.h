/* SecOS BusyBox overlay: <sys/socket.h>. POSIX socket API mapped onto the
 * SecOS socket syscalls (AF_INET + SOCK_STREAM/SOCK_DGRAM only) by the shims
 * in user/busybox_compat.c; a socket is an ordinary kernel fd.
 * SPDX-License-Identifier: MIT */
#ifndef _BB_SYS_SOCKET_H
#define _BB_SYS_SOCKET_H
#include <sys/types.h>
#include <sys/uio.h>
#include "libsecos.h"
typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;
struct sockaddr { sa_family_t sa_family; char sa_data[14]; };
struct sockaddr_storage { sa_family_t ss_family; char __ss_pad[126]; unsigned long __align; };
struct msghdr { void* msg_name; socklen_t msg_namelen; struct iovec* msg_iov; int msg_iovlen;
                void* msg_control; socklen_t msg_controllen; int msg_flags; };
struct cmsghdr { socklen_t cmsg_len; int cmsg_level; int cmsg_type; };
struct linger { int l_onoff; int l_linger; };
struct ucred { pid_t pid; uid_t uid; gid_t gid; };
#define CMSG_ALIGN(len) (((len) + sizeof(long) - 1) & ~(sizeof(long) - 1))
#define CMSG_DATA(c) ((unsigned char*)((struct cmsghdr*)(c) + 1))
#define CMSG_SPACE(len) (CMSG_ALIGN(len) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_LEN(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_FIRSTHDR(m) ((m)->msg_controllen >= sizeof(struct cmsghdr) ? (struct cmsghdr*)(m)->msg_control : (struct cmsghdr*)0)
#define CMSG_NXTHDR(m, c) ((struct cmsghdr*)0)
#define SOCK_RAW       3
#define SOCK_RDM       4
#define SOCK_SEQPACKET 5
#define SOCK_PACKET    10
#define SOCK_NONBLOCK  0x800
#define SOCK_CLOEXEC   0x80000
#define AF_UNSPEC  0
#define AF_UNIX    1
#define AF_LOCAL   AF_UNIX
#define AF_INET    2
#define AF_INET6   10
#define AF_NETLINK 16
#define AF_PACKET  17
#define PF_UNSPEC  AF_UNSPEC
#define PF_UNIX    AF_UNIX
#define PF_LOCAL   AF_LOCAL
#define PF_INET    AF_INET
#define PF_INET6   AF_INET6
#define PF_NETLINK AF_NETLINK
#define PF_PACKET  AF_PACKET
#define SOL_SOCKET 1
#define SO_DEBUG     1
#define SO_REUSEADDR 2
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_DONTROUTE 5
#define SO_BROADCAST 6
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define SO_KEEPALIVE 9
#define SO_OOBINLINE 10
#define SO_LINGER    13
#define SO_REUSEPORT 15
#define SO_PASSCRED  16
#define SO_PEERCRED  17
#define SO_RCVLOWAT  18
#define SO_SNDLOWAT  19
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21
#define SO_BINDTODEVICE 25
#define SO_TIMESTAMP 29
#define SCM_RIGHTS      1
#define SCM_CREDENTIALS 2
#define MSG_OOB       0x1
#define MSG_PEEK      0x2
#define MSG_DONTROUTE 0x4
#define MSG_CTRUNC    0x8
#define MSG_TRUNC     0x20
#define MSG_DONTWAIT  0x40
#define MSG_EOR       0x80
#define MSG_WAITALL   0x100
#define MSG_NOSIGNAL  0x4000
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2
#define SOMAXCONN 128
int     sbb_socket(int domain, int type, int proto);
int     sbb_connect(int fd, const struct sockaddr* a, socklen_t l);
int     sbb_bind(int fd, const struct sockaddr* a, socklen_t l);
int     sbb_listen(int fd, int backlog);
int     sbb_accept(int fd, struct sockaddr* a, socklen_t* l);
ssize_t sbb_send(int fd, const void* b, size_t n, int flags);
ssize_t sbb_recv(int fd, void* b, size_t n, int flags);
ssize_t sbb_sendto(int fd, const void* b, size_t n, int flags, const struct sockaddr* a, socklen_t l);
ssize_t sbb_recvfrom(int fd, void* b, size_t n, int flags, struct sockaddr* a, socklen_t* l);
ssize_t sendmsg(int fd, const struct msghdr* m, int flags);
ssize_t recvmsg(int fd, struct msghdr* m, int flags);
int     accept4(int fd, struct sockaddr* a, socklen_t* l, int flags);
int     shutdown(int fd, int how);
int     getsockname(int fd, struct sockaddr* a, socklen_t* l);
int     getpeername(int fd, struct sockaddr* a, socklen_t* l);
int     getsockopt(int fd, int lvl, int opt, void* v, socklen_t* l);
int     setsockopt(int fd, int lvl, int opt, const void* v, socklen_t l);
int     socketpair(int d, int t, int p, int sv[2]);
#endif
