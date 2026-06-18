/*
 * ahci.c — [M21] AHCI (SATA) block driver.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Minimal AHCI: probe the HBA over PCI, bring up EVERY established SATA port,
 * IDENTIFY each for its capacity, and read/write sectors with polled DMA on a
 * single command slot. Registers each disk as "sda", "sdb", ... so the mount
 * logic can try each (on VMware the boot ESP and the data disk are both SATA, so
 * we must not assume the first port is the data disk). DMA structures are static,
 * aligned kernel buffers; physical addresses come from kvirt_to_phys. Transfers
 * bounce through a shared static buffer (commands are serialized + polled).
 */
#include "ahci.h"
#include "pci.h"
#include "io.h"
#include "block.h"
#include "debugcon.h"
#include "vmm.h"        // phys_to_virt / kvirt_to_phys / vmm_extend_physmap
#include <stddef.h>

// ---- HBA / port registers ----
#define HBA_GHC   0x04
#define HBA_PI    0x0C
#define GHC_AE    (1u << 31)

#define PORT_BASE(p)  (0x100 + (p) * 0x80)
#define PxCLB   0x00
#define PxCLBU  0x04
#define PxFB    0x08
#define PxFBU   0x0C
#define PxIS    0x10
#define PxCMD   0x18
#define PxTFD   0x20
#define PxSIG   0x24
#define PxSSTS  0x28
#define PxSERR  0x30
#define PxCI    0x38

#define CMD_ST   (1u << 0)
#define CMD_FRE  (1u << 4)
#define CMD_FR   (1u << 14)
#define CMD_CR   (1u << 15)
#define TFD_BSY  (1u << 7)
#define TFD_DRQ  (1u << 3)
#define TFD_ERR  (1u << 0)
#define IS_TFES  (1u << 30)
#define SIG_SATA 0x00000101u

#define ATA_READ_DMA_EX   0x25
#define ATA_WRITE_DMA_EX  0x35
#define ATA_IDENTIFY      0xEC

#define AHCI_MAX_DISKS 4

// Per-port command list + received-FIS (the HBA reads these continuously).
static uint8_t g_clb[AHCI_MAX_DISKS][1024] __attribute__((aligned(1024)));
static uint8_t g_fis[AHCI_MAX_DISKS][256]  __attribute__((aligned(256)));
// Shared (one command runs at a time): command table + DMA bounce.
static uint8_t g_ctba[256]      __attribute__((aligned(128)));
static uint8_t g_dma[64 * 1024] __attribute__((aligned(4096)));
#define DMA_SECTORS (sizeof(g_dma) / 512)

struct ahci_disk { int port; int idx; block_dev_t dev; };
static struct ahci_disk g_disks[AHCI_MAX_DISKS];
static int g_ndisks;
static volatile uint8_t* g_abar;

static inline uint64_t phys_of(const void* p) { return kvirt_to_phys((uint64_t)(uintptr_t)p); }
static inline uint32_t hr(uint32_t off) { return *(volatile uint32_t*)(g_abar + off); }
static inline void     hw(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_abar + off) = v; }
static inline uint32_t pr(int port, uint32_t r) { return hr(PORT_BASE(port) + r); }
static inline void     pw(int port, uint32_t r, uint32_t v) { hw(PORT_BASE(port) + r, v); }
static void zero(uint8_t* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = 0; }

static void port_stop(int port) {
    pw(port, PxCMD, pr(port, PxCMD) & ~CMD_ST);
    pw(port, PxCMD, pr(port, PxCMD) & ~CMD_FRE);
    for (volatile int i = 0; i < 1000000; i++)
        if (!(pr(port, PxCMD) & (CMD_CR | CMD_FR))) break;
}
static void port_start(int port) {
    while (pr(port, PxCMD) & CMD_CR) { }
    pw(port, PxCMD, pr(port, PxCMD) | CMD_FRE);
    pw(port, PxCMD, pr(port, PxCMD) | CMD_ST);
}
static void port_rebase(int port, int idx) {
    port_stop(port);
    uint64_t clb = phys_of(g_clb[idx]), fb = phys_of(g_fis[idx]);
    pw(port, PxCLB,  (uint32_t)clb);  pw(port, PxCLBU, (uint32_t)(clb >> 32));
    pw(port, PxFB,   (uint32_t)fb);   pw(port, PxFBU,  (uint32_t)(fb >> 32));
    zero(g_clb[idx], 1024); zero(g_fis[idx], 256);
    pw(port, PxSERR, 0xFFFFFFFFu); pw(port, PxIS, 0xFFFFFFFFu);
    port_start(port);
}

// Issue one ATA command on a disk's port/slot, single PRDT over g_dma.
static int ahci_cmd(struct ahci_disk* d, uint8_t cmd, uint64_t lba,
                    uint32_t count, uint32_t nbytes, int write) {
    int port = d->port;
    pw(port, PxIS, 0xFFFFFFFFu);

    uint32_t* hdr = (uint32_t*)g_clb[d->idx];   // command header, slot 0
    uint64_t ctba = phys_of(g_ctba);
    hdr[0] = 5u | ((uint32_t)(write ? 1u : 0u) << 6) | (1u << 16); // CFL=5, W, PRDTL=1
    hdr[1] = 0;
    hdr[2] = (uint32_t)ctba; hdr[3] = (uint32_t)(ctba >> 32);
    hdr[4] = hdr[5] = hdr[6] = hdr[7] = 0;

    zero(g_ctba, sizeof(g_ctba));
    uint8_t* cfis = g_ctba;                     // H2D register FIS
    cfis[0] = 0x27; cfis[1] = 0x80; cfis[2] = cmd; cfis[3] = 0;
    cfis[4] = (uint8_t)lba; cfis[5] = (uint8_t)(lba >> 8); cfis[6] = (uint8_t)(lba >> 16);
    cfis[7] = 0x40;                             // LBA mode
    cfis[8] = (uint8_t)(lba >> 24); cfis[9] = (uint8_t)(lba >> 32); cfis[10] = (uint8_t)(lba >> 40);
    cfis[12] = (uint8_t)count; cfis[13] = (uint8_t)(count >> 8);

    uint32_t* prdt = (uint32_t*)(g_ctba + 0x80);
    uint64_t dba = phys_of(g_dma);
    prdt[0] = (uint32_t)dba; prdt[1] = (uint32_t)(dba >> 32);
    prdt[2] = 0; prdt[3] = (nbytes - 1) & 0x3FFFFF;

    for (volatile int i = 0; i < 2000000; i++)
        if (!(pr(port, PxTFD) & (TFD_BSY | TFD_DRQ))) break;
    io_mfence();
    pw(port, PxCI, 1u << 0);
    for (volatile uint64_t i = 0; ; i++) {
        if (!(pr(port, PxCI) & (1u << 0))) break;
        if (pr(port, PxIS) & IS_TFES) return -1;
        if (i > 50000000ull) return -1;
    }
    if (pr(port, PxTFD) & TFD_ERR) return -1;
    return 0;
}

static struct ahci_disk* disk_of(block_dev_t* dev) {
    for (int i = 0; i < g_ndisks; i++) if (&g_disks[i].dev == dev) return &g_disks[i];
    return NULL;
}

static int ahci_rw(struct ahci_disk* d, uint64_t lba, uint8_t* buf, uint32_t count, int write) {
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done; if (chunk > DMA_SECTORS) chunk = DMA_SECTORS;
        uint32_t nbytes = chunk * 512;
        if (write) for (uint32_t i = 0; i < nbytes; i++) g_dma[i] = buf[done * 512 + i];
        if (ahci_cmd(d, write ? ATA_WRITE_DMA_EX : ATA_READ_DMA_EX, lba + done, chunk, nbytes, write) != 0)
            return -1;
        if (!write) for (uint32_t i = 0; i < nbytes; i++) buf[done * 512 + i] = g_dma[i];
        done += chunk;
    }
    return (int)count;
}

static int ahci_read(block_dev_t* dev, uint64_t lba, void* buf, uint32_t count) {
    struct ahci_disk* d = disk_of(dev); if (!d) return -1;
    return ahci_rw(d, lba, (uint8_t*)buf, count, 0);
}
static int ahci_write(block_dev_t* dev, uint64_t lba, const void* buf, uint32_t count) {
    struct ahci_disk* d = disk_of(dev); if (!d) return -1;
    return ahci_rw(d, lba, (uint8_t*)buf, count, 1);
}

static uint64_t ahci_identify(struct ahci_disk* d) {
    if (ahci_cmd(d, ATA_IDENTIFY, 0, 0, 512, 0) != 0) return 0;
    uint64_t lba48 = 0;
    for (int i = 0; i < 8; i++) lba48 |= (uint64_t)g_dma[200 + i] << (8 * i);
    if (lba48) return lba48;
    uint32_t lba28 = 0;
    for (int i = 0; i < 4; i++) lba28 |= (uint32_t)g_dma[120 + i] << (8 * i);
    return lba28;
}

int ahci_init(void) {
    pci_device_t dev;
    if (pci_find_class(0x01, 0x06, 0x01, &dev) != 0) {
        debugcon_writestring("[AHCI] no controller on PCI\n");
        return -1;
    }
    pci_enable_mem_and_busmaster(&dev);
    uint32_t abar_phys = pci_bar_mem(&dev, 5);
    if (abar_phys == 0) { debugcon_writestring("[AHCI] BAR5 not memory\n"); return -1; }
    vmm_extend_physmap((uint64_t)abar_phys + 0x2000);
    g_abar = (volatile uint8_t*)phys_to_virt(abar_phys);

    hw(HBA_GHC, hr(HBA_GHC) | GHC_AE);
    uint32_t pi = hr(HBA_PI);
    debugcon_writestring("[AHCI] HBA ABAR=0x"); debugcon_print_hex(abar_phys);
    debugcon_writestring(" PI=0x"); debugcon_print_hex(pi); debugcon_writestring("\n");

    static const char* names[AHCI_MAX_DISKS] = { "sda", "sdb", "sdc", "sdd" };
    g_ndisks = 0;
    for (int port = 0; port < 32 && g_ndisks < AHCI_MAX_DISKS; port++) {
        if (!(pi & (1u << port))) continue;
        uint32_t ssts = pr(port, PxSSTS);
        if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) continue;
        if (pr(port, PxSIG) != SIG_SATA) continue;
        struct ahci_disk* d = &g_disks[g_ndisks];
        d->port = port; d->idx = g_ndisks;
        port_rebase(port, d->idx);
        uint64_t sectors = ahci_identify(d);
        if (sectors == 0) { debugcon_writestring("[AHCI] IDENTIFY failed\n"); continue; }
        d->dev.name = names[g_ndisks];
        d->dev.sector_size = 512;
        d->dev.sector_count = sectors;
        d->dev.read = ahci_read;
        d->dev.write = ahci_write;
        if (block_register(&d->dev) != 0) { debugcon_writestring("[AHCI] block_register failed\n"); continue; }
        debugcon_writestring("[AHCI] SATA disk "); debugcon_writestring(names[g_ndisks]);
        debugcon_writestring(" port="); debugcon_print_hex((uint64_t)port);
        debugcon_writestring(" sectors=0x"); debugcon_print_hex(sectors); debugcon_writestring("\n");
        g_ndisks++;
    }
    if (g_ndisks == 0) { debugcon_writestring("[AHCI] no SATA disk found\n"); return -1; }
    debugcon_writestring("[AHCI] ready, disks="); debugcon_print_hex((uint64_t)g_ndisks);
    debugcon_writestring("\n");
    return 0;
}
