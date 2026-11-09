/*
 * secos/udrv.h — Driver Space v2 ABI (Phase 5, M57): ring-3 PCI drivers.
 *
 * Shared by the kernel (kernel/udrv.c includes this file) and user space. Only
 * fixed-width types; every struct has its exact size checked by both sides.
 *
 * SYS_UDRV (92): long udrv(op, handle, arg, size)
 *   op     UDRV_OP_*
 *   handle the IRQ/event fd returned by UDRV_OP_CLAIM (ignored by CLAIM)
 *   arg    pointer to the op's struct (copied in and out by the kernel)
 *   size   sizeof that struct (must match exactly, else -EINVAL)
 * Returns >= 0 on success, -errno on failure.
 *
 * Who may call it: a process whose signed manifest is a v3 DRIVER manifest
 * (user/note_udrv.S) and whose effective uid is 0 at claim time. The device must
 * be named by the boot word udrv=<bb:dd.f|vvvv:dddd>[,...] and match one of the
 * manifest's match entries. Storage controllers are never assignable.
 *
 * The claim fd (UIO-style):
 *   read()  -> struct udrv_events; blocks until the IRQ count changed since the
 *              last read (O_NONBLOCK: -EAGAIN). Each vector that fired is
 *              masked by the kernel until acknowledged.
 *   write() <- struct udrv_ack: acknowledge and UNMASK the vectors in vec_mask
 *              (also the way to enable a vector after UDRV_OP_IRQ_SETUP, which
 *              leaves every vector masked).
 *   poll()  -> POLLIN when read() would not block.
 * Only the claiming process may use the fd (a fork child gets -EPERM). The
 * claim ends when its owner exits or execs, or with UDRV_OP_RELEASE; the fd
 * then reads -ENODEV.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SECOS_UDRV_H
#define SECOS_UDRV_H
#include <stdint.h>

#define SYS_UDRV            92

#define UDRV_OP_CLAIM       1   /* struct udrv_claim  -> fd                     */
#define UDRV_OP_INFO        2   /* struct udrv_info   (out)                     */
#define UDRV_OP_MAP_BAR     3   /* struct udrv_map_bar (in bar, out uva/len)    */
#define UDRV_OP_CFG_READ    4   /* struct udrv_cfg (in off/width, out val)      */
#define UDRV_OP_CFG_WRITE   5   /* struct udrv_cfg (whitelisted, mediated)      */
#define UDRV_OP_DMA_ALLOC   6   /* struct udrv_dma (in size/dir, out uva/iova)  */
#define UDRV_OP_DMA_FREE    7   /* struct udrv_dma (in uva)                     */
#define UDRV_OP_IRQ_SETUP   8   /* struct udrv_irq (in nvec, out mode/nvec)     */
#define UDRV_OP_RESET       9   /* no struct: FLR, else PM D3hot->D0, else
                                   -EOPNOTSUPP. Frees the IRQ setup.           */
#define UDRV_OP_RELEASE     10  /* no struct: end the claim now                 */

/* udrv_claim.flags */
#define UDRV_CLAIM_BY_ID    0x1 /* match vendor/device instead of the BDF       */

struct udrv_claim {
    uint8_t  bus, dev, fn, pad0;
    uint16_t vendor, device;    /* with UDRV_CLAIM_BY_ID                         */
    uint32_t flags;
    uint64_t dma_mask;          /* largest IOVA the device can address
                                   (0 = 32 bits)                               */
};

/* udrv_info.irq_mode / udrv_irq.mode */
#define UDRV_IRQ_NONE       0
#define UDRV_IRQ_MSIX       1
#define UDRV_IRQ_MSI        2

struct udrv_info {
    uint16_t vendor, device;
    uint8_t  bus, dev, fn, class_code;
    uint8_t  subclass, prog_if, revision, nbars;
    uint8_t  bar_flags[6];      /* PCI_BAR_* of the kernel registry: 0x01 present,
                                   0x02 I/O, 0x04 64-bit, 0x08 prefetch, 0x10
                                   upper half                                  */
    uint16_t msix_count;        /* MSI-X table entries (0 = none)               */
    uint8_t  msi_max;           /* MSI messages (0 = no MSI)                    */
    uint8_t  msi_pvm;           /* MSI per-vector masking                       */
    uint8_t  iommu;             /* 1: the device's DMA is translated            */
    uint8_t  ir;                /* 1: interrupt remapping is on                 */
    uint8_t  dma_ok;            /* 1: DMA_ALLOC / BUSMASTER allowed             */
    uint8_t  unsafe;            /* 1: udrv.unsafe (DMA without IOMMU)           */
    uint8_t  flr;               /* 1: function level reset available            */
    uint8_t  pm_reset;          /* 1: PM D3hot->D0 reset available              */
    uint16_t pad1;
    uint32_t irq_mode;          /* UDRV_IRQ_*                                   */
    uint64_t bar_size[6];
    uint64_t dma_used, dma_max; /* bytes                                        */
    uint32_t msix_table_bar, msix_table_off;
    uint32_t msix_pba_bar, msix_pba_off;
    uint64_t irq_count;
    uint32_t bar_mask, cfg_write_mask;  /* from the signed manifest             */
};

/* udrv_map_bar: the whole BAR is mapped (uncached, NX, never inherited by
 * fork, munmap/mprotect refused). Pages holding the MSI-X table or PBA are
 * left unmapped (a hole: touching it is SIGSEGV). I/O BARs and BARs smaller
 * than 4 KiB are refused. */
struct udrv_map_bar {
    uint32_t bar;
    uint32_t flags;
    uint64_t uva;               /* out: mapping start                           */
    uint64_t len;               /* out: mapping length (whole BAR, page-rounded)*/
};

/* udrv_cfg: off 0..0xFFF, width 1/2/4, naturally aligned. Reads: anything.
 * Writes: the command register (MEM, BUSMASTER, INTx Disable; I/O space and
 * BUSMASTER without DMA rights are -EPERM) and device-specific bytes 0x40..0xFF
 * outside every capability structure, each only if the manifest's
 * cfg_write_mask allows it. Status bits (write-1-to-clear) are never written. */
struct udrv_cfg {
    uint32_t off;
    uint32_t width;
    uint32_t val;
    uint32_t pad;
};

/* udrv_dma.dir */
#define UDRV_DMA_TO_DEV     1   /* the device reads the buffer                  */
#define UDRV_DMA_FROM_DEV   2   /* the device writes the buffer                 */
#define UDRV_DMA_BIDIR      3

/* udrv_dma: zeroed, page granular, mapped RW+NX into the driver and into the
 * device's own IOMMU domain only (iova != phys). Charged to the manifest's
 * dma_max and max_mem. DMA_FREE unmaps the device side (IOTLB flushed) before
 * the user side and the frames. */
struct udrv_dma {
    uint64_t size;
    uint32_t dir;
    uint32_t flags;
    uint64_t uva;               /* out (ALLOC) / in (FREE)                      */
    uint64_t iova;              /* out: the address to program into the device */
};

struct udrv_irq {
    uint32_t nvec;              /* in: vectors wanted (<= manifest irq_max, 32) */
    uint32_t mode;              /* out: UDRV_IRQ_MSIX / UDRV_IRQ_MSI            */
};

/* read() of the claim fd */
#define UDRV_EV_TX_KICK     0x1 /* reserved (M58: the kernel queued frames)     */
#define UDRV_EV_STOP        0x2 /* the kernel wants the driver to quiesce       */
struct udrv_events {
    uint64_t irq_count;         /* interrupts taken since the claim             */
    uint32_t vec_pending_mask;  /* vectors fired and still masked (not acked)   */
    uint32_t flags;             /* UDRV_EV_*                                    */
};

/* write() of the claim fd (at least 4 bytes) */
struct udrv_ack {
    uint32_t vec_mask;
    uint32_t flags;
};

/* Manifest v3 driver fields (QSEC version 3, see user/note_udrv.S). */
#define UDRV_CFGW_COMMAND   0x1 /* may write the command register               */
#define UDRV_CFGW_DEVSPEC   0x2 /* may write 0x40..0xFF outside capabilities    */

#endif /* SECOS_UDRV_H */
