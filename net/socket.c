/*
 * socket.c — [M24] BSD-style socket layer over TCP/UDP.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * A small kernel-owned socket table. Socket descriptors are a namespace separate
 * from the per-process file fds (a program knows which is which). Each TCP socket
 * wraps a tcp_conn_t; each UDP socket binds a port whose callback enqueues
 * datagrams into a per-socket ring. The blocking calls (connect/accept/recv) spin
 * on `hlt` so the timer tick drives net_tick()->poll for RX — the same idiom as
 * the ping self-test, valid because the timer ISR runs net_tick even while a
 * ring-0 syscall is in progress. CAP_NET (signed manifest) is enforced by the
 * syscall dispatcher before any of these run.
 */
#include "tcp.h"
#include "udp.h"
#include "net.h"
#include "debugcon.h"
#include <stddef.h>

extern uint64_t timer_get_ticks(void);

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define MAX_SOCK    16
#define SOCK_DGRAM_MAX 1024
#define SOCK_DQ     6

struct dgram { uint32_t ip; uint16_t port; uint16_t len; uint8_t data[SOCK_DGRAM_MAX]; };

struct sock {
    int      used;
    int      type;            /* SOCK_STREAM / SOCK_DGRAM */
    uint32_t owner;           /* pid */
    /* TCP */
    tcp_conn_t* tcp;
    int      listening;
    /* UDP */
    uint16_t udp_port;
    int      udp_bound;
    struct dgram dq[SOCK_DQ];
    int      dq_head, dq_count;
    uint32_t peer_ip;         /* connected default peer (UDP) */
    uint16_t peer_port;
};

static struct sock g_sock[MAX_SOCK];

static struct sock* sk(int fd){
    if (fd < 0 || fd >= MAX_SOCK || !g_sock[fd].used) return NULL;
    return &g_sock[fd];
}

// UDP receive callback: enqueue the datagram (drop if the ring is full).
static void sock_udp_cb(void* ctx, net_dev_t* dev, uint32_t src_ip,
                        uint16_t src_port, const uint8_t* data, uint32_t len){
    (void)dev;
    struct sock* s = (struct sock*)ctx;
    if (s->dq_count >= SOCK_DQ) return;
    if (len > SOCK_DGRAM_MAX) len = SOCK_DGRAM_MAX;
    int slot = (s->dq_head + s->dq_count) % SOCK_DQ;
    s->dq[slot].ip = src_ip; s->dq[slot].port = src_port; s->dq[slot].len = (uint16_t)len;
    for (uint32_t i = 0; i < len; i++) s->dq[slot].data[i] = data[i];
    s->dq_count++;
}

int socket_create(uint32_t pid, int type){
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -1;
    for (int i = 0; i < MAX_SOCK; i++) if (!g_sock[i].used){
        struct sock* s = &g_sock[i];
        for (size_t k = 0; k < sizeof(*s); k++) ((uint8_t*)s)[k] = 0;
        s->used = 1; s->type = type; s->owner = pid;
        return i;
    }
    return -1;
}

static void sock_release(struct sock* s){
    if (s->type == SOCK_DGRAM && s->udp_bound) udp_unbind(s->udp_port);
    if (s->type == SOCK_STREAM && s->tcp){ tcp_close(s->tcp); s->tcp = NULL; }
    s->used = 0;
}

int socket_close(uint32_t pid, int fd){
    struct sock* s = sk(fd);
    if (!s || s->owner != pid) return -1;
    sock_release(s);
    return 0;
}

void socket_owner_cleanup(uint32_t pid){
    for (int i = 0; i < MAX_SOCK; i++)
        if (g_sock[i].used && g_sock[i].owner == pid) sock_release(&g_sock[i]);
}

int socket_bind(uint32_t pid, int fd, uint16_t port){
    struct sock* s = sk(fd);
    if (!s || s->owner != pid) return -1;
    if (s->type == SOCK_DGRAM){
        if (s->udp_bound) udp_unbind(s->udp_port);
        if (udp_bind(port, sock_udp_cb, s) != 0) return -1;
        s->udp_port = port; s->udp_bound = 1;
        return 0;
    }
    /* TCP bind is folded into listen() for simplicity. */
    s->udp_port = port;
    return 0;
}

int socket_connect(uint32_t pid, int fd, uint32_t ip, uint16_t port){
    struct sock* s = sk(fd);
    if (!s || s->owner != pid) return -1;
    net_dev_t* dev = net_primary();
    if (!dev) return -1;
    if (s->type == SOCK_DGRAM){
        s->peer_ip = ip; s->peer_port = port;
        if (!s->udp_bound){                         /* auto-bind an ephemeral port */
            uint16_t ep = udp_ephemeral_port();
            if (udp_bind(ep, sock_udp_cb, s) != 0) return -1;
            s->udp_port = ep; s->udp_bound = 1;
        }
        return 0;
    }
    s->tcp = tcp_connect(dev, ip, port);            /* blocking handshake */
    return s->tcp ? 0 : -1;
}

int socket_listen(uint32_t pid, int fd, int backlog){
    (void)backlog;
    struct sock* s = sk(fd);
    if (!s || s->owner != pid || s->type != SOCK_STREAM) return -1;
    net_dev_t* dev = net_primary();
    if (!dev) return -1;
    s->tcp = tcp_alloc();
    if (!s->tcp) return -1;
    if (tcp_start_listen(s->tcp, dev, s->udp_port) != 0) return -1;
    s->listening = 1;
    return 0;
}

int socket_accept(uint32_t pid, int fd){
    struct sock* s = sk(fd);
    if (!s || s->owner != pid || !s->listening) return -1;
    uint64_t deadline = timer_get_ticks() + 30000;  /* up to ~30 s */
    tcp_conn_t* ch = NULL;
    while (timer_get_ticks() < deadline){
        ch = tcp_accept_ready(s->tcp);
        if (ch) break;
        __asm__ volatile ("sti; hlt");
    }
    if (!ch) return -1;
    int nfd = socket_create(pid, SOCK_STREAM);
    if (nfd < 0){ tcp_close(ch); return -1; }
    g_sock[nfd].tcp = ch;
    return nfd;
}

int socket_send(uint32_t pid, int fd, const void* buf, int len){
    struct sock* s = sk(fd);
    if (!s || s->owner != pid || len < 0) return -1;
    net_dev_t* dev = net_primary();
    if (!dev) return -1;
    if (s->type == SOCK_STREAM){
        if (!s->tcp) return -1;
        return tcp_send_all(s->tcp, buf, (uint32_t)len) == 0 ? len : -1;
    }
    /* UDP: send to the connected peer. */
    if (!s->peer_ip) return -1;
    if (!s->udp_bound){
        uint16_t ep = udp_ephemeral_port();
        if (udp_bind(ep, sock_udp_cb, s) != 0) return -1;
        s->udp_port = ep; s->udp_bound = 1;
    }
    return udp_send(dev, s->peer_ip, s->udp_port, s->peer_port, buf, (uint32_t)len) == 0 ? len : -1;
}

int socket_sendto(uint32_t pid, int fd, const void* buf, int len, uint32_t ip, uint16_t port){
    struct sock* s = sk(fd);
    if (!s || s->owner != pid || s->type != SOCK_DGRAM || len < 0) return -1;
    net_dev_t* dev = net_primary();
    if (!dev) return -1;
    if (!s->udp_bound){
        uint16_t ep = udp_ephemeral_port();
        if (udp_bind(ep, sock_udp_cb, s) != 0) return -1;
        s->udp_port = ep; s->udp_bound = 1;
    }
    return udp_send(dev, ip, s->udp_port, port, buf, (uint32_t)len) == 0 ? len : -1;
}

// Blocking receive (bounded). For TCP returns stream bytes (0 = EOF); for UDP
// returns one datagram and fills *src_ip/*src_port. Returns bytes, 0 = none/EOF.
int socket_recv(uint32_t pid, int fd, void* buf, int len, uint32_t* src_ip, uint16_t* src_port){
    struct sock* s = sk(fd);
    if (!s || s->owner != pid || len <= 0) return -1;
    uint64_t deadline = timer_get_ticks() + 30000;
    if (s->type == SOCK_STREAM){
        if (!s->tcp) return -1;
        while (timer_get_ticks() < deadline){
            int n = tcp_read(s->tcp, buf, (uint32_t)len);
            if (n > 0) return n;
            if (n < 0) return 0;                     /* peer closed, drained = EOF */
            __asm__ volatile ("sti; hlt");
        }
        return 0;
    }
    /* UDP datagram. */
    while (timer_get_ticks() < deadline){
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
        __asm__ volatile ("sti; hlt");
    }
    return 0;
}
