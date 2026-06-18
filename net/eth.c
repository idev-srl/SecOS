/*
 * eth.c — [M24] Ethernet II framing + L2 demux.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "net.h"
#include <stddef.h>

#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP  0x0806

// Outgoing-frame scratch (the stack is single-threaded/serialized).
static uint8_t g_txframe[NET_FRAME_MAX];

int eth_send(net_dev_t* dev, const uint8_t dst_mac[6], uint16_t ethertype,
             const void* payload, uint32_t len) {
    if (!dev || !dev->transmit) return -1;
    if (len > NET_MTU) return -1;
    uint8_t* f = g_txframe;
    for (int i = 0; i < 6; i++) f[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) f[6 + i] = dev->mac[i];
    f[12] = (uint8_t)(ethertype >> 8); f[13] = (uint8_t)ethertype;
    const uint8_t* p = (const uint8_t*)payload;
    for (uint32_t i = 0; i < len; i++) f[14 + i] = p[i];
    uint32_t total = 14 + len;
    if (total < 60) { for (uint32_t i = total; i < 60; i++) f[i] = 0; total = 60; } // min frame
    return dev->transmit(dev, f, total);
}

void eth_input(net_dev_t* dev, const uint8_t* frame, uint32_t len) {
    if (len < 14) return;
    uint16_t type = (uint16_t)((frame[12] << 8) | frame[13]);
    const uint8_t* src_mac = frame + 6;
    if (type == ETH_TYPE_ARP) {
        arp_input(dev, frame, len);
    } else if (type == ETH_TYPE_IPV4) {
        ipv4_input(dev, frame, len, src_mac);
    }
    // other ethertypes ignored
}
