/*
 * dhcp.c — [M24] DHCP client (RFC 2131) over UDP 68->67.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * A minimal four-way DISCOVER/OFFER/REQUEST/ACK client. The exchange is an
 * asynchronous state machine (dhcp_start/dhcp_poll): replies arrive through the
 * bound UDP callback and dhcp_poll() sends the next message or retries. The
 * kernel shell's `dhcp` spins on it with `hlt` (dhcp_configure); ring 3 (init's
 * `ifup`) polls it through SYS_NETCTL. On ACK it writes the lease (IP / netmask
 * / router / DNS) into the net_dev_t.
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

/* ---- Asynchronous client ---------------------------------------------------
 * One exchange at a time, driven by dhcp_poll(): the UDP callback (RX path)
 * only records OFFER/ACK/NAK in g_dh.st, and dhcp_poll() moves the state
 * machine on (send REQUEST, retry, give up) from the caller's context. The
 * kernel shell spins on it (dhcp_configure); ring 3 polls it through
 * SYS_NETCTL between sleeps (net/netctl.c), so no syscall spins in ring 0
 * waiting for the network. */
#define DHCP_ATTEMPTS   4
#define DHCP_STEP_MS    1000     /* wait per OFFER and per ACK (1 kHz tick) */

static struct {
    net_dev_t*        dev;
    struct dhcp_state st;
    int               phase;     /* DHCP_PH_* */
    int               attempt;
    uint64_t          deadline;
    uint32_t          old_ip, old_mask, old_gw, old_dns;
} g_dh;
static net_dev_t* g_lease_dev;   /* the NIC whose address came from a DHCP ACK */

static void dhcp_send(uint8_t msg) {
    uint32_t n = dhcp_build(g_dh.dev, &g_dh.st, msg);
    udp_output(g_dh.dev, 0xFFFFFFFFu, 68, 67, g_pkt, n);
    g_dh.deadline = timer_get_ticks() + DHCP_STEP_MS;
}

static void dhcp_discover(void) {
    g_dh.st.got_offer = 0; g_dh.st.got_ack = 0; g_dh.st.got_nak = 0;
    g_dh.phase = DHCP_PH_DISCOVER;
    dhcp_send(DHCP_DISCOVER);
}

static void dhcp_restore(void) {
    net_dev_t* dev = g_dh.dev;
    dev->ip = g_dh.old_ip; dev->netmask = g_dh.old_mask;
    dev->gateway = g_dh.old_gw; dev->dns = g_dh.old_dns;
}

static void dhcp_finish(int ok) {
    net_dev_t* dev = g_dh.dev;
    udp_unbind(68);
    if (!ok) {
        debugcon_writestring("[DHCP] no lease (timeout)\n");
        dhcp_restore();
        g_dh.phase = DHCP_PH_FAILED;
        return;
    }
    dev->ip      = g_dh.st.yiaddr;
    dev->netmask = g_dh.st.mask ? g_dh.st.mask : 0x00FFFFFFu;   /* default /24 if absent */
    dev->gateway = g_dh.st.router;
    dev->dns     = g_dh.st.dns ? g_dh.st.dns : g_dh.st.server_id;
    g_lease_dev  = dev;
    g_dh.phase   = DHCP_PH_BOUND;
    debugcon_writestring("[DHCP] lease ip="); debugcon_print_hex(dev->ip);
    debugcon_writestring(" gw="); debugcon_print_hex(dev->gateway);
    debugcon_writestring(" dns="); debugcon_print_hex(dev->dns);
    debugcon_writestring("\n");
}

/* [M47] g_dh, g_pkt and g_lease_dev are guarded by the stack lock; the UDP
 * callback already runs under it, the entry points below take it. */
static int dhcp_start_locked(net_dev_t* dev) {
    if (!dev) return -19;                                       /* -ENODEV */
    uint64_t now = timer_get_ticks();
    int busy = (g_dh.phase == DHCP_PH_DISCOVER || g_dh.phase == DHCP_PH_REQUEST);
    /* An exchange nobody has polled for a while (its caller was killed) is
     * stale: take it over instead of refusing forever. */
    if (busy && now < g_dh.deadline + 2 * DHCP_STEP_MS) return -16;   /* -EBUSY */
    if (busy) { udp_unbind(68); dhcp_restore(); }
    g_dh.dev = dev;
    g_dh.old_ip = dev->ip; g_dh.old_mask = dev->netmask;
    g_dh.old_gw = dev->gateway; g_dh.old_dns = dev->dns;
    for (size_t i = 0; i < sizeof(g_dh.st); i++) ((uint8_t*)&g_dh.st)[i] = 0;
    g_dh.attempt = 0;
    /* xid: mix MAC + uptime so retries differ. */
    g_dh.st.xid = ((uint32_t)dev->mac[2]<<24) ^ ((uint32_t)dev->mac[3]<<16)
                ^ ((uint32_t)dev->mac[4]<<8) ^ dev->mac[5] ^ (uint32_t)now;
    if (g_lease_dev == dev) g_lease_dev = NULL;
    /* DHCP runs with an unconfigured source: clear the address so ipv4_send uses
     * 0.0.0.0 and ipv4_input accepts the broadcast reply. */
    dev->ip = 0; dev->netmask = 0; dev->gateway = 0; dev->dns = 0;
    if (udp_bind(68, dhcp_cb, &g_dh.st) != 0) {
        dhcp_restore();
        g_dh.phase = DHCP_PH_FAILED;
        return -16;                                             /* -EBUSY */
    }
    debugcon_writestring("[DHCP] DISCOVER on "); debugcon_writestring(dev->name);
    debugcon_writestring("\n");
    dhcp_discover();
    return 0;
}

int dhcp_start(net_dev_t* dev) {
    uint64_t fl = net_lock();
    int r = dhcp_start_locked(dev);
    net_unlock(fl);
    return r;
}

int dhcp_poll_locked(net_dev_t* dev) {
    NET_ASSERT_STACK_LOCKED();
    if (dev && dev != g_dh.dev) return DHCP_PH_IDLE;
    uint64_t now = timer_get_ticks();
    if (g_dh.phase == DHCP_PH_DISCOVER) {
        if (g_dh.st.got_offer) {
            g_dh.st.got_ack = 0; g_dh.st.got_nak = 0;
            g_dh.phase = DHCP_PH_REQUEST;
            dhcp_send(DHCP_REQUEST);
        } else if (now >= g_dh.deadline) {
            if (++g_dh.attempt >= DHCP_ATTEMPTS) dhcp_finish(0);
            else dhcp_discover();
        }
    } else if (g_dh.phase == DHCP_PH_REQUEST) {
        if (g_dh.st.got_ack) dhcp_finish(1);
        else if (g_dh.st.got_nak || now >= g_dh.deadline) {
            if (++g_dh.attempt >= DHCP_ATTEMPTS) dhcp_finish(0);
            else dhcp_discover();
        }
    }
    return g_dh.phase;
}

int dhcp_poll(net_dev_t* dev) {
    uint64_t fl = net_lock();
    int r = dhcp_poll_locked(dev);
    net_unlock(fl);
    return r;
}

int dhcp_has_lease(const net_dev_t* dev) {
    uint64_t fl = net_lock();
    int r = dev && g_lease_dev == dev;
    net_unlock(fl);
    return r;
}

int dhcp_configure(net_dev_t* dev) {
    if (dhcp_start(dev) < 0) return -1;
    int ph;
    while ((ph = dhcp_poll(dev)) == DHCP_PH_DISCOVER || ph == DHCP_PH_REQUEST)
        __asm__ volatile ("sti; hlt");   /* the tick drives RX and the deadlines */
    return ph == DHCP_PH_BOUND ? 0 : -1;
}
