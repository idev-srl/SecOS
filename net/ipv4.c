/*
 * ipv4.c — [M24] IPv4 input/output (no fragmentation).
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [U10] Additions for the socket layer:
 *  - ipv4_send_ex(): per-socket TTL and TOS, and optionally queue the packet
 *    while ARP resolves the next hop (a datagram socket's first sendto to a
 *    new neighbour no longer vanishes; TCP keeps its own SYN retry);
 *  - ipv4_output_hdr(): send a packet whose IP header the caller built
 *    (SOCK_RAW with IP_HDRINCL); the header is checked and completed here;
 *  - the received packet's TTL/TOS (ipv4_rx_ttl/tos, for IP_RECVTTL) and a
 *    copy of every packet to the raw sockets of its protocol (sock_raw_input).
 */
#include "net.h"
#include "heap.h"
#include <stddef.h>

#define ETH_TYPE_IPV4 0x0800
#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define IP_PROTO_TCP  6

static uint16_t g_ip_id = 1;                 /* [M47] both guarded by the stack lock */
static uint8_t  g_ipbuf[NET_FRAME_MAX];

/* [U10] Metadata of the packet being demultiplexed (stack lock held). */
static uint8_t g_rx_ttl, g_rx_tos;
uint8_t ipv4_rx_ttl(void) { return g_rx_ttl; }
uint8_t ipv4_rx_tos(void) { return g_rx_tos; }

extern void udp_input(net_dev_t* dev, uint32_t src_ip, const uint8_t* seg, uint32_t len) __attribute__((weak));
extern void tcp_input(net_dev_t* dev, uint32_t src_ip, const uint8_t* seg, uint32_t len) __attribute__((weak));
extern void sock_raw_input(net_dev_t* dev, const uint8_t* ip, uint32_t len) __attribute__((weak));

void ipv4_input(net_dev_t* dev, const uint8_t* frame, uint32_t len, const uint8_t src_mac[6]) {
    (void)src_mac;
    if (len < 14 + 20) return;
    const uint8_t* ip = frame + 14;
    if ((ip[0] >> 4) != 4) return;
    uint8_t ihl = (uint8_t)((ip[0] & 0x0F) * 4);
    if (ihl < 20) return;
    uint16_t tot = (uint16_t)((ip[2] << 8) | ip[3]);
    if (tot < ihl || 14u + tot > len) return;
    if (net_checksum(ip, ihl) != 0) return;                        /* [U10] bad header */
    if (((ip[6] & 0x3F) | ip[7]) != 0) return;                     /* a fragment: no reassembly */
    uint8_t proto = ip[9];
    uint32_t dst = (uint32_t)ip[16] | ((uint32_t)ip[17]<<8) | ((uint32_t)ip[18]<<16) | ((uint32_t)ip[19]<<24);
    uint32_t src = (uint32_t)ip[12] | ((uint32_t)ip[13]<<8) | ((uint32_t)ip[14]<<16) | ((uint32_t)ip[15]<<24);
    if (dev->ip && dst != dev->ip && dst != 0xFFFFFFFFu) return;   // not for us
    g_rx_ttl = ip[8]; g_rx_tos = ip[1];
    if (sock_raw_input) sock_raw_input(dev, ip, tot);               /* [U10] SOCK_RAW copies */
    const uint8_t* payload = ip + ihl;
    uint32_t plen = (uint32_t)tot - ihl;
    if (proto == IP_PROTO_ICMP)      icmp_input(dev, src, payload, plen);
    else if (proto == IP_PROTO_UDP && udp_input) udp_input(dev, src, payload, plen);
    else if (proto == IP_PROTO_TCP && tcp_input) tcp_input(dev, src, payload, plen);
}

/* ---- [U10] packets waiting for ARP ------------------------------------- */
#define ARPQ_MAX  16
#define ARPQ_MS   3000                /* drop what ARP did not resolve by then */
struct arpq_ent { uint32_t nexthop; uint64_t deadline; net_dev_t* dev; uint32_t len; uint8_t* pkt; };
static struct arpq_ent g_arpq[ARPQ_MAX];
extern uint64_t timer_get_ticks(void);

static int arpq_add(net_dev_t* dev, uint32_t nexthop, const uint8_t* pkt, uint32_t len) {
    int slot = -1;
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < ARPQ_MAX; i++) {
        if (g_arpq[i].pkt && now >= g_arpq[i].deadline) { kfree(g_arpq[i].pkt); g_arpq[i].pkt = NULL; }
        if (!g_arpq[i].pkt && slot < 0) slot = i;
    }
    if (slot < 0) return -1;
    uint8_t* copy = (uint8_t*)kmalloc(len);
    if (!copy) return -1;
    for (uint32_t i = 0; i < len; i++) copy[i] = pkt[i];
    g_arpq[slot].pkt = copy; g_arpq[slot].len = len; g_arpq[slot].dev = dev;
    g_arpq[slot].nexthop = nexthop; g_arpq[slot].deadline = now + ARPQ_MS;
    return 0;
}

/* arp_input learned nexthop's MAC: send what waited for it. */
void ipv4_arp_resolved(net_dev_t* dev, uint32_t nexthop, const uint8_t mac[6]) {
    NET_ASSERT_STACK_LOCKED();
    for (int i = 0; i < ARPQ_MAX; i++) {
        struct arpq_ent* q = &g_arpq[i];
        if (!q->pkt || q->nexthop != nexthop || q->dev != dev) continue;
        uint8_t* pkt = q->pkt; q->pkt = NULL;
        eth_send(dev, mac, ETH_TYPE_IPV4, pkt, q->len);
        kfree(pkt);
    }
}

/* Next-hop MAC for dst (broadcasts need no ARP). 0 = resolved, -1 = pending. */
static int ipv4_route(net_dev_t* dev, uint32_t dst_ip, uint8_t dmac[6], uint32_t* nexthop) {
    uint32_t bcast = dev->netmask ? (dev->ip | ~dev->netmask) : 0xFFFFFFFFu;
    *nexthop = 0;
    if (dst_ip == 0xFFFFFFFFu || (dev->netmask && dst_ip == bcast)) {
        for (int i = 0; i < 6; i++) dmac[i] = 0xFF;
        return 0;
    }
    // Route: on-subnet -> direct, else via gateway.
    *nexthop = ((dst_ip & dev->netmask) == (dev->ip & dev->netmask)) ? dst_ip : dev->gateway;
    return arp_resolve(dev, *nexthop, dmac);
}

int ipv4_send_ex(net_dev_t* dev, uint32_t dst_ip, uint8_t proto, const void* payload,
                 uint32_t len, uint8_t ttl, uint8_t tos, int queue) {
    NET_ASSERT_STACK_LOCKED();
    if (!dev || len > NET_MTU - 20) return -1;
    uint8_t* ip = g_ipbuf;
    uint16_t tot = (uint16_t)(20 + len);
    ip[0] = 0x45; ip[1] = tos;
    ip[2] = (uint8_t)(tot >> 8); ip[3] = (uint8_t)tot;
    ip[4] = (uint8_t)(g_ip_id >> 8); ip[5] = (uint8_t)g_ip_id; g_ip_id++;
    ip[6] = 0x40; ip[7] = 0;          // DF, no fragment offset
    ip[8] = ttl ? ttl : 64; ip[9] = proto;
    ip[10] = 0; ip[11] = 0;           // checksum (computed below)
    ip[12]=(uint8_t)dev->ip;     ip[13]=(uint8_t)(dev->ip>>8);
    ip[14]=(uint8_t)(dev->ip>>16);ip[15]=(uint8_t)(dev->ip>>24);
    ip[16]=(uint8_t)dst_ip;      ip[17]=(uint8_t)(dst_ip>>8);
    ip[18]=(uint8_t)(dst_ip>>16);ip[19]=(uint8_t)(dst_ip>>24);
    uint16_t csum = net_checksum(ip, 20);
    ip[10] = (uint8_t)(csum >> 8); ip[11] = (uint8_t)csum;
    const uint8_t* p = (const uint8_t*)payload;
    for (uint32_t i = 0; i < len; i++) ip[20 + i] = p[i];
    uint8_t dmac[6]; uint32_t nh;
    if (ipv4_route(dev, dst_ip, dmac, &nh) != 0)   // ARP pending (request sent)
        return (queue && arpq_add(dev, nh, ip, 20 + len) == 0) ? 0 : -1;
    return eth_send(dev, dmac, ETH_TYPE_IPV4, ip, 20 + len);
}

int ipv4_send(net_dev_t* dev, uint32_t dst_ip, uint8_t proto, const void* payload, uint32_t len) {
    return ipv4_send_ex(dev, dst_ip, proto, payload, len, 64, 0, 0);
}

/* [U10] SOCK_RAW + IP_HDRINCL: the caller's packet starts with an IPv4 header.
 * Like Linux, the kernel always fills the total length and the header
 * checksum, and an id / source address left 0. Returns 0, -1 (no route yet:
 * queued or dropped) or -22 (malformed header). */
int ipv4_output_hdr(net_dev_t* dev, const void* pkt, uint32_t len) {
    NET_ASSERT_STACK_LOCKED();
    if (!dev || len < 20 || len > NET_MTU) return -22;
    const uint8_t* in = (const uint8_t*)pkt;
    uint32_t ihl = (uint32_t)(in[0] & 0x0F) * 4;
    if ((in[0] >> 4) != 4 || ihl < 20 || ihl > len) return -22;
    uint8_t* ip = g_ipbuf;
    for (uint32_t i = 0; i < len; i++) ip[i] = in[i];
    ip[2] = (uint8_t)(len >> 8); ip[3] = (uint8_t)len;
    if (!ip[4] && !ip[5]) { ip[4] = (uint8_t)(g_ip_id >> 8); ip[5] = (uint8_t)g_ip_id; g_ip_id++; }
    if (!ip[12] && !ip[13] && !ip[14] && !ip[15]) {
        ip[12]=(uint8_t)dev->ip; ip[13]=(uint8_t)(dev->ip>>8);
        ip[14]=(uint8_t)(dev->ip>>16); ip[15]=(uint8_t)(dev->ip>>24);
    }
    ip[10] = 0; ip[11] = 0;
    uint16_t csum = net_checksum(ip, ihl);
    ip[10] = (uint8_t)(csum >> 8); ip[11] = (uint8_t)csum;
    uint32_t dst = (uint32_t)ip[16] | ((uint32_t)ip[17]<<8) | ((uint32_t)ip[18]<<16) | ((uint32_t)ip[19]<<24);
    uint8_t dmac[6]; uint32_t nh;
    if (ipv4_route(dev, dst, dmac, &nh) != 0)
        return arpq_add(dev, nh, ip, len) == 0 ? 0 : -1;
    return eth_send(dev, dmac, ETH_TYPE_IPV4, ip, len);
}
