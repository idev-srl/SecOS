/*
 * e1000e.c — [M24] Intel e1000e (PCIe gigabit) NIC driver.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Intel e1000e NIC — IMPLEMENTED BUT NOT YET TESTED (QEMU e1000e exists but
 * needs the full M24 stack; validate on hardware/VMware).
 *
 * Minimal e1000e: probe the controller over PCI (Intel 8086:10D3 and the
 * common e1000e-family device ids), map BAR0 (32- or 64-bit MMIO) through the
 * physmap, reset the device, set auto-speed + link-up, read the MAC from
 * RAL/RAH, and bring up legacy RX/TX descriptor rings with per-descriptor DMA
 * packet buffers. Every DMA structure is a static page-aligned kernel buffer;
 * physical addresses come from kvirt_to_phys. transmit() copies into the next
 * TX buffer and bumps TDT; poll() drains the RX ring NAPI-style, handing each
 * frame to net_rx(). Interrupt wiring is delegated to net_request_irq() (the
 * core picks MSI-X / INTx / timer-tick polling); this driver only supplies the
 * poll() drain routine. Registers the device with net_register_dev() and emits
 * a [E1000E] debugcon marker on success, matching the [AHCI]/[NVME] style.
 */
#include "e1000e.h"
#include "net.h"
#include "pci.h"
#include "io.h"
#include "debugcon.h"
#include "vmm.h"        // phys_to_virt / kvirt_to_phys / vmm_extend_physmap
#include <stddef.h>

/* ---- Device register offsets (BAR0 MMIO) ---- */
#define E1000_CTRL    0x00000   // Device Control
#define E1000_STATUS  0x00008   // Device Status
#define E1000_CTRL_EXT 0x00018  // Extended Device Control
#define E1000_ICR     0x000C0   // Interrupt Cause Read (read-to-clear)
#define E1000_IMS     0x000D0   // Interrupt Mask Set/Read
#define E1000_IMC     0x000D8   // Interrupt Mask Clear
#define E1000_RCTL    0x00100   // Receive Control
#define E1000_TCTL    0x00400   // Transmit Control
#define E1000_TIPG    0x00410   // Transmit Inter-Packet Gap
#define E1000_RDBAL   0x02800   // RX Descriptor Base Low
#define E1000_RDBAH   0x02804   // RX Descriptor Base High
#define E1000_RDLEN   0x02808   // RX Descriptor Ring Length (bytes)
#define E1000_RDH     0x02810   // RX Descriptor Head
#define E1000_RDT     0x02818   // RX Descriptor Tail
#define E1000_TDBAL   0x03800   // TX Descriptor Base Low
#define E1000_TDBAH   0x03804   // TX Descriptor Base High
#define E1000_TDLEN   0x03808   // TX Descriptor Ring Length (bytes)
#define E1000_TDH     0x03810   // TX Descriptor Head
#define E1000_TDT     0x03818   // TX Descriptor Tail
#define E1000_RAL0    0x05400   // Receive Address Low (entry 0)
#define E1000_RAH0    0x05404   // Receive Address High (entry 0)

/* ---- CTRL bits ---- */
#define CTRL_SLU      (1u << 6)    // Set Link Up
#define CTRL_ASDE     (1u << 5)    // Auto-Speed Detection Enable
#define CTRL_RST      (1u << 26)   // Device Reset
#define CTRL_PHY_RST  (1u << 31)   // PHY Reset

/* ---- STATUS bits ---- */
#define STATUS_LU     (1u << 1)    // Link Up

/* ---- RCTL bits ---- */
#define RCTL_EN       (1u << 1)    // Receiver Enable
#define RCTL_SBP      (1u << 2)    // Store Bad Packets
#define RCTL_UPE      (1u << 3)    // Unicast Promiscuous Enable
#define RCTL_MPE      (1u << 4)    // Multicast Promiscuous Enable
#define RCTL_BAM      (1u << 15)   // Broadcast Accept Mode
#define RCTL_BSIZE_2048 (0u << 16) // 2048-byte buffers (BSIZE=00, BSEX=0)
#define RCTL_SECRC    (1u << 26)   // Strip Ethernet CRC

/* ---- TCTL bits ---- */
#define TCTL_EN       (1u << 1)    // Transmitter Enable
#define TCTL_PSP      (1u << 3)    // Pad Short Packets
#define TCTL_CT_SHIFT 4            // Collision Threshold
#define TCTL_COLD_SHIFT 12         // Collision Distance

/* ---- Legacy TX descriptor CMD/STA bits (in the dword fields) ---- */
#define TXD_CMD_EOP   (1u << 0)    // End Of Packet
#define TXD_CMD_IFCS  (1u << 1)    // Insert FCS
#define TXD_CMD_RS    (1u << 3)    // Report Status
#define TXD_STA_DD    (1u << 0)    // Descriptor Done

/* ---- Legacy RX descriptor status bits ---- */
#define RXD_STAT_DD   (1u << 0)    // Descriptor Done
#define RXD_STAT_EOP  (1u << 1)    // End Of Packet

#define E1000_RX_DESCS 32
#define E1000_TX_DESCS 32
#define E1000_BUF_SIZE 2048

/* Legacy receive descriptor (16 bytes). */
struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

/* Legacy transmit descriptor (16 bytes). */
struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

/* Static, page-aligned DMA structures (kernel is identity/physmap mapped). */
static struct e1000_rx_desc g_rx_ring[E1000_RX_DESCS] __attribute__((aligned(4096)));
static struct e1000_tx_desc g_tx_ring[E1000_TX_DESCS] __attribute__((aligned(4096)));
static uint8_t g_rx_buf[E1000_RX_DESCS][E1000_BUF_SIZE] __attribute__((aligned(4096)));
static uint8_t g_tx_buf[E1000_TX_DESCS][E1000_BUF_SIZE] __attribute__((aligned(4096)));

static volatile uint8_t* g_mmio;
static uint32_t g_tx_tail;          // next TX descriptor to fill
static net_dev_t g_dev;

static inline uint64_t phys_of(const void* p) { return kvirt_to_phys((uint64_t)(uintptr_t)p); }
static inline uint32_t reg_rd(uint32_t off) { return *(volatile uint32_t*)(g_mmio + off); }
static inline void     reg_wr(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_mmio + off) = v; }

/* The set of e1000e-family device ids we accept (Intel vendor 0x8086). */
static const uint16_t k_e1000e_ids[] = {
    0x10D3,   // 82574L (the QEMU "e1000e" device)
    0x10F5,   // 82567LM
    0x150C,   // 82583V
    0x1502,   // 82579LM
    0x153A,   // I217-LM
    0x15B7,   // I219-LM
    0x15B8,   // I219-V
    0x15D6,   // I219-V (5)
    0x15D7,   // I219-LM (4)
    0x15D8,   // I219-V (4)
};

static int e1000e_find(pci_device_t* out) {
    /* Primary: the QEMU e1000e device id. */
    if (pci_find(0x8086, 0x10D3, out) == 0) return 0;
    /* Otherwise scan the wider family list. */
    for (size_t i = 0; i < sizeof(k_e1000e_ids) / sizeof(k_e1000e_ids[0]); i++) {
        if (pci_find(0x8086, k_e1000e_ids[i], out) == 0) return 0;
    }
    return -1;
}

/* Read the 6-byte MAC from RAL0/RAH0 into dev->mac. */
static void e1000e_read_mac(net_dev_t* dev) {
    uint32_t ral = reg_rd(E1000_RAL0);
    uint32_t rah = reg_rd(E1000_RAH0);
    dev->mac[0] = (uint8_t)(ral >> 0);
    dev->mac[1] = (uint8_t)(ral >> 8);
    dev->mac[2] = (uint8_t)(ral >> 16);
    dev->mac[3] = (uint8_t)(ral >> 24);
    dev->mac[4] = (uint8_t)(rah >> 0);
    dev->mac[5] = (uint8_t)(rah >> 8);
}

static void e1000e_setup_rx(void) {
    for (uint32_t i = 0; i < E1000_RX_DESCS; i++) {
        g_rx_ring[i].addr   = phys_of(g_rx_buf[i]);
        g_rx_ring[i].length = 0;
        g_rx_ring[i].status = 0;
        g_rx_ring[i].errors = 0;
    }
    uint64_t base = phys_of(g_rx_ring);
    reg_wr(E1000_RDBAL, (uint32_t)base);
    reg_wr(E1000_RDBAH, (uint32_t)(base >> 32));
    reg_wr(E1000_RDLEN, E1000_RX_DESCS * (uint32_t)sizeof(struct e1000_rx_desc));
    reg_wr(E1000_RDH, 0);
    /* Tail points one past the last owned-by-hardware descriptor: all are
     * available to the NIC, so RDT = last index. */
    reg_wr(E1000_RDT, E1000_RX_DESCS - 1);

    reg_wr(E1000_RCTL,
           RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC);
}

static void e1000e_setup_tx(void) {
    for (uint32_t i = 0; i < E1000_TX_DESCS; i++) {
        g_tx_ring[i].addr   = phys_of(g_tx_buf[i]);
        g_tx_ring[i].length = 0;
        g_tx_ring[i].cmd    = 0;
        g_tx_ring[i].status = TXD_STA_DD;   // mark free so transmit() can reuse
    }
    uint64_t base = phys_of(g_tx_ring);
    reg_wr(E1000_TDBAL, (uint32_t)base);
    reg_wr(E1000_TDBAH, (uint32_t)(base >> 32));
    reg_wr(E1000_TDLEN, E1000_TX_DESCS * (uint32_t)sizeof(struct e1000_tx_desc));
    reg_wr(E1000_TDH, 0);
    reg_wr(E1000_TDT, 0);
    g_tx_tail = 0;

    /* CT = 0x0F (collision threshold), COLD = 0x40 (full-duplex distance). */
    reg_wr(E1000_TCTL,
           TCTL_EN | TCTL_PSP | (0x0Fu << TCTL_CT_SHIFT) | (0x40u << TCTL_COLD_SHIFT));
    /* IPGT=10, IPGR1=8, IPGR2=6 (standard 802.3 timings). */
    reg_wr(E1000_TIPG, 10u | (8u << 10) | (6u << 20));
}

/* Send one fully-formed Ethernet frame. Returns 0 on success, <0 on error. */
static int e1000e_transmit(net_dev_t* dev, const void* frame, uint32_t len) {
    (void)dev;
    if (frame == NULL || len == 0 || len > E1000_BUF_SIZE) return -1;

    uint32_t idx = g_tx_tail;
    /* Wait for this descriptor to be free (DD set) — single slot in flight. */
    for (volatile uint64_t i = 0; !(g_tx_ring[idx].status & TXD_STA_DD); i++)
        if (i > 50000000ull) return -2;

    const uint8_t* src = (const uint8_t*)frame;
    for (uint32_t i = 0; i < len; i++) g_tx_buf[idx][i] = src[i];

    g_tx_ring[idx].addr   = phys_of(g_tx_buf[idx]);
    g_tx_ring[idx].length = (uint16_t)len;
    g_tx_ring[idx].cso    = 0;
    g_tx_ring[idx].cmd    = (uint8_t)(TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS);
    g_tx_ring[idx].status = 0;

    g_tx_tail = (idx + 1) % E1000_TX_DESCS;
    io_mfence();
    reg_wr(E1000_TDT, g_tx_tail);

    /* Wait for the descriptor to report done (DD) so the buffer can be reused. */
    for (volatile uint64_t i = 0; !(g_tx_ring[idx].status & TXD_STA_DD); i++)
        if (i > 50000000ull) return -3;
    return 0;
}

/* Drain the RX ring NAPI-style: from RDT+1 while DD is set, hand each frame to
 * net_rx(), clear the descriptor status, and advance RDT to recycle buffers. */
static void e1000e_poll(net_dev_t* dev) {
    uint32_t tail = reg_rd(E1000_RDT);
    for (;;) {
        uint32_t idx = (tail + 1) % E1000_RX_DESCS;
        struct e1000_rx_desc* rd = &g_rx_ring[idx];
        if (!(rd->status & RXD_STAT_DD)) break;

        if (rd->status & RXD_STAT_EOP) {
            uint32_t len = rd->length;
            if (len > 0 && len <= E1000_BUF_SIZE)
                net_rx(dev, g_rx_buf[idx], len);
        }
        /* Recycle: reset the descriptor and hand it back to the NIC. */
        rd->status = 0;
        rd->length = 0;
        rd->addr   = phys_of(g_rx_buf[idx]);
        tail = idx;
        io_mfence();
        reg_wr(E1000_RDT, tail);
    }
}

int e1000e_init(void) {
    pci_device_t dev;
    if (e1000e_find(&dev) != 0) {
        debugcon_writestring("[E1000E] no controller on PCI\n");
        return -1;
    }
    pci_enable_mem_and_busmaster(&dev);

    uint64_t bar = pci_bar_mem64(&dev, 0);
    if (bar == 0) { debugcon_writestring("[E1000E] BAR0 not memory\n"); return -1; }
    vmm_extend_physmap(bar + 0x20000);   // register window is up to ~0x14000
    g_mmio = (volatile uint8_t*)phys_to_virt(bar);

    debugcon_writestring("[E1000E] dev=0x"); debugcon_print_hex(dev.device_id);
    debugcon_writestring(" BAR0=0x"); debugcon_print_hex(bar); debugcon_writestring("\n");

    /* Mask off all interrupts during setup (the core decides IRQ mode later). */
    reg_wr(E1000_IMC, 0xFFFFFFFFu);
    (void)reg_rd(E1000_ICR);   // read-to-clear pending causes

    /* Reset the device, then wait for the reset bit to self-clear. */
    reg_wr(E1000_CTRL, reg_rd(E1000_CTRL) | CTRL_RST);
    io_mfence();
    for (volatile uint64_t i = 0; (reg_rd(E1000_CTRL) & CTRL_RST); i++)
        if (i > 50000000ull) { debugcon_writestring("[E1000E] reset timeout\n"); return -1; }

    /* Re-mask interrupts (reset re-arms some) and clear causes again. */
    reg_wr(E1000_IMC, 0xFFFFFFFFu);
    (void)reg_rd(E1000_ICR);

    /* Auto-speed detection + set link up. */
    reg_wr(E1000_CTRL, reg_rd(E1000_CTRL) | CTRL_ASDE | CTRL_SLU);

    e1000e_read_mac(&g_dev);
    e1000e_setup_rx();
    e1000e_setup_tx();

    /* Give the link a moment to settle, then sample STATUS.LU. */
    for (volatile uint64_t i = 0; i < 2000000ull; i++) { /* spin */ }
    int link = (reg_rd(E1000_STATUS) & STATUS_LU) ? 1 : 0;

    /* Fill the net_dev_t contract. */
    g_dev.name[0] = 'e'; g_dev.name[1] = 't'; g_dev.name[2] = 'h';
    g_dev.name[3] = '0'; g_dev.name[4] = '\0';
    g_dev.transmit = e1000e_transmit;
    g_dev.poll     = e1000e_poll;
    g_dev.priv     = NULL;
    g_dev.pci      = dev;
    g_dev.link_up  = link;

    if (net_register_dev(&g_dev) != 0) {
        debugcon_writestring("[E1000E] net_register_dev failed\n");
        return -1;
    }

    /* Let the core wire the interrupt (MSI-X / INTx / timer-tick poll). */
    net_request_irq(&g_dev);

    debugcon_writestring("[E1000E] ready eth0 MAC=");
    for (int i = 0; i < ETH_ALEN; i++) {
        debugcon_print_hex(g_dev.mac[i]);
        if (i != ETH_ALEN - 1) debugcon_putchar(':');
    }
    debugcon_writestring(" link=");
    debugcon_writestring(link ? "up" : "down");
    debugcon_writestring("\n");
    return 0;
}
