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
 * aligned __dma kernel buffers; their bus addresses come from the DMA API
 * (kernel/dma.h, [M55]: with iommu=on the HBA can reach only those). Transfers
 * bounce through a per-port static buffer (commands are polled).
 *
 * [M50 B3] Locking: AHCI ports are independent (own command list, FIS area,
 * PxIS/PxCI/PxTFD registers), and every per-command structure — command table
 * and DMA bounce — is per port too, so each disk is serialized only by its own
 * block_dev_t lock (fs/block.h): two CPUs can drive sda and sdb at once. The
 * HBA-global registers (GHC, PI, CAP) are touched only by ahci_init.
 */
#include "ahci.h"
#include "pci.h"
#include "io.h"
#include "block.h"
#include "debugcon.h"
#include "vmm.h"        // vmm_map_mmio
#include "dma.h"        // [M55] bus addresses, DMA mask
#include <stddef.h>

// ---- HBA / port registers ----
#define HBA_CAP   0x00
#define CAP_S64A  (1u << 31)   // 64-bit addressing
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
#define IS_HBFS  (1u << 29)    // host bus fatal error (e.g. an IOMMU-blocked DMA)
#define IS_HBDS  (1u << 28)    // host bus data error
#define IS_IFS   (1u << 27)    // interface fatal error
#define IS_ERR   (IS_TFES | IS_HBFS | IS_HBDS | IS_IFS)
#define SIG_SATA 0x00000101u

#define ATA_READ_DMA_EX   0x25
#define ATA_WRITE_DMA_EX  0x35
#define ATA_IDENTIFY      0xEC

#define AHCI_MAX_DISKS 4

// Per-port command list + received-FIS (the HBA reads these continuously).
static uint8_t g_clb[AHCI_MAX_DISKS][1024] __dma __attribute__((aligned(1024)));
static uint8_t g_fis[AHCI_MAX_DISKS][256]  __dma __attribute__((aligned(256)));
// Per port (one command in flight per port): command table + DMA bounce.
static uint8_t g_ctba[AHCI_MAX_DISKS][256]      __dma __attribute__((aligned(128)));
static uint8_t g_dma[AHCI_MAX_DISKS][64 * 1024] __dma __attribute__((aligned(4096)));
#define DMA_SECTORS (sizeof(g_dma[0]) / 512)

struct ahci_disk { int port; int idx; block_dev_t dev; };
static struct ahci_disk g_disks[AHCI_MAX_DISKS];
static int g_ndisks;
static volatile uint8_t* g_abar;
static dma_dev_t* g_dd;                 // [M55] the HBA's DMA handle
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
    uint64_t clb = dma_static(g_dd, g_clb[idx], sizeof(g_clb[idx]));
    uint64_t fb  = dma_static(g_dd, g_fis[idx], sizeof(g_fis[idx]));
    pw(port, PxCLB,  (uint32_t)clb);  pw(port, PxCLBU, (uint32_t)(clb >> 32));
    pw(port, PxFB,   (uint32_t)fb);   pw(port, PxFBU,  (uint32_t)(fb >> 32));
    zero(g_clb[idx], 1024); zero(g_fis[idx], 256);
    pw(port, PxSERR, 0xFFFFFFFFu); pw(port, PxIS, 0xFFFFFFFFu);
    port_start(port);
}

// [M55] After a failed command (task-file error, or a host-bus error such as
// a DMA the IOMMU blocked) the port stops processing its command list until
// software restarts it: stop (clears PxCI), clear the error status, start.
static void port_recover(int port) {
    port_stop(port);
    pw(port, PxSERR, 0xFFFFFFFFu); pw(port, PxIS, 0xFFFFFFFFu);
    port_start(port);
}

// Issue one ATA command on a disk's port/slot, single PRDT at bus address dba.
static int ahci_cmd_at(struct ahci_disk* d, uint8_t cmd, uint64_t lba,
                       uint32_t count, uint32_t nbytes, int write, uint64_t dba) {
    int port = d->port;
    uint8_t* ctba = g_ctba[d->idx];
    pw(port, PxIS, 0xFFFFFFFFu);

    uint32_t* hdr = (uint32_t*)g_clb[d->idx];   // command header, slot 0
    uint64_t ctba_phys = dma_static(g_dd, ctba, sizeof(g_ctba[0]));
    hdr[0] = 5u | ((uint32_t)(write ? 1u : 0u) << 6) | (1u << 16); // CFL=5, W, PRDTL=1
    hdr[1] = 0;
    hdr[2] = (uint32_t)ctba_phys; hdr[3] = (uint32_t)(ctba_phys >> 32);
    hdr[4] = hdr[5] = hdr[6] = hdr[7] = 0;

    zero(ctba, sizeof(g_ctba[0]));
    uint8_t* cfis = ctba;                       // H2D register FIS
    cfis[0] = 0x27; cfis[1] = 0x80; cfis[2] = cmd; cfis[3] = 0;
    cfis[4] = (uint8_t)lba; cfis[5] = (uint8_t)(lba >> 8); cfis[6] = (uint8_t)(lba >> 16);
    cfis[7] = 0x40;                             // LBA mode
    cfis[8] = (uint8_t)(lba >> 24); cfis[9] = (uint8_t)(lba >> 32); cfis[10] = (uint8_t)(lba >> 40);
    cfis[12] = (uint8_t)count; cfis[13] = (uint8_t)(count >> 8);

    uint32_t* prdt = (uint32_t*)(ctba + 0x80);
    prdt[0] = (uint32_t)dba; prdt[1] = (uint32_t)(dba >> 32);
    prdt[2] = 0; prdt[3] = (nbytes - 1) & 0x3FFFFF;

    for (volatile int i = 0; i < 2000000; i++)
        if (!(pr(port, PxTFD) & (TFD_BSY | TFD_DRQ))) break;
    io_mfence();
    pw(port, PxCI, 1u << 0);
    for (volatile uint64_t i = 0; ; i++) {
        if (!(pr(port, PxCI) & (1u << 0))) break;
        if (pr(port, PxIS) & IS_ERR) { port_recover(port); return -1; }
        if (i > 50000000ull) { port_recover(port); return -1; }
    }
    if ((pr(port, PxIS) & IS_ERR) || (pr(port, PxTFD) & TFD_ERR)) { port_recover(port); return -1; }
    return 0;
}

static int ahci_cmd(struct ahci_disk* d, uint8_t cmd, uint64_t lba,
                    uint32_t count, uint32_t nbytes, int write) {
    return ahci_cmd_at(d, cmd, lba, count, nbytes, write,
                       dma_static(g_dd, g_dma[d->idx], nbytes));
}

static struct ahci_disk* disk_of(block_dev_t* dev) {
    for (int i = 0; i < g_ndisks; i++) if (&g_disks[i].dev == dev) return &g_disks[i];
    return NULL;
}

static int ahci_rw(struct ahci_disk* d, uint64_t lba, uint8_t* buf, uint32_t count, int write) {
    uint8_t* dma = g_dma[d->idx];
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done; if (chunk > DMA_SECTORS) chunk = DMA_SECTORS;
        uint32_t nbytes = chunk * 512;
        if (write) for (uint32_t i = 0; i < nbytes; i++) dma[i] = buf[done * 512 + i];
        if (ahci_cmd(d, write ? ATA_WRITE_DMA_EX : ATA_READ_DMA_EX, lba + done, chunk, nbytes, write) != 0)
            return -1;
        if (!write) for (uint32_t i = 0; i < nbytes; i++) buf[done * 512 + i] = dma[i];
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
    const uint8_t* id = g_dma[d->idx];
    uint64_t lba48 = 0;
    for (int i = 0; i < 8; i++) lba48 |= (uint64_t)id[200 + i] << (8 * i);
    if (lba48) return lba48;
    uint32_t lba28 = 0;
    for (int i = 0; i < 4; i++) lba28 |= (uint32_t)id[120 + i] << (8 * i);
    return lba28;
}

#ifdef SECOS_DMA_TEST
#include "iommu.h"
// [M55] Negative test (-DSECOS_DMA_TEST, strict kernel DMA domain only): point
// one PRD at memory outside .dmabss/coherent and check that the IOMMU blocks
// the transfer and the kernel survives:
//   1. READ DMA into a .text page  -> device write fault, .text hash unchanged;
//   2. WRITE DMA from a heap page  -> device read fault, the page's bytes do
//      not reach the disk (the sector keeps the marker written before);
//   3. a normal read afterwards     -> works.
// The command status is logged but not required: QEMU's ich9-ahci completes a
// command whose DMA the IOMMU refused with success (no HBFS/TFES; it drops the
// data), real HBAs report a host bus error (handled by port_recover()).
static int dmat_fault_seen(uint64_t before, uint64_t iova, int is_write) {
    for (int i = 0; i < 200; i++) {
        iommu_fault_poll();
        iommu_fault_stats_t st; iommu_fault_stats(&st);
        if (st.total > before)
            return (st.last.iova & ~0xFFFull) == (iova & ~0xFFFull) && st.last.is_write == (is_write ? 1 : 0);
        for (volatile int k = 0; k < 100000; k++) { }
    }
    return 0;
}
static void ahci_dma_negative_test(struct ahci_disk* d) {
    int pass = 0, fail = 0;
    if (!dma_dev_strict(g_dd)) {
        debugcon_writestring("[DMA-TEST] SKIP: the AHCI HBA is not in the strict kernel DMA domain\n");
        return;
    }
    uint64_t far_lba = d->dev.sector_count > 64 ? d->dev.sector_count - 16 : 0;
    uint8_t* dma = g_dma[d->idx];
    // 1. Device writes into .text.
    uint8_t h0[32], h1[32];
    dma_test_text_hash(h0);
    void* kva = 0;
    uint64_t bad = dma_test_bad_addr(0, &kva);
    iommu_fault_stats_t st; iommu_fault_stats(&st);
    int rc = ahci_cmd_at(d, ATA_READ_DMA_EX, 0, 8, 4096, 0, bad);
    int seen = dmat_fault_seen(st.total, bad, 1);
    dma_test_text_hash(h1);
    int same = 1; for (int i = 0; i < 32; i++) if (h0[i] != h1[i]) same = 0;
    debugcon_writestring("[DMA-TEST] ahci read into .text phys="); debugcon_print_hex(bad);
    debugcon_writestring(rc ? " (command failed)" : " (command completed)");
    debugcon_writestring(seen ? ", IOMMU write fault seen" : ", NO matching IOMMU fault");
    debugcon_writestring(same ? ", .text hash unchanged\n" : ", .text hash CHANGED\n");
    if (seen && same) pass++; else fail++;
    // 2. Device reads a heap page (would leak kernel memory to the disk).
    for (int i = 0; i < 4096; i++) dma[i] = 0xC3;              // marker sector
    int mk = ahci_cmd(d, ATA_WRITE_DMA_EX, far_lba, 8, 4096, 1);
    bad = dma_test_bad_addr(1, &kva);
    if (bad != DMA_ADDR_ERROR && mk == 0) {
        for (int i = 0; i < 3 * 4096; i++) ((uint8_t*)kva)[i] = 0x5A;   // "secret"
        iommu_fault_stats(&st);
        rc = ahci_cmd_at(d, ATA_WRITE_DMA_EX, far_lba, 8, 4096, 1, bad);
        seen = dmat_fault_seen(st.total, bad, 0);
        int leaked = 0;
        for (int i = 0; i < 4096; i++) dma[i] = 0;
        if (ahci_cmd(d, ATA_READ_DMA_EX, far_lba, 8, 4096, 0) != 0) leaked = -1;
        else for (int i = 0; i < 4096; i++) if (dma[i] == 0x5A) { leaked = 1; break; }
        debugcon_writestring("[DMA-TEST] ahci write from a heap page phys="); debugcon_print_hex(bad);
        debugcon_writestring(rc ? " (command failed)" : " (command completed)");
        debugcon_writestring(seen ? ", IOMMU read fault seen" : ", NO matching IOMMU fault");
        debugcon_writestring(leaked == 0 ? ", heap bytes not on disk\n"
                             : leaked > 0 ? ", heap bytes LEAKED to disk\n" : ", read-back FAILED\n");
        if (seen && leaked == 0) pass++; else fail++;
        dma_test_free(kva);
    } else {
        debugcon_writestring("[DMA-TEST] heap page / marker write failed\n"); fail++;
        if (kva) dma_test_free(kva);
    }
    // 3. The port still works.
    rc = ahci_cmd(d, ATA_READ_DMA_EX, 0, 8, 4096, 0);
    debugcon_writestring(rc ? "[DMA-TEST] ahci read after the faults FAILED\n"
                            : "[DMA-TEST] ahci read after the faults ok\n");
    if (rc == 0) pass++; else fail++;
    debugcon_writestring("[DMA-TEST] RESULT pass="); debugcon_print_hex((uint64_t)pass);
    debugcon_writestring(" fail="); debugcon_print_hex((uint64_t)fail); debugcon_writestring("\n");
}
#endif

int ahci_init(void) {
    pci_device_t dev;
    if (pci_find_class(0x01, 0x06, 0x01, &dev) != 0) {
        debugcon_writestring("[AHCI] no controller on PCI\n");
        return -1;
    }
    g_dd = dma_dev_legacy(&dev);    // [M55] joins the kernel DMA domain first
    pci_enable_mem_and_busmaster(&dev);
    uint32_t abar_phys = pci_bar_mem(&dev, 5);
    if (abar_phys == 0) { debugcon_writestring("[AHCI] BAR5 not memory\n"); return -1; }
    g_abar = (volatile uint8_t*)pci_bar_map(&dev, 5, 0x2000);   // [M49] uncached
    if (!g_abar) { debugcon_writestring("[AHCI] ABAR map failed\n"); return -1; }

    hw(HBA_GHC, hr(HBA_GHC) | GHC_AE);
    // [M49] Every DMA structure is a static kernel buffer (command lists, FIS
    // areas, command table, the 64 KB bounce): an HBA without CAP.S64A can only
    // reach them while the kernel image sits below 4 GiB. [M55] = the DMA mask.
    if (!(hr(HBA_CAP) & CAP_S64A)) dma_set_mask(g_dd, DMA_MASK_32);
    if (!dma_reachable(g_dd, g_dma, sizeof(g_dma)) || !dma_reachable(g_dd, g_clb, sizeof(g_clb)) ||
        !dma_reachable(g_dd, g_fis, sizeof(g_fis)) || !dma_reachable(g_dd, g_ctba, sizeof(g_ctba))) {
        debugcon_writestring("[AHCI] 32-bit DMA HBA but the kernel image is above 4 GiB\n");
        return -1;
    }
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
#ifdef SECOS_DMA_TEST
    ahci_dma_negative_test(&g_disks[g_ndisks - 1]);
#endif
    return 0;
}
