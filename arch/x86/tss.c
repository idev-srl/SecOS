/*
 * SecOS Kernel - TSS & GDT Setup
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * M2: IST stacks are now allocated via vmm_alloc_ist_stack() and mapped at
 * fixed virtual addresses in the M2 stack region (0xFFFFFF8000000000).
 * Each IST has explicit guard pages (NP PTEs) above and below the usable region.
 * No more raw physical pointer casts for IST pointers.
 */
#include "tss.h"
#include "vmm.h"     // vmm_alloc_ist_stack, M2_IST* constants
#include "terminal.h"
// Forward declaration of print_hex defined in kernel.c
extern void print_hex(uint64_t value);

// 64-bit GDT layout: null, kernel code, kernel data, user data, user code, TSS (2 slots)
// Build a raw buffer (5 normal entries + TSS descriptor) then load.
static gdt_entry_t gdt_entries[5];
static gdt_tss_entry_t gdt_tss; // occupies 16 bytes
static uint8_t gdt_raw[5 * sizeof(gdt_entry_t) + sizeof(gdt_tss_entry_t)];
static gdt_ptr_t gdt_ptr;
static tss_t tss;

// M2: IST virtual top addresses (set by tss_init, returned by tss_get_ist_bases)
static uint64_t m2_ist1_top = 0;
static uint64_t m2_ist2_top = 0;
static uint64_t m2_ist3_top = 0;

// External assembly functions
extern void gdt_flush(uint64_t gdt_ptr_addr);
extern void tss_flush(uint16_t tss_selector);

// Set a GDT entry
static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_entries[num].base_low = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high = (base >> 24) & 0xFF;

    gdt_entries[num].limit_low = (limit & 0xFFFF);
    gdt_entries[num].granularity = (limit >> 16) & 0x0F;
    gdt_entries[num].granularity |= gran & 0xF0;
    gdt_entries[num].access = access;
}

// Set TSS descriptor in GDT
static void gdt_set_tss(uint64_t base, uint32_t limit) {
    gdt_tss.limit_low = limit & 0xFFFF;
    gdt_tss.base_low = base & 0xFFFF;
    gdt_tss.base_middle = (base >> 16) & 0xFF;
    gdt_tss.base_high = (base >> 24) & 0xFF;
    gdt_tss.base_upper = (base >> 32) & 0xFFFFFFFF;

    // Access byte: Present, DPL=0, Type=0x9 (Available TSS)
    gdt_tss.access = 0x89;
    gdt_tss.granularity = 0x00;
    gdt_tss.reserved = 0;
}

// M2: tss_init takes the kernel stack RSP_INIT (M2_KSTACK_TOP).
// Must be called after vmm_init_physmap() and vmm_alloc_kernel_stack().
void tss_init(uint64_t kernel_rsp0) {
    terminal_writestring("[M2] Initializing TSS with guarded IST stacks...\n");

    // --- Allocate and map IST stacks via VMM ---
    // vmm_alloc_ist_stack() allocates PMM frames, maps them at the given VAs,
    // installs guard PTEs (NP) at guard_lo and guard_hi, returns top VA.
    m2_ist1_top = vmm_alloc_ist_stack(M2_IST1_GUARD_LO, M2_IST1_BASE,
                                       M2_IST1_TOP, M2_IST1_GUARD_HI, 1);
    m2_ist2_top = vmm_alloc_ist_stack(M2_IST2_GUARD_LO, M2_IST2_BASE,
                                       M2_IST2_TOP, M2_IST2_GUARD_HI, 2);
    m2_ist3_top = vmm_alloc_ist_stack(M2_IST3_GUARD_LO, M2_IST3_BASE,
                                       M2_IST3_TOP, M2_IST3_GUARD_HI, 3);

    // --- Zero the TSS ---
    uint8_t* tss_ptr = (uint8_t*)&tss;
    for (int i = 0; i < (int)sizeof(tss_t); i++) tss_ptr[i] = 0;

    // --- Configure TSS fields ---
    // rsp0: kernel stack pointer used on ring-0 entry from ring-3
    tss.rsp0 = kernel_rsp0;

    // IST pointers: virtual tops (stacks grow downward from top)
    tss.ist1 = m2_ist1_top;   // Double Fault   (#8)
    tss.ist2 = m2_ist2_top;   // Page Fault     (#14)
    tss.ist3 = m2_ist3_top;   // GPF            (#13)

    tss.iomap_base = sizeof(tss_t);

    // --- Build GDT ---
    gdt_set_gate(0, 0, 0, 0, 0);                 // Null
    gdt_set_gate(1, 0, 0x000FFFFF, 0x9A, 0xA0);  // Kernel code
    gdt_set_gate(2, 0, 0x000FFFFF, 0x92, 0xC0);  // Kernel data
    gdt_set_gate(3, 0, 0x000FFFFF, 0xF2, 0xC0);  // User data
    gdt_set_gate(4, 0, 0x000FFFFF, 0xFA, 0xA0);  // User code

    // TSS descriptor (occupies 2 consecutive GDT slots)
    gdt_set_tss((uint64_t)&tss, sizeof(tss_t) - 1);

    // Copy GDT entries into the contiguous raw buffer
    uint8_t* dst = gdt_raw;
    for (int i = 0; i < 5; i++) {
        const uint8_t* src = (const uint8_t*)&gdt_entries[i];
        for (int b = 0; b < (int)sizeof(gdt_entry_t); b++)
            dst[i * sizeof(gdt_entry_t) + b] = src[b];
    }
    // Copy TSS descriptor (16 bytes)
    const uint8_t* tss_src = (const uint8_t*)&gdt_tss;
    int base_off = 5 * sizeof(gdt_entry_t);
    for (int b = 0; b < (int)sizeof(gdt_tss_entry_t); b++)
        dst[base_off + b] = tss_src[b];

    gdt_ptr.limit = sizeof(gdt_raw) - 1;
    gdt_ptr.base = (uint64_t)&gdt_raw[0];

    // --- Load GDT and TSS ---
    terminal_writestring("[M2] GDT base: "); print_hex(gdt_ptr.base);
    terminal_writestring(" limit: "); print_hex(gdt_ptr.limit); terminal_writestring("\n");
    terminal_writestring("[M2] TSS addr: "); print_hex((uint64_t)&tss);
    terminal_writestring(" rsp0: "); print_hex(tss.rsp0); terminal_writestring("\n");
    terminal_writestring("[M2] TSS.ist1: "); print_hex(tss.ist1); terminal_writestring("\n");
    terminal_writestring("[M2] TSS.ist2: "); print_hex(tss.ist2); terminal_writestring("\n");
    terminal_writestring("[M2] TSS.ist3: "); print_hex(tss.ist3); terminal_writestring("\n");

    gdt_flush((uint64_t)&gdt_ptr);
    tss_flush(0x28);

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[M2][OK] TSS loaded with guarded IST stacks\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

void tss_set_kernel_stack(uint64_t stack) {
    tss.rsp0 = stack;
}

// M2: returns virtual top addresses (not physical identity pointers)
void tss_get_ist_bases(uint64_t* out_ist1, uint64_t* out_ist2, uint64_t* out_ist3) {
    if (out_ist1) *out_ist1 = m2_ist1_top;
    if (out_ist2) *out_ist2 = m2_ist2_top;
    if (out_ist3) *out_ist3 = m2_ist3_top;
}
