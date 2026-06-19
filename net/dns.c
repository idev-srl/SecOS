/*
 * dns.c — [M24] minimal DNS A-record client (RFC 1035) over UDP 53.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Sends a single recursive A query to dev->dns and parses the first A answer.
 * Synchronous from the shell/idle context (same hlt-driven RX as DHCP). No
 * caching, no CNAME chasing beyond what the resolver flattens, IPv4 only.
 */
#include "udp.h"
#include "debugcon.h"
#include <stddef.h>

extern uint64_t timer_get_ticks(void);

struct dns_state {
    uint16_t id;
    volatile int got;
    uint32_t ip;          /* resolved A record, network-order octets, 0 = none */
};

static uint8_t g_query[300];

static uint32_t rd32be(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

// Advance past a (possibly compressed) DNS name starting at off. Returns the
// offset just after the name, or 0 on malformed input.
static uint32_t name_skip(const uint8_t* msg, uint32_t len, uint32_t off) {
    while (off < len) {
        uint8_t b = msg[off];
        if ((b & 0xC0) == 0xC0) return off + 2;       /* compression pointer ends the name */
        if (b == 0) return off + 1;                   /* root label */
        off += 1u + b;                                /* label */
    }
    return 0;
}

static void dns_cb(void* ctx, net_dev_t* dev, uint32_t src_ip,
                   uint16_t src_port, const uint8_t* data, uint32_t len) {
    (void)dev; (void)src_ip; (void)src_port;
    struct dns_state* st = (struct dns_state*)ctx;
    if (len < 12) return;
    uint16_t id = (uint16_t)((data[0]<<8) | data[1]);
    if (id != st->id) return;
    uint16_t flags = (uint16_t)((data[2]<<8) | data[3]);
    if (!(flags & 0x8000)) return;                    /* not a response */
    uint16_t qd = (uint16_t)((data[4]<<8) | data[5]);
    uint16_t an = (uint16_t)((data[6]<<8) | data[7]);

    uint32_t off = 12;
    for (uint16_t q = 0; q < qd; q++) {               /* skip questions */
        off = name_skip(data, len, off);
        if (!off || off + 4 > len) { st->got = 1; return; }
        off += 4;                                     /* QTYPE + QCLASS */
    }
    for (uint16_t a = 0; a < an; a++) {
        off = name_skip(data, len, off);
        if (!off || off + 10 > len) break;
        uint16_t type = (uint16_t)((data[off]<<8) | data[off+1]);
        uint16_t cls  = (uint16_t)((data[off+2]<<8) | data[off+3]);
        uint16_t rdl  = (uint16_t)((data[off+8]<<8) | data[off+9]);
        uint32_t rdata = off + 10;
        if (rdata + rdl > len) break;
        if (type == 1 && cls == 1 && rdl == 4) {      /* A record, IN class */
            st->ip = rd32be(data + rdata);
            st->got = 1;
            return;
        }
        off = rdata + rdl;
    }
    st->got = 1;                                      /* response seen, no A record */
}

// Encode "example.com" -> 7example3com0 into out. Returns bytes written, 0 err.
static uint32_t encode_qname(const char* name, uint8_t* out, uint32_t cap) {
    uint32_t w = 0;
    const char* p = name;
    while (*p) {
        const char* start = p;
        while (*p && *p != '.') p++;
        uint32_t l = (uint32_t)(p - start);
        if (l == 0 || l > 63 || w + 1 + l >= cap) return 0;
        out[w++] = (uint8_t)l;
        for (uint32_t i = 0; i < l; i++) out[w++] = (uint8_t)start[i];
        if (*p == '.') p++;
    }
    if (w + 1 > cap) return 0;
    out[w++] = 0;                                     /* root */
    return w;
}

int dns_resolve(net_dev_t* dev, const char* name, uint32_t* out_ip) {
    if (!dev || !name || !out_ip || !dev->dns) return -1;
    struct dns_state st;
    st.id = (uint16_t)(timer_get_ticks() ^ 0x5EC0); st.got = 0; st.ip = 0;

    uint8_t* q = g_query;
    q[0]=(uint8_t)(st.id>>8); q[1]=(uint8_t)st.id;
    q[2]=0x01; q[3]=0x00;                             /* RD */
    q[4]=0; q[5]=1;                                   /* QDCOUNT=1 */
    q[6]=0; q[7]=0; q[8]=0; q[9]=0; q[10]=0; q[11]=0;
    uint32_t n = encode_qname(name, q + 12, sizeof(g_query) - 12 - 4);
    if (!n) return -1;
    uint32_t o = 12 + n;
    q[o++]=0; q[o++]=1;                               /* QTYPE = A */
    q[o++]=0; q[o++]=1;                               /* QCLASS = IN */

    uint16_t port = udp_ephemeral_port();
    if (udp_bind(port, dns_cb, &st) != 0) return -1;

    int resolved = 0;
    for (int attempt = 0; attempt < 3 && !resolved; attempt++) {
        st.got = 0;
        udp_send(dev, dev->dns, port, 53, q, o);
        uint64_t deadline = timer_get_ticks() + 1500;
        while (timer_get_ticks() < deadline && !st.got)
            __asm__ volatile ("sti; hlt");
        if (st.got && st.ip) resolved = 1;
    }
    udp_unbind(port);

    if (!resolved) { debugcon_writestring("[DNS] no answer\n"); return -1; }
    *out_ip = st.ip;
    return 0;
}
