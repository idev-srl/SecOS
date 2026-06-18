/*
 * igc.c — Intel igc (I225/I226 2.5GbE) NIC driver.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Intel igc (I225/I226 2.5GbE) NIC — IMPLEMENTED BUT NOT YET TESTED (not
 * emulated by QEMU; validate on hardware).
 *
 * The Intel Foxville I225/I226 controller is a 2.5 GbE evolution of the e1000e
 * register model: same control/status/RAL/RAH layout, but the descriptor rings
 * use the *advanced* descriptor format (16-byte read / write-back unions). We
 * map BAR0 (64-bit MMIO) through the physmap, reset the MAC (CTRL.RST), set the
 * link up (CTRL.SLU), read the station MAC from RAL0/RAH0, and program one RX
 * and one TX queue (queue 0) with static page-aligned DMA rings + 2 KB per-desc
 * buffers. All hardware-visible physical addresses come from kvirt_to_phys
 * (the kernel is higher-half; rings live in the kernel image / physmap).
 *
 * transmit() copies the frame into the next TX buffer, fills an advanced TX
 * data descriptor (DTYP=data, EOP|IFCS|RS, paylen) and bumps TDT. poll()
 * drains the RX ring NAPI-style: for every descriptor whose write-back DD bit
 * is set, hand the frame to net_rx(), reset the descriptor to read format
 * (refill), and advance RDT.
 */
#include "igc.h"
#include "net.h"
#include "pci.h"
#include "io.h"
#include "debugcon.h"
#include "vmm.h"        /* phys_to_virt / kvirt_to_phys / vmm_extend_physmap */
#include <stddef.h>

/* ---- Supported PCI device IDs (vendor 0x8086) ---- */
#define IGC_VENDOR        0x8086
static const uint16_t igc_dev_ids[] = {
    0x15F2,   /* I225-LM */
    0x15F3,   /* I225-V  */
    0x0D9F,   /* I225-IT */
    0x125B,   /* I226-LM */
    0x125C,   /* I226-V  */
    0x125D,   /* I226-IT */
};
#define IGC_NUM_IDS (sizeof(igc_dev_ids) / sizeof(igc_dev_ids[0]))

/* ---- Device registers (BAR0 MMIO offsets) ---- */
#define IGC_CTRL      0x00000   /* Device Control */
#define IGC_STATUS    0x00008   /* Device Status */
#define IGC_CTRL_EXT  0x00018   /* Extended Device Control */
#define IGC_RCTL      0x00100   /* Receive Control */
#define IGC_TCTL      0x00400   /* Transmit Control */

/* Per-queue RX registers (queue 0). */
#define IGC_RDBAL0    0x0C000   /* RX Descriptor Base Low */
#define IGC_RDBAH0    0x0C004   /* RX Descriptor Base High */
#define IGC_RDLEN0    0x0C008   /* RX Descriptor Ring Length (bytes) */
#define IGC_SRRCTL0   0x0C00C   /* Split & Replication Receive Control */
#define IGC_RDH0      0x0C010   /* RX Descriptor Head */
#define IGC_RDT0      0x0C018   /* RX Descriptor Tail */
#define IGC_RXDCTL0   0x0C028   /* RX Descriptor Control */

/* Per-queue TX registers (queue 0). */
#define IGC_TDBAL0    0x0E000   /* TX Descriptor Base Low */
#define IGC_TDBAH0    0x0E004   /* TX Descriptor Base High */
#define IGC_TDLEN0    0x0E008   /* TX Descriptor Ring Length (bytes) */
#define IGC_TDH0      0x0E010   /* TX Descriptor Head */
#define IGC_TDT0      0x0E018   /* TX Descriptor Tail */
#define IGC_TXDCTL0   0x0E028   /* TX Descriptor Control */

/* Receive Address registers (filter entry 0 = station MAC). */
#define IGC_RAL0      0x05400
#define IGC_RAH0      0x05404

/* Interrupt registers — used only to mask everything off (we poll). */
#define IGC_IMC       0x000D8   /* Interrupt Mask Clear (write 1 to clear) */
#define IGC_ICR       0x000C0   /* Interrupt Cause Read */

/* CTRL bits. */
#define CTRL_SLU      (1u << 6)    /* Set Link Up */
#define CTRL_RST      (1u << 26)   /* Device Reset */

/* STATUS bits. */
#define STATUS_LU     (1u << 1)    /* Link Up */

/* RCTL bits. */
#define RCTL_EN       (1u << 1)    /* Receiver Enable */
#define RCTL_BAM      (1u << 15)   /* Broadcast Accept Mode */
#define RCTL_SECRC    (1u << 26)   /* Strip Ethernet CRC */

/* SRRCTL fields. */
#define SRRCTL_BSIZEPKT_2K   (2u)          /* BSIZEPACKET in KB units (bits 6:0) */
#define SRRCTL_DESCTYPE_ADV  (1u << 25)    /* Advanced descriptor, one buffer */
#define SRRCTL_DROP_EN       (1u << 31)    /* drop packet if no descriptor */

/* RXDCTL / TXDCTL bits. */
#define XDCTL_ENABLE  (1u << 25)   /* queue Enable */

/* TCTL bits. */
#define TCTL_EN       (1u << 1)    /* Transmit Enable */
#define TCTL_PSP      (1u << 3)    /* Pad Short Packets */

/* Advanced TX data descriptor — DCMD / DTYP (cmd_type_len, dword 2). */
#define IGC_ADVTXD_DTYP_DATA  (0x3u << 20)  /* descriptor type = advanced data */
#define IGC_ADVTXD_DCMD_DEXT  (1u << 29)    /* descriptor extension (advanced) */
#define IGC_ADVTXD_DCMD_EOP   (1u << 24)    /* End Of Packet */
#define IGC_ADVTXD_DCMD_IFCS  (1u << 25)    /* Insert FCS */
#define IGC_ADVTXD_DCMD_RS    (1u << 27)    /* Report Status (set DD on done) */
#define IGC_ADVTXD_STAT_DD    (1u << 0)     /* write-back: descriptor done */
/* PAYLEN sits in the upper bits of olinfo_status (dword 3), shifted by 14. */
#define IGC_ADVTXD_PAYLEN_SHIFT  14

/* Advanced RX descriptor write-back status (lower-status dword). */
#define IGC_ADVRXD_STAT_DD    (1u << 0)     /* descriptor done */
#define IGC_ADVRXD_STAT_EOP   (1u << 1)     /* end of packet */

/* Ring sizing. Power-of-two; ring byte length must be 128-byte aligned. */
#define RX_RING      32
#define TX_RING      32
#define RX_BUF_SIZE  2048
#define TX_BUF_SIZE  2048

/*
 * Advanced RX descriptor (16 bytes). Two layouts share the same memory:
 *  - read format  (we write before handing to the NIC): packet buffer address
 *    + header buffer address (we use one-buffer mode, so header addr = 0).
 *  - write-back   (the NIC writes on completion): status/error + length.
 * We model both as two little-endian 64-bit halves and pick fields by offset.
 */
struct igc_rx_desc {
    volatile uint64_t addr;    /* read: packet buffer phys addr */
    volatile uint64_t status;  /* read: header addr (0); write-back: status/len */
} __attribute__((packed));

/* Advanced TX data descriptor (16 bytes). */
struct igc_tx_desc {
    volatile uint64_t addr;            /* buffer phys addr */
    volatile uint32_t cmd_type_len;    /* DTALEN | DTYP | DCMD */
    volatile uint32_t olinfo_status;   /* PAYLEN | status (DD write-back) */
} __attribute__((packed));

/* Static, page-aligned DMA: rings + per-descriptor packet buffers. */
static struct igc_rx_desc g_rx_ring[RX_RING] __attribute__((aligned(4096)));
static struct igc_tx_desc g_tx_ring[TX_RING] __attribute__((aligned(4096)));
static uint8_t g_rx_buf[RX_RING][RX_BUF_SIZE] __attribute__((aligned(4096)));
static uint8_t g_tx_buf[TX_RING][TX_BUF_SIZE] __attribute__((aligned(4096)));

static volatile uint8_t* g_bar;     /* BAR0 mapped through the physmap */
static uint32_t g_rx_tail;          /* next RDT value (last refilled + 1) */
static uint32_t g_tx_tail;          /* next free TX slot */
static net_dev_t g_dev;

static inline uint64_t phys_of(const void* p) {
    return kvirt_to_phys((uint64_t)(uintptr_t)p);
}
static inline uint32_t rd32(uint32_t off) {
    return *(volatile uint32_t*)(g_bar + off);
}
static inline void wr32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(g_bar + off) = v;
}

static void igc_delay(uint32_t loops) {
    for (volatile uint32_t i = 0; i < loops; i++) { /* spin */ }
}

/* Reset the RX ring to read format and reset head/tail bookkeeping. */
static void igc_setup_rx(void) {
    for (uint32_t i = 0; i < RX_RING; i++) {
        g_rx_ring[i].addr   = phys_of(g_rx_buf[i]);
        g_rx_ring[i].status = 0;
    }
    uint64_t ring = phys_of(g_rx_ring);
    wr32(IGC_RDBAL0, (uint32_t)ring);
    wr32(IGC_RDBAH0, (uint32_t)(ring >> 32));
    wr32(IGC_RDLEN0, (uint32_t)(RX_RING * sizeof(struct igc_rx_desc)));

    /* 2 KB buffers, advanced one-buffer descriptors. */
    wr32(IGC_SRRCTL0, SRRCTL_BSIZEPKT_2K | SRRCTL_DESCTYPE_ADV | SRRCTL_DROP_EN);

    wr32(IGC_RDH0, 0);
    wr32(IGC_RDT0, 0);

    /* Enable the queue and wait for the ENABLE bit to read back. */
    wr32(IGC_RXDCTL0, rd32(IGC_RXDCTL0) | XDCTL_ENABLE);
    for (uint32_t i = 0; i < 1000000; i++) {
        if (rd32(IGC_RXDCTL0) & XDCTL_ENABLE) break;
    }

    /* Receiver on: accept broadcast, strip CRC. */
    wr32(IGC_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    /* Hand all descriptors to the NIC: tail = last index. */
    g_rx_tail = RX_RING - 1;
    wr32(IGC_RDT0, g_rx_tail);
}

static void igc_setup_tx(void) {
    for (uint32_t i = 0; i < TX_RING; i++) {
        g_tx_ring[i].addr = 0;
        g_tx_ring[i].cmd_type_len = 0;
        g_tx_ring[i].olinfo_status = IGC_ADVTXD_STAT_DD; /* mark free initially */
    }
    uint64_t ring = phys_of(g_tx_ring);
    wr32(IGC_TDBAL0, (uint32_t)ring);
    wr32(IGC_TDBAH0, (uint32_t)(ring >> 32));
    wr32(IGC_TDLEN0, (uint32_t)(TX_RING * sizeof(struct igc_tx_desc)));
    wr32(IGC_TDH0, 0);
    wr32(IGC_TDT0, 0);
    g_tx_tail = 0;

    wr32(IGC_TXDCTL0, rd32(IGC_TXDCTL0) | XDCTL_ENABLE);
    for (uint32_t i = 0; i < 1000000; i++) {
        if (rd32(IGC_TXDCTL0) & XDCTL_ENABLE) break;
    }

    wr32(IGC_TCTL, TCTL_EN | TCTL_PSP);
}

/* Send one fully-formed Ethernet frame (dst+src+type+payload). */
static int igc_transmit(net_dev_t* dev, const void* frame, uint32_t len) {
    (void)dev;
    if (len == 0 || len > TX_BUF_SIZE) return -1;

    uint32_t idx = g_tx_tail;
    struct igc_tx_desc* d = &g_tx_ring[idx];

    /* Wait for this slot's previous transmit to complete (DD set). The ring is
     * shallow and transmits are serialized, so this is effectively immediate. */
    for (uint32_t i = 0; i < 5000000; i++) {
        if (d->olinfo_status & IGC_ADVTXD_STAT_DD) break;
    }

    const uint8_t* src = (const uint8_t*)frame;
    for (uint32_t i = 0; i < len; i++) g_tx_buf[idx][i] = src[i];

    d->addr = phys_of(g_tx_buf[idx]);
    /* DTALEN (bytes in this buffer) in low 16 bits, advanced data type, and
     * the command bits: EOP + IFCS + RS + DEXT. */
    d->cmd_type_len = (len & 0xFFFF) | IGC_ADVTXD_DTYP_DATA |
                      IGC_ADVTXD_DCMD_DEXT | IGC_ADVTXD_DCMD_EOP |
                      IGC_ADVTXD_DCMD_IFCS | IGC_ADVTXD_DCMD_RS;
    /* PAYLEN of the whole packet (single-descriptor packet == len). Clear the
     * DD status bit; the NIC sets it on completion. */
    d->olinfo_status = ((uint32_t)len << IGC_ADVTXD_PAYLEN_SHIFT);

    g_tx_tail = (idx + 1) % TX_RING;
    io_mfence();
    wr32(IGC_TDT0, g_tx_tail);
    return 0;
}

/* Drain the RX ring NAPI-style: deliver every completed frame, refill, advance
 * the tail. Called from the NIC IRQ and/or the timer-tick poll. */
static void igc_poll(net_dev_t* dev) {
    for (;;) {
        uint32_t idx = (g_rx_tail + 1) % RX_RING;   /* oldest unconsumed desc */
        struct igc_rx_desc* d = &g_rx_ring[idx];
        uint64_t wb = d->status;                    /* write-back status/length */
        uint32_t status = (uint32_t)wb;
        if (!(status & IGC_ADVRXD_STAT_DD)) break;  /* nothing ready */

        /* Length is bits 47:32 of the write-back qword (PKT_LEN field). */
        uint32_t len = (uint32_t)((wb >> 32) & 0xFFFF);
        if (len > 0 && len <= RX_BUF_SIZE && (status & IGC_ADVRXD_STAT_EOP)) {
            net_rx(dev, g_rx_buf[idx], len);
        }

        /* Refill: restore the descriptor to read format and give it back. */
        d->addr   = phys_of(g_rx_buf[idx]);
        d->status = 0;
        io_mfence();

        g_rx_tail = idx;
        wr32(IGC_RDT0, g_rx_tail);
    }
}

static void igc_read_mac(net_dev_t* dev) {
    uint32_t ral = rd32(IGC_RAL0);
    uint32_t rah = rd32(IGC_RAH0);
    dev->mac[0] = (uint8_t)(ral);
    dev->mac[1] = (uint8_t)(ral >> 8);
    dev->mac[2] = (uint8_t)(ral >> 16);
    dev->mac[3] = (uint8_t)(ral >> 24);
    dev->mac[4] = (uint8_t)(rah);
    dev->mac[5] = (uint8_t)(rah >> 8);
}

int igc_init(void) {
    pci_device_t dev;
    int found = 0;
    for (uint32_t i = 0; i < IGC_NUM_IDS; i++) {
        if (pci_find(IGC_VENDOR, igc_dev_ids[i], &dev) == 0) { found = 1; break; }
    }
    if (!found) {
        debugcon_writestring("[IGC] no I225/I226 NIC on PCI\n");
        return -1;
    }

    pci_enable_mem_and_busmaster(&dev);
    uint64_t bar = pci_bar_mem64(&dev, 0);
    if (bar == 0) { debugcon_writestring("[IGC] BAR0 not memory\n"); return -1; }
    /* igc register space is 128 KB; map a generous window through the physmap. */
    vmm_extend_physmap(bar + 0x20000);
    g_bar = (volatile uint8_t*)phys_to_virt(bar);

    debugcon_writestring("[IGC] dev=0x"); debugcon_print_hex(dev.device_id);
    debugcon_writestring(" BAR=0x"); debugcon_print_hex(bar); debugcon_writestring("\n");

    /* Mask all interrupts (we poll). */
    wr32(IGC_IMC, 0xFFFFFFFFu);
    (void)rd32(IGC_ICR);

    /* Device reset, then wait for RST to self-clear. */
    wr32(IGC_CTRL, rd32(IGC_CTRL) | CTRL_RST);
    io_mfence();
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(rd32(IGC_CTRL) & CTRL_RST)) break;
        igc_delay(100);
    }
    igc_delay(200000);   /* post-reset settle */

    /* Mask interrupts again (reset re-enables some defaults). */
    wr32(IGC_IMC, 0xFFFFFFFFu);
    (void)rd32(IGC_ICR);

    /* Set link up. */
    wr32(IGC_CTRL, rd32(IGC_CTRL) | CTRL_SLU);

    igc_read_mac(&g_dev);
    igc_setup_rx();
    igc_setup_tx();

    /* Sample link state (may still be negotiating; reported best-effort). */
    int link = (rd32(IGC_STATUS) & STATUS_LU) ? 1 : 0;

    /* Fill the net_dev_t and register. */
    for (int i = 0; i < 16; i++) g_dev.name[i] = 0;
    g_dev.name[0] = 'e'; g_dev.name[1] = 't'; g_dev.name[2] = 'h'; g_dev.name[3] = '0';
    g_dev.transmit = igc_transmit;
    g_dev.poll = igc_poll;
    g_dev.priv = NULL;
    g_dev.pci = dev;
    g_dev.link_up = link;

    if (net_register_dev(&g_dev) != 0) {
        debugcon_writestring("[IGC] net_register_dev failed\n");
        return -1;
    }

    /* The core chooses MSI-X / INTx / poll; we only provide dev->poll. */
    g_dev.irq_mode = net_request_irq(&g_dev);

    debugcon_writestring("[IGC] up MAC=");
    for (int i = 0; i < ETH_ALEN; i++) {
        debugcon_print_hex(g_dev.mac[i]);
        if (i != ETH_ALEN - 1) debugcon_putchar(':');
    }
    debugcon_writestring(" link=");
    debugcon_writestring(link ? "UP" : "DOWN");
    debugcon_writestring("\n");
    return 0;
}
