/*
 * net_core.c — [M24] network core: device registry, RX dispatch, IRQ wiring.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Owns the NIC registry, delivers received frames into the stack (net_rx ->
 * eth_input), drives polled NICs + protocol timers from the timer tick, and
 * wires NIC interrupts (MSI-X preferred for 2.5GbE+, else legacy INTx, else
 * timer-tick polling). Holds the one's-complement checksum helper.
 */
#include "net.h"
#include "debugcon.h"
#include <stddef.h>

#define NET_MAX_DEV 4

static net_dev_t* g_devs[NET_MAX_DEV];
static int        g_ndev;
static net_dev_t* g_poll[NET_MAX_DEV];   /* devices serviced from the timer tick */
static int        g_npoll;

int net_register_dev(net_dev_t* dev) {
    if (!dev || g_ndev >= NET_MAX_DEV) return -1;
    for (int i = 0; i < g_ndev; i++) if (g_devs[i] == dev) return 0;
    // Name eth0, eth1, ...
    dev->name[0]='e'; dev->name[1]='t'; dev->name[2]='h';
    dev->name[3]=(char)('0'+g_ndev); dev->name[4]=0;
    g_devs[g_ndev++] = dev;
    return 0;
}
int net_dev_count(void) { return g_ndev; }
net_dev_t* net_get_dev(int i) { return (i>=0 && i<g_ndev) ? g_devs[i] : NULL; }
net_dev_t* net_primary(void) { return g_ndev ? g_devs[0] : NULL; }

void net_rx(net_dev_t* dev, const void* frame, uint32_t len) {
    if (!dev || !frame || len < 14 || len > NET_FRAME_MAX) return;
    eth_input(dev, (const uint8_t*)frame, len);
}

net_irq_mode_t net_request_irq(net_dev_t* dev) {
    // [M24] First cut: timer-tick polling of dev->poll (guaranteed to work on
    // every NIC/firmware). MSI-X (NAPI-style, for 2.5GbE+) and legacy INTx are
    // the performance upgrade tracked for the follow-up — the contract stays the
    // same (the driver only ever provides dev->poll).
    if (dev && dev->poll && g_npoll < NET_MAX_DEV) {
        g_poll[g_npoll++] = dev;
        dev->irq_mode = NET_IRQ_POLL;
        return NET_IRQ_POLL;
    }
    dev->irq_mode = NET_IRQ_NONE;
    return NET_IRQ_NONE;
}

void net_tick(void) {
    for (int i = 0; i < g_npoll; i++)
        if (g_poll[i] && g_poll[i]->poll) g_poll[i]->poll(g_poll[i]);
    arp_tick();
}

// One's-complement Internet checksum (RFC 1071).
uint16_t net_checksum(const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t sum = 0;
    while (len > 1) { sum += (uint32_t)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (uint32_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

int net_init(void) {
    extern int e1000_init(void);
    int n = 0;
    if (e1000_init() == 0) n++;
    // (e1000e / vmxnet3 / igc are wired in once their driver branches merge.)
    if (g_ndev == 0) { debugcon_writestring("[NET] no NIC found\n"); return 0; }

    // [M24] Static IPv4 config for the primary NIC — QEMU user-mode networking
    // (SLIRP) hands out 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3. DHCP comes
    // in the next wave. Stored in network byte order.
    net_dev_t* p = net_primary();
    p->ip      = htonl(0x0A000208 + 7); /* 10.0.2.15 */
    p->netmask = htonl(0xFFFFFF00);     /* 255.255.255.0 */
    p->gateway = htonl(0x0A000202);     /* 10.0.2.2 */
    p->dns     = htonl(0x0A000203);     /* 10.0.2.3 */

    extern int timer_register_tick_callback(void (*)(void));
    timer_register_tick_callback(net_tick);

    debugcon_writestring("[NET] up: "); debugcon_writestring(p->name);
    debugcon_writestring(" ip=10.0.2.15 gw=10.0.2.2 mode=");
    debugcon_writestring(p->irq_mode==NET_IRQ_MSIX?"msix":p->irq_mode==NET_IRQ_INTX?"intx":"poll");
    debugcon_writestring("\n");
    return g_ndev;
}

// [M24] Boot self-test: ARP + ping the given IPv4 (network byte order). Drives the
// NIC poll loop directly (the scheduler/timer may not be running yet). Logs result.
void net_ping_test(uint32_t dst_ip) {
    net_dev_t* d = net_primary();
    if (!d) return;
    extern uint64_t timer_get_ticks(void);
    uint8_t mac[6];
    // The timer tick drives net_tick()->poll and, crucially, halting the CPU
    // (hlt) yields to the host so received DMA actually lands. Bounded by ticks.
    __asm__ volatile ("sti");

    arp_request(d, dst_ip);
    uint64_t deadline = timer_get_ticks() + 300;        // ~300 ms at 1 kHz
    while (timer_get_ticks() < deadline && arp_lookup(dst_ip, mac) != 0)
        __asm__ volatile ("hlt");
    if (arp_lookup(dst_ip, mac) != 0) { debugcon_writestring("[NET] ARP timeout (no peer)\n"); return; }
    debugcon_writestring("[NET] ARP resolved gateway\n");

    uint32_t before = icmp_echo_replies();
    icmp_send_echo(d, dst_ip, 1, 1);
    deadline = timer_get_ticks() + 300;
    while (timer_get_ticks() < deadline && icmp_echo_replies() == before)
        __asm__ volatile ("hlt");
    debugcon_writestring(icmp_echo_replies() > before ? "[NET] PING OK\n" : "[NET] PING timeout\n");
}
