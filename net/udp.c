/*
 * udp.c — [M24] UDP transport (RFC 768).
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * A port-binding table over IPv4. udp_input() (called by ipv4_input via the weak
 * hook) demuxes by destination port to a bound callback; udp_send() prepends the
 * 8-byte header, computes the pseudo-header checksum, and hands the segment to
 * ipv4_send(). The stack is single-threaded (poll/shell context), so the TX
 * scratch and binding table need no locking.
 */
#include "udp.h"
#include <stddef.h>

#define IP_PROTO_UDP 17
#define UDP_MAX_BIND 16

struct udp_binding { uint16_t port; udp_recv_cb cb; void* ctx; int used; };
static struct udp_binding g_bind[UDP_MAX_BIND];
static uint16_t g_ephemeral = 49152;          /* IANA dynamic range start */

/* Pseudo-header + segment scratch: checksum covers [pseudo(12)|segment], only the
 * segment (offset 12) is transmitted. */
static uint8_t g_csbuf[12 + NET_FRAME_MAX];

int udp_bind(uint16_t port, udp_recv_cb cb, void* ctx) {
    for (int i = 0; i < UDP_MAX_BIND; i++)
        if (g_bind[i].used && g_bind[i].port == port) return -2;
    for (int i = 0; i < UDP_MAX_BIND; i++)
        if (!g_bind[i].used) {
            g_bind[i].port = port; g_bind[i].cb = cb; g_bind[i].ctx = ctx; g_bind[i].used = 1;
            return 0;
        }
    return -1;
}

void udp_unbind(uint16_t port) {
    for (int i = 0; i < UDP_MAX_BIND; i++)
        if (g_bind[i].used && g_bind[i].port == port) g_bind[i].used = 0;
}

uint16_t udp_ephemeral_port(void) {
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = g_ephemeral++;
        if (g_ephemeral == 0) g_ephemeral = 49152;
        int taken = 0;
        for (int i = 0; i < UDP_MAX_BIND; i++)
            if (g_bind[i].used && g_bind[i].port == p) { taken = 1; break; }
        if (!taken) return p;
    }
    return 0;
}

// ipv4_input -> udp_input (weak hook): demux to the bound port. 'seg' is the UDP
// datagram (header + payload); 'len' the IP payload length.
void udp_input(net_dev_t* dev, uint32_t src_ip, const uint8_t* seg, uint32_t len) {
    if (len < 8) return;
    uint16_t src_port = (uint16_t)((seg[0] << 8) | seg[1]);
    uint16_t dst_port = (uint16_t)((seg[2] << 8) | seg[3]);
    uint16_t ulen     = (uint16_t)((seg[4] << 8) | seg[5]);
    if (ulen < 8 || ulen > len) return;          /* truncated/oversized */
    const uint8_t* data = seg + 8;
    uint32_t dlen = (uint32_t)ulen - 8;
    for (int i = 0; i < UDP_MAX_BIND; i++)
        if (g_bind[i].used && g_bind[i].port == dst_port && g_bind[i].cb) {
            g_bind[i].cb(g_bind[i].ctx, dev, src_ip, src_port, data, dlen);
            return;
        }
    /* no listener: silently drop (ICMP port-unreachable is optional) */
}

int udp_send(net_dev_t* dev, uint32_t dst_ip, uint16_t src_port,
             uint16_t dst_port, const void* data, uint32_t len) {
    if (!dev) return -1;
    if (len > NET_FRAME_MAX - 12 - 8) return -1;
    uint16_t ulen = (uint16_t)(8 + len);

    uint8_t* seg = g_csbuf + 12;                 /* segment after the pseudo-header */
    seg[0] = (uint8_t)(src_port >> 8); seg[1] = (uint8_t)src_port;
    seg[2] = (uint8_t)(dst_port >> 8); seg[3] = (uint8_t)dst_port;
    seg[4] = (uint8_t)(ulen >> 8);     seg[5] = (uint8_t)ulen;
    seg[6] = 0; seg[7] = 0;                       /* checksum (computed below) */
    const uint8_t* p = (const uint8_t*)data;
    for (uint32_t i = 0; i < len; i++) seg[8 + i] = p[i];

    /* Pseudo-header: src(4) dst(4) zero(1) proto(1) udplen(2). IPs are already
     * the network-order byte sequence in memory (octet0 at the low address). */
    uint8_t* ph = g_csbuf;
    ph[0]=(uint8_t)dev->ip;     ph[1]=(uint8_t)(dev->ip>>8);
    ph[2]=(uint8_t)(dev->ip>>16);ph[3]=(uint8_t)(dev->ip>>24);
    ph[4]=(uint8_t)dst_ip;      ph[5]=(uint8_t)(dst_ip>>8);
    ph[6]=(uint8_t)(dst_ip>>16);ph[7]=(uint8_t)(dst_ip>>24);
    ph[8]=0; ph[9]=IP_PROTO_UDP;
    ph[10]=(uint8_t)(ulen>>8); ph[11]=(uint8_t)ulen;

    uint16_t csum = net_checksum(g_csbuf, 12u + ulen);
    if (csum == 0) csum = 0xFFFF;                 /* 0 means "no checksum" on the wire */
    seg[6] = (uint8_t)(csum >> 8); seg[7] = (uint8_t)csum;

    return ipv4_send(dev, dst_ip, IP_PROTO_UDP, seg, ulen);
}
