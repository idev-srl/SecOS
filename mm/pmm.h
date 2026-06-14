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

// Allocate a physical frame
void* pmm_alloc_frame(void);

// [M12] Allocate `count` physically-contiguous frames; returns the base
// physical address, or NULL if no run of that length is free. Used by the heap
// to back allocations larger than one frame (the physmap maps contiguous
// physical memory to contiguous virtual memory, so this yields a usable region).
void* pmm_alloc_contiguous(size_t count);
// [M12] Free `count` frames previously returned by pmm_alloc_contiguous.
void pmm_free_contiguous(void* addr, size_t count);

// Free a physical frame
void pmm_free_frame(void* addr);

// Memory info accessors
uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_used_memory(void);
uint64_t pmm_get_free_memory(void);
// Maximum physical address seen (end address, not size)
uint64_t pmm_get_max_phys(void);

// Debug
void pmm_print_stats(void);

#endif