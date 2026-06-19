/*
 * ipv4.c — [M24] IPv4 input/output (no fragmentation).
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "net.h"
#include <stddef.h>

#define ETH_TYPE_IPV4 0x0800
#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define IP_PROTO_TCP  6

static uint16_t g_ip_id = 1;
static uint8_t  g_ipbuf[NET_FRAME_MAX];

extern void udp_input(net_dev_t* dev, uint32_t src_ip, const uint8_t* seg, uint32_t len) __attribute__((weak));
extern void tcp_input(net_dev_t* dev, uint32_t src_ip, const uint8_t* seg, uint32_t len) __attribute__((weak));

void ipv4_input(net_dev_t* dev, const uint8_t* frame, uint32_t len, const uint8_t src_mac[6]) {
    (void)src_mac;
    if (len < 14 + 20) return;
    const uint8_t* ip = frame + 14;
    uint8_t ihl = (uint8_t)((ip[0] & 0x0F) * 4);
    if (ihl < 20) return;
    uint16_t tot = (uint16_t)((ip[2] << 8) | ip[3]);
    if (tot < ihl || 14u + tot > len) return;
    uint8_t proto = ip[9];
    uint32_t dst = (uint32_t)ip[16] | ((uint32_t)ip[17]<<8) | ((uint32_t)ip[18]<<16) | ((uint32_t)ip[19]<<24);
    uint32_t src = (uint32_t)ip[12] | ((uint32_t)ip[13]<<8) | ((uint32_t)ip[14]<<16) | ((uint32_t)ip[15]<<24);
    if (dev->ip && dst != dev->ip && dst != 0xFFFFFFFFu) return;   // not for us
    const uint8_t* payload = ip + ihl;
    uint32_t plen = (uint32_t)tot - ihl;
    if (proto == IP_PROTO_ICMP)      icmp_input(dev, src, payload, plen);
    else if (proto == IP_PROTO_UDP && udp_input) udp_input(dev, src, payload, plen);
    else if (proto == IP_PROTO_TCP && tcp_input) tcp_input(dev, src, payload, plen);
}

int ipv4_send(net_dev_t* dev, uint32_t dst_ip, uint8_t proto, const void* payload, uint32_t len) {
    if (!dev || len > NET_MTU - 20) return -1;
    uint8_t dmac[6];
    // Limited broadcast (255.255.255.255) and the subnet directed broadcast go to
    // the Ethernet broadcast address with no ARP — needed for DHCP, which runs
    // before dev->ip is configured.
    uint32_t bcast = dev->netmask ? (dev->ip | ~dev->netmask) : 0xFFFFFFFFu;
    if (dst_ip == 0xFFFFFFFFu || (dev->netmask && dst_ip == bcast)) {
        for (int i = 0; i < 6; i++) dmac[i] = 0xFF;
    } else {
        // Route: on-subnet -> direct, else via gateway.
        uint32_t nexthop = ((dst_ip & dev->netmask) == (dev->ip & dev->netmask)) ? dst_ip : dev->gateway;
        if (arp_resolve(dev, nexthop, dmac) != 0) return -1;   // ARP pending: caller retries
    }

    uint8_t* ip = g_ipbuf;
    uint16_t tot = (uint16_t)(20 + len);
    ip[0] = 0x45; ip[1] = 0;
    ip[2] = (uint8_t)(tot >> 8); ip[3] = (uint8_t)tot;
    ip[4] = (uint8_t)(g_ip_id >> 8); ip[5] = (uint8_t)g_ip_id; g_ip_id++;
    ip[6] = 0x40; ip[7] = 0;          // DF, no fragment offset
    ip[8] = 64;  ip[9] = proto;       // TTL, proto
    ip[10] = 0; ip[11] = 0;           // checksum (computed below)
    ip[12]=(uint8_t)dev->ip;     ip[13]=(uint8_t)(dev->ip>>8);
    ip[14]=(uint8_t)(dev->ip>>16);ip[15]=(uint8_t)(dev->ip>>24);
    ip[16]=(uint8_t)dst_ip;      ip[17]=(uint8_t)(dst_ip>>8);
    ip[18]=(uint8_t)(dst_ip>>16);ip[19]=(uint8_t)(dst_ip>>24);
    uint16_t csum = net_checksum(ip, 20);
    ip[10] = (uint8_t)(csum >> 8); ip[11] = (uint8_t)csum;
    const uint8_t* p = (const uint8_t*)payload;
    for (uint32_t i = 0; i < len; i++) ip[20 + i] = p[i];
    return eth_send(dev, dmac, ETH_TYPE_IPV4, ip, 20 + len);
}
