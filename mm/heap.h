#ifndef HEAP_H
#define HEAP_H
/*

 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

// Initialize heap allocator
void heap_init(void);

// Allocate dynamic memory
void* kmalloc(size_t size);

// Allocate memory with specified alignment
void* kmalloc_aligned(size_t size, size_t alignment);

// Free previously allocated memory
void kfree(void* ptr);

// Print heap statistics
void heap_print_stats(void);

#endif