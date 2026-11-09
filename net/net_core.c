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
#include "netctl.h"
#include "debugcon.h"
#include <stddef.h>

#define NET_MAX_DEV 4

static net_dev_t* g_devs[NET_MAX_DEV];
static int        g_ndev;
static net_dev_t* g_poll[NET_MAX_DEV];   /* devices serviced from the timer tick */
static int        g_npoll;

/* [M47] counters (stack lock, except rx_rekicks which is a racy statistic) */
static uint64_t g_rx_frames, g_tx_frames, g_tx_errors;
static volatile uint64_t g_rx_rekicks;

int net_register_dev(net_dev_t* dev) {
    if (!dev || g_ndev >= NET_MAX_DEV) return -1;
    for (int i = 0; i < g_ndev; i++) if (g_devs[i] == dev) return 0;
    netlk_init(&dev->rx_lock, NETLK_RX, "nic-rx");
    netlk_init(&dev->tx_lock, NETLK_TX, "nic-tx");
    dev->rx_kick = 0;
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
    if (!dev) return;
    if (!frame || len < 14 || len > NET_FRAME_MAX) { dev->stats.rx_dropped++; return; }
    dev->stats.rx_packets++;                       /* [U5] rx_lock held (poll) */
    dev->stats.rx_bytes += len;
    uint64_t fl = net_lock();
    g_rx_frames++;
    eth_input(dev, (const uint8_t*)frame, len);
    net_unlock(fl);
}

// [M47] RX drain. The MSI-X ISR, the timer-tick backstop and (with SMP) any
// CPU may ask for a drain at the same time; only one runs dev->poll(). A CPU
// that finds the ring busy leaves rx_kick set, and the draining CPU re-checks
// it after its pass and again after dropping the lock, so a frame that landed
// during a drain is never left behind until the next tick.
void net_dev_poll(net_dev_t* dev) {
    if (!dev || !dev->poll) return;
    dev->rx_kick = 1;
    for (;;) {
        uint64_t fl;
        if (!netlk_try(&dev->rx_lock, &fl)) { g_rx_rekicks++; return; }
        while (dev->rx_kick) {
            dev->rx_kick = 0;
            __sync_synchronize();
            dev->poll(dev);
        }
        netlk_release(&dev->rx_lock, fl);
        __sync_synchronize();
        if (!dev->rx_kick) return;
    }
}

// [M47] The one place frames reach a driver: serialises the TX ring.
int net_dev_xmit(net_dev_t* dev, const void* frame, uint32_t len) {
    NET_ASSERT_STACK_LOCKED();
    if (!dev || !dev->transmit) return -1;
    uint64_t fl = netlk_acquire(&dev->tx_lock);
    int r = dev->transmit(dev, frame, len);
    if (r < 0) dev->stats.tx_errors++;             /* [U5] under tx_lock */
    else { dev->stats.tx_packets++; dev->stats.tx_bytes += len; }
    netlk_release(&dev->tx_lock, fl);
    if (r < 0) g_tx_errors++; else g_tx_frames++;
    return r;
}

// [M24] The MSI-X-wired NIC (NAPI). At most one is driven by interrupts; any
// others fall back to timer-tick polling. NULL when no NIC uses MSI-X.
static net_dev_t* g_irq_dev;

#include "irq.h"
#include "lapic.h"
#include "pci_msi.h"
#include "klock.h"   /* [M50] cpu_wait_irq */

static void net_irq_handler(uint8_t vec, void* ctx);

net_irq_mode_t net_request_irq(net_dev_t* dev) {
    if (!dev || !dev->poll) { if (dev) dev->irq_mode = NET_IRQ_NONE; return NET_IRQ_NONE; }
#ifndef NET_NO_MSIX
    // [M24] NAPI via MSI-X is now the DEFAULT RX path: interrupt-driven, low
    // latency. Only the first NIC is wired. If MSI-X/MSI or the LAPIC are
    // unavailable (e.g. QEMU's legacy e1000), fall through to timer-tick polling.
    // Build with -DNET_NO_MSIX to force pure polling.
    // [M53c] The vector comes from the dynamic allocator (targeted at the BSP,
    // as before) and the message is programmed on MSI-X entry 0 / MSI message 0.
    if (!g_irq_dev && lapic_enable() == 0) {
        int vec = irq_vec_alloc(IRQ_CPU_ANY, net_irq_handler, dev, "net");
        int how = vec < 0 ? -1 : pci_irq_setup_single(&dev->pci, (uint8_t)vec, (uint32_t)irq_vec_cpu(vec));
        if (how > 0) {
            g_irq_dev = dev;
            if (dev->irq_enable) dev->irq_enable(dev);   // NIC unmasks its RX cause
            dev->irq_mode = NET_IRQ_MSIX;
            // Hybrid NAPI: the MSI-X interrupt is the primary RX wakeup, but we also
            // keep a timer-tick poll backstop (g_poll) so RX stays complete even if a
            // NIC's interrupt re-arm semantics drop an edge — a watchdog poll is
            // standard practice in production NIC drivers. Belt and suspenders.
            if (g_npoll < NET_MAX_DEV) g_poll[g_npoll++] = dev;
            debugcon_writestring(how == 1 ? "[NET] MSI-X NAPI armed vector=0x" : "[NET] MSI NAPI armed vector=0x");
            debugcon_print_hex((uint64_t)vec);
            debugcon_writestring(" (+poll backstop)\n");
            return NET_IRQ_MSIX;                         // timers still run via net_tick
        }
        if (vec >= 0) irq_vec_free(vec);
        debugcon_writestring("[NET] no MSI-X/MSI; staying polled\n");
    }
#endif
    // Fallback: timer-tick polling of dev->poll (works on every NIC/firmware).
    if (g_npoll < NET_MAX_DEV) {
        g_poll[g_npoll++] = dev;
        dev->irq_mode = NET_IRQ_POLL;
        return NET_IRQ_POLL;
    }
    dev->irq_mode = NET_IRQ_NONE;
    return NET_IRQ_NONE;
}

// [M24] NIC interrupt handler (an allocated vector, [M53c]). NAPI: ack the NIC,
// drain the RX ring via dev->poll; irq_dispatch EOIs the LAPIC. Protocol timers
// (ARP/TCP) stay on the timer tick via net_tick(), so they don't run per-packet.
volatile uint32_t g_net_irq_count;
static void net_irq_handler(uint8_t vec, void* ctx) {
    (void)vec;
    g_net_irq_count++;
    net_dev_t* d = (net_dev_t*)ctx;
    if (d) {
        // Ack unconditionally (a cause left set may stop the NIC from raising
        // the next MSI), then drain — or hand the drain to the CPU already in it.
        if (d->irq_ack) d->irq_ack(d);
        net_dev_poll(d);
    }
}

// [M58] A device serviced from the timer tick only (the ring-3 NIC proxy,
// net/net_udrv.c: its "interrupt" is the driver's kick). 0, or -1 when full.
int net_poll_add(net_dev_t* dev) {
    if (!dev || g_npoll >= NET_MAX_DEV) return -1;
    for (int i = 0; i < g_npoll; i++) if (g_poll[i] == dev) return 0;
    g_poll[g_npoll++] = dev;
    return 0;
}

extern void tcp_tick(void) __attribute__((weak));

// Timer tick (IRQ context). [M47] One CPU at a time: protocol timers must not
// run twice per tick if several CPUs deliver the tick callback.
static net_spinlock_t g_tick_lock = NET_SPINLOCK_INIT(NETLK_TICK, "net-tick");
void net_tick(void) {
    uint64_t tfl;
    if (!netlk_try(&g_tick_lock, &tfl)) return;     /* another CPU runs this tick */
    for (int i = 0; i < g_npoll; i++)
        if (g_poll[i]) net_dev_poll(g_poll[i]);
    uint64_t fl = net_lock();
    arp_tick();
    if (tcp_tick) tcp_tick();
    net_unlock(fl);
    netlk_release(&g_tick_lock, tfl);
}

extern int sock_count_used(void);
extern void tcp_count(uint32_t* used, uint32_t* orphans);
extern void tcp_alloc_failures(uint64_t* full, uint64_t* nomem);
extern int udp_bind_count(void);
void net_get_stats(struct secos_netstats* st) {
    uint64_t fl = net_lock();
    st->sockets = (uint32_t)sock_count_used();
    tcp_count(&st->tcp_conns, &st->tcp_orphans);
    st->udp_binds = (uint32_t)udp_bind_count();
    st->arp_entries = (uint32_t)arp_entries();
    { extern uint32_t tcp_peak(void); st->tcp_peak = tcp_peak(); }
    st->rx_frames = g_rx_frames;
    st->tx_frames = g_tx_frames;
    st->tx_errors = g_tx_errors;
    st->rx_rekicks = g_rx_rekicks;
    tcp_alloc_failures(&st->tcp_full, &st->tcp_nomem);
    net_unlock(fl);
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

// [M24] ---- performance / latency helpers ----
static inline uint64_t rdtsc(void){
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Cycles per microsecond. Prefer the kernel's calibrated TSC clock (M28-3); fall
// back to a local calibration against the 1 kHz PIT if it isn't up yet.
uint64_t net_tsc_per_us(void) {
    static uint64_t cached;
    if (cached) return cached;
    { extern uint64_t tsc_hz(void);
      uint64_t hz = tsc_hz();
      if (hz) { cached = hz / 1000000u; if (!cached) cached = 1; return cached; } }
    extern uint64_t timer_get_ticks(void);
    uint64_t t = timer_get_ticks();
    while (timer_get_ticks() == t) cpu_wait_irq();  // align to a tick edge
    uint64_t start_tick = timer_get_ticks();
    uint64_t start_tsc = rdtsc();
    while (timer_get_ticks() < start_tick + 100) cpu_wait_irq(); // 100 ms
    uint64_t cyc = rdtsc() - start_tsc;
    cached = cyc / 100000;                       // cycles per us (100 ms = 100000 us)
    if (!cached) cached = 1;
    return cached;
}

// [M47] Shell-side helpers: one locked step each, so the hlt waits between
// them never hold the stack lock.
static int lk_arp_lookup(uint32_t ip, uint8_t mac[6]) {
    uint64_t fl = net_lock(); int r = arp_lookup(ip, mac); net_unlock(fl); return r;
}
static void lk_arp_request(net_dev_t* d, uint32_t ip) {
    uint64_t fl = net_lock(); arp_request(d, ip); net_unlock(fl);
}
static int lk_echo(net_dev_t* d, uint32_t ip, uint16_t id, uint16_t seq) {
    uint64_t fl = net_lock(); int r = icmp_send_echo(d, ip, id, seq); net_unlock(fl); return r;
}
static uint32_t lk_nexthop(net_dev_t* d, uint32_t dst_ip) {
    uint64_t fl = net_lock();
    uint32_t nh = (d->netmask && (dst_ip & d->netmask) == (d->ip & d->netmask)) ? dst_ip : d->gateway;
    net_unlock(fl);
    return nh;
}

int net_ping_rtt(uint32_t dst_ip, uint16_t seq, uint64_t* rtt_tsc) {
    net_dev_t* d = net_primary();
    if (!d) return -1;
    extern uint64_t timer_get_ticks(void);
    uint8_t mac[6];
    __asm__ volatile("sti");
    // Resolve the NEXT HOP, not the destination (you can't ARP a host that isn't
    // on the local subnet): off-subnet -> gateway, on-subnet -> the dst itself.
    // This mirrors ipv4_send()'s routing — without it, pinging any external IP
    // ARPed the unreachable destination and timed out before sending the echo.
    uint32_t nexthop = lk_nexthop(d, dst_ip);
    if (lk_arp_lookup(nexthop, mac) != 0) {
        lk_arp_request(d, nexthop);
        uint64_t adl = timer_get_ticks() + 1000;
        while (timer_get_ticks() < adl && lk_arp_lookup(nexthop, mac) != 0)
            cpu_wait_irq();
        if (lk_arp_lookup(nexthop, mac) != 0) return -1;
    }
    uint32_t before = icmp_echo_replies();
    uint64_t deadline = timer_get_ticks() + 2000;        // 2 s safety timeout
    uint64_t t0 = rdtsc();
    lk_echo(d, dst_ip, 0x1234, seq);
    // Wait via hlt: the reply is drained by the MSI-X interrupt (net_irq_handler)
    // or the timer-tick poll, both of which bump icmp_echo_replies(). hlt yields
    // the CPU so the host/NIC delivers the RX DMA — no tight RDT hammering (which
    // could wedge the ring during the longer wait for an off-subnet reply).
    while (icmp_echo_replies() == before) {
        cpu_wait_irq();
        if (timer_get_ticks() >= deadline) return -1;
    }
    if (rtt_tsc) *rtt_tsc = rdtsc() - t0;
    return 0;
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
    /* [M38] Probe the Atheros AR9300-family WiFi (AR9565 on the ASUS E406S). The
     * scaffold identifies the chip + maps its registers; it does not yet present
     * an Ethernet net_dev (that needs association), so it never becomes primary. */
    extern int ath9k_init(void);
    ath9k_init();
    /* [M58] One proxy interface per network function given to a ring-3
     * driver (udrv=; the probes above never see those). Registered now, link
     * down, so ethN exists before its driver runs (net/net_udrv.c). */
    extern int net_udrv_init(void);
    net_udrv_init();
    if (g_ndev == 0) { debugcon_writestring("[NET] no Ethernet NIC found (WiFi-only systems use the ath9k scaffold)\n"); return 0; }

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

    // Next hop (gateway for off-subnet), not the destination — see net_ping_rtt().
    uint32_t nexthop = lk_nexthop(d, dst_ip);
    lk_arp_request(d, nexthop);
    uint64_t deadline = timer_get_ticks() + 1000;       // ~1 s at 1 kHz
    while (timer_get_ticks() < deadline && lk_arp_lookup(nexthop, mac) != 0)
        cpu_wait_irq();
    if (lk_arp_lookup(nexthop, mac) != 0) { debugcon_writestring("[NET] ARP timeout (no peer)\n"); return -1; }
    debugcon_writestring("[NET] ARP resolved\n");

    uint32_t before = icmp_echo_replies();
    lk_echo(d, dst_ip, 1, 1);
    deadline = timer_get_ticks() + 1000;
    while (timer_get_ticks() < deadline && icmp_echo_replies() == before)
        cpu_wait_irq();
    int ok = icmp_echo_replies() > before;
    debugcon_writestring(ok ? "[NET] PING OK\n" : "[NET] PING timeout\n");
    return ok ? 0 : -1;
}
