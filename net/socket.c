/*
 * socket.c — [M24] BSD-style socket layer over TCP/UDP.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * A small kernel-owned socket table. Sockets are ordinary per-process file
 * descriptors: the fd entry carries `is_sock` (socket index + 1), so read/write/
 * close/dup/dup2/fork/poll work on them and a socket survives fork with a
 * reference count (the fd table is the only authority — there is no per-pid
 * owner check any more). Each TCP socket wraps a tcp_conn_t; each UDP socket
 * binds a port whose callback enqueues datagrams into a per-socket ring.
 *
 * Every operation here is non-blocking: "nothing to do yet" returns -EAGAIN and
 * the syscall layer (kernel/syscall_trap.c) turns that into a scheduler sleep +
 * re-run for a blocking descriptor, so a process waiting on the network no
 * longer spins in ring 0 and starves the CPU. tcp_connect() (active open) still
 * spins for its handshake. CAP_NET (signed manifest) is enforced by the syscall
 * dispatcher before socket creation and the socket-specific calls.
 */
#include "socket.h"
#include "tcp.h"
#include "udp.h"
#include "net.h"
#include "debugcon.h"
#include <stddef.h>

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define MAX_SOCK    32
#define SOCK_DGRAM_MAX 1024
#define SOCK_DQ     6

#define E_AGAIN   (-11)
#define E_BADF    (-9)
#define E_INVAL   (-22)
#define E_PIPE    (-32)
#define E_NOBUFS  (-105)
#define E_NOTCONN (-107)
#define E_CONNREFUSED (-111)
#define E_ADDRINUSE   (-98)
#define E_NETDOWN (-100)
#define E_OPNOTSUPP (-95)

struct dgram { uint32_t ip; uint16_t port; uint16_t len; uint8_t data[SOCK_DGRAM_MAX]; };

struct sock {
    int      used;
    int      type;            /* SOCK_STREAM / SOCK_DGRAM */
    int      refs;            /* fd-table references (dup/fork) */
    uint16_t port;            /* bound local port (TCP: used by listen) */
    /* TCP */
    tcp_conn_t* tcp;
    int      listening;
    /* UDP */
    int      udp_bound;
    struct dgram dq[SOCK_DQ];
    int      dq_head, dq_count;
    uint32_t peer_ip;         /* connected default peer (UDP) */
    uint16_t peer_port;
};

static struct sock g_sock[MAX_SOCK];

static struct sock* sk(int idx){
    if (idx < 0 || idx >= MAX_SOCK || !g_sock[idx].used) return NULL;
    return &g_sock[idx];
}

// UDP receive callback: enqueue the datagram (drop if the ring is full).
static void sock_udp_cb(void* ctx, net_dev_t* dev, uint32_t src_ip,
                        uint16_t src_port, const uint8_t* data, uint32_t len){
    (void)dev;
    struct sock* s = (struct sock*)ctx;
    if (!s->used || s->dq_count >= SOCK_DQ) return;
    if (len > SOCK_DGRAM_MAX) len = SOCK_DGRAM_MAX;
    int slot = (s->dq_head + s->dq_count) % SOCK_DQ;
    s->dq[slot].ip = src_ip; s->dq[slot].port = src_port; s->dq[slot].len = (uint16_t)len;
    for (uint32_t i = 0; i < len; i++) s->dq[slot].data[i] = data[i];
    s->dq_count++;
}

int socket_create(int type){
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return E_INVAL;
    for (int i = 0; i < MAX_SOCK; i++) if (!g_sock[i].used){
        struct sock* s = &g_sock[i];
        for (size_t k = 0; k < sizeof(*s); k++) ((uint8_t*)s)[k] = 0;
        s->used = 1; s->type = type; s->refs = 1;
        return i;
    }
    return E_NOBUFS;
}

static void sock_release(struct sock* s){
    if (s->type == SOCK_DGRAM && s->udp_bound) udp_unbind(s->port);
    if (s->type == SOCK_STREAM && s->tcp){ tcp_release(s->tcp); s->tcp = NULL; }
    s->used = 0;
}

void socket_ref(int idx){ struct sock* s = sk(idx); if (s) s->refs++; }
void socket_unref(int idx){
    struct sock* s = sk(idx);
    if (!s) return;
    if (--s->refs <= 0) sock_release(s);
}

static int udp_autobind(struct sock* s){
    if (s->udp_bound) return 0;
    uint16_t ep = udp_ephemeral_port();
    if (udp_bind(ep, sock_udp_cb, s) != 0) return E_NOBUFS;
    s->port = ep; s->udp_bound = 1;
    return 0;
}

int socket_bind(int idx, uint16_t port){
    struct sock* s = sk(idx);
    if (!s) return E_BADF;
    if (s->type == SOCK_DGRAM){
        if (s->udp_bound) udp_unbind(s->port);
        s->udp_bound = 0;
        if (udp_bind(port, sock_udp_cb, s) != 0) return E_ADDRINUSE;
        s->port = port; s->udp_bound = 1;
        return 0;
    }
    if (s->tcp) return E_INVAL;                     /* already listening/connected */
    s->port = port;                                 /* TCP bind is folded into listen() */
    return 0;
}

int socket_connect(int idx, uint32_t ip, uint16_t port){
    struct sock* s = sk(idx);
    if (!s) return E_BADF;
    net_dev_t* dev = net_primary();
    if (!dev) return E_NETDOWN;
    if (s->type == SOCK_DGRAM){
        s->peer_ip = ip; s->peer_port = port;
        return udp_autobind(s);
    }
    if (s->tcp) return E_INVAL;
    s->tcp = tcp_connect(dev, ip, port);            /* blocking handshake (spins) */
    return s->tcp ? 0 : E_CONNREFUSED;
}

int socket_listen(int idx, int backlog){
    struct sock* s = sk(idx);
    if (!s || s->type != SOCK_STREAM) return E_OPNOTSUPP;
    if (s->listening) return 0;
    if (s->tcp || !s->port) return E_INVAL;
    net_dev_t* dev = net_primary();
    if (!dev) return E_NETDOWN;
    s->tcp = tcp_alloc();
    if (!s->tcp) return E_NOBUFS;
    if (tcp_start_listen(s->tcp, dev, s->port, backlog) != 0){ tcp_free(s->tcp); s->tcp = NULL; return E_ADDRINUSE; }
    s->listening = 1;
    return 0;
}

// Non-blocking: a new socket index, or -EAGAIN when no connection is pending.
int socket_accept(int idx){
    struct sock* s = sk(idx);
    if (!s || !s->listening) return E_INVAL;
    if (!tcp_accept_pending(s->tcp)) return E_AGAIN;
    int nidx = socket_create(SOCK_STREAM);
    if (nidx < 0) return nidx;                      /* leave the child queued */
    tcp_conn_t* ch = tcp_accept_ready(s->tcp);
    if (!ch){ g_sock[nidx].used = 0; return E_AGAIN; }
    g_sock[nidx].tcp = ch;
    return nidx;
}

// Non-blocking: bytes queued (may be short), -EAGAIN when the send buffer is
// full, -EPIPE once the connection can no longer carry data.
int socket_send(int idx, const void* buf, int len){
    struct sock* s = sk(idx);
    if (!s || len < 0) return E_BADF;
    net_dev_t* dev = net_primary();
    if (!dev) return E_NETDOWN;
    if (s->type == SOCK_STREAM){
        if (!s->tcp || s->listening) return E_NOTCONN;
        tcp_state_t st = tcp_get_state(s->tcp);
        if (st != TCP_ESTABLISHED && st != TCP_CLOSE_WAIT) return E_PIPE;
        if (len == 0) return 0;
        int n = tcp_write(s->tcp, buf, (uint32_t)len);
        if (n < 0) return E_PIPE;
        return n > 0 ? n : E_AGAIN;
    }
    if (!s->peer_ip) return E_NOTCONN;
    int r = udp_autobind(s); if (r) return r;
    return udp_send(dev, s->peer_ip, s->port, s->peer_port, buf, (uint32_t)len) == 0 ? len : E_NOBUFS;
}

int socket_sendto(int idx, const void* buf, int len, uint32_t ip, uint16_t port){
    struct sock* s = sk(idx);
    if (!s || s->type != SOCK_DGRAM || len < 0) return E_INVAL;
    net_dev_t* dev = net_primary();
    if (!dev) return E_NETDOWN;
    int r = udp_autobind(s); if (r) return r;
    return udp_send(dev, ip, s->port, port, buf, (uint32_t)len) == 0 ? len : E_NOBUFS;
}

// Non-blocking receive. TCP: stream bytes, 0 = EOF (peer closed and drained, or
// reset). UDP: one datagram, filling *src_ip/*src_port. -EAGAIN = nothing yet.
int socket_recv(int idx, void* buf, int len, uint32_t* src_ip, uint16_t* src_port){
    struct sock* s = sk(idx);
    if (!s || len <= 0) return E_INVAL;
    if (s->type == SOCK_STREAM){
        if (!s->tcp || s->listening) return E_NOTCONN;
        int n = tcp_read(s->tcp, buf, (uint32_t)len);
        if (n > 0) return n;
        if (n < 0) return 0;                        /* peer closed, drained = EOF */
        return E_AGAIN;
    }
    if (s->dq_count > 0){
        struct dgram* d = &s->dq[s->dq_head];
        int n = d->len; if (n > len) n = len;
        uint8_t* o = (uint8_t*)buf;
        for (int i = 0; i < n; i++) o[i] = d->data[i];
        if (src_ip) *src_ip = d->ip;
        if (src_port) *src_port = d->port;
        s->dq_head = (s->dq_head + 1) % SOCK_DQ; s->dq_count--;
        return n;
    }
    return E_AGAIN;
}

// Readiness for poll()/select(): Linux revents bits, never blocks. `pid` is
// unused (kept for the kiface poll hook signature; access is via the fd table).
int socket_poll(uint32_t pid, int idx){
    (void)pid;
    struct sock* s = sk(idx);
    if (!s) return SOCK_POLLNVAL;
    int ev = 0;
    if (s->type == SOCK_DGRAM){
        if (s->dq_count > 0) ev |= SOCK_POLLIN;
        return ev | SOCK_POLLOUT;
    }
    if (!s->tcp) return 0;
    if (s->listening) return tcp_accept_pending(s->tcp) ? SOCK_POLLIN : 0;
    tcp_state_t st = tcp_get_state(s->tcp);
    if (tcp_rx_available(s->tcp) > 0) ev |= SOCK_POLLIN;
    if (st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT){
        if (tcp_tx_space(s->tcp) > 0) ev |= SOCK_POLLOUT;
    }
    if (st == TCP_CLOSE_WAIT || st == TCP_CLOSING || st == TCP_LAST_ACK ||
        st == TCP_TIME_WAIT || st == TCP_CLOSED)
        ev |= SOCK_POLLIN;                          /* EOF is readable */
    if (st == TCP_CLOSED || tcp_was_reset(s->tcp)) ev |= SOCK_POLLHUP | SOCK_POLLOUT;
    if (tcp_was_reset(s->tcp)) ev |= SOCK_POLLERR;
    return ev;
}

// getsockname (peer=0) / getpeername (peer=1).
int socket_name(int idx, int peer, uint32_t* ip, uint16_t* port){
    struct sock* s = sk(idx);
    if (!s) return E_BADF;
    *ip = 0; *port = 0;
    if (s->type == SOCK_DGRAM){
        if (peer){ if (!s->peer_ip) return E_NOTCONN; *ip = s->peer_ip; *port = s->peer_port; return 0; }
        net_dev_t* dev = net_primary();
        *ip = dev ? dev->ip : 0; *port = s->port;
        return 0;
    }
    if (!s->tcp){ if (peer) return E_NOTCONN; *port = s->port; return 0; }
    uint32_t lip, rip; uint16_t lport, rport;
    tcp_get_addrs(s->tcp, &lip, &lport, &rip, &rport);
    if (peer){
        if (s->listening) return E_NOTCONN;
        *ip = rip; *port = rport;
    } else { *ip = lip; *port = lport; }
    return 0;
}

