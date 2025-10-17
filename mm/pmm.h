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

// Allocate a physical frame
void* pmm_alloc_frame(void);

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