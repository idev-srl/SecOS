/*
 * arp.c — [M24] ARP (IPv4 <-> Ethernet address resolution) + cache.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "net.h"
#include <stddef.h>

#define ARP_HTYPE_ETH 1
#define ARP_PTYPE_IP  0x0800
#define ARP_OP_REQ    1
#define ARP_OP_REPLY  2
#define ETH_TYPE_ARP  0x0806

#define ARP_CACHE 16
struct arp_entry { uint32_t ip; uint8_t mac[6]; int valid; uint32_t age; };
static struct arp_entry g_cache[ARP_CACHE];

static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static void cache_put(uint32_t ip, const uint8_t mac[6]) {
    int slot = -1;
    for (int i = 0; i < ARP_CACHE; i++) { if (g_cache[i].valid && g_cache[i].ip==ip) { slot=i; break; } }
    if (slot < 0) for (int i = 0; i < ARP_CACHE; i++) { if (!g_cache[i].valid) { slot=i; break; } }
    if (slot < 0) slot = 0; // evict slot 0 if full
    g_cache[slot].ip = ip;
    for (int i=0;i<6;i++) g_cache[slot].mac[i]=mac[i];
    g_cache[slot].valid = 1; g_cache[slot].age = 0;
}

int arp_lookup(uint32_t ip, uint8_t out_mac[6]) {
    for (int i = 0; i < ARP_CACHE; i++)
        if (g_cache[i].valid && g_cache[i].ip == ip) {
            for (int j=0;j<6;j++) out_mac[j]=g_cache[i].mac[j];
            return 0;
        }
    return -1;
}

int arp_resolve(net_dev_t* dev, uint32_t ip, uint8_t out_mac[6]) {
    if (arp_lookup(ip, out_mac) == 0) return 0;
    arp_request(dev, ip);
    return -1;   // pending
}

void arp_request(net_dev_t* dev, uint32_t target_ip) {
    uint8_t pkt[28];
    pkt[0]=ARP_HTYPE_ETH>>8; pkt[1]=ARP_HTYPE_ETH&0xFF;
    pkt[2]=ARP_PTYPE_IP>>8;  pkt[3]=ARP_PTYPE_IP&0xFF;
    pkt[4]=6; pkt[5]=4;
    pkt[6]=ARP_OP_REQ>>8;    pkt[7]=ARP_OP_REQ&0xFF;
    for (int i=0;i<6;i++) pkt[8+i]=dev->mac[i];        // sender HW
    pkt[14]=(uint8_t)(dev->ip);     pkt[15]=(uint8_t)(dev->ip>>8);   // sender IP (net order bytes)
    pkt[16]=(uint8_t)(dev->ip>>16); pkt[17]=(uint8_t)(dev->ip>>24);
    for (int i=0;i<6;i++) pkt[18+i]=0;                 // target HW (unknown)
    pkt[24]=(uint8_t)(target_ip);     pkt[25]=(uint8_t)(target_ip>>8);
    pkt[26]=(uint8_t)(target_ip>>16); pkt[27]=(uint8_t)(target_ip>>24);
    eth_send(dev, BCAST, ETH_TYPE_ARP, pkt, sizeof(pkt));
}

void arp_input(net_dev_t* dev, const uint8_t* frame, uint32_t len) {
    if (len < 14 + 28) return;
    const uint8_t* a = frame + 14;
    uint16_t op = (uint16_t)((a[6]<<8)|a[7]);
    uint32_t spa = (uint32_t)a[14] | ((uint32_t)a[15]<<8) | ((uint32_t)a[16]<<16) | ((uint32_t)a[17]<<24);
    uint32_t tpa = (uint32_t)a[24] | ((uint32_t)a[25]<<8) | ((uint32_t)a[26]<<16) | ((uint32_t)a[27]<<24);
    const uint8_t* sha = a + 8;
    cache_put(spa, sha);                                // learn the sender

    if (op == ARP_OP_REQ && tpa == dev->ip && dev->ip) {
        // Reply with our MAC.
        uint8_t pkt[28];
        pkt[0]=ARP_HTYPE_ETH>>8; pkt[1]=ARP_HTYPE_ETH&0xFF;
        pkt[2]=ARP_PTYPE_IP>>8;  pkt[3]=ARP_PTYPE_IP&0xFF;
        pkt[4]=6; pkt[5]=4;
        pkt[6]=ARP_OP_REPLY>>8;  pkt[7]=ARP_OP_REPLY&0xFF;
        for (int i=0;i<6;i++) pkt[8+i]=dev->mac[i];
        pkt[14]=(uint8_t)(dev->ip);     pkt[15]=(uint8_t)(dev->ip>>8);
        pkt[16]=(uint8_t)(dev->ip>>16); pkt[17]=(uint8_t)(dev->ip>>24);
        for (int i=0;i<6;i++) pkt[18+i]=sha[i];
        pkt[24]=(uint8_t)(spa);     pkt[25]=(uint8_t)(spa>>8);
        pkt[26]=(uint8_t)(spa>>16); pkt[27]=(uint8_t)(spa>>24);
        eth_send(dev, sha, ETH_TYPE_ARP, pkt, sizeof(pkt));
    }
}

void arp_tick(void) {
    for (int i = 0; i < ARP_CACHE; i++)
        if (g_cache[i].valid && ++g_cache[i].age > 600000) g_cache[i].valid = 0; // coarse aging
}
