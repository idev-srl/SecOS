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
 * binds a port whose callback enqueues datagrams into a per-socket queue.
 *
 * Every operation here is non-blocking: "nothing to do yet" returns -EAGAIN and
 * the syscall layer (kernel/syscall_trap.c, kernel/lx_net.c) turns that into a
 * scheduler sleep + re-run for a blocking descriptor. [M47] connect() is
 * non-blocking too: it returns -EINPROGRESS / -EALREADY while the handshake
 * runs and 0 once established. CAP_NET (signed manifest) is enforced by the
 * syscall layer before socket creation and the socket-specific calls.
 *
 * [M47] Locking: the socket table and every socket's state (including the
 * datagram queues the RX path fills) are guarded by the stack lock. Each
 * socket_* entry point takes it for the whole operation; the buffers it is
 * given are kernel bounce buffers (user copies happen in the syscall layer,
 * outside the lock). A socket's slot is reused only after its last fd
 * reference is dropped (socket_unref), and that release unbinds the UDP port
 * and hands the TCP control block to tcp_release() under the same lock, so the
 * RX path — which only reaches a socket through a UDP binding, a TCP control
 * block or a scan of the table, all under the lock — never sees a freed one.
 *
 * [U10] Linux-shaped sockets (kernel/lx_net.c):
 *  - kinds: TCP, UDP, ICMP ping sockets (SOCK_DGRAM + IPPROTO_ICMP) and raw
 *    IP sockets (SOCK_RAW, root or CAP_NET_RAW: checked by the syscall layer);
 *  - ping sockets work like Linux's: the kernel owns the echo identifier (the
 *    socket's "port": bind() chooses it, else the first send picks a free one),
 *    sends only ICMP echo requests (type 8, code 0) with that identifier
 *    written in and the checksum recomputed, builds the IP header itself, and
 *    delivers to the socket only echo replies carrying its identifier (from
 *    its peer when connected) and the ICMP errors quoting one of its requests;
 *  - raw sockets get a copy of every received IPv4 packet of their protocol,
 *    IP header included (ICMP_FILTER honoured), and send either a payload
 *    (the kernel adds the header) or, with IP_HDRINCL / IPPROTO_RAW, a whole
 *    packet whose header the kernel completes;
 *  - ICMP errors (destination unreachable, time exceeded, parameter problem)
 *    are matched to the UDP, ping or raw socket whose packet they quote and
 *    reported as on Linux: sk_err (the next call fails once with the mapped
 *    errno) for a connected socket or one with IP_RECVERR, plus an entry in
 *    the socket's error queue (recvmsg(MSG_ERRQUEUE), struct
 *    sock_extended_err + the ICMP sender) with IP_RECVERR;
 *  - datagrams are queued individually from the heap (bounded per socket by
 *    SK_RCVBUF bytes), so a 1024-entry table costs no memory until used.
 */
#include "socket.h"
#include "tcp.h"
#include "udp.h"
#include "net.h"
#include "heap.h"
#include "kclock.h"
#include "debugcon.h"
#include <stddef.h>

extern uint64_t timer_get_ticks(void);

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define MAX_SOCK    1024        /* [U10] was 64 ([M47] was 32) */
#define SK_DGRAM_MAX 1480       /* largest datagram we can receive (no reassembly) */
#define SK_RCVBUF   212992      /* bytes queued per datagram socket (Linux default) */
#define SK_SNDBUF   212992
#define SK_ERRQ     4           /* MSG_ERRQUEUE depth */
#define SK_ERRDATA  64          /* bytes of the offending packet kept per error */

#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_RAW  255

#define E_PERM    (-1)
#define E_AGAIN   (-11)
#define E_BADF    (-9)
#define E_ACCES   (-13)
#define E_FAULT   (-14)
#define E_INVAL   (-22)
#define E_PIPE    (-32)
#define E_NODEV   (-19)
#define E_MSGSIZE (-90)
#define E_NOPROTOOPT (-92)
#define E_OPNOTSUPP (-95)
#define E_ADDRINUSE   (-98)
#define E_ADDRNOTAVAIL (-99)
#define E_NETDOWN (-100)
#define E_NETUNREACH (-101)
#define E_NOBUFS  (-105)
#define E_ISCONN  (-106)
#define E_NOTCONN (-107)
#define E_TIMEDOUT  (-110)
#define E_CONNREFUSED (-111)
#define E_ALREADY   (-114)
#define E_INPROGRESS (-115)
#define E_DESTADDRREQ (-89)
#define SOCK_CONNECT_MS 20000      /* active open gives up after 20 s */

/* A received datagram (UDP payload, ICMP message, or whole IP packet for raw). */
struct dgram {
    struct dgram* next;
    uint32_t ip; uint16_t port; uint16_t len;
    uint8_t  ttl, tos;
    uint64_t ts_ns;
    uint8_t  data[];
};

/* An ICMP error queued for MSG_ERRQUEUE. */
struct sk_err {
    uint32_t err;            /* errno */
    uint8_t  type, code;
    uint32_t info;
    uint32_t offender;       /* who sent the ICMP error */
    uint32_t dst_ip; uint16_t dst_port;   /* the original destination */
    uint16_t dlen;
    uint8_t  data[SK_ERRDATA];   /* the original transport header/payload */
};

struct sock {
    int      used;
    int      kind;            /* SKIND_* */
    int      proto;           /* raw: IP protocol */
    int      refs;            /* fd-table references (dup/fork) */
    uint32_t bound_ip;        /* bind() address (0 = any) */
    uint16_t port;            /* local port; ping: echo identifier */
    uint8_t  bound;           /* bind() or autobind gave it a port */
    uint8_t  shut;            /* 1 = SHUT_RD, 2 = SHUT_WR */
    /* TCP */
    tcp_conn_t* tcp;
    int      listening;
    int      connecting;      /* active open started, not yet reported */
    uint64_t conn_deadline;
    /* datagrams */
    int      udp_bound;
    struct dgram *dq_head, *dq_tail;
    uint32_t dq_bytes, dq_count;
    uint32_t peer_ip;         /* connected default peer */
    uint16_t peer_port;
    /* errors */
    int      so_error;        /* pending errno (sk_err), reported once */
    struct sk_err errq[SK_ERRQ];
    int      errq_head, errq_n;
    /* options */
    uint8_t  reuseaddr, reuseport, keepalive, nodelay, broadcast, oobinline;
    uint8_t  recverr, recvttl, recvtos, timestamp, timestampns, hdrincl, pktinfo;
    uint8_t  ttl, tos, mcast_ttl, mcast_loop, mtu_disc;
    uint8_t  linger_on;
    int      linger_s;
    uint32_t icmp_filter;     /* raw ICMP: bit n set = drop type n */
    uint32_t mark, priority;
    int      keepidle, keepintvl, keepcnt, user_timeout;
    uint64_t rcvtimeo_ns, sndtimeo_ns;
};

static struct sock g_sock[MAX_SOCK];      /* [M47] stack lock */
static int g_nraw, g_nping;               /* raw / ping sockets open (RX fast path) */

int sock_count_used(void){
    int n = 0;
    for (int i = 0; i < MAX_SOCK; i++) if (g_sock[i].used) n++;
    return n;
}

static struct sock* sk(int idx){
    NET_ASSERT_STACK_LOCKED();
    if (idx < 0 || idx >= MAX_SOCK || !g_sock[idx].used) return NULL;
    return &g_sock[idx];
}

static void bzero_(void* p, size_t n){ uint8_t* b = (uint8_t*)p; while (n--) *b++ = 0; }
static void bcopy_(void* d, const void* s, size_t n){ uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while (n--) *a++ = *b++; }

/* ---------------------------------------------------------- datagram queue */
static void dq_push(struct sock* s, uint32_t ip, uint16_t port, const uint8_t* data, uint32_t len){
    if (len > SK_DGRAM_MAX + 60) len = SK_DGRAM_MAX + 60;
    if (s->dq_bytes + len > SK_RCVBUF) return;              /* receive buffer full: drop */
    struct dgram* d = (struct dgram*)kmalloc(sizeof(*d) + len);
    if (!d) return;
    d->next = NULL; d->ip = ip; d->port = port; d->len = (uint16_t)len;
    d->ttl = ipv4_rx_ttl(); d->tos = ipv4_rx_tos();
    d->ts_ns = (s->timestamp || s->timestampns) ? kclock_real_ns() : 0;
    bcopy_(d->data, data, len);
    if (s->dq_tail) s->dq_tail->next = d; else s->dq_head = d;
    s->dq_tail = d;
    s->dq_bytes += len; s->dq_count++;
}
static void dq_pop(struct sock* s){
    struct dgram* d = s->dq_head;
    if (!d) return;
    s->dq_head = d->next;
    if (!s->dq_head) s->dq_tail = NULL;
    s->dq_bytes -= d->len; s->dq_count--;
    kfree(d);
}
static void dq_flush(struct sock* s){ while (s->dq_head) dq_pop(s); }

// UDP receive callback: enqueue the datagram (drop if the queue is full).
static void sock_udp_cb(void* ctx, net_dev_t* dev, uint32_t src_ip,
                        uint16_t src_port, const uint8_t* data, uint32_t len){
    (void)dev;
    struct sock* s = (struct sock*)ctx;
    if (!s->used || (s->shut & 1)) return;
    if (s->peer_ip && (src_ip != s->peer_ip || src_port != s->peer_port)) return;  /* connected */
    dq_push(s, src_ip, src_port, data, len);
}

/* ------------------------------------------------------------- lifecycle */
static int l_create_kind(int kind, int proto){
    if (kind < SKIND_TCP || kind > SKIND_RAW) return E_INVAL;
    for (int i = 0; i < MAX_SOCK; i++) if (!g_sock[i].used){
        struct sock* s = &g_sock[i];
        bzero_(s, sizeof(*s));
        s->used = 1; s->kind = kind; s->proto = proto; s->refs = 1;
        s->ttl = 64; s->mcast_ttl = 1; s->mcast_loop = 1;
        if (kind == SKIND_RAW) { g_nraw++; if (proto == IPPROTO_RAW) s->hdrincl = 1; }
        if (kind == SKIND_PING) g_nping++;
        return i;
    }
    return E_NOBUFS;
}
static int l_create(int type){
    if (type == SOCK_STREAM) return l_create_kind(SKIND_TCP, IPPROTO_TCP);
    if (type == SOCK_DGRAM)  return l_create_kind(SKIND_UDP, IPPROTO_UDP);
    return E_INVAL;
}

static void sock_release(struct sock* s){
    if (s->kind == SKIND_UDP && s->udp_bound) udp_unbind(s->port);
    if (s->kind == SKIND_TCP && s->tcp){
        if (s->linger_on && s->linger_s == 0 && !s->listening) tcp_abort(s->tcp);
        else tcp_release(s->tcp);
        s->tcp = NULL;
    }
    if (s->kind == SKIND_RAW) g_nraw--;
    if (s->kind == SKIND_PING) g_nping--;
    dq_flush(s);
    s->used = 0;
}

static void l_ref(int idx){ struct sock* s = sk(idx); if (s) s->refs++; }
static void l_unref(int idx){
    struct sock* s = sk(idx);
    if (!s) return;
    if (--s->refs <= 0) sock_release(s);
}

/* --------------------------------------------------------------- helpers */
static int is_bcast(const net_dev_t* dev, uint32_t ip){
    if (ip == 0xFFFFFFFFu) return 1;
    return dev && dev->netmask && ip == (dev->ip | ~dev->netmask);
}

/* Ports: is `port` taken for a new binding of kind `kind`? */
static int port_taken(const struct sock* self, int kind, uint16_t port, int reuse){
    for (int i = 0; i < MAX_SOCK; i++){
        const struct sock* o = &g_sock[i];
        if (!o->used || o == self || o->kind != kind || !o->bound || o->port != port) continue;
        if (kind == SKIND_TCP && reuse && o->reuseaddr && !o->listening) continue;
        return 1;
    }
    if (kind == SKIND_TCP){
        int u = tcp_port_use(port);
        if (u == 2 || (u == 1 && !reuse)) return 1;
    }
    return 0;
}

static int udp_autobind(struct sock* s){
    if (s->udp_bound) return 0;
    uint16_t ep = 0;
    for (int tries = 0; tries < 64; tries++){
        ep = udp_ephemeral_port();
        if (ep && !port_taken(s, SKIND_UDP, ep, 0)) break;
        ep = 0;
    }
    if (!ep || udp_bind(ep, sock_udp_cb, s) != 0) return E_NOBUFS;
    s->port = ep; s->udp_bound = 1; s->bound = 1;
    return 0;
}

/* Ping sockets: a free echo identifier. */
static uint16_t g_ping_next = 1000;
static int ping_id_taken(const struct sock* self, uint16_t id){
    for (int i = 0; i < MAX_SOCK; i++){
        const struct sock* o = &g_sock[i];
        if (o->used && o != self && o->kind == SKIND_PING && o->bound && o->port == id) return 1;
    }
    return 0;
}
static int ping_autobind(struct sock* s){
    if (s->bound) return 0;
    for (int tries = 0; tries < 65536; tries++){
        uint16_t id = g_ping_next++;
        if (g_ping_next == 0) g_ping_next = 1;
        if (id && !ping_id_taken(s, id)){ s->port = id; s->bound = 1; return 0; }
    }
    return E_ADDRINUSE;
}

static void err_set(struct sock* s, int e){ s->so_error = e; }
/* so_error holds a positive errno; reported once (calls return -err_take). */
static int err_take(struct sock* s){ int e = s->so_error; s->so_error = 0; return e; }

/* ------------------------------------------------------------ bind/connect */
static int l_bind_ex(int idx, uint32_t ip, uint16_t port, int old_abi){
    struct sock* s = sk(idx);
    if (!s) return E_BADF;
    net_dev_t* dev = net_primary();
    if (ip && (!dev || ip != dev->ip) && !(s->kind != SKIND_TCP && is_bcast(dev, ip)))
        return E_ADDRNOTAVAIL;
    switch (s->kind){
    case SKIND_UDP:
        if (s->bound && !old_abi) return E_INVAL;
        if (s->udp_bound) udp_unbind(s->port);
        s->udp_bound = 0; s->bound = 0;
        if (port == 0) { int r = udp_autobind(s); if (r) return r; s->bound_ip = ip; return 0; }
        if (port_taken(s, SKIND_UDP, port, 0) || udp_bind(port, sock_udp_cb, s) != 0) return E_ADDRINUSE;
        s->port = port; s->udp_bound = 1; s->bound = 1; s->bound_ip = ip;
        return 0;
    case SKIND_PING:
        if (s->bound) return E_INVAL;
        if (port == 0) { int r = ping_autobind(s); if (r) return r; }
        else { if (ping_id_taken(s, port)) return E_ADDRINUSE; s->port = port; s->bound = 1; }
        s->bound_ip = ip;
        return 0;
    case SKIND_RAW:
        s->bound_ip = ip;                         /* raw: address only (filters RX) */
        return 0;
    default:
        break;
    }
    if (s->tcp) return E_INVAL;                   /* already listening/connected */
    if (s->bound && !old_abi) return E_INVAL;
    if (port == 0) {
        for (int tries = 0; tries < 64 && !port; tries++){
            uint16_t p = tcp_ephemeral_port();
            if (p && !port_taken(s, SKIND_TCP, p, 0)) port = p;
        }
        if (!port) return E_ADDRINUSE;
    } else if (port_taken(s, SKIND_TCP, port, old_abi || s->reuseaddr)) {
        return E_ADDRINUSE;
    }
    s->port = port; s->bound = 1; s->bound_ip = ip;   /* TCP bind is folded into listen() */
    return 0;
}

static int l_connect_ex(int idx, uint32_t ip, uint16_t port, int unspec){
    struct sock* s = sk(idx);
    if (!s) return E_BADF;
    net_dev_t* dev = net_primary();
    if (s->kind != SKIND_TCP){
        if (unspec){ s->peer_ip = 0; s->peer_port = 0; return 0; }
        if (!dev) return E_NETDOWN;
        if (!ip) return E_NETUNREACH;             /* no loopback interface yet */
        if (is_bcast(dev, ip) && !s->broadcast) return E_ACCES;
        if (s->kind == SKIND_UDP && port == 0) return E_INVAL;
        s->peer_ip = ip; s->peer_port = port;
        if (s->kind == SKIND_UDP){ dq_flush(s); return udp_autobind(s); }
        if (s->kind == SKIND_PING) return ping_autobind(s);
        return 0;
    }
    if (unspec) return E_INVAL;
    if (!dev) return E_NETDOWN;
    if (s->listening) return E_INVAL;
    if (!s->tcp) {
        if (s->so_error && !s->connecting) return -err_take(s);
        if (!ip) return E_NETUNREACH;
        if (is_bcast(dev, ip)) return E_NETUNREACH;
        /* Start the handshake; the caller re-runs connect() to collect it. */
        s->tcp = tcp_alloc();
        if (!s->tcp) return E_NOBUFS;
        if (tcp_start_connect(s->tcp, dev, ip, port, s->bound ? s->port : 0) != 0) {
            tcp_free(s->tcp); s->tcp = NULL;
            return E_CONNREFUSED;
        }
        if (!s->bound) { uint32_t li, ri; uint16_t lp, rp; tcp_get_addrs(s->tcp, &li, &lp, &ri, &rp); s->port = lp; }
        s->connecting = 1;
        s->conn_deadline = timer_get_ticks() + SOCK_CONNECT_MS;
        return E_INPROGRESS;
    }
    if (!s->connecting) return E_ISCONN;
    tcp_state_t st = tcp_get_state(s->tcp);
    if (st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT) { s->connecting = 0; return 0; }
    int reset = tcp_was_reset(s->tcp) || st == TCP_CLOSED;
    if (reset || timer_get_ticks() >= s->conn_deadline) {
        tcp_release(s->tcp); s->tcp = NULL;
        s->connecting = 0;
        return reset ? E_CONNREFUSED : E_TIMEDOUT;
    }
    return E_ALREADY;
}

/* Collect a finished non-blocking connect into so_error (SO_ERROR, poll). */
static void connect_settle(struct sock* s){
    if (s->kind != SKIND_TCP || !s->connecting || !s->tcp) return;
    tcp_state_t st = tcp_get_state(s->tcp);
    if (st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT) { s->connecting = 0; return; }
    int reset = tcp_was_reset(s->tcp) || st == TCP_CLOSED;
    if (reset || timer_get_ticks() >= s->conn_deadline) {
        tcp_release(s->tcp); s->tcp = NULL;
        s->connecting = 0;
        err_set(s, reset ? -E_CONNREFUSED : -E_TIMEDOUT);
    }
}

static int l_listen(int idx, int backlog){
    struct sock* s = sk(idx);
    if (!s || s->kind != SKIND_TCP) return E_OPNOTSUPP;
    if (s->listening) return 0;
    if (s->tcp) return E_INVAL;
    if (!s->port) { int r = l_bind_ex(idx, 0, 0, 0); if (r) return r; }   /* Linux autobinds */
    net_dev_t* dev = net_primary();
    if (!dev) return E_NETDOWN;
    s->tcp = tcp_alloc();
    if (!s->tcp) return E_NOBUFS;
    if (tcp_start_listen(s->tcp, dev, s->port, backlog) != 0){ tcp_free(s->tcp); s->tcp = NULL; return E_ADDRINUSE; }
    s->listening = 1;
    return 0;
}

// Non-blocking: a new socket index, or -EAGAIN when no connection is pending.
static int l_accept_ex(int idx, uint32_t* ip, uint16_t* port){
    struct sock* s = sk(idx);
    if (!s || !s->listening) return E_INVAL;
    if (!tcp_accept_pending(s->tcp)) return E_AGAIN;
    int nidx = l_create_kind(SKIND_TCP, IPPROTO_TCP);
    if (nidx < 0) return nidx;                      /* leave the child queued */
    tcp_conn_t* ch = tcp_accept_ready(s->tcp);
    if (!ch){ g_sock[nidx].used = 0; return E_AGAIN; }
    struct sock* n = &g_sock[nidx];
    n->tcp = ch;
    uint32_t li, ri; uint16_t lp, rp;
    tcp_get_addrs(ch, &li, &lp, &ri, &rp);
    n->port = lp; n->bound = 0;                     /* shares the listener's port */
    n->nodelay = s->nodelay; n->keepalive = s->keepalive;
    if (ip) *ip = ri;
    if (port) *port = rp;
    return nidx;
}

/* ------------------------------------------------------------ send */
static int ping_send(struct sock* s, net_dev_t* dev, uint32_t dst, const uint8_t* buf, int len){
    if (len < 8) return E_INVAL;
    if (buf[0] != 8 || buf[1] != 0) return E_INVAL;       /* ICMP_ECHO, code 0 only */
    if (len > NET_MTU - 20) return E_MSGSIZE;
    int r = ping_autobind(s); if (r) return r;
    uint8_t* p = (uint8_t*)kmalloc((size_t)len);
    if (!p) return E_NOBUFS;
    bcopy_(p, buf, (size_t)len);
    p[4] = (uint8_t)(s->port >> 8); p[5] = (uint8_t)s->port;   /* the kernel's identifier */
    p[2] = 0; p[3] = 0;
    uint16_t c = net_checksum(p, (uint32_t)len);
    p[2] = (uint8_t)(c >> 8); p[3] = (uint8_t)c;
    int ok = ipv4_send_ex(dev, dst, IPPROTO_ICMP, p, (uint32_t)len, s->ttl, s->tos, 1);
    kfree(p);
    return ok == 0 ? len : E_NOBUFS;
}

static int raw_send(struct sock* s, net_dev_t* dev, uint32_t dst, const uint8_t* buf, int len){
    if (s->hdrincl){
        if (len < 20) return E_INVAL;
        int r = ipv4_output_hdr(dev, buf, (uint32_t)len);
        return r == 0 ? len : (r == -22 ? E_INVAL : E_NOBUFS);
    }
    if (len > NET_MTU - 20) return E_MSGSIZE;
    int ok = ipv4_send_ex(dev, dst, (uint8_t)s->proto, buf, (uint32_t)len, s->ttl, s->tos, 1);
    return ok == 0 ? len : E_NOBUFS;
}

static int dgram_send(struct sock* s, const void* buf, int len, int has_dst, uint32_t ip, uint16_t port){
    net_dev_t* dev = net_primary();
    if (!dev) return E_NETDOWN;
    if (s->shut & 2) return E_PIPE;
    if (s->so_error) return -err_take(s);
    if (!has_dst){
        if (!s->peer_ip) return E_DESTADDRREQ;
        ip = s->peer_ip; port = s->peer_port;
    }
    if (!ip) return E_NETUNREACH;
    if (is_bcast(dev, ip) && !s->broadcast) return E_ACCES;
    if (s->kind == SKIND_PING) return ping_send(s, dev, ip, (const uint8_t*)buf, len);
    if (s->kind == SKIND_RAW)  return raw_send(s, dev, ip, (const uint8_t*)buf, len);
    if (port == 0) return E_INVAL;
    if (len > NET_MTU - 28) return E_MSGSIZE;
    int r = udp_autobind(s); if (r) return r;
    /* udp_output has no TTL argument: the UDP header is built here instead. */
    uint32_t ulen = 8u + (uint32_t)len;
    uint8_t* seg = (uint8_t*)kmalloc(12 + ulen);
    if (!seg) return E_NOBUFS;
    uint8_t* u = seg + 12;
    u[0] = (uint8_t)(s->port >> 8); u[1] = (uint8_t)s->port;
    u[2] = (uint8_t)(port >> 8);    u[3] = (uint8_t)port;
    u[4] = (uint8_t)(ulen >> 8);    u[5] = (uint8_t)ulen;
    u[6] = 0; u[7] = 0;
    bcopy_(u + 8, buf, (size_t)len);
    uint8_t* ph = seg;
    ph[0]=(uint8_t)dev->ip; ph[1]=(uint8_t)(dev->ip>>8); ph[2]=(uint8_t)(dev->ip>>16); ph[3]=(uint8_t)(dev->ip>>24);
    ph[4]=(uint8_t)ip; ph[5]=(uint8_t)(ip>>8); ph[6]=(uint8_t)(ip>>16); ph[7]=(uint8_t)(ip>>24);
    ph[8]=0; ph[9]=IPPROTO_UDP; ph[10]=(uint8_t)(ulen>>8); ph[11]=(uint8_t)ulen;
    uint16_t cs = net_checksum(seg, 12 + ulen);
    if (cs == 0) cs = 0xFFFF;
    u[6] = (uint8_t)(cs >> 8); u[7] = (uint8_t)cs;
    int ok = ipv4_send_ex(dev, ip, IPPROTO_UDP, u, ulen, s->ttl, s->tos, 1);
    kfree(seg);
    return ok == 0 ? len : E_NOBUFS;
}

// Non-blocking: bytes queued (may be short), -EAGAIN when the send buffer is
// full, -EPIPE once the connection can no longer carry data.
static int l_sendmsg(int idx, const void* buf, int len, int flags, int has_dst, uint32_t ip, uint16_t port){
    struct sock* s = sk(idx);
    if (!s || len < 0) return E_BADF;
    if (flags & SK_MSG_OOB) return E_OPNOTSUPP;
    if (s->kind != SKIND_TCP) return dgram_send(s, buf, len, has_dst, ip, port);
    net_dev_t* dev = net_primary();
    if (!dev) return E_NETDOWN;
    if (!s->tcp && s->so_error) return -err_take(s);
    if (!s->tcp || s->listening) return E_NOTCONN;
    if (s->connecting) {
        tcp_state_t cs = tcp_get_state(s->tcp);
        if (cs == TCP_SYN_SENT) return E_AGAIN;       /* still connecting: wait */
        s->connecting = 0;
    }
    if (s->shut & 2) return E_PIPE;
    tcp_state_t st = tcp_get_state(s->tcp);
    if (st != TCP_ESTABLISHED && st != TCP_CLOSE_WAIT) return tcp_was_reset(s->tcp) ? -104 /*ECONNRESET*/ : E_PIPE;
    if (len == 0) return 0;
    int n = tcp_write(s->tcp, buf, (uint32_t)len);
    if (n < 0) return E_PIPE;
    return n > 0 ? n : E_AGAIN;
}
static int l_send(int idx, const void* buf, int len){ return l_sendmsg(idx, buf, len, 0, 0, 0, 0); }
static int l_sendto(int idx, const void* buf, int len, uint32_t ip, uint16_t port){
    struct sock* s = sk(idx);
    if (!s || s->kind == SKIND_TCP || len < 0) return E_INVAL;
    return dgram_send(s, buf, len, 1, ip, port);
}

/* ------------------------------------------------------------ receive */
static void meta_wishes(const struct sock* s, sock_rxmeta_t* m){
    m->want_ttl = s->recvttl; m->want_tos = s->recvtos;
    m->want_ts = s->timestamp; m->want_tsns = s->timestampns;
}

static int errq_recv(struct sock* s, void* buf, int len, sock_rxmeta_t* m){
    if (!s->errq_n) return E_AGAIN;
    struct sk_err* e = &s->errq[s->errq_head];
    int n = e->dlen; if (n > len) n = len;
    bcopy_(buf, e->data, (size_t)n);
    if (m){
        m->ip = e->dst_ip; m->port = e->dst_port; m->has_addr = 1;
        m->full_len = e->dlen;
        m->ee_errno = e->err; m->ee_origin = 2 /* SO_EE_ORIGIN_ICMP */;
        m->ee_type = e->type; m->ee_code = e->code; m->ee_info = e->info;
        m->offender = e->offender;
    }
    s->errq_head = (s->errq_head + 1) % SK_ERRQ; s->errq_n--;
    return n;
}

// Non-blocking receive. TCP: stream bytes, 0 = EOF (peer closed and drained, or
// reset). Datagrams: one each, filling the metadata. -EAGAIN = nothing yet.
static int l_recvmsg(int idx, void* buf, int len, int flags, sock_rxmeta_t* m){
    struct sock* s = sk(idx);
    if (!s || len < 0) return E_INVAL;
    if (m) { bzero_(m, sizeof(*m)); meta_wishes(s, m); }
    if (flags & SK_MSG_ERRQUEUE) return errq_recv(s, buf, len, m);
    if (s->kind == SKIND_TCP){
        if (flags & SK_MSG_OOB) return E_INVAL;
        if (!s->tcp && s->so_error) return -err_take(s);
        if (!s->tcp || s->listening) return E_NOTCONN;
        if (s->connecting) {
            if (tcp_get_state(s->tcp) == TCP_SYN_SENT) return E_AGAIN;
            s->connecting = 0;
        }
        if (s->shut & 1) return 0;
        if (len == 0) return 0;
        int avail = tcp_rx_available(s->tcp);
        tcp_state_t st = tcp_get_state(s->tcp);
        int open = (st == TCP_ESTABLISHED || st == TCP_FIN_WAIT_1 || st == TCP_FIN_WAIT_2);
        /* MSG_WAITALL: wait (without consuming) until it all fits, while
         * the stream can still deliver it. */
        if ((flags & SK_MSG_WAITALL) && open && avail < len && (uint32_t)len <= tcp_rcvbuf_size())
            return E_AGAIN;
        if (flags & SK_MSG_PEEK){
            int n = tcp_peek(s->tcp, buf, (uint32_t)len);
            if (n > 0) return n;
            return open ? E_AGAIN : 0;
        }
        int n = tcp_read(s->tcp, buf, (uint32_t)len);
        if (n > 0) return n;
        if (n < 0 || tcp_was_reset(s->tcp)) return 0;    /* peer closed and drained / reset: EOF */
        return E_AGAIN;
    }
    if (s->so_error) return -err_take(s);
    if (!s->dq_head) return (s->shut & 1) ? 0 : E_AGAIN;
    struct dgram* d = s->dq_head;
    int n = d->len; if (n > len) n = len;
    bcopy_(buf, d->data, (size_t)n);
    if (m){
        m->ip = d->ip; m->port = d->port; m->has_addr = 1;
        m->full_len = d->len; m->ttl = d->ttl; m->tos = d->tos; m->ts_ns = d->ts_ns;
    }
    if (!(flags & SK_MSG_PEEK)) dq_pop(s);
    return n;
}
static int l_recv(int idx, void* buf, int len, uint32_t* src_ip, uint16_t* src_port){
    if (len <= 0) return E_INVAL;
    sock_rxmeta_t m;
    int r = l_recvmsg(idx, buf, len, 0, &m);
    if (r >= 0){ if (src_ip) *src_ip = m.ip; if (src_port) *src_port = m.port; }
    return r;
}

/* ---------------------------------------------------------------- poll */
// Readiness for poll()/select(): Linux revents bits, never blocks. `pid` is
// unused (kept for the kiface poll hook signature; access is via the fd table).
static int l_poll(uint32_t pid, int idx){
    (void)pid;
    struct sock* s = sk(idx);
    if (!s) return SOCK_POLLNVAL;
    int ev = 0;
    if (s->so_error || s->errq_n) ev |= SOCK_POLLERR;
    if (s->kind != SKIND_TCP){
        if (s->dq_head || (s->shut & 1)) ev |= SOCK_POLLIN;
        if (s->shut & 1) ev |= SOCK_POLLRDHUP;
        if (s->shut == 3) ev |= SOCK_POLLHUP;
        return ev | SOCK_POLLOUT;
    }
    connect_settle(s);
    if (!s->tcp) return ev | SOCK_POLLOUT | SOCK_POLLHUP;   /* failed connect / unconnected (Linux) */
    if (s->listening) return tcp_accept_pending(s->tcp) ? SOCK_POLLIN : 0;
    tcp_state_t st = tcp_get_state(s->tcp);
    if (st == TCP_SYN_SENT) return ev;               /* connect() still running */
    if (tcp_rx_available(s->tcp) > 0) ev |= SOCK_POLLIN;
    if (st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT){
        if (tcp_tx_space(s->tcp) > 0) ev |= SOCK_POLLOUT;
    }
    if (st == TCP_CLOSE_WAIT || st == TCP_CLOSING || st == TCP_LAST_ACK ||
        st == TCP_TIME_WAIT || st == TCP_CLOSED || (s->shut & 1))
        ev |= SOCK_POLLIN | SOCK_POLLRDHUP;          /* EOF is readable */
    if (st == TCP_CLOSED || tcp_was_reset(s->tcp)) ev |= SOCK_POLLHUP | SOCK_POLLOUT;
    if (tcp_was_reset(s->tcp)) ev |= SOCK_POLLERR;
    return ev;
}

// getsockname (peer=0) / getpeername (peer=1).
static int l_name(int idx, int peer, uint32_t* ip, uint16_t* port){
    struct sock* s = sk(idx);
    if (!s) return E_BADF;
    *ip = 0; *port = 0;
    net_dev_t* dev = net_primary();
    if (s->kind != SKIND_TCP){
        if (peer){ if (!s->peer_ip) return E_NOTCONN; *ip = s->peer_ip; *port = s->peer_port; return 0; }
        *ip = s->bound_ip ? s->bound_ip : (s->peer_ip && dev ? dev->ip : 0);
        *port = (s->kind == SKIND_RAW) ? 0 : s->port;
        return 0;
    }
    if (!s->tcp){ if (peer) return E_NOTCONN; *ip = s->bound_ip; *port = s->port; return 0; }
    uint32_t lip, rip; uint16_t lport, rport;
    tcp_get_addrs(s->tcp, &lip, &lport, &rip, &rport);
    if (peer){
        if (s->listening) return E_NOTCONN;
        if (s->connecting && tcp_get_state(s->tcp) == TCP_SYN_SENT) return E_NOTCONN;
        *ip = rip; *port = rport;
    } else {
        *ip = s->listening ? s->bound_ip : lip; *port = lport;
    }
    return 0;
}

/* ------------------------------------------------------------- shutdown */
static int l_shutdown(int idx, int how){
    struct sock* s = sk(idx);
    if (!s) return E_BADF;
    if (how < 0 || how > 2) return E_INVAL;
    uint8_t bits = (how == 0) ? 1 : (how == 1) ? 2 : 3;
    if (s->kind == SKIND_TCP){
        if (s->listening){ s->shut |= bits; return 0; }
        if (!s->tcp || s->connecting) return E_NOTCONN;
        if ((bits & 2) && !(s->shut & 2)) tcp_start_close(s->tcp);   /* FIN after queued data */
        s->shut |= bits;
        return 0;
    }
    if (!s->peer_ip) return E_NOTCONN;
    s->shut |= bits;
    return 0;
}

/* -------------------------------------------------------------- options */
#define SOL_SOCKET_ 1
#define SOL_IP_     0
#define SOL_TCP_    6
#define SOL_UDP_    17
#define SOL_RAW_    255

static int opt_int(const void* v, uint32_t len, int* out){
    if (len < sizeof(int)) {
        if (len == 1) { *out = *(const uint8_t*)v; return 0; }   /* Linux accepts a char for IP_TTL etc. */
        return E_INVAL;
    }
    *out = *(const int*)v;
    return 0;
}
struct k_timeval { int64_t sec, usec; };
static int opt_tv(const void* v, uint32_t len, uint64_t* ns){
    if (len < sizeof(struct k_timeval)) return E_INVAL;
    const struct k_timeval* t = (const struct k_timeval*)v;
    if (t->usec < 0 || t->usec >= 1000000) return -33;      /* EDOM, as Linux */
    if (t->sec < 0) { *ns = 0; return 0; }
    *ns = (uint64_t)t->sec * 1000000000ull + (uint64_t)t->usec * 1000ull;
    return 0;
}

static int l_setopt(int idx, int level, int name, const void* val, uint32_t len, int priv){
    struct sock* s = sk(idx);
    if (!s) return E_BADF;
    int v = 0, r;
    if (level == SOL_SOCKET_){
        switch (name){
        case 20: return opt_tv(val, len, &s->rcvtimeo_ns);                    /* SO_RCVTIMEO */
        case 21: return opt_tv(val, len, &s->sndtimeo_ns);                    /* SO_SNDTIMEO */
        case 13: {                                                            /* SO_LINGER */
            if (len < 8) return E_INVAL;
            const int* l = (const int*)val;
            s->linger_on = l[0] ? 1 : 0; s->linger_s = l[1] < 0 ? 0 : l[1];
            return 0;
        }
        case 25: {                                                            /* SO_BINDTODEVICE */
            if (!priv) return E_PERM;
            const char* nm = (const char*)val;
            if (len == 0 || nm[0] == 0) return 0;
            for (int i = 0; i < net_dev_count(); i++){
                net_dev_t* d = net_get_dev(i);
                int k = 0;
                while (k < 15 && k < (int)len && d->name[k] && nm[k] == d->name[k]) k++;
                if ((k == (int)len || nm[k] == 0) && d->name[k] == 0) return 0;
            }
            return E_NODEV;
        }
        }
        if ((r = opt_int(val, len, &v))) return r;
        switch (name){
        case 1: return 0;                                                      /* SO_DEBUG */
        case 2: s->reuseaddr = v ? 1 : 0; return 0;                            /* SO_REUSEADDR */
        case 15: s->reuseport = v ? 1 : 0; return 0;                           /* SO_REUSEPORT */
        case 5: return 0;                                                      /* SO_DONTROUTE */
        case 6: s->broadcast = v ? 1 : 0; return 0;                            /* SO_BROADCAST */
        case 7: case 8: case 32: case 33: return 0;     /* SO_SNDBUF/RCVBUF(FORCE): fixed sizes */
        case 9: s->keepalive = v ? 1 : 0; return 0;                            /* SO_KEEPALIVE */
        case 10: s->oobinline = v ? 1 : 0; return 0;                           /* SO_OOBINLINE */
        case 11: return 0;                                                     /* SO_NO_CHECK */
        case 12: s->priority = (uint32_t)v; return 0;                          /* SO_PRIORITY */
        case 16: return 0;                                                     /* SO_PASSCRED */
        case 18: case 19: return 0;                                            /* SO_RCV/SNDLOWAT */
        case 29: s->timestamp = v ? 1 : 0; return 0;                           /* SO_TIMESTAMP */
        case 35: s->timestampns = v ? 1 : 0; return 0;                         /* SO_TIMESTAMPNS */
        case 36: if (!priv) return E_PERM; s->mark = (uint32_t)v; return 0;    /* SO_MARK */
        }
        return E_NOPROTOOPT;
    }
    if (level == SOL_IP_){
        if (name == 4) return (len == 0) ? 0 : E_INVAL;                        /* IP_OPTIONS: none */
        if (name == 35 || name == 36) return E_NOPROTOOPT;                     /* multicast groups */
        if (name == 32) return 0;                                              /* IP_MULTICAST_IF */
        if ((r = opt_int(val, len, &v))) return r;
        switch (name){
        case 1: s->tos = (uint8_t)v; return 0;                                 /* IP_TOS */
        case 2:                                                                /* IP_TTL */
            if (v == -1) v = 64;
            if (v < 1 || v > 255) return E_INVAL;
            s->ttl = (uint8_t)v; return 0;
        case 3: if (s->kind != SKIND_RAW) return E_NOPROTOOPT;                 /* IP_HDRINCL */
                s->hdrincl = (v || s->proto == IPPROTO_RAW) ? 1 : 0; return 0;
        case 6: case 7: return 0;                                              /* IP_RECVOPTS/RETOPTS */
        case 8: s->pktinfo = v ? 1 : 0; return 0;                              /* IP_PKTINFO (not delivered yet) */
        case 10: if (v < 0 || v > 5) return E_INVAL; s->mtu_disc = (uint8_t)v; return 0;   /* IP_MTU_DISCOVER */
        case 11: s->recverr = v ? 1 : 0;                                        /* IP_RECVERR */
                 if (!v) { s->errq_n = 0; s->errq_head = 0; }
                 return 0;
        case 12: s->recvttl = v ? 1 : 0; return 0;                             /* IP_RECVTTL */
        case 13: s->recvtos = v ? 1 : 0; return 0;                             /* IP_RECVTOS */
        case 33: if (v == -1) v = 1; if (v < 0 || v > 255) return E_INVAL;     /* IP_MULTICAST_TTL */
                 s->mcast_ttl = (uint8_t)v; return 0;
        case 34: s->mcast_loop = v ? 1 : 0; return 0;                          /* IP_MULTICAST_LOOP */
        }
        return E_NOPROTOOPT;
    }
    if (level == SOL_TCP_){
        if (s->kind != SKIND_TCP) return E_OPNOTSUPP;
        if (name == 13) return 0;                                              /* TCP_CONGESTION */
        if ((r = opt_int(val, len, &v))) return r;
        switch (name){
        case 1: s->nodelay = v ? 1 : 0; return 0;      /* TCP_NODELAY: segments always go out at once */
        case 2: return 0;                                                      /* TCP_MAXSEG */
        case 3: return 0;                                                      /* TCP_CORK */
        case 4: if (v < 1) return E_INVAL; s->keepidle = v; return 0;          /* TCP_KEEPIDLE */
        case 5: if (v < 1) return E_INVAL; s->keepintvl = v; return 0;         /* TCP_KEEPINTVL */
        case 6: if (v < 1) return E_INVAL; s->keepcnt = v; return 0;           /* TCP_KEEPCNT */
        case 9: case 10: case 12: return 0;            /* DEFER_ACCEPT, WINDOW_CLAMP, QUICKACK */
        case 18: if (v < 0) return E_INVAL; s->user_timeout = v; return 0;     /* TCP_USER_TIMEOUT */
        }
        /* TCP_FASTOPEN(_CONNECT) and the rest: not supported (musl's resolver
         * then falls back to connect + send). */
        return E_NOPROTOOPT;
    }
    if (level == SOL_RAW_ && name == 1){                                       /* ICMP_FILTER */
        if (s->kind != SKIND_RAW || s->proto != IPPROTO_ICMP) return E_OPNOTSUPP;
        if (len < 4) return E_INVAL;
        s->icmp_filter = *(const uint32_t*)val;
        return 0;
    }
    if (level == SOL_UDP_) return E_NOPROTOOPT;
    return E_NOPROTOOPT;
}

static int put_int(void* val, uint32_t* len, int v){
    if (*len >= sizeof(int)) { *(int*)val = v; *len = sizeof(int); return 0; }
    if (*len == 0) return E_INVAL;
    *(uint8_t*)val = (uint8_t)v; *len = 1;
    return 0;
}

static int l_getopt(int idx, int level, int name, void* val, uint32_t* len){
    struct sock* s = sk(idx);
    if (!s) return E_BADF;
    if (level == SOL_SOCKET_){
        switch (name){
        case 3: return put_int(val, len, s->kind == SKIND_TCP ? 1 : s->kind == SKIND_RAW ? 3 : 2); /* SO_TYPE */
        case 39: return put_int(val, len, 2);                                  /* SO_DOMAIN: AF_INET */
        case 38: return put_int(val, len, s->proto);                           /* SO_PROTOCOL */
        case 4: connect_settle(s); return put_int(val, len, err_take(s));      /* SO_ERROR */
        case 30: return put_int(val, len, s->listening);                       /* SO_ACCEPTCONN */
        case 2: return put_int(val, len, s->reuseaddr);
        case 15: return put_int(val, len, s->reuseport);
        case 6: return put_int(val, len, s->broadcast);
        case 9: return put_int(val, len, s->keepalive);
        case 10: return put_int(val, len, s->oobinline);
        case 12: return put_int(val, len, (int)s->priority);
        case 29: return put_int(val, len, s->timestamp);
        case 35: return put_int(val, len, s->timestampns);
        case 36: return put_int(val, len, (int)s->mark);
        case 8:  return put_int(val, len, s->kind == SKIND_TCP ? (int)tcp_rcvbuf_size() : SK_RCVBUF);
        case 7:  return put_int(val, len, s->kind == SKIND_TCP ? (int)tcp_sndbuf_size() : SK_SNDBUF);
        case 18: case 19: return put_int(val, len, 1);
        case 1: case 5: case 11: case 16: return put_int(val, len, 0);
        case 13:                                                               /* SO_LINGER */
            if (*len < 8) return E_INVAL;
            ((int*)val)[0] = s->linger_on; ((int*)val)[1] = s->linger_s; *len = 8; return 0;
        case 20: case 21: {                                                    /* SO_RCV/SNDTIMEO */
            if (*len < sizeof(struct k_timeval)) return E_INVAL;
            uint64_t ns = name == 20 ? s->rcvtimeo_ns : s->sndtimeo_ns;
            struct k_timeval* t = (struct k_timeval*)val;
            t->sec = (int64_t)(ns / 1000000000ull); t->usec = (int64_t)((ns % 1000000000ull) / 1000ull);
            *len = sizeof(*t); return 0;
        }
        }
        return E_NOPROTOOPT;
    }
    if (level == SOL_IP_){
        switch (name){
        case 1: return put_int(val, len, s->tos);
        case 2: return put_int(val, len, s->ttl);
        case 3: return put_int(val, len, s->hdrincl);
        case 8: return put_int(val, len, s->pktinfo);
        case 10: return put_int(val, len, s->mtu_disc);
        case 11: return put_int(val, len, s->recverr);
        case 12: return put_int(val, len, s->recvttl);
        case 13: return put_int(val, len, s->recvtos);
        case 14:                                                               /* IP_MTU */
            if (!s->peer_ip && !(s->tcp && !s->listening)) return E_NOTCONN;
            return put_int(val, len, NET_MTU);
        case 33: return put_int(val, len, s->mcast_ttl);
        case 34: return put_int(val, len, s->mcast_loop);
        case 4: *len = 0; return 0;                                            /* IP_OPTIONS */
        }
        return E_NOPROTOOPT;
    }
    if (level == SOL_TCP_){
        if (s->kind != SKIND_TCP) return E_OPNOTSUPP;
        switch (name){
        case 1: return put_int(val, len, s->nodelay);
        case 2: return put_int(val, len, (int)tcp_mss());
        case 3: return put_int(val, len, 0);
        case 4: return put_int(val, len, s->keepidle ? s->keepidle : 7200);
        case 5: return put_int(val, len, s->keepintvl ? s->keepintvl : 75);
        case 6: return put_int(val, len, s->keepcnt ? s->keepcnt : 9);
        case 18: return put_int(val, len, s->user_timeout);
        }
        return E_NOPROTOOPT;
    }
    if (level == SOL_RAW_ && name == 1){
        if (s->kind != SKIND_RAW || s->proto != IPPROTO_ICMP) return E_OPNOTSUPP;
        if (*len < 4) return E_INVAL;
        *(uint32_t*)val = s->icmp_filter; *len = 4; return 0;
    }
    return E_NOPROTOOPT;
}

/* ----------------------------------------------------------- RX hooks */
/* Every received IPv4 packet: a copy to each raw socket of its protocol. */
void sock_raw_input(net_dev_t* dev, const uint8_t* ip, uint32_t len){
    (void)dev;
    NET_ASSERT_STACK_LOCKED();
    if (!g_nraw || len < 20) return;
    uint8_t proto = ip[9];
    uint32_t ihl = (uint32_t)(ip[0] & 0x0F) * 4;
    uint32_t src = (uint32_t)ip[12] | ((uint32_t)ip[13]<<8) | ((uint32_t)ip[14]<<16) | ((uint32_t)ip[15]<<24);
    uint32_t dst = (uint32_t)ip[16] | ((uint32_t)ip[17]<<8) | ((uint32_t)ip[18]<<16) | ((uint32_t)ip[19]<<24);
    for (int i = 0; i < MAX_SOCK; i++){
        struct sock* s = &g_sock[i];
        if (!s->used || s->kind != SKIND_RAW || s->proto != proto || (s->shut & 1)) continue;
        if (s->bound_ip && s->bound_ip != dst) continue;
        if (s->peer_ip && s->peer_ip != src) continue;
        if (proto == IPPROTO_ICMP && len > ihl){
            uint8_t t = ip[ihl];
            if (t < 32 && (s->icmp_filter & (1u << t))) continue;
        }
        dq_push(s, src, 0, ip, len);
    }
}

/* An echo reply: to the ping socket owning its identifier. 1 = delivered. */
int sock_ping_reply(uint32_t src, const uint8_t* icmp, uint32_t len){
    NET_ASSERT_STACK_LOCKED();
    if (!g_nping || len < 8) return 0;
    uint16_t id = (uint16_t)((icmp[4] << 8) | icmp[5]);
    for (int i = 0; i < MAX_SOCK; i++){
        struct sock* s = &g_sock[i];
        if (!s->used || s->kind != SKIND_PING || !s->bound || s->port != id) continue;
        if (s->peer_ip && s->peer_ip != src) return 1;
        if (s->shut & 1) return 1;
        dq_push(s, src, 0, icmp, len);
        return 1;
    }
    return 0;
}

/* Linux icmp_err_convert: errno + "hard error" for an ICMP error type/code. */
static int icmp_errno(uint8_t type, uint8_t code, int* hard){
    *hard = 0;
    if (type == 11) return 113;                                /* time exceeded: EHOSTUNREACH */
    if (type == 12) { *hard = 1; return 71; }                  /* parameter problem: EPROTO */
    if (type != 3) return 0;
    static const struct { uint8_t e; uint8_t hard; } tab[16] = {
        {101,0},{113,0},{92,1},{111,1},{90,0},{95,0},{101,1},{112,1},
        {64,1},{101,1},{113,1},{101,0},{113,0},{113,1},{113,1},{113,1},
    };
    if (code > 15) return 113;
    *hard = tab[code].hard;
    return tab[code].e;
}

static void errq_push(struct sock* s, int err, uint8_t type, uint8_t code, uint32_t info,
                      uint32_t offender, uint32_t dip, uint16_t dport, const uint8_t* orig, uint32_t olen){
    struct sk_err* e;
    if (s->errq_n == SK_ERRQ){ s->errq_head = (s->errq_head + 1) % SK_ERRQ; s->errq_n--; }  /* drop oldest */
    e = &s->errq[(s->errq_head + s->errq_n) % SK_ERRQ];
    e->err = (uint32_t)err; e->type = type; e->code = code; e->info = info;
    e->offender = offender; e->dst_ip = dip; e->dst_port = dport;
    if (olen > SK_ERRDATA) olen = SK_ERRDATA;
    e->dlen = (uint16_t)olen;
    bcopy_(e->data, orig, olen);
    s->errq_n++;
}

/* An ICMP error (type 3/11/12) from `from`, quoting one of our packets. */
void sock_icmp_error(uint32_t from, const uint8_t* icmp, uint32_t len){
    NET_ASSERT_STACK_LOCKED();
    if (len < 8 + 20) return;
    uint8_t type = icmp[0], code = icmp[1];
    uint32_t info = 0;
    if (type == 3 && code == 4) info = (uint32_t)((icmp[6] << 8) | icmp[7]);   /* next-hop MTU */
    if (type == 12) info = icmp[4];                                            /* pointer */
    const uint8_t* in = icmp + 8;                     /* the quoted IP header */
    uint32_t ihl = (uint32_t)(in[0] & 0x0F) * 4;
    if ((in[0] >> 4) != 4 || ihl < 20 || 8 + ihl > len) return;
    uint8_t proto = in[9];
    uint32_t dip = (uint32_t)in[16] | ((uint32_t)in[17]<<8) | ((uint32_t)in[18]<<16) | ((uint32_t)in[19]<<24);
    const uint8_t* tp = in + ihl;                     /* the quoted transport header */
    uint32_t tlen = len - 8 - ihl;
    int hard; int err = icmp_errno(type, code, &hard);
    if (!err) return;
    for (int i = 0; i < MAX_SOCK; i++){
        struct sock* s = &g_sock[i];
        if (!s->used) continue;
        int match = 0; uint16_t dport = 0;
        if (s->kind == SKIND_UDP && proto == IPPROTO_UDP && tlen >= 4 && s->udp_bound){
            uint16_t sp = (uint16_t)((tp[0] << 8) | tp[1]);
            dport = (uint16_t)((tp[2] << 8) | tp[3]);
            match = (sp == s->port) && (!s->peer_ip || (s->peer_ip == dip && s->peer_port == dport));
        } else if (s->kind == SKIND_PING && proto == IPPROTO_ICMP && tlen >= 8 && s->bound){
            uint16_t id = (uint16_t)((tp[4] << 8) | tp[5]);
            match = tp[0] == 8 && id == s->port && (!s->peer_ip || s->peer_ip == dip);
        }
        if (!match) continue;
        /* Linux: with IP_RECVERR every error is reported (queue + sk_err);
         * without it only a hard error on a connected socket is (sk_err). */
        if (s->recverr){
            errq_push(s, err, type, code, info, from, dip, dport, tp, tlen);
            err_set(s, err);
        } else if (s->peer_ip && hard){
            err_set(s, err);
        }
        return;
    }
}

/* ------------------------------------------------------- misc queries */
static int l_nread(int idx){
    struct sock* s = sk(idx);
    if (!s) return E_BADF;
    if (s->kind == SKIND_TCP) return (s->tcp && !s->listening) ? tcp_rx_available(s->tcp) : 0;
    return s->dq_head ? s->dq_head->len : 0;
}

/* ---- [M47] public entry points: each takes the stack lock for the call ---- */
int socket_create(int type){
    uint64_t fl = net_lock(); int r = l_create(type); net_unlock(fl); return r;
}
void socket_ref(int idx){
    uint64_t fl = net_lock(); l_ref(idx); net_unlock(fl);
}
void socket_unref(int idx){
    uint64_t fl = net_lock(); l_unref(idx); net_unlock(fl);
}
int socket_bind(int idx, uint16_t port){
    uint64_t fl = net_lock(); int r = l_bind_ex(idx, 0, port, 1); net_unlock(fl); return r;
}
int socket_connect(int idx, uint32_t ip, uint16_t port){
    uint64_t fl = net_lock(); int r = l_connect_ex(idx, ip, port, 0); net_unlock(fl); return r;
}
int socket_listen(int idx, int backlog){
    uint64_t fl = net_lock(); int r = l_listen(idx, backlog); net_unlock(fl); return r;
}
int socket_accept(int idx){
    uint64_t fl = net_lock(); int r = l_accept_ex(idx, 0, 0); net_unlock(fl); return r;
}
int socket_send(int idx, const void* buf, int len){
    uint64_t fl = net_lock(); int r = l_send(idx, buf, len); net_unlock(fl); return r;
}
int socket_sendto(int idx, const void* buf, int len, uint32_t ip, uint16_t port){
    uint64_t fl = net_lock(); int r = l_sendto(idx, buf, len, ip, port); net_unlock(fl); return r;
}
int socket_recv(int idx, void* buf, int len, uint32_t* src_ip, uint16_t* src_port){
    uint64_t fl = net_lock(); int r = l_recv(idx, buf, len, src_ip, src_port); net_unlock(fl); return r;
}
int socket_poll(uint32_t pid, int idx){
    uint64_t fl = net_lock(); int r = l_poll(pid, idx); net_unlock(fl); return r;
}
int socket_name(int idx, int peer, uint32_t* ip, uint16_t* port){
    uint64_t fl = net_lock(); int r = l_name(idx, peer, ip, port); net_unlock(fl); return r;
}

/* ---- [U10] ---- */
int socket_create_ex(int kind, int proto){
    uint64_t fl = net_lock(); int r = l_create_kind(kind, proto); net_unlock(fl); return r;
}
int socket_kind(int idx, int* proto){
    uint64_t fl = net_lock();
    struct sock* s = sk(idx);
    int r = s ? s->kind : E_BADF;
    if (s && proto) *proto = s->proto;
    net_unlock(fl);
    return r;
}
int socket_bind_ex(int idx, uint32_t ip, uint16_t port){
    uint64_t fl = net_lock(); int r = l_bind_ex(idx, ip, port, 0); net_unlock(fl); return r;
}
int socket_connect_ex(int idx, uint32_t ip, uint16_t port, int unspec){
    uint64_t fl = net_lock(); int r = l_connect_ex(idx, ip, port, unspec); net_unlock(fl); return r;
}
int socket_accept_ex(int idx, uint32_t* ip, uint16_t* port){
    uint64_t fl = net_lock(); int r = l_accept_ex(idx, ip, port); net_unlock(fl); return r;
}
int socket_recvmsg(int idx, void* buf, int len, int flags, sock_rxmeta_t* m){
    uint64_t fl = net_lock(); int r = l_recvmsg(idx, buf, len, flags, m); net_unlock(fl); return r;
}
int socket_sendmsg(int idx, const void* buf, int len, int flags, int has_dst, uint32_t ip, uint16_t port){
    uint64_t fl = net_lock(); int r = l_sendmsg(idx, buf, len, flags, has_dst, ip, port); net_unlock(fl); return r;
}
int socket_setopt(int idx, int level, int name, const void* val, uint32_t len, int priv){
    uint64_t fl = net_lock(); int r = l_setopt(idx, level, name, val, len, priv); net_unlock(fl); return r;
}
int socket_getopt(int idx, int level, int name, void* val, uint32_t* len){
    uint64_t fl = net_lock(); int r = l_getopt(idx, level, name, val, len); net_unlock(fl); return r;
}
int socket_shutdown(int idx, int how){
    uint64_t fl = net_lock(); int r = l_shutdown(idx, how); net_unlock(fl); return r;
}
int socket_nread(int idx){
    uint64_t fl = net_lock(); int r = l_nread(idx); net_unlock(fl); return r;
}
uint64_t socket_timeo(int idx, int send){
    uint64_t fl = net_lock();
    struct sock* s = sk(idx);
    uint64_t r = s ? (send ? s->sndtimeo_ns : s->rcvtimeo_ns) : 0;
    net_unlock(fl);
    return r;
}
int socket_pending_error(int idx){
    uint64_t fl = net_lock();
    struct sock* s = sk(idx);
    int r = s ? s->so_error : 0;
    net_unlock(fl);
    return r;
}
