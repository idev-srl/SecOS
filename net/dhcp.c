/*
 * dhcp.c — [M24] DHCP client (RFC 2131) over UDP 68->67.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * A minimal four-way DISCOVER/OFFER/REQUEST/ACK client. Runs synchronously from
 * the shell/idle context: it sends a broadcast request, then spins on `hlt` so
 * the timer tick drives net_tick()->poll and the OFFER/ACK arrive via the bound
 * UDP callback (the same RX trick the ping self-test uses). On ACK it writes the
 * lease (IP / netmask / router / DNS) into the net_dev_t.
 */
#include "udp.h"
#include "debugcon.h"
#include <stddef.h>

extern uint64_t timer_get_ticks(void);

#define DHCP_OP_REQUEST   1
#define DHCP_OP_REPLY     2
#define DHCP_HTYPE_ETH    1
#define DHCP_MAGIC        0x63825363u

#define DHCP_DISCOVER     1
#define DHCP_OFFER        2
#define DHCP_REQUEST      3
#define DHCP_ACK          5
#define DHCP_NAK          6

#define OPT_SUBNET        1
#define OPT_ROUTER        3
#define OPT_DNS           6
#define OPT_REQ_IP       50
#define OPT_MSG_TYPE     53
#define OPT_SERVER_ID    54
#define OPT_PARAM_LIST   55
#define OPT_END         255

/* Captured from OFFER/ACK by the UDP callback. */
struct dhcp_state {
    uint32_t xid;
    uint32_t yiaddr, mask, router, dns, server_id;
    volatile int got_offer, got_ack, got_nak;
};

static uint8_t  g_pkt[548];   /* BOOTP fixed (236) + cookie (4) + options */

static uint32_t rd32be(const uint8_t* p) {
    /* Read 4 wire bytes (network order) into our octet0-at-LSB convention. */
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

// Parse OFFER/ACK options. Returns the message type, fills *st fields.
static void dhcp_cb(void* ctx, net_dev_t* dev, uint32_t src_ip,
                    uint16_t src_port, const uint8_t* data, uint32_t len) {
    (void)dev; (void)src_ip; (void)src_port;
    struct dhcp_state* st = (struct dhcp_state*)ctx;
    if (len < 240) return;
    if (data[0] != DHCP_OP_REPLY) return;
    /* xid is an opaque token; read it back in the same big-endian order we wrote
     * it in dhcp_build() so the round-trip compares equal. */
    uint32_t xid = ((uint32_t)data[4]<<24)|((uint32_t)data[5]<<16)|((uint32_t)data[6]<<8)|data[7];
    if (xid != st->xid) return;
    if (!(data[236]==0x63 && data[237]==0x82 && data[238]==0x53 && data[239]==0x63)) return;

    uint32_t yi = rd32be(data + 16);               /* yiaddr */
    uint8_t  msg = 0;
    uint32_t mask=0, router=0, dns=0, sid=0;
    uint32_t i = 240;
    while (i < len) {
        uint8_t code = data[i++];
        if (code == OPT_END) break;
        if (code == 0) continue;                   /* pad */
        if (i >= len) break;
        uint8_t l = data[i++];
        if (i + l > len) break;
        const uint8_t* v = data + i;
        if (code == OPT_MSG_TYPE && l >= 1) msg = v[0];
        else if (code == OPT_SUBNET && l >= 4) mask = rd32be(v);
        else if (code == OPT_ROUTER && l >= 4) router = rd32be(v);
        else if (code == OPT_DNS && l >= 4) dns = rd32be(v);
        else if (code == OPT_SERVER_ID && l >= 4) sid = rd32be(v);
        i += l;
    }
    st->yiaddr = yi; st->mask = mask; st->router = router; st->dns = dns; st->server_id = sid;
    if (msg == DHCP_OFFER) st->got_offer = 1;
    else if (msg == DHCP_ACK) st->got_ack = 1;
    else if (msg == DHCP_NAK) st->got_nak = 1;
}

// Build a BOOTREQUEST into g_pkt. msg_type = DHCP_DISCOVER / DHCP_REQUEST.
// Returns the total length. For REQUEST, includes requested-IP + server-id.
static uint32_t dhcp_build(net_dev_t* dev, struct dhcp_state* st, uint8_t msg_type) {
    for (int i = 0; i < (int)sizeof(g_pkt); i++) g_pkt[i] = 0;
    g_pkt[0] = DHCP_OP_REQUEST;
    g_pkt[1] = DHCP_HTYPE_ETH;
    g_pkt[2] = 6;                                  /* hlen */
    g_pkt[3] = 0;                                  /* hops */
    g_pkt[4]=(uint8_t)(st->xid>>24); g_pkt[5]=(uint8_t)(st->xid>>16);
    g_pkt[6]=(uint8_t)(st->xid>>8);  g_pkt[7]=(uint8_t)st->xid;
    g_pkt[10] = 0x80;                              /* flags: broadcast */
    for (int i = 0; i < 6; i++) g_pkt[28 + i] = dev->mac[i];   /* chaddr */
    g_pkt[236]=0x63; g_pkt[237]=0x82; g_pkt[238]=0x53; g_pkt[239]=0x63;  /* magic */

    uint32_t o = 240;
    g_pkt[o++]=OPT_MSG_TYPE; g_pkt[o++]=1; g_pkt[o++]=msg_type;
    if (msg_type == DHCP_REQUEST) {
        g_pkt[o++]=OPT_REQ_IP; g_pkt[o++]=4;
        g_pkt[o++]=(uint8_t)st->yiaddr; g_pkt[o++]=(uint8_t)(st->yiaddr>>8);
        g_pkt[o++]=(uint8_t)(st->yiaddr>>16); g_pkt[o++]=(uint8_t)(st->yiaddr>>24);
        g_pkt[o++]=OPT_SERVER_ID; g_pkt[o++]=4;
        g_pkt[o++]=(uint8_t)st->server_id; g_pkt[o++]=(uint8_t)(st->server_id>>8);
        g_pkt[o++]=(uint8_t)(st->server_id>>16); g_pkt[o++]=(uint8_t)(st->server_id>>24);
    }
    g_pkt[o++]=OPT_PARAM_LIST; g_pkt[o++]=3; g_pkt[o++]=OPT_SUBNET; g_pkt[o++]=OPT_ROUTER; g_pkt[o++]=OPT_DNS;
    g_pkt[o++]=OPT_END;
    return o;
}

// Spin (hlt) until *flag is set or the deadline passes. Returns 1 if set.
static int dhcp_wait(volatile int* flag, uint64_t ms) {
    uint64_t deadline = timer_get_ticks() + ms;    /* 1 kHz tick ~= 1 ms */
    while (timer_get_ticks() < deadline && !*flag)
        __asm__ volatile ("sti; hlt");
    return *flag;
}

int dhcp_configure(net_dev_t* dev) {
    if (!dev) return -1;
    struct dhcp_state st;
    for (size_t i = 0; i < sizeof(st); i++) ((uint8_t*)&st)[i] = 0;
    /* xid: mix MAC + uptime so retries differ. */
    st.xid = ((uint32_t)dev->mac[2]<<24) ^ ((uint32_t)dev->mac[3]<<16)
           ^ ((uint32_t)dev->mac[4]<<8) ^ dev->mac[5] ^ (uint32_t)timer_get_ticks();

    /* DHCP runs with an unconfigured source: clear the address so ipv4_send uses
     * 0.0.0.0 and ipv4_input accepts the broadcast reply. */
    uint32_t old_ip=dev->ip, old_mask=dev->netmask, old_gw=dev->gateway, old_dns=dev->dns;
    dev->ip = 0; dev->netmask = 0; dev->gateway = 0; dev->dns = 0;

    if (udp_bind(68, dhcp_cb, &st) != 0) { dev->ip=old_ip; dev->netmask=old_mask; dev->gateway=old_gw; dev->dns=old_dns; return -1; }

    int ok = 0;
    for (int attempt = 0; attempt < 4 && !ok; attempt++) {
        st.got_offer = 0;
        uint32_t n = dhcp_build(dev, &st, DHCP_DISCOVER);
        udp_send(dev, 0xFFFFFFFFu, 68, 67, g_pkt, n);
        if (!dhcp_wait(&st.got_offer, 1000)) continue;     /* no OFFER, retry */

        st.got_ack = 0; st.got_nak = 0;
        n = dhcp_build(dev, &st, DHCP_REQUEST);
        udp_send(dev, 0xFFFFFFFFu, 68, 67, g_pkt, n);
        if (dhcp_wait(&st.got_ack, 1000)) ok = 1;
    }

    udp_unbind(68);

    if (!ok) {
        debugcon_writestring("[DHCP] no lease (timeout)\n");
        dev->ip=old_ip; dev->netmask=old_mask; dev->gateway=old_gw; dev->dns=old_dns;
        return -1;
    }
    dev->ip      = st.yiaddr;
    dev->netmask = st.mask ? st.mask : 0x00FFFFFFu;        /* default /24 if absent */
    dev->gateway = st.router;
    dev->dns     = st.dns ? st.dns : st.server_id;
    debugcon_writestring("[DHCP] lease ip="); debugcon_print_hex(dev->ip);
    debugcon_writestring(" gw="); debugcon_print_hex(dev->gateway);
    debugcon_writestring(" dns="); debugcon_print_hex(dev->dns);
    debugcon_writestring("\n");
    return 0;
}
