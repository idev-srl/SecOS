/*
 * dma.h — the kernel DMA API (Phase 5, M55; docs/PHASE5_PLAN.md D2/D3).
 *
 * Every address an in-kernel driver hands to a bus-master device comes from
 * here. Nothing else in a driver may turn a kernel pointer into a bus address
 * (no kvirt_to_phys / virt_to_phys for DMA outside kernel/dma.c).
 *
 * Memory a device may reach:
 *   - the .dmabss region: static driver buffers tagged __dma. The linker
 *     gathers them into one page-aligned, physically contiguous range
 *     [__dma_start, __dma_end) inside .bss (linker.ld, input section
 *     ".bss.dma", zero-filled, never relocated by KASLR physically);
 *   - frames from dma_alloc_coherent() (and, in future, dma_map_single()).
 *
 * Kernel DMA domain (boot word `iommu.kernel=`, only with `iommu=on` and a
 * working IOMMU):
 *   strict (default)  one translated domain shared by every kernel-driven
 *                     function, identity-mapped (bus address == phys) over
 *                     exactly .dmabss + the live coherent/streaming mappings.
 *                     .text/.rodata/.data, the rest of .bss, the heap, the
 *                     page cache and user memory are invisible to devices.
 *   pt                the M54 behaviour: the function stays in the shared
 *                     passthrough domain (fallback).
 * A function joins the strict domain when its driver calls dma_dev(), so
 * functions nobody drives stay in the M54 passthrough domain (the plan moves
 * them to the blocked domain in M59). Without an IOMMU everything is
 * bus == phys and the API only checks.
 *
 * Checks: dma_static() refuses (logs "[DMA] BUG", counts) a range outside
 * .dmabss/coherent memory or above the device's DMA mask and returns the
 * physical address anyway, so the old behaviour is kept and the IOMMU (when
 * strict) blocks the access. Built with -DSECOS_DMA_DEBUG such a violation
 * panics, also with no IOMMU (i440fx), and every dma_dev() is logged.
 *
 * Locking: g_dma_lock (LK_S63_DMA, IRQ-safe spinlock) guards the device table
 * and the mapping table; it nests the IOMMU locks (S65/S67/S68), the heap and
 * the PMM inside. dma_static()/dma_reachable() on a .dmabss address take no
 * lock (hot path: every TX descriptor). Callable from IRQ context and with
 * driver locks held (S45 xhci_lock, N3 tx_lock, the K40 block locks). Nothing
 * sleeps.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KERNEL_DMA_H
#define KERNEL_DMA_H
#include <stdint.h>
#include <stddef.h>
#include "pci.h"

typedef uint64_t dma_addr_t;
typedef struct dma_dev dma_dev_t;

#define DMA_ADDR_ERROR   ((dma_addr_t)-1)
#define DMA_MASK_32      0xFFFFFFFFull
#define DMA_MASK_64      0xFFFFFFFFFFFFFFFFull
#define DMA_MASK_BITS(n) ((n) >= 64 ? DMA_MASK_64 : ((1ull << (n)) - 1))

/* Directions (dma_map_single); informational on x86 (cache coherent). */
#define DMA_TO_DEVICE     1
#define DMA_FROM_DEVICE   2
#define DMA_BIDIRECTIONAL 3

/* dma_alloc_coherent() flags: none yet (frames are always zeroed). */
#define DMA_F_NONE        0

/* Tag a static DMA buffer. Keeps the buffer's own alignment attribute. */
#define __dma __attribute__((section(".bss.dma")))

/* Boot: read `iommu.kernel=`, build the strict domain over .dmabss. Call once,
 * right after iommu_init() and before any driver probes. Logs `[DMA] ...`. */
void dma_init(void);

/* The DMA handle of a PCI function (idempotent; NULL only for pd == NULL when
 * the table is full — callers then fall back to dma_dev(NULL), a handle with
 * no IOMMU domain). In strict mode the first call moves the function into the
 * kernel DMA domain, so call it before enabling bus mastering. */
dma_dev_t* dma_dev(const pci_dev_t* pd);
/* Convenience for drivers holding the legacy pci_device_t. */
dma_dev_t* dma_dev_legacy(const pci_device_t* d);

/* The device's addressing limit (default 64 bit). The 4 GiB checks of the
 * 32-bit controllers (AHCI without S64A, xHCI without AC64) are this mask. */
void     dma_set_mask(dma_dev_t* dd, uint64_t mask);
uint64_t dma_get_mask(const dma_dev_t* dd);

/* 1 if [kva, kva+len) is device-reachable memory (.dmabss or a live
 * coherent/streaming mapping) and its bus address range fits the mask. */
int dma_reachable(const dma_dev_t* dd, const void* kva, size_t len);
/* Bus address of a static __dma buffer (or of coherent memory). On a
 * violation: logged + counted (panic with SECOS_DMA_DEBUG), phys returned. */
dma_addr_t dma_static(const dma_dev_t* dd, const void* kva, size_t len);

/* Page-granular, zeroed, below the device mask (DMA32 via the PMM), mapped
 * into the kernel DMA domain. NULL on failure. */
void* dma_alloc_coherent(dma_dev_t* dd, size_t len, dma_addr_t* out, int flags);
void  dma_free_coherent(dma_dev_t* dd, void* kva, size_t len, dma_addr_t addr);

/* Streaming mappings of a page-aligned, whole-page kernel buffer (physmap or
 * image). Nothing uses them yet (every buffer is static); defined for the
 * future. With the strict domain the pages must not already be mapped (a
 * buffer sharing a page with .dmabss / another mapping is refused). */
dma_addr_t dma_map_single(dma_dev_t* dd, void* kva, size_t len, int dir);
void       dma_unmap_single(dma_dev_t* dd, dma_addr_t addr, size_t len, int dir);
static inline void dma_sync_for_cpu(dma_dev_t* dd, dma_addr_t a, size_t n, int dir)
{ (void)dd; (void)a; (void)n; (void)dir; }
static inline void dma_sync_for_device(dma_dev_t* dd, dma_addr_t a, size_t n, int dir)
{ (void)dd; (void)a; (void)n; (void)dir; }

/* 1 when the kernel DMA domain is strict and this device is in it. */
int dma_dev_strict(const dma_dev_t* dd);
/* "strict", "pt" or "off" (no IOMMU). */
const char* dma_mode(void);

/* /proc/iommu: the DMA lines (called from iommu_generate). */
void dma_generate(void (*emit)(void* ctx, const char* line), void* ctx);

#ifdef SECOS_DMA_TEST
/* Negative tests only: the bus address of a page that is NOT DMA memory
 * (.text when which == 0, a fresh kmalloc page when 1, freed by the caller
 * through dma_test_free). Never use outside -DSECOS_DMA_TEST code. */
dma_addr_t dma_test_bad_addr(int which, void** kva);
void       dma_test_free(void* kva);
/* SHA-256 of .text+.rodata as linked (boot_measure's value). */
void       dma_test_text_hash(uint8_t out[32]);
#endif

#endif /* KERNEL_DMA_H */
