/*
 * nvme.c — [M22] NVMe block driver.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Minimal NVMe: find the controller over PCI, map its register BAR (64-bit MMIO)
 * through the physmap, reset+enable it with a single admin queue pair, IDENTIFY
 * the controller and namespace 1, create one I/O queue pair, and read/write
 * sectors with polled DMA on a single command at a time. Every DMA structure is
 * a static page-aligned kernel buffer; physical addresses come from
 * kvirt_to_phys. Transfers bounce through a shared 64 KB buffer described by a
 * PRP1 + PRP-list page. Registers the namespace as block device "nvme0n1".
 */
#include "nvme.h"
#include "pci.h"
#include "io.h"
#include "block.h"
#include "debugcon.h"
#include "vmm.h"        // phys_to_virt / kvirt_to_phys / vmm_extend_physmap
#include <stddef.h>

// ---- Controller registers (BAR0 MMIO) ----
#define NVME_CAP   0x00   // 64-bit capabilities
#define NVME_VS    0x08
#define NVME_CC    0x14   // controller configuration
#define NVME_CSTS  0x1C   // controller status
#define NVME_AQA   0x24   // admin queue attributes
#define NVME_ASQ   0x28   // admin submission queue base (64-bit)
#define NVME_ACQ   0x30   // admin completion queue base (64-bit)
#define NVME_DBS   0x1000 // doorbell registers start here

#define CC_EN      (1u << 0)
#define CSTS_RDY   (1u << 0)
#define CSTS_CFS   (1u << 1)  // controller fatal status

// ---- Admin / NVM opcodes ----
#define ADMIN_CREATE_IO_SQ  0x01
#define ADMIN_CREATE_IO_CQ  0x05
#define ADMIN_IDENTIFY      0x06
#define NVM_WRITE           0x01
#define NVM_READ            0x02

#define QDEPTH      8          // entries per queue (>= 2)
#define IO_QID      1
#define NVME_NSID   1

// Static, page-aligned DMA structures. SQ entry = 64 B, CQ entry = 16 B.
static uint8_t g_asq[QDEPTH * 64] __attribute__((aligned(4096)));
static uint8_t g_acq[QDEPTH * 16] __attribute__((aligned(4096)));
static uint8_t g_iosq[QDEPTH * 64] __attribute__((aligned(4096)));
static uint8_t g_iocq[QDEPTH * 16] __attribute__((aligned(4096)));
static uint8_t g_idbuf[4096]       __attribute__((aligned(4096)));
static uint8_t g_prplist[4096]     __attribute__((aligned(4096)));
static uint8_t g_dma[64 * 1024]    __attribute__((aligned(4096)));
#define DMA_PAGES (sizeof(g_dma) / 4096)

struct nvme_queue {
    uint8_t* sq;
    uint8_t* cq;
    uint32_t sq_tail;
    uint32_t cq_head;
    uint32_t phase;          // expected CQ phase bit
    volatile uint32_t* sq_db;
    volatile uint32_t* cq_db;
};

static volatile uint8_t* g_bar;
static uint32_t g_dstrd_bytes;       // doorbell stride in bytes
static struct nvme_queue g_admin, g_io;
static uint16_t g_cid;

static block_dev_t g_dev;
static uint32_t g_sector_size;
static uint64_t g_sector_count;

static inline uint64_t phys_of(const void* p) { return kvirt_to_phys((uint64_t)(uintptr_t)p); }
static inline uint32_t rd32(uint32_t off) { return *(volatile uint32_t*)(g_bar + off); }
static inline void     wr32(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_bar + off) = v; }
static inline uint64_t rd64(uint32_t off) {
    return (uint64_t)rd32(off) | ((uint64_t)rd32(off + 4) << 32);
}
static inline void wr64(uint32_t off, uint64_t v) {
    wr32(off, (uint32_t)v); wr32(off + 4, (uint32_t)(v >> 32));
}
static void zero(uint8_t* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = 0; }

// Doorbell offsets: SQ tail of queue y at DBS + (2y)*stride, CQ head at +1.
static void queue_doorbells(struct nvme_queue* q, int qid) {
    q->sq_db = (volatile uint32_t*)(g_bar + NVME_DBS + (2 * qid) * g_dstrd_bytes);
    q->cq_db = (volatile uint32_t*)(g_bar + NVME_DBS + (2 * qid + 1) * g_dstrd_bytes);
}

// Submit a 64-byte command (16 dwords) and poll its completion. Returns the
// 15-bit NVMe status field (0 == success), or 0xFFFF on timeout.
static uint16_t nvme_submit(struct nvme_queue* q, const uint32_t* cmd) {
    uint8_t* slot = q->sq + q->sq_tail * 64;
    for (int i = 0; i < 16; i++) ((uint32_t*)slot)[i] = cmd[i];
    q->sq_tail = (q->sq_tail + 1) % QDEPTH;
    io_mfence();
    *q->sq_db = q->sq_tail;

    volatile uint32_t* cqe = (volatile uint32_t*)(q->cq + q->cq_head * 16);
    for (volatile uint64_t i = 0; ; i++) {
        uint32_t dw3 = cqe[3];
        if (((dw3 >> 16) & 1u) == q->phase) {
            uint16_t status = (uint16_t)((dw3 >> 17) & 0x7FFF);
            q->cq_head = (q->cq_head + 1) % QDEPTH;
            if (q->cq_head == 0) q->phase ^= 1u;  // wrapped -> flip expected phase
            io_mfence();
            *q->cq_db = q->cq_head;
            return status;
        }
        if (i > 50000000ull) return 0xFFFF;
    }
}

// Build an IDENTIFY command (CNS in CDW10) into a 4 KB PRP1 buffer.
static uint16_t nvme_identify(uint32_t cns, uint32_t nsid, const void* buf) {
    uint32_t c[16]; for (int i = 0; i < 16; i++) c[i] = 0;
    c[0] = ADMIN_IDENTIFY | ((uint32_t)(g_cid++) << 16);
    c[1] = nsid;
    uint64_t prp1 = phys_of(buf);
    c[6] = (uint32_t)prp1; c[7] = (uint32_t)(prp1 >> 32);
    c[10] = cns;
    return nvme_submit(&g_admin, c);
}

// Fill PRP1/PRP2 for a transfer of nbytes from the page-aligned g_dma buffer.
static void fill_prp(uint32_t* c, uint32_t nbytes) {
    uint64_t base = phys_of(g_dma);
    c[6] = (uint32_t)base; c[7] = (uint32_t)(base >> 32);    // PRP1 = page 0
    if (nbytes <= 4096) {
        c[8] = 0; c[9] = 0;
    } else if (nbytes <= 8192) {
        uint64_t p2 = base + 4096;
        c[8] = (uint32_t)p2; c[9] = (uint32_t)(p2 >> 32);    // PRP2 = page 1
    } else {
        uint32_t pages = (nbytes + 4095) / 4096;
        uint64_t* list = (uint64_t*)g_prplist;
        for (uint32_t i = 1; i < pages; i++) list[i - 1] = base + (uint64_t)i * 4096;
        uint64_t lp = phys_of(g_prplist);
        c[8] = (uint32_t)lp; c[9] = (uint32_t)(lp >> 32);    // PRP2 = PRP list
    }
}

static int nvme_rw(uint64_t lba, uint8_t* buf, uint32_t count, int write) {
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > DMA_PAGES * 4096 / g_sector_size) chunk = DMA_PAGES * 4096 / g_sector_size;
        uint32_t nbytes = chunk * g_sector_size;
        if (write) for (uint32_t i = 0; i < nbytes; i++) g_dma[i] = buf[done * g_sector_size + i];

        uint32_t c[16]; for (int i = 0; i < 16; i++) c[i] = 0;
        c[0] = (write ? NVM_WRITE : NVM_READ) | ((uint32_t)(g_cid++) << 16);
        c[1] = NVME_NSID;
        fill_prp(c, nbytes);
        uint64_t slba = lba + done;
        c[10] = (uint32_t)slba; c[11] = (uint32_t)(slba >> 32);
        c[12] = (chunk - 1) & 0xFFFF;          // NLB = (blocks - 1)
        if (nvme_submit(&g_io, c) != 0) return -1;

        if (!write) for (uint32_t i = 0; i < nbytes; i++) buf[done * g_sector_size + i] = g_dma[i];
        done += chunk;
    }
    return (int)count;
}

static int nvme_read(block_dev_t* dev, uint64_t lba, void* buf, uint32_t count) {
    (void)dev; return nvme_rw(lba, (uint8_t*)buf, count, 0);
}
static int nvme_write(block_dev_t* dev, uint64_t lba, const void* buf, uint32_t count) {
    (void)dev; return nvme_rw(lba, (uint8_t*)buf, count, 1);
}

int nvme_init(void) {
    pci_device_t dev;
    if (pci_find_class(0x01, 0x08, 0x02, &dev) != 0) {
        debugcon_writestring("[NVME] no controller on PCI\n");
        return -1;
    }
    pci_enable_mem_and_busmaster(&dev);
    uint64_t bar = pci_bar_mem64(&dev, 0);
    if (bar == 0) { debugcon_writestring("[NVME] BAR0 not memory\n"); return -1; }
    vmm_extend_physmap(bar + 0x2000);
    g_bar = (volatile uint8_t*)phys_to_virt(bar);

    uint64_t cap = rd64(NVME_CAP);
    g_dstrd_bytes = 4u << ((cap >> 32) & 0xF);   // CAP.DSTRD
    debugcon_writestring("[NVME] BAR=0x"); debugcon_print_hex(bar);
    debugcon_writestring(" CAP=0x"); debugcon_print_hex(cap); debugcon_writestring("\n");

    // Disable the controller, wait for not-ready.
    wr32(NVME_CC, rd32(NVME_CC) & ~CC_EN);
    for (volatile uint64_t i = 0; (rd32(NVME_CSTS) & CSTS_RDY); i++)
        if (i > 50000000ull) { debugcon_writestring("[NVME] disable timeout\n"); return -1; }

    // Admin queue pair.
    g_admin.sq = g_asq; g_admin.cq = g_acq;
    g_admin.sq_tail = g_admin.cq_head = 0; g_admin.phase = 1;
    zero(g_asq, sizeof(g_asq)); zero(g_acq, sizeof(g_acq));
    queue_doorbells(&g_admin, 0);
    wr32(NVME_AQA, ((QDEPTH - 1) << 16) | (QDEPTH - 1));
    wr64(NVME_ASQ, phys_of(g_asq));
    wr64(NVME_ACQ, phys_of(g_acq));

    // Enable: CSS=0 (NVM), MPS=0 (4 KB), IOSQES=6 (64 B), IOCQES=4 (16 B), EN=1.
    uint32_t cc = (6u << 16) | (4u << 20) | CC_EN;
    wr32(NVME_CC, cc);
    for (volatile uint64_t i = 0; !(rd32(NVME_CSTS) & CSTS_RDY); i++) {
        if (rd32(NVME_CSTS) & CSTS_CFS) { debugcon_writestring("[NVME] fatal on enable\n"); return -1; }
        if (i > 50000000ull) { debugcon_writestring("[NVME] enable timeout\n"); return -1; }
    }

    // IDENTIFY controller (CNS=1) — sanity, then IDENTIFY namespace 1 (CNS=0).
    if (nvme_identify(1, 0, g_idbuf) != 0) { debugcon_writestring("[NVME] identify ctrl failed\n"); return -1; }
    if (nvme_identify(0, NVME_NSID, g_idbuf) != 0) { debugcon_writestring("[NVME] identify ns failed\n"); return -1; }

    // Parse IDENTIFY namespace: NSZE @0 (8 B), FLBAS @26, LBAF[] @128 (4 B each).
    uint64_t nsze = 0;
    for (int i = 0; i < 8; i++) nsze |= (uint64_t)g_idbuf[i] << (8 * i);
    uint8_t flbas = g_idbuf[26] & 0xF;
    uint32_t lbaf = 0;
    for (int i = 0; i < 4; i++) lbaf |= (uint32_t)g_idbuf[128 + flbas * 4 + i] << (8 * i);
    uint8_t lbads = (uint8_t)((lbaf >> 16) & 0xFF);     // LBA data size = 2^lbads
    g_sector_size = (lbads >= 9 && lbads <= 12) ? (1u << lbads) : 512;
    g_sector_count = nsze;
    if (nsze == 0) { debugcon_writestring("[NVME] empty namespace\n"); return -1; }

    // Create one I/O queue pair (CQ first, then SQ referencing it).
    g_io.sq = g_iosq; g_io.cq = g_iocq;
    g_io.sq_tail = g_io.cq_head = 0; g_io.phase = 1;
    zero(g_iosq, sizeof(g_iosq)); zero(g_iocq, sizeof(g_iocq));
    queue_doorbells(&g_io, IO_QID);
    {
        uint32_t c[16]; for (int i = 0; i < 16; i++) c[i] = 0;
        c[0] = ADMIN_CREATE_IO_CQ | ((uint32_t)(g_cid++) << 16);
        uint64_t cqp = phys_of(g_iocq);
        c[6] = (uint32_t)cqp; c[7] = (uint32_t)(cqp >> 32);
        c[10] = ((QDEPTH - 1) << 16) | IO_QID;
        c[11] = 1u;                       // PC=1 (physically contiguous), IEN=0
        if (nvme_submit(&g_admin, c) != 0) { debugcon_writestring("[NVME] create IOCQ failed\n"); return -1; }
    }
    {
        uint32_t c[16]; for (int i = 0; i < 16; i++) c[i] = 0;
        c[0] = ADMIN_CREATE_IO_SQ | ((uint32_t)(g_cid++) << 16);
        uint64_t sqp = phys_of(g_iosq);
        c[6] = (uint32_t)sqp; c[7] = (uint32_t)(sqp >> 32);
        c[10] = ((QDEPTH - 1) << 16) | IO_QID;
        c[11] = ((uint32_t)IO_QID << 16) | 1u;  // CQID=IO_QID, PC=1
        if (nvme_submit(&g_admin, c) != 0) { debugcon_writestring("[NVME] create IOSQ failed\n"); return -1; }
    }

    g_dev.name = "nvme0n1";
    g_dev.sector_size = g_sector_size;
    g_dev.sector_count = g_sector_count;
    g_dev.read = nvme_read;
    g_dev.write = nvme_write;
    if (block_register(&g_dev) != 0) { debugcon_writestring("[NVME] block_register failed\n"); return -1; }

    debugcon_writestring("[NVME] ready nvme0n1 sectsz=0x"); debugcon_print_hex(g_sector_size);
    debugcon_writestring(" sectors=0x"); debugcon_print_hex(g_sector_count); debugcon_writestring("\n");
    return 0;
}
