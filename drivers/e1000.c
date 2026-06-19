/*
 * e1000.c — [M24] Intel e1000 (82540EM) NIC driver.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * The reference NIC driver: QEMU's default `-device e1000` (8086:100E) and a
 * classic VMware adapter option. Implements the net_dev_t contract — legacy
 * RX/TX descriptor rings (static page-aligned DMA, kvirt_to_phys), polled drain
 * via dev->poll. MMIO BAR0 through the physmap.
 */
#include "net.h"
#include "pci.h"
#include "io.h"
#include "vmm.h"
#include "debugcon.h"
#include <stddef.h>

#define E1000_VENDOR 0x8086
#define E1000_DEV_82540EM 0x100E

// Registers
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_RAL    0x5400
#define REG_RAH    0x5404
#define REG_MTA    0x5200

#define CTRL_SLU   (1u << 6)
#define CTRL_ASDE  (1u << 5)
#define CTRL_RST   (1u << 26)
#define STATUS_LU  (1u << 1)
#define RCTL_EN    (1u << 1)
#define RCTL_UPE   (1u << 3)         // unicast promiscuous
#define RCTL_MPE   (1u << 4)         // multicast promiscuous
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)        // strip Ethernet CRC
#define RCTL_BSIZE_2048 0            // (0<<16)
#define RAH_AV     (1u << 31)        // receive address valid
#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)
#define TCTL_CT    (0x0F << 4)
#define TCTL_COLD  (0x40 << 12)

#define RX_DESC 256   /* [M25] deeper RX ring (was 64) to absorb bursts between polls */
#define TX_DESC 64    /* [M25] deeper TX ring (was 16) for back-to-back sends */
#define BUF_SZ  2048

struct rx_desc { uint64_t addr; uint16_t len; uint16_t csum; uint8_t status; uint8_t errors; uint16_t special; } __attribute__((packed));
struct tx_desc { uint64_t addr; uint16_t len; uint8_t cso; uint8_t cmd; uint8_t status; uint8_t css; uint16_t special; } __attribute__((packed));

#define TX_CMD_EOP  (1u << 0)
#define TX_CMD_IFCS (1u << 1)
#define TX_CMD_RS   (1u << 3)
#define DESC_DD     (1u << 0)
#define RX_STAT_EOP (1u << 1)

// volatile: the NIC DMA-writes the descriptor status (DD) back to memory; the
// poll loop must re-read it each iteration rather than caching it in a register.
static volatile struct rx_desc g_rx[RX_DESC] __attribute__((aligned(4096)));
static volatile struct tx_desc g_tx[TX_DESC] __attribute__((aligned(4096)));
static uint8_t g_rxbuf[RX_DESC][BUF_SZ] __attribute__((aligned(16)));
static uint8_t g_txbuf[TX_DESC][BUF_SZ] __attribute__((aligned(16)));

static volatile uint8_t* g_mmio;
static uint32_t g_rx_cur, g_tx_cur;
static net_dev_t g_dev;

static inline uint64_t phys_of(const void* p) { return kvirt_to_phys((uint64_t)(uintptr_t)p); }
static inline uint32_t rd(uint32_t r) { return *(volatile uint32_t*)(g_mmio + r); }
static inline void     wr(uint32_t r, uint32_t v) { *(volatile uint32_t*)(g_mmio + r) = v; }

static int e1000_transmit(net_dev_t* dev, const void* frame, uint32_t len) {
    (void)dev;
    if (len > BUF_SZ) return -1;
    uint32_t i = g_tx_cur;
    const uint8_t* p = (const uint8_t*)frame;
    for (uint32_t k = 0; k < len; k++) g_txbuf[i][k] = p[k];
    g_tx[i].addr = phys_of(g_txbuf[i]);
    g_tx[i].len = (uint16_t)len;
    g_tx[i].cso = 0;
    g_tx[i].cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    g_tx[i].status = 0;
    g_tx_cur = (i + 1) % TX_DESC;
    io_mfence();
    wr(REG_TDT, g_tx_cur);
    // Wait for completion (DD), bounded.
    for (volatile uint64_t t = 0; !(g_tx[i].status & DESC_DD); t++)
        if (t > 5000000ull) return -1;
    return 0;
}

static void e1000_poll(net_dev_t* dev) {
    while (g_rx[g_rx_cur].status & DESC_DD) {
        volatile struct rx_desc* d = &g_rx[g_rx_cur];
        if (d->status & RX_STAT_EOP) net_rx(dev, (const void*)g_rxbuf[g_rx_cur], d->len);
        d->status = 0;
        wr(REG_RDT, g_rx_cur);                 // hand the descriptor back to HW
        g_rx_cur = (g_rx_cur + 1) % RX_DESC;
    }
    // Re-publish the tail every poll: a write to RDT makes QEMU flush any packet
    // it queued while it briefly considered the ring not-ready, and is harmless
    // on real hardware.
    wr(REG_RDT, (g_rx_cur + RX_DESC - 1) % RX_DESC);
}

// [M24] MSI-X/NAPI hooks (used only when net_request_irq picks MSI-X). Enable
// arms the receive interrupts; ack reads ICR (read-to-clear) inside the ISR.
#define IMS_RXT0   (1u << 7)         // receiver timer (per-packet with RDTR=0)
#define IMS_RXDMT0 (1u << 4)         // RX descriptor minimum threshold
#define IMS_RXO    (1u << 6)         // RX overrun
static void e1000_irq_enable(net_dev_t* dev) {
    (void)dev;
    wr(0x2820 /*RDTR*/, 0);                  // no RX delay: interrupt per packet
    wr(REG_IMS, IMS_RXT0 | IMS_RXDMT0 | IMS_RXO);
}
static void e1000_irq_ack(net_dev_t* dev) {
    (void)dev;
    (void)rd(REG_ICR);                        // read-to-clear the interrupt cause
}

// The 82540EM (QEMU `-device e1000`) and 82545EM (VMware's "e1000" adapter) share
// the same register layout, so one driver covers both.
#define E1000_DEV_82545EM 0x100F

int e1000_init(void) {
    pci_device_t pci;
    if (pci_find(E1000_VENDOR, E1000_DEV_82540EM, &pci) != 0 &&
        pci_find(E1000_VENDOR, E1000_DEV_82545EM, &pci) != 0) {
        debugcon_writestring("[E1000] no 8086:100E/100F on PCI\n");
        return -1;
    }
    pci_enable_mem_and_busmaster(&pci);
    uint64_t bar = pci_bar_mem64(&pci, 0);
    if (bar == 0) { debugcon_writestring("[E1000] BAR0 not memory\n"); return -1; }
    vmm_extend_physmap(bar + 0x20000);
    g_mmio = (volatile uint8_t*)phys_to_virt(bar);

    // Disable interrupts, reset, disable again.
    wr(REG_IMC, 0xFFFFFFFFu); (void)rd(REG_ICR);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    for (volatile uint64_t t = 0; t < 1000000 && (rd(REG_CTRL) & CTRL_RST); t++) { }
    wr(REG_IMC, 0xFFFFFFFFu); (void)rd(REG_ICR);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    // Read MAC from RAL/RAH.
    uint32_t ral = rd(REG_RAL), rah = rd(REG_RAH);
    g_dev.mac[0]=(uint8_t)ral; g_dev.mac[1]=(uint8_t)(ral>>8);
    g_dev.mac[2]=(uint8_t)(ral>>16); g_dev.mac[3]=(uint8_t)(ral>>24);
    g_dev.mac[4]=(uint8_t)rah; g_dev.mac[5]=(uint8_t)(rah>>8);
    // Re-arm the receive-address filter (set AV) so unicast frames are accepted.
    wr(REG_RAL, ral);
    wr(REG_RAH, (rah & 0xFFFF) | RAH_AV);

    // Clear the multicast table.
    for (int i = 0; i < 128; i++) wr(REG_MTA + i*4, 0);

    // RX ring.
    for (int i = 0; i < RX_DESC; i++) { g_rx[i].addr = phys_of(g_rxbuf[i]); g_rx[i].status = 0; }
    uint64_t rp = phys_of((const void*)g_rx);
    wr(REG_RDBAL, (uint32_t)rp); wr(REG_RDBAH, (uint32_t)(rp>>32));
    wr(REG_RDLEN, RX_DESC * (uint32_t)sizeof(struct rx_desc));
    wr(REG_RDH, 0); wr(REG_RDT, RX_DESC - 1);
    g_rx_cur = 0;
    // First cut: promiscuous (UPE|MPE) guarantees reception on a single-NIC guest;
    // the RAH.AV filter above is the correct unicast path for later.
    wr(REG_RCTL, RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);

    // TX ring.
    for (int i = 0; i < TX_DESC; i++) { g_tx[i].addr = 0; g_tx[i].status = DESC_DD; }
    uint64_t tp = phys_of((const void*)g_tx);
    wr(REG_TDBAL, (uint32_t)tp); wr(REG_TDBAH, (uint32_t)(tp>>32));
    wr(REG_TDLEN, TX_DESC * (uint32_t)sizeof(struct tx_desc));
    wr(REG_TDH, 0); wr(REG_TDT, 0);
    g_tx_cur = 0;
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    wr(REG_TIPG, 10 | (8 << 10) | (6 << 20));

    g_dev.transmit = e1000_transmit;
    g_dev.poll = e1000_poll;
    g_dev.irq_enable = e1000_irq_enable;     // [M24] used only under NET_USE_MSIX
    g_dev.irq_ack = e1000_irq_ack;
    g_dev.pci = pci;
    g_dev.link_up = (rd(REG_STATUS) & STATUS_LU) ? 1 : 0;
    if (net_register_dev(&g_dev) != 0) { debugcon_writestring("[E1000] register failed\n"); return -1; }
    net_request_irq(&g_dev);

    debugcon_writestring("[E1000] ready "); debugcon_writestring(g_dev.name);
    debugcon_writestring(" MAC=");
    for (int i = 0; i < 6; i++) { debugcon_print_hex(g_dev.mac[i]); if(i<5) debugcon_writestring(":"); }
    debugcon_writestring(g_dev.link_up?" link=up\n":" link=down\n");
    return 0;
}
