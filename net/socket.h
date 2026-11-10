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
 *
 * [U10] AF_INET sockets for the Linux-shaped calls (kernel/lx_net.c): four
 * kinds (TCP, UDP, ICMP "ping" datagram sockets, raw IP sockets), socket
 * options, shutdown, peeking, per-datagram metadata (source, TTL, arrival
 * time) and an ICMP error queue (IP_RECVERR). Addresses are network-order
 * octets in a uint32_t (as sin_addr.s_addr), ports host order. The M24
 * calls below the "old ABI" line keep their behaviour for SYS_SOCKET..
 * SYS_SOCKNAME (BusyBox, Dropbear and the M24/M47 tests).
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
#define SOCK_POLLRDHUP 0x2000

/* ---- old ABI (M24) ---- */
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

/* ---- [U10] Linux-shaped AF_INET ---- */
enum { SKIND_TCP = 1, SKIND_UDP = 2, SKIND_PING = 3, SKIND_RAW = 4 };

/* Linux MSG_* flags understood by socket_recvmsg/socket_sendmsg. */
#define SK_MSG_OOB       0x1
#define SK_MSG_PEEK      0x2
#define SK_MSG_DONTROUTE 0x4
#define SK_MSG_CTRUNC    0x8
#define SK_MSG_TRUNC     0x20
#define SK_MSG_DONTWAIT  0x40
#define SK_MSG_WAITALL   0x100
#define SK_MSG_ERRQUEUE  0x2000
#define SK_MSG_NOSIGNAL  0x4000
#define SK_MSG_MORE      0x8000
#define SK_MSG_FASTOPEN  0x20000000

/* What a receive returns besides the bytes. */
typedef struct sock_rxmeta {
    uint32_t ip;  uint16_t port; int has_addr;   /* the datagram's source */
    uint32_t full_len;                           /* datagram length (MSG_TRUNC) */
    uint8_t  ttl, tos;  uint64_t ts_ns;          /* arrival TTL/TOS, CLOCK_REALTIME */
    /* the socket's cmsg wishes (IP_RECVTTL / IP_RECVTOS / SO_TIMESTAMP[NS]) */
    uint8_t  want_ttl, want_tos, want_ts, want_tsns;
    /* MSG_ERRQUEUE: struct sock_extended_err fields + the ICMP sender */
    uint32_t ee_errno; uint8_t ee_origin, ee_type, ee_code; uint32_t ee_info;
    uint32_t offender;
} sock_rxmeta_t;

int  socket_create_ex(int kind, int proto);      /* index or -errno */
int  socket_kind(int idx, int* proto);           /* SKIND_* or -EBADF */
int  socket_bind_ex(int idx, uint32_t ip, uint16_t port);
/* unspec: AF_UNSPEC = dissolve a datagram association. */
int  socket_connect_ex(int idx, uint32_t ip, uint16_t port, int unspec);
int  socket_accept_ex(int idx, uint32_t* ip, uint16_t* port);
int  socket_recvmsg(int idx, void* buf, int len, int flags, sock_rxmeta_t* m);
int  socket_sendmsg(int idx, const void* buf, int len, int flags,
                    int has_dst, uint32_t ip, uint16_t port);
/* Options on kernel buffers. priv = the caller is root (SO_MARK, ...). */
int  socket_setopt(int idx, int level, int name, const void* val, uint32_t len, int priv);
int  socket_getopt(int idx, int level, int name, void* val, uint32_t* len);
int  socket_shutdown(int idx, int how);
int  socket_nread(int idx);                      /* FIONREAD */
/* SO_RCVTIMEO / SO_SNDTIMEO in ns (0 = none). */
uint64_t socket_timeo(int idx, int send);
/* An error is pending (so_error) or the socket is ready: for connect(). */
int  socket_pending_error(int idx);

#endif /* NET_SOCKET_H */
