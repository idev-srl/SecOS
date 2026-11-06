#ifndef PMM_H
#define PMM_H
/*

 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Frame size (4KB)
#define PMM_FRAME_SIZE 4096

// Initialize PMM using Multiboot1 memory map
void pmm_init(void* mboot_info);
// Initialize PMM using Multiboot2 structure (info pointer)
void pmm_init_mb2(void* mb2_info);
// Initialize PMM using UEFI memory descriptors (bootinfo handoff)
void pmm_init_uefi(void* mem_descs, uint64_t desc_count, uint64_t desc_size, uint64_t desc_version);

// [M49] Second init phase, once the physmap maps all RAM: builds the sectioned
// bitmap allocator (metadata in the physmap). Before it runs, frames come from an
// early bump allocator (below 512 MiB) and pmm_free_* are no-ops.
void pmm_late_init(void);
// [M49] Exclude [addr, addr+len) from allocation (call before pmm_late_init).
void pmm_reserve(uint64_t addr, uint64_t len);
// [M49] Add RAM-like memory the physmap must map write-back (ACPI tables, UEFI
// runtime/loader data, ...) but the PMM does not hand out.
void pmm_add_ram(uint64_t addr, uint64_t len);
// [M49] Sorted, merged RAM-like ranges [start, end) (usable RAM included), for
// the physmap builder. Returns -1 past the end.
int pmm_ram_range_count(void);
int pmm_ram_range(int i, uint64_t* start, uint64_t* end);

// Allocate a physical frame. Prefers frames at or above 4 GiB (the DMA32 zone
// is used only once the rest is exhausted).
void* pmm_alloc_frame(void);
// [M49] Allocate a frame whose physical address is below `limit` (e.g.
// 0x100000000 for a device with 32-bit DMA, 0x100000 for real-mode code).
void* pmm_alloc_below(uint64_t limit);

// [M12] Allocate `count` physically-contiguous frames; returns the base
// physical address, or NULL if no run of that length is free. Used by the heap
// to back allocations larger than one frame (the physmap maps contiguous
// physical memory to contiguous virtual memory, so this yields a usable region).
void* pmm_alloc_contiguous(size_t count);
// [M12] Free `count` frames previously returned by pmm_alloc_contiguous.
void pmm_free_contiguous(void* addr, size_t count);
// [M49] Contiguous frames below `limit` (32-bit DMA rings/buffers). A run never
// crosses a 1 GiB section boundary, so count <= 262144.
void* pmm_alloc_contiguous_below(size_t count, uint64_t limit);

// Free a physical frame
void pmm_free_frame(void* addr);
int  pmm_share_frame(void* addr);   // [M19] COW: add a reference; -1 = out of memory
int  pmm_unref_frame(void* addr);   // [M19] COW: drop a reference; 1=still shared, 0=free it
int  pmm_frame_shared(void* addr);  // 1 if more than one mapping references the frame

// Memory info accessors
uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_used_memory(void);
uint64_t pmm_get_free_memory(void);
// Maximum physical address seen (end address, not size)
uint64_t pmm_get_max_phys(void);
// [M49] Bytes of allocator metadata (section array + descriptors + refcounts).
uint64_t pmm_get_meta_bytes(void);
// [M49] Free frames below `limit`, rounded to 1 GiB sections (zone reporting).
uint64_t pmm_free_frames_below(uint64_t limit);

// Debug
void pmm_print_stats(void);

#endif