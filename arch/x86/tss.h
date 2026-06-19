#ifndef TSS_H
#define TSS_H
/*
 * SecOS Kernel - TSS & GDT Definitions
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

// Task State Segment (x86-64)
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;        // Ring 0 stack pointer
    uint64_t rsp1;        // Ring 1 stack pointer
    uint64_t rsp2;        // Ring 2 stack pointer
    uint64_t reserved1;
    uint64_t ist1;        // Interrupt Stack Table #1
    uint64_t ist2;        // Interrupt Stack Table #2
    uint64_t ist3;        // Interrupt Stack Table #3
    uint64_t ist4;        // Interrupt Stack Table #4
    uint64_t ist5;        // Interrupt Stack Table #5
    uint64_t ist6;        // Interrupt Stack Table #6
    uint64_t ist7;        // Interrupt Stack Table #7
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

// Standard 8-byte GDT entry
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

// 64-bit TSS descriptor (occupies 2 slots)
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed)) gdt_tss_entry_t;

// GDT pointer structure
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

// Public functions

// M2: tss_init now takes the kernel RSP0 value (M2_KSTACK_TOP).
// It allocates IST stacks via vmm_alloc_ist_stack(), maps them at
// fixed virtual addresses in the M2 stack region, and loads GDT/TSS.
// Must be called after vmm_init_physmap() and vmm_alloc_kernel_stack().
void tss_init(uint64_t kernel_rsp0);

// Update TSS.rsp0 (used when entering ring 0 from ring 3).
void tss_set_kernel_stack(uint64_t stack);

// [M29] Build + load an application processor's own GDT/TSS/IST (idx = CPU index,
// kstack_top = that CPU's kernel stack). Called from ap_entry on the AP.
void tss_setup_ap(uint32_t idx, uint64_t kstack_top);

// Return the virtual tops of the IST stacks (M2: virtual, not physical).
void tss_get_ist_bases(uint64_t* out_ist1, uint64_t* out_ist2, uint64_t* out_ist3);

#endif