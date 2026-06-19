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

// [M24] The MSI-X-wired NIC (NAPI). At most one is driven by interrupts; any
// others fall back to timer-tick polling. NULL when no NIC uses MSI-X.
static net_dev_t* g_irq_dev;

#ifdef NET_USE_MSIX
#include "idt.h"
#include "lapic.h"
#include "pci.h"
#endif

net_irq_mode_t net_request_irq(net_dev_t* dev) {
    if (!dev || !dev->poll) { if (dev) dev->irq_mode = NET_IRQ_NONE; return NET_IRQ_NONE; }
#ifdef NET_USE_MSIX
    // [M24] NAPI via MSI-X (perf path for 2.5 GbE+). Gated + additive: only the
    // first NIC is wired; if MSI-X/MSI or the LAPIC are unavailable (e.g. QEMU's
    // legacy e1000), fall through to polling — behaviour-identical to the default
    // build. NOT validated on hardware yet (NICs that have MSI-X lack a QEMU model).
    if (!g_irq_dev && lapic_enable() == 0) {
        idt_install_msi_vector(IDT_VECTOR_NET, isr_net);
        extern void isr_net(void);
        if (pci_enable_msix(&dev->pci, IDT_VECTOR_NET) == 0 ||
            pci_enable_msi(&dev->pci, IDT_VECTOR_NET) == 0) {
            g_irq_dev = dev;
            if (dev->irq_enable) dev->irq_enable(dev);   // NIC unmasks its RX cause
            dev->irq_mode = NET_IRQ_MSIX;
            // Hybrid NAPI: the MSI-X interrupt gives low-latency RX wakeups, but we
            // also keep a timer-tick poll backstop (g_poll) so RX is complete even
            // if a NIC's interrupt re-arm semantics drop an edge — a watchdog poll
            // is standard practice in production NIC drivers. Belt and suspenders.
            if (g_npoll < NET_MAX_DEV) g_poll[g_npoll++] = dev;
            debugcon_writestring("[NET] MSI-X NAPI armed vector=0x42 (+poll backstop)\n");
            return NET_IRQ_MSIX;                         // timers still run via net_tick
        }
        debugcon_writestring("[NET] no MSI-X/MSI; staying polled\n");
    }
#endif
    // Default: timer-tick polling of dev->poll (works on every NIC/firmware).
    if (g_npoll < NET_MAX_DEV) {
        g_poll[g_npoll++] = dev;
        dev->irq_mode = NET_IRQ_POLL;
        return NET_IRQ_POLL;
    }
    dev->irq_mode = NET_IRQ_NONE;
    return NET_IRQ_NONE;
}

// [M24] C half of isr_net (vector 0x42). NAPI: ack the NIC, drain the RX ring via
// dev->poll, EOI the LAPIC. Protocol timers (ARP/TCP) stay on the timer tick via
// net_tick(), so they don't run per-packet here. Only fires under NET_USE_MSIX.
volatile uint32_t g_net_irq_count;
void net_irq_handler(void) {
    g_net_irq_count++;
    if (g_net_irq_count <= 3) debugcon_writestring("[NET] IRQ fired\n");
    net_dev_t* d = g_irq_dev;
    if (d) {
        if (d->irq_ack) d->irq_ack(d);
        if (d->poll) d->poll(d);
    }
    extern void lapic_eoi(void);
    lapic_eoi();
}

extern void tcp_tick(void) __attribute__((weak));

void net_tick(void) {
    for (int i = 0; i < g_npoll; i++)
        if (g_poll[i] && g_poll[i]->poll) g_poll[i]->poll(g_poll[i]);
    arp_tick();
    if (tcp_tick) tcp_tick();
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
    extern int e1000e_init(void);
    extern int vmxnet3_init(void);
    extern int igc_init(void);
    // Probe each supported NIC; each is a no-op when its device is absent. The
    // first one to register becomes the primary. (e1000 verified in QEMU;
    // e1000e/vmxnet3/igc implemented but not yet tested on real hardware.)
    e1000_init();
    e1000e_init();
    vmxnet3_init();
    igc_init();
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

// [M24] ARP + ping the given IPv4 (network byte order). Drives the NIC poll loop
// directly via hlt (yields the vCPU so received DMA lands). Returns 0 on an echo
// reply, -1 on timeout; mirrors the outcome to debugcon.
int net_ping_test(uint32_t dst_ip) {
    net_dev_t* d = net_primary();
    if (!d) return -1;
    extern uint64_t timer_get_ticks(void);
    uint8_t mac[6];
    __asm__ volatile ("sti");

    arp_request(d, dst_ip);
    uint64_t deadline = timer_get_ticks() + 1000;       // ~1 s at 1 kHz
    while (timer_get_ticks() < deadline && arp_lookup(dst_ip, mac) != 0)
        __asm__ volatile ("hlt");
    if (arp_lookup(dst_ip, mac) != 0) { debugcon_writestring("[NET] ARP timeout (no peer)\n"); return -1; }
    debugcon_writestring("[NET] ARP resolved\n");

    uint32_t before = icmp_echo_replies();
    icmp_send_echo(d, dst_ip, 1, 1);
    deadline = timer_get_ticks() + 1000;
    while (timer_get_ticks() < deadline && icmp_echo_replies() == before)
        __asm__ volatile ("hlt");
    int ok = icmp_echo_replies() > before;
    debugcon_writestring(ok ? "[NET] PING OK\n" : "[NET] PING timeout\n");
    return ok ? 0 : -1;
}
