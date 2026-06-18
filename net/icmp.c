/*
 * icmp.c — [M24] ICMP echo (ping): reply to requests, track replies.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "net.h"
#include "debugcon.h"
#include <stddef.h>

#define IP_PROTO_ICMP 1
#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

static volatile uint32_t g_echo_replies = 0;   // counter for the ping self-test
static uint8_t g_icmpbuf[NET_FRAME_MAX];

uint32_t icmp_echo_replies(void) { return g_echo_replies; }

void icmp_input(net_dev_t* dev, uint32_t src_ip, const uint8_t* pkt, uint32_t len) {
    if (len < 8) return;
    uint8_t type = pkt[0];
    if (type == ICMP_ECHO_REPLY) {
        g_echo_replies++;
        debugcon_writestring("[ICMP] echo reply from 0x"); debugcon_print_hex(src_ip);
        debugcon_writestring("\n");
        return;
    }
    if (type == ICMP_ECHO_REQUEST) {
        // Build an echo reply: same payload, type=0, recompute checksum.
        if (len > sizeof(g_icmpbuf)) return;
        for (uint32_t i = 0; i < len; i++) g_icmpbuf[i] = pkt[i];
        g_icmpbuf[0] = ICMP_ECHO_REPLY;
        g_icmpbuf[1] = 0;
        g_icmpbuf[2] = 0; g_icmpbuf[3] = 0;             // checksum field
        uint16_t c = net_checksum(g_icmpbuf, len);
        g_icmpbuf[2] = (uint8_t)(c >> 8); g_icmpbuf[3] = (uint8_t)c;
        ipv4_send(dev, src_ip, IP_PROTO_ICMP, g_icmpbuf, len);
    }
}

// Send an ICMP echo request (used by the boot ping self-test).
int icmp_send_echo(net_dev_t* dev, uint32_t dst_ip, uint16_t id, uint16_t seq) {
    uint8_t p[16];
    p[0] = ICMP_ECHO_REQUEST; p[1] = 0;
    p[2] = 0; p[3] = 0;                                  // checksum
    p[4] = (uint8_t)(id >> 8);  p[5] = (uint8_t)id;
    p[6] = (uint8_t)(seq >> 8); p[7] = (uint8_t)seq;
    for (int i = 0; i < 8; i++) p[8 + i] = (uint8_t)('a' + i);   // payload
    uint16_t c = net_checksum(p, sizeof(p));
    p[2] = (uint8_t)(c >> 8); p[3] = (uint8_t)c;
    return ipv4_send(dev, dst_ip, IP_PROTO_ICMP, p, sizeof(p));
}
