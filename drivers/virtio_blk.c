/*
 * SecOS Kernel - virtio-blk driver (legacy/transitional PCI, polling)
 *
 * Targets the legacy virtio interface exposed by QEMU's transitional
 * virtio-blk-pci device (PCI vendor 0x1AF4, device 0x1001) over the BAR0 I/O
 * region. A single split virtqueue is placed in a page-aligned static buffer:
 * the kernel is identity-mapped (phys == virt below 128 MB), so the buffer's
 * virtual address doubles as the guest-physical address handed to the device.
 *
 * DMA always targets an internal page-aligned bounce buffer (physically
 * contiguous because it lives in the contiguously-loaded kernel image), so the
 * caller's buffer needs no contiguity guarantee. Requests are issued one at a
 * time and completion is polled on the used ring — no interrupts.
 *
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "virtio_blk.h"
#include "pci.h"
#include "io.h"
#include "block.h"
#include "debugcon.h"
#include "vmm.h"   // [M12] kvirt_to_phys for higher-half DMA address translation
#include <stddef.h>

/* ── PCI identity ────────────────────────────────────────────────────────── */
#define VIRTIO_VENDOR_ID        0x1AF4
#define VIRTIO_BLK_DEVICE_LEGACY 0x1001   /* transitional virtio-blk */

/* ── Legacy virtio PCI header (offsets from BAR0 I/O base, no MSI-X) ───────── */
#define VIRTIO_PCI_HOST_FEATURES   0x00   /* u32 RO */
#define VIRTIO_PCI_GUEST_FEATURES  0x04   /* u32    */
#define VIRTIO_PCI_QUEUE_PFN       0x08   /* u32: guest-phys page number */
#define VIRTIO_PCI_QUEUE_NUM       0x0C   /* u16 RO: max queue size */
#define VIRTIO_PCI_QUEUE_SEL       0x0E   /* u16 */
#define VIRTIO_PCI_QUEUE_NOTIFY    0x10   /* u16 */
#define VIRTIO_PCI_STATUS          0x12   /* u8  */
#define VIRTIO_PCI_ISR             0x13   /* u8  RO */
#define VIRTIO_PCI_CONFIG          0x14   /* device-specific config (no MSI-X) */

/* Device status bits. */
#define VIRTIO_STATUS_ACK          0x01
#define VIRTIO_STATUS_DRIVER       0x02
#define VIRTIO_STATUS_DRIVER_OK    0x04
#define VIRTIO_STATUS_FAILED       0x80

/* virtq_desc.flags */
#define VIRTQ_DESC_F_NEXT          0x1
#define VIRTQ_DESC_F_WRITE         0x2    /* device writes (read from disk) */

/* virtio-blk request types. */
#define VIRTIO_BLK_T_IN            0      /* read  */
#define VIRTIO_BLK_T_OUT           1      /* write */

#define VIRTIO_BLK_SECTOR          512    /* virtio-blk sector is always 512 */
#define VIRTIO_BLK_S_OK            0

/* ── Split virtqueue structures (legacy layout) ──────────────────────────── */
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
} __attribute__((packed));

struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

/* ── Static DMA region ───────────────────────────────────────────────────── */
#define VQ_MAX_SIZE     256            /* accept queue sizes up to this */
#define VQ_REGION_BYTES 16384          /* >= layout for qsz=256 (12288) */
#define BOUNCE_SECTORS  64
#define BOUNCE_BYTES    (BOUNCE_SECTORS * VIRTIO_BLK_SECTOR)

static uint8_t g_vq_mem[VQ_REGION_BYTES]   __attribute__((aligned(4096)));
static uint8_t g_bounce[BOUNCE_BYTES]      __attribute__((aligned(4096)));
static struct virtio_blk_req_hdr g_hdr     __attribute__((aligned(16)));
static volatile uint8_t g_status_byte      __attribute__((aligned(16)));

/* Virtqueue pointers into g_vq_mem. */
static struct virtq_desc*  g_desc;
static struct virtq_avail* g_avail;
static struct virtq_used*  g_used;
static uint16_t            g_qsz;
static uint16_t            g_last_used;    /* last seen used->idx */

static uint16_t g_io_base;
static int      g_ready;

/* [M12] Higher-half: a kernel-image static's symbol address is a high VMA, not
 * its physical address. Translate to physical for the device (DMA descriptors,
 * queue PFN). The derived CPU pointers (g_desc/g_avail/g_used) then address the
 * low identity map, which the kernel keeps mapped (the DMA region is < 512 MB). */
static inline uint64_t phys_of(const void* p) { return kvirt_to_phys((uint64_t)(uintptr_t)p); }

/* Round up to the next 4 KiB boundary. */
static inline uint64_t align4k(uint64_t x) { return (x + 0xFFF) & ~0xFFFULL; }

static void vq_layout(uint16_t qsz) {
    uint64_t base = phys_of(g_vq_mem);
    uint64_t desc_bytes  = (uint64_t)16 * qsz;
    uint64_t avail_bytes = 6 + (uint64_t)2 * qsz;           /* flags+idx+ring+used_event */
    g_desc  = (struct virtq_desc*)(uintptr_t)base;
    g_avail = (struct virtq_avail*)(uintptr_t)(base + desc_bytes);
    g_used  = (struct virtq_used*)(uintptr_t)align4k(base + desc_bytes + avail_bytes);
}

/* Issue one request covering [bounce .. nsec sectors]; returns 0 on OK. */
static int virtio_blk_xfer(uint64_t sector, uint32_t nsec, int write) {
    if (!g_ready || nsec == 0 || nsec > BOUNCE_SECTORS) return -1;

    g_hdr.type     = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    g_hdr.reserved = 0;
    g_hdr.sector   = sector;
    g_status_byte  = 0xFF;

    /* Three-descriptor chain: header (RO), data, status (device-writable). */
    g_desc[0].addr  = phys_of(&g_hdr);
    g_desc[0].len   = sizeof(struct virtio_blk_req_hdr);
    g_desc[0].flags = VIRTQ_DESC_F_NEXT;
    g_desc[0].next  = 1;

    g_desc[1].addr  = phys_of(g_bounce);
    g_desc[1].len   = nsec * VIRTIO_BLK_SECTOR;
    g_desc[1].flags = VIRTQ_DESC_F_NEXT | (write ? 0 : VIRTQ_DESC_F_WRITE);
    g_desc[1].next  = 2;

    g_desc[2].addr  = phys_of((const void*)&g_status_byte);
    g_desc[2].len   = 1;
    g_desc[2].flags = VIRTQ_DESC_F_WRITE;
    g_desc[2].next  = 0;

    /* Publish head descriptor 0 in the available ring. */
    g_avail->ring[g_avail->idx % g_qsz] = 0;
    io_mfence();
    g_avail->idx++;
    io_mfence();

    /* Doorbell: notify queue 0. */
    io_outw(g_io_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);

    /* Poll the used ring for completion. */
    uint64_t spins = 0;
    while (g_used->idx == g_last_used) {
        io_mfence();
        if (++spins > 100000000ULL) {
            debugcon_writestring("[VIRTIO-BLK] timeout waiting for completion\n");
            return -1;
        }
    }
    g_last_used = g_used->idx;
    io_mfence();

    if (g_status_byte != VIRTIO_BLK_S_OK) return -1;
    return 0;
}

/* Local byte copy/fill — the kernel rolls its own; no libc memcpy/memset. */
static void vb_copy(uint8_t* d, const uint8_t* s, size_t n) { while (n--) *d++ = *s++; }
static void vb_zero(uint8_t* d, size_t n) { while (n--) *d++ = 0; }

static int vblk_read(block_dev_t* dev, uint64_t lba, void* buf, uint32_t count) {
    (void)dev;
    uint8_t* out = (uint8_t*)buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > BOUNCE_SECTORS) chunk = BOUNCE_SECTORS;
        if (virtio_blk_xfer(lba + done, chunk, 0) != 0) return -1;
        vb_copy(out + (size_t)done * VIRTIO_BLK_SECTOR, g_bounce,
                (size_t)chunk * VIRTIO_BLK_SECTOR);
        done += chunk;
    }
    return (int)count;
}

static int vblk_write(block_dev_t* dev, uint64_t lba, const void* buf, uint32_t count) {
    (void)dev;
    const uint8_t* in = (const uint8_t*)buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > BOUNCE_SECTORS) chunk = BOUNCE_SECTORS;
        vb_copy(g_bounce, in + (size_t)done * VIRTIO_BLK_SECTOR,
                (size_t)chunk * VIRTIO_BLK_SECTOR);
        if (virtio_blk_xfer(lba + done, chunk, 1) != 0) return -1;
        done += chunk;
    }
    return (int)count;
}

static block_dev_t g_vblk_dev = {
    .name = "vda",
    .sector_size = VIRTIO_BLK_SECTOR,
    .sector_count = 0,
    .read = vblk_read,
    .write = vblk_write,
};

int virtio_blk_init(void) {
    pci_device_t dev;
    if (pci_find(VIRTIO_VENDOR_ID, VIRTIO_BLK_DEVICE_LEGACY, &dev) != 0) {
        debugcon_writestring("[VIRTIO-BLK] no device found on PCI\n");
        return -1;
    }
    pci_enable_io_and_busmaster(&dev);
    g_io_base = pci_bar0_io_base(&dev);
    if (g_io_base == 0) {
        debugcon_writestring("[VIRTIO-BLK] BAR0 is not an I/O BAR\n");
        return -1;
    }

    /* Reset, then ACKNOWLEDGE + DRIVER. */
    io_outb(g_io_base + VIRTIO_PCI_STATUS, 0);
    io_outb(g_io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK);
    io_outb(g_io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* Negotiate no optional features (basic read/write only). */
    (void)io_inl(g_io_base + VIRTIO_PCI_HOST_FEATURES);
    io_outl(g_io_base + VIRTIO_PCI_GUEST_FEATURES, 0);

    /* Select queue 0 and read its size. */
    io_outw(g_io_base + VIRTIO_PCI_QUEUE_SEL, 0);
    uint16_t qsz = io_inw(g_io_base + VIRTIO_PCI_QUEUE_NUM);
    if (qsz == 0 || qsz > VQ_MAX_SIZE) {
        debugcon_writestring("[VIRTIO-BLK] unsupported queue size\n");
        io_outb(g_io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    g_qsz = qsz;

    vb_zero(g_vq_mem, sizeof(g_vq_mem));
    vq_layout(qsz);
    g_last_used = 0;

    /* Hand the queue's guest-physical page number to the device. */
    uint64_t pfn = phys_of(g_vq_mem) >> 12;
    io_outl(g_io_base + VIRTIO_PCI_QUEUE_PFN, (uint32_t)pfn);

    io_outb(g_io_base + VIRTIO_PCI_STATUS,
            VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    /* Read capacity (in 512-byte sectors) from device config. */
    uint32_t cap_lo = io_inl(g_io_base + VIRTIO_PCI_CONFIG + 0);
    uint32_t cap_hi = io_inl(g_io_base + VIRTIO_PCI_CONFIG + 4);
    g_vblk_dev.sector_count = ((uint64_t)cap_hi << 32) | cap_lo;
    g_ready = 1;

    debugcon_writestring("[VIRTIO-BLK] ready, capacity(sectors)=");
    debugcon_print_hex(g_vblk_dev.sector_count);
    debugcon_writestring(" qsz=");
    debugcon_print_hex(g_qsz);
    debugcon_writestring("\n");

    if (block_register(&g_vblk_dev) != 0) {
        debugcon_writestring("[VIRTIO-BLK] block_register failed\n");
        return -1;
    }
    return 0;
}
