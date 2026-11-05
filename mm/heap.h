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

// Allocate memory with specified alignment (power of two). The result is
// released with kfree() like any other allocation.
void* kmalloc_aligned(size_t size, size_t alignment);

// Free previously allocated memory. Double/invalid frees and overflows past the
// requested size are detected and panic (see the layout comment in heap.c).
void kfree(void* ptr);

// Print heap statistics
void heap_print_stats(void);

// Validate every block (headers, links, tail canaries, free poison in debug
// builds). Returns 0 when sound; corruption panics with a diagnostic.
int heap_check(void);

// heap_check() plus a printed summary (shell `heapcheck`).
void heap_check_report(void);

// Boot selftest: allocation patterns + detection paths. Returns failures.
int heap_selftest(void);

#endif