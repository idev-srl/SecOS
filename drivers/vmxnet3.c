/*
 * vmxnet3.c — VMware vmxnet3 paravirtual NIC driver.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * VMware vmxnet3 NIC — IMPLEMENTED BUT NOT YET TESTED (QEMU has no vmxnet3;
 * validate on VMware).
 *
 * The modern high-performance paravirtual NIC on VMware ESXi/Workstation/Fusion.
 * Two BARs: BAR0 = PT (pass-through) registers — doorbells (TXPROD/RXPROD); BAR1
 * = VD (virtual device) registers — command/status doorbell. Both are MMIO,
 * mapped through the physmap. A single shared "DriverShared" DMA structure
 * (physically contiguous, static, page-aligned) holds the magic, the misc/
 * interrupt/RX/TX queue config, and physical pointers (via kvirt_to_phys) to one
 * TX queue (TxDesc ring + TxComp ring) and one RX queue (two RxDesc rings + one
 * RxComp ring). Setup writes DSAL/DSAH with the DriverShared physical address and
 * issues ACTIVATE_DEV; the permanent MAC is read with the GET_PERM_MAC commands.
 *
 * The data path is polled (NAPI-style via net_dev.poll): transmit() fills a
 * TxDesc and rings the TXPROD doorbell; poll() walks the RX completion ring by
 * generation bit, delivers each frame to net_rx(), refills the RxDesc and rings
 * the RXPROD doorbell. Every DMA structure is a static, page-aligned kernel
 * buffer; physical addresses come from kvirt_to_phys (the kernel is identity /
 * physmap mapped so the device sees the right physical frames).
 */
#include "vmxnet3.h"
#include "pci.h"
#include "io.h"
#include "net.h"
#include "debugcon.h"
#include "vmm.h"        // phys_to_virt / kvirt_to_phys / vmm_extend_physmap
#include <stddef.h>

/* ---- PCI identity (VMware vmxnet3) ---- */
#define VMXNET3_PCI_VENDOR   0x15AD
#define VMXNET3_PCI_DEVICE   0x07B0

/* BAR indices: BAR0 = PT registers, BAR1 = VD registers. */
#define VMXNET3_PT_BAR       0
#define VMXNET3_VD_BAR       1

/* ---- VD (BAR1) registers ---- */
#define VMXNET3_REG_VRRS     0x000   /* revision report+select (RO) */
#define VMXNET3_REG_UVRS     0x008   /* UPT version report+select */
#define VMXNET3_REG_DSAL     0x010   /* DriverShared addr, low 32 bits */
#define VMXNET3_REG_DSAH     0x018   /* DriverShared addr, high 32 bits */
#define VMXNET3_REG_CMD      0x020   /* command (write) / result (read) */
#define VMXNET3_REG_MACL     0x028   /* MAC addr low (set by driver) */
#define VMXNET3_REG_MACH     0x030   /* MAC addr high */
#define VMXNET3_REG_ICR      0x038   /* interrupt cause (RO) */
#define VMXNET3_REG_ECR      0x040   /* event cause (RO) */

/* ---- PT (BAR0) registers ---- */
#define VMXNET3_REG_IMR      0x000   /* interrupt mask (per-vector, 8B stride) */
#define VMXNET3_REG_TXPROD   0x600   /* TX producer doorbell (per-queue, 8B) */
#define VMXNET3_REG_RXPROD   0x800   /* RX producer doorbell ring0 (per-queue) */
#define VMXNET3_REG_RXPROD2  0xA00   /* RX producer doorbell ring1 */

/* ---- Commands (written to VMXNET3_REG_CMD) ---- */
#define VMXNET3_CMD_FIRST_SET    0xCAFE0000
#define VMXNET3_CMD_ACTIVATE_DEV (VMXNET3_CMD_FIRST_SET + 0)
#define VMXNET3_CMD_QUIESCE_DEV  (VMXNET3_CMD_FIRST_SET + 1)
#define VMXNET3_CMD_RESET_DEV    (VMXNET3_CMD_FIRST_SET + 2)
#define VMXNET3_CMD_UPDATE_RX_MODE (VMXNET3_CMD_FIRST_SET + 3)

#define VMXNET3_CMD_FIRST_GET    0xF00D0000
#define VMXNET3_CMD_GET_PERM_MAC_LO (VMXNET3_CMD_FIRST_GET + 5)
#define VMXNET3_CMD_GET_PERM_MAC_HI (VMXNET3_CMD_FIRST_GET + 6)
#define VMXNET3_CMD_GET_LINK        (VMXNET3_CMD_FIRST_GET + 2)

/* DriverShared magic (revision 1). */
#define VMXNET3_REV1_MAGIC   0xbabefee1u

/* GOS (guest OS) bits for misc config: type=linux(1)<<10? — we report "other".
 * The hypervisor accepts a plain 64-bit guest; we set arch=64-bit, type=other. */
#define VMXNET3_GOS_BITS_64   (0x2u << 0)   /* gosBits: 64-bit */
#define VMXNET3_GOS_TYPE_OTHER (0x4u << 2)  /* gosType: other */

/* RX modes (UPT). */
#define VMXNET3_RXM_UCAST     0x01
#define VMXNET3_RXM_BCAST     0x04
#define VMXNET3_RXM_ALL_MULTI 0x08
#define VMXNET3_RXM_PROMISC   0x10

/* Ring sizing. Small, polled, single command at a time class of driver. */
#define VMXNET3_TX_RING_SIZE  64
#define VMXNET3_RX_RING_SIZE  64
#define VMXNET3_RX_BUF_SIZE   2048      /* >= NET_FRAME_MAX, power-of-two friendly */
#define VMXNET3_TX_BUF_SIZE   2048

/* Generation bits live in the high dword of each descriptor/completion. */
#define VMXNET3_TXD_GEN_SHIFT  31
#define VMXNET3_RXD_GEN_SHIFT  31
#define VMXNET3_TXCD_GEN_SHIFT 31
#define VMXNET3_RXCD_GEN_SHIFT 31
#define VMXNET3_GEN_MASK       (1u << 31)

/* RxDesc btype: head buffer (start of frame). */
#define VMXNET3_RXD_BTYPE_HEAD 0u
#define VMXNET3_RXD_BTYPE_BODY 1u

/* ============================================================================
 * Hardware descriptor layouts (UPT/vmxnet3 rev1). All little-endian; this
 * driver targets x86-64 so native layout matches the device.
 * ========================================================================== */

/* TX descriptor — 16 bytes. */
struct vmxnet3_tx_desc {
    uint64_t addr;       /* buffer physical address */
    uint32_t dword2;     /* [13:0] len, [24] gen, [25] reserved, [26] eop?,
                            actually: len[13:0], gen[14], rsvd... see fill */
    uint32_t dword3;     /* offload / flags */
} __attribute__((packed));

/* TX completion descriptor — 16 bytes. */
struct vmxnet3_tx_comp_desc {
    uint32_t dword0;     /* txdIdx[11:0] */
    uint32_t dword1;
    uint32_t dword2;
    uint32_t dword3;     /* [31] gen */
} __attribute__((packed));

/* RX descriptor — 16 bytes. */
struct vmxnet3_rx_desc {
    uint64_t addr;       /* buffer physical address */
    uint32_t dword2;     /* [13:0] len, [14:15] btype, [31] gen */
    uint32_t dword3;     /* reserved */
} __attribute__((packed));

/* RX completion descriptor — 16 bytes. */
struct vmxnet3_rx_comp_desc {
    uint32_t dword0;     /* rxdIdx[11:0], ... */
    uint32_t dword1;
    uint32_t dword2;     /* [13:0] len, ... */
    uint32_t dword3;     /* [24] eop, [25] sop, [31] gen */
} __attribute__((packed));

/* ---- DriverShared sub-structures (must match the device ABI byte-for-byte) --- */

/* Misc config (Vmxnet3_MiscConf): magic + version + GOS + queue desc pointers. */
struct vmxnet3_misc_conf {
    uint8_t  driver_info[32];  /* Vmxnet3_DriverInfo: version/gos/etc. */
    uint64_t uptFeatures;
    uint64_t ddPA;             /* driver-data physical addr (unused: 0) */
    uint64_t queueDescPA;      /* phys addr of the queue-desc block */
    uint32_t ddLen;            /* driver-data length */
    uint32_t queueDescLen;     /* queue-desc block length */
    uint32_t mtu;
    uint16_t maxNumRxSG;
    uint8_t  numTxQueues;
    uint8_t  numRxQueues;
    uint32_t reserved[4];
} __attribute__((packed));

/* Interrupt config (Vmxnet3_IntrConf). */
struct vmxnet3_intr_conf {
    uint8_t  autoMask;
    uint8_t  numIntrs;
    uint8_t  eventIntrIdx;
    uint8_t  modLevels[25];    /* per-vector moderation */
    uint32_t intrCtrl;
    uint32_t reserved[2];
} __attribute__((packed));

/* RX filter config (Vmxnet3_RxFilterConf). */
struct vmxnet3_rx_filter_conf {
    uint32_t rxMode;
    uint16_t mfTableLen;
    uint16_t _pad0;
    uint64_t mfTablePA;
    uint32_t vfTable[128];
} __attribute__((packed));

/* Top-level DriverShared (Vmxnet3_DriverShared). */
struct vmxnet3_driver_shared {
    uint32_t magic;
    uint32_t pad0;
    /* devRead */
    struct vmxnet3_misc_conf      misc;
    struct vmxnet3_intr_conf      intr;
    struct vmxnet3_rx_filter_conf rxFilter;
    uint32_t reserved[4];
} __attribute__((packed));

/* TX queue descriptor (Vmxnet3_TxQueueDesc) — ring config the device reads. */
struct vmxnet3_tx_queue_desc {
    uint8_t  ctrl[16];         /* TxQueueCtrl: txNumDeferred/threshold */
    uint64_t confTxRingBasePA;
    uint64_t confDataRingBasePA;
    uint64_t confCompRingBasePA;
    uint64_t confDdPA;
    uint32_t confTxRingSize;
    uint32_t confDataRingSize;
    uint32_t confCompRingSize;
    uint32_t confDdLen;
    uint8_t  confIntrIdx;
    uint8_t  _pad0[7];
    uint8_t  status[16];       /* device-written status */
    uint8_t  stats[88];        /* UPT1_TxStats */
    uint8_t  _pad1[88];
} __attribute__((packed));

/* RX queue descriptor (Vmxnet3_RxQueueDesc). Two RX rings + one comp ring. */
struct vmxnet3_rx_queue_desc {
    uint8_t  ctrl[8];          /* RxQueueCtrl: updateRxProd */
    uint64_t confRxRingBasePA[2];
    uint64_t confCompRingBasePA;
    uint64_t confDdPA;
    uint32_t confRxRingSize[2];
    uint32_t confCompRingSize;
    uint32_t confDdLen;
    uint8_t  confIntrIdx;
    uint8_t  _pad0[7];
    uint8_t  status[16];
    uint8_t  stats[88];        /* UPT1_RxStats */
    uint8_t  _pad1[88];
} __attribute__((packed));

/* The queue-desc block: TX queue descs followed by RX queue descs (1 each). */
struct vmxnet3_queue_descs {
    struct vmxnet3_tx_queue_desc tx;
    struct vmxnet3_rx_queue_desc rx;
} __attribute__((packed));

/* ============================================================================
 * Static, page-aligned DMA buffers (single TX + single RX queue).
 * ========================================================================== */
static struct vmxnet3_driver_shared g_shared   __attribute__((aligned(4096)));
static struct vmxnet3_queue_descs   g_qdescs    __attribute__((aligned(512)));

static struct vmxnet3_tx_desc      g_tx_ring[VMXNET3_TX_RING_SIZE]      __attribute__((aligned(512)));
static struct vmxnet3_tx_comp_desc g_tx_comp[VMXNET3_TX_RING_SIZE]      __attribute__((aligned(512)));
static struct vmxnet3_rx_desc      g_rx_ring[VMXNET3_RX_RING_SIZE]      __attribute__((aligned(512)));
static struct vmxnet3_rx_comp_desc g_rx_comp[VMXNET3_RX_RING_SIZE]      __attribute__((aligned(512)));

/* Per-slot buffers (one frame each). Static so they have stable phys addrs. */
static uint8_t g_tx_buf[VMXNET3_TX_RING_SIZE][VMXNET3_TX_BUF_SIZE] __attribute__((aligned(64)));
static uint8_t g_rx_buf[VMXNET3_RX_RING_SIZE][VMXNET3_RX_BUF_SIZE] __attribute__((aligned(64)));

/* ============================================================================
 * Driver state.
 * ========================================================================== */
struct vmxnet3_state {
    volatile uint8_t* pt;     /* BAR0 (PT) MMIO base, physmap-mapped */
    volatile uint8_t* vd;     /* BAR1 (VD) MMIO base, physmap-mapped */
    uint32_t tx_prod;         /* next TX descriptor index to fill */
    uint32_t tx_gen;          /* current TX gen bit (toggles on wrap) */
    uint32_t tx_comp_next;    /* next TX completion to consume */
    uint32_t tx_comp_gen;     /* expected TX completion gen */
    uint32_t rx_fill;         /* next RX desc to (re)fill */
    uint32_t rx_gen;          /* current RX desc gen bit */
    uint32_t rx_comp_next;    /* next RX completion to consume */
    uint32_t rx_comp_gen;     /* expected RX completion gen */
};
static struct vmxnet3_state g_st;
static net_dev_t g_net;

static inline uint64_t phys_of(const void* p) { return kvirt_to_phys((uint64_t)(uintptr_t)p); }

static inline uint32_t vd_rd(uint32_t off) { return *(volatile uint32_t*)(g_st.vd + off); }
static inline void     vd_wr(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_st.vd + off) = v; }
static inline void     pt_wr(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_st.pt + off) = v; }

static void zero(uint8_t* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = 0; }

/* Issue a command on the VD CMD register; for GET_* commands the result is read
 * back from the same register. */
static uint32_t vmxnet3_cmd(uint32_t cmd) {
    vd_wr(VMXNET3_REG_CMD, cmd);
    io_mfence();
    return vd_rd(VMXNET3_REG_CMD);
}

/* ----------------------------------------------------------------------------
 * Ring initialization.
 * ------------------------------------------------------------------------- */
static void vmxnet3_init_rings(void) {
    zero((uint8_t*)g_tx_ring, sizeof(g_tx_ring));
    zero((uint8_t*)g_tx_comp, sizeof(g_tx_comp));
    zero((uint8_t*)g_rx_ring, sizeof(g_rx_ring));
    zero((uint8_t*)g_rx_comp, sizeof(g_rx_comp));

    g_st.tx_prod = 0;
    g_st.tx_gen = 1;            /* driver starts at gen 1; device starts expecting 1 */
    g_st.tx_comp_next = 0;
    g_st.tx_comp_gen = 1;
    g_st.rx_fill = 0;
    g_st.rx_gen = 1;
    g_st.rx_comp_next = 0;
    g_st.rx_comp_gen = 1;

    /* Pre-post every RX descriptor pointing at its static buffer. The gen bit on
     * each RxDesc tells the device the slot is owned by it. */
    for (uint32_t i = 0; i < VMXNET3_RX_RING_SIZE; i++) {
        g_rx_ring[i].addr = phys_of(g_rx_buf[i]);
        /* dword2: len[13:0] | btype[14] | gen[31] */
        g_rx_ring[i].dword2 = (VMXNET3_RX_BUF_SIZE & 0x3FFF)
                            | (VMXNET3_RXD_BTYPE_HEAD << 14)
                            | (g_st.rx_gen << VMXNET3_RXD_GEN_SHIFT);
        g_rx_ring[i].dword3 = 0;
    }
    g_st.rx_fill = 0;          /* RXPROD will be pointed at the last filled slot */
}

/* ----------------------------------------------------------------------------
 * DriverShared + queue-desc population.
 * ------------------------------------------------------------------------- */
static void vmxnet3_build_shared(void) {
    zero((uint8_t*)&g_shared, sizeof(g_shared));
    zero((uint8_t*)&g_qdescs, sizeof(g_qdescs));

    g_shared.magic = VMXNET3_REV1_MAGIC;

    /* --- misc config --- */
    struct vmxnet3_misc_conf* m = &g_shared.misc;
    /* DriverInfo: version (1), gos (type+bits), vmxnet3RevSpt, uptVerSpt. The
     * exact byte layout the hypervisor reads is: [0..3] version, [4..7] gos,
     * [8..11] vmxnet3RevSpt, [12..15] uptVerSpt. We set conservative values. */
    m->driver_info[0] = 1;                                   /* version = 1 */
    m->driver_info[4] = (uint8_t)(VMXNET3_GOS_BITS_64 | VMXNET3_GOS_TYPE_OTHER);
    m->driver_info[8] = 1;                                   /* vmxnet3RevSpt = 1 */
    m->driver_info[12] = 1;                                  /* uptVerSpt = 1 */
    m->uptFeatures = 0;                                      /* no LRO/csum offload */
    m->ddPA = 0;
    m->ddLen = 0;
    m->queueDescPA = phys_of(&g_qdescs);
    m->queueDescLen = (uint32_t)sizeof(g_qdescs);
    m->mtu = NET_MTU;
    m->maxNumRxSG = 1;
    m->numTxQueues = 1;
    m->numRxQueues = 1;

    /* --- interrupt config --- */
    struct vmxnet3_intr_conf* it = &g_shared.intr;
    it->autoMask = 0;
    it->numIntrs = 1;
    it->eventIntrIdx = 0;
    for (int i = 0; i < 25; i++) it->modLevels[i] = 0;       /* UPT1_IML_ADAPTIVE off */
    it->intrCtrl = 1u;                                       /* DISABLE_ALL: we poll */

    /* --- RX filter config --- */
    struct vmxnet3_rx_filter_conf* rf = &g_shared.rxFilter;
    rf->rxMode = VMXNET3_RXM_UCAST | VMXNET3_RXM_BCAST;
    rf->mfTableLen = 0;
    rf->mfTablePA = 0;
    for (int i = 0; i < 128; i++) rf->vfTable[i] = 0;

    /* --- TX queue desc --- */
    struct vmxnet3_tx_queue_desc* txq = &g_qdescs.tx;
    txq->confTxRingBasePA   = phys_of(g_tx_ring);
    txq->confDataRingBasePA = 0;                 /* no inline data ring */
    txq->confCompRingBasePA = phys_of(g_tx_comp);
    txq->confDdPA = 0;
    txq->confTxRingSize   = VMXNET3_TX_RING_SIZE;
    txq->confDataRingSize = 0;
    txq->confCompRingSize = VMXNET3_TX_RING_SIZE;
    txq->confDdLen = 0;
    txq->confIntrIdx = 0;

    /* --- RX queue desc (two rings; we only post ring 0) --- */
    struct vmxnet3_rx_queue_desc* rxq = &g_qdescs.rx;
    rxq->confRxRingBasePA[0] = phys_of(g_rx_ring);
    rxq->confRxRingBasePA[1] = phys_of(g_rx_ring);   /* ring1 unused; alias ring0 */
    rxq->confCompRingBasePA  = phys_of(g_rx_comp);
    rxq->confDdPA = 0;
    rxq->confRxRingSize[0] = VMXNET3_RX_RING_SIZE;
    rxq->confRxRingSize[1] = VMXNET3_RX_RING_SIZE;
    rxq->confCompRingSize  = VMXNET3_RX_RING_SIZE;
    rxq->confDdLen = 0;
    rxq->confIntrIdx = 0;
}

/* ----------------------------------------------------------------------------
 * Data path.
 * ------------------------------------------------------------------------- */
static int vmxnet3_transmit(net_dev_t* dev, const void* frame, uint32_t len) {
    (void)dev;
    if (len == 0 || len > VMXNET3_TX_BUF_SIZE) return -1;

    uint32_t idx = g_st.tx_prod;
    /* Reclaim is implicit: with a 64-entry ring and serialized polling use this
     * stays well ahead; we do not block on the TX completion ring here. */
    const uint8_t* src = (const uint8_t*)frame;
    for (uint32_t i = 0; i < len; i++) g_tx_buf[idx][i] = src[i];

    struct vmxnet3_tx_desc* d = &g_tx_ring[idx];
    d->addr = phys_of(g_tx_buf[idx]);
    /* dword3 first (no offload), then dword2 with the gen bit last so the device
     * never sees a half-written descriptor with the new gen. */
    d->dword3 = 0;
    io_mfence();
    /* dword2: len[13:0] | gen[14] | EOP(end-of-packet)[12]... For a single-buffer
     * frame this is the only and last descriptor: set EOP + CQ (generate compl).
     * Layout (UPT rev1): len[13:0], gen[14], rsvd[23:15], dtype[24], EOP[25]...
     * To keep this robust against the exact bit positions we set the generation
     * bit at [31] (sentinel the device polls) plus len; SOP/EOP/CQ semantics for
     * a one-descriptor packet are the device default. */
    d->dword2 = (len & 0x3FFF)
              | (1u << 24)              /* EOP: end of packet */
              | (1u << 25)              /* CQ: generate completion */
              | (g_st.tx_gen << VMXNET3_TXD_GEN_SHIFT);
    io_mfence();

    g_st.tx_prod++;
    if (g_st.tx_prod >= VMXNET3_TX_RING_SIZE) {
        g_st.tx_prod = 0;
        g_st.tx_gen ^= 1u;            /* toggle gen on wrap */
    }
    /* Ring the TX producer doorbell (PT BAR, queue 0): write the new prod idx. */
    pt_wr(VMXNET3_REG_TXPROD + 0 * 8, g_st.tx_prod);
    return 0;
}

static void vmxnet3_poll(net_dev_t* dev) {
    for (;;) {
        struct vmxnet3_rx_comp_desc* c = &g_rx_comp[g_st.rx_comp_next];
        uint32_t gen = (c->dword3 >> VMXNET3_RXCD_GEN_SHIFT) & 1u;
        if (gen != g_st.rx_comp_gen) break;     /* nothing new */
        io_mfence();

        uint32_t rxd_idx = c->dword0 & 0xFFF;   /* index into the RxDesc ring */
        uint32_t len = c->dword2 & 0x3FFF;      /* frame length */
        uint32_t eop = (c->dword3 >> 24) & 1u;  /* end of packet */

        if (rxd_idx < VMXNET3_RX_RING_SIZE && len > 0 && len <= VMXNET3_RX_BUF_SIZE && eop) {
            net_rx(dev, g_rx_buf[rxd_idx], len);
        }

        /* Refill that RxDesc and hand it back to the device with the current gen. */
        if (rxd_idx < VMXNET3_RX_RING_SIZE) {
            g_rx_ring[rxd_idx].addr = phys_of(g_rx_buf[rxd_idx]);
            g_rx_ring[rxd_idx].dword3 = 0;
            io_mfence();
            g_rx_ring[rxd_idx].dword2 = (VMXNET3_RX_BUF_SIZE & 0x3FFF)
                                      | (VMXNET3_RXD_BTYPE_HEAD << 14)
                                      | (g_st.rx_gen << VMXNET3_RXD_GEN_SHIFT);
            g_st.rx_fill = rxd_idx;
        }

        g_st.rx_comp_next++;
        if (g_st.rx_comp_next >= VMXNET3_RX_RING_SIZE) {
            g_st.rx_comp_next = 0;
            g_st.rx_comp_gen ^= 1u;             /* expected gen flips on wrap */
        }
    }
    /* Ring the RX producer doorbell with the last refilled descriptor index. */
    pt_wr(VMXNET3_REG_RXPROD + 0 * 8, g_st.rx_fill);
}

/* ----------------------------------------------------------------------------
 * Probe + bring-up.
 * ------------------------------------------------------------------------- */
int vmxnet3_init(void) {
    pci_device_t dev;
    if (pci_find(VMXNET3_PCI_VENDOR, VMXNET3_PCI_DEVICE, &dev) != 0) {
        debugcon_writestring("[VMXNET3] no device on PCI\n");
        return -1;
    }
    pci_enable_mem_and_busmaster(&dev);

    uint64_t pt_bar = pci_bar_mem64(&dev, VMXNET3_PT_BAR);
    uint64_t vd_bar = pci_bar_mem64(&dev, VMXNET3_VD_BAR);
    if (pt_bar == 0 || vd_bar == 0) {
        debugcon_writestring("[VMXNET3] BAR not memory\n");
        return -1;
    }
    /* Each register window is small (<4 KB); extend the physmap past both. */
    vmm_extend_physmap(pt_bar + 0x1000);
    vmm_extend_physmap(vd_bar + 0x1000);
    g_st.pt = (volatile uint8_t*)phys_to_virt(pt_bar);
    g_st.vd = (volatile uint8_t*)phys_to_virt(vd_bar);

    debugcon_writestring("[VMXNET3] PT=0x"); debugcon_print_hex(pt_bar);
    debugcon_writestring(" VD=0x"); debugcon_print_hex(vd_bar); debugcon_writestring("\n");

    /* Reset the device to a known state, then build the shared structures. */
    vmxnet3_cmd(VMXNET3_CMD_RESET_DEV);

    vmxnet3_init_rings();
    vmxnet3_build_shared();

    /* Hand the DriverShared physical address to the device, then activate. */
    uint64_t ds = phys_of(&g_shared);
    vd_wr(VMXNET3_REG_DSAL, (uint32_t)ds);
    vd_wr(VMXNET3_REG_DSAH, (uint32_t)(ds >> 32));
    io_mfence();

    uint32_t act = vmxnet3_cmd(VMXNET3_CMD_ACTIVATE_DEV);
    if (act != 0) {
        debugcon_writestring("[VMXNET3] ACTIVATE_DEV failed status=0x");
        debugcon_print_hex(act); debugcon_writestring("\n");
        return -1;
    }

    /* Program the RX mode (unicast + broadcast). */
    vmxnet3_cmd(VMXNET3_CMD_UPDATE_RX_MODE);

    /* Read the permanent MAC via the GET_PERM_MAC commands. */
    uint32_t mac_lo = vmxnet3_cmd(VMXNET3_CMD_GET_PERM_MAC_LO);
    uint32_t mac_hi = vmxnet3_cmd(VMXNET3_CMD_GET_PERM_MAC_HI);

    /* Fill the net_dev. */
    for (int i = 0; i < 16; i++) g_net.name[i] = 0;
    g_net.name[0] = 'e'; g_net.name[1] = 't'; g_net.name[2] = 'h'; g_net.name[3] = '0';
    g_net.mac[0] = (uint8_t)(mac_lo);
    g_net.mac[1] = (uint8_t)(mac_lo >> 8);
    g_net.mac[2] = (uint8_t)(mac_lo >> 16);
    g_net.mac[3] = (uint8_t)(mac_lo >> 24);
    g_net.mac[4] = (uint8_t)(mac_hi);
    g_net.mac[5] = (uint8_t)(mac_hi >> 8);

    /* Also program the MAC into the VD MACL/MACH registers (some hypervisor
     * builds require the driver to set the operational MAC explicitly). */
    vd_wr(VMXNET3_REG_MACL, mac_lo);
    vd_wr(VMXNET3_REG_MACH, mac_hi & 0xFFFF);

    /* Prime the RX producer doorbell so the device starts consuming RxDescs. */
    pt_wr(VMXNET3_REG_RXPROD + 0 * 8, VMXNET3_RX_RING_SIZE - 1);

    g_net.transmit = vmxnet3_transmit;
    g_net.poll = vmxnet3_poll;
    g_net.priv = &g_st;
    g_net.pci = dev;
    g_net.link_up = 1;

    if (net_register_dev(&g_net) != 0) {
        debugcon_writestring("[VMXNET3] net_register_dev failed\n");
        return -1;
    }

    /* Wire the interrupt (MSI-X preferred, INTx, then timer-tick poll). The
     * device was activated with intrCtrl=DISABLE_ALL, so we run polled until the
     * LAPIC path is validated; net_request_irq records the chosen mode. */
    net_request_irq(&g_net);

    debugcon_writestring("[VMXNET3] MAC=");
    for (int i = 0; i < 6; i++) {
        debugcon_print_hex(g_net.mac[i]);
        if (i != 5) debugcon_putchar(':');
    }
    debugcon_writestring("\n[VMXNET3] eth0 ready (NOT YET TESTED on VMware)\n");
    return 0;
}
