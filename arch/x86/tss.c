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
#include "percpu.h"  // [M29] per-CPU TSS pointer
// Forward declaration of print_hex defined in kernel.c
extern void print_hex(uint64_t value);

// 64-bit GDT layout: null, kernel code, kernel data, user code, user data, TSS (2 slots)
// Build a raw buffer (5 normal entries + TSS descriptor) then load.
static gdt_entry_t gdt_entries[5];
static gdt_tss_entry_t gdt_tss; // occupies 16 bytes
static uint8_t gdt_raw[5 * sizeof(gdt_entry_t) + sizeof(gdt_tss_entry_t)];
static gdt_ptr_t gdt_ptr;
static tss_t tss;

// [M29] Per-CPU GDT + TSS for the application processors (the BSP uses the globals
// above). Each AP must have its own TSS so a ring-3->ring-0 transition lands on
// that core's kernel stack (rsp0), not a shared one.
#define GDT_RAW_SZ (5 * sizeof(gdt_entry_t) + sizeof(gdt_tss_entry_t))
static gdt_entry_t     ap_gdt_entries[SMP_MAX_CPUS][5];
static gdt_tss_entry_t ap_gdt_tss[SMP_MAX_CPUS];
static uint8_t         ap_gdt_raw[SMP_MAX_CPUS][GDT_RAW_SZ];
static gdt_ptr_t       ap_gdt_ptr[SMP_MAX_CPUS];
static tss_t           ap_tss[SMP_MAX_CPUS];

// M2: IST virtual top addresses (set by tss_init, returned by tss_get_ist_bases)
static uint64_t m2_ist1_top = 0;
static uint64_t m2_ist2_top = 0;
static uint64_t m2_ist3_top = 0;

// External assembly functions
extern void gdt_flush(uint64_t gdt_ptr_addr);
extern void tss_flush(uint16_t tss_selector);

// Set a GDT entry (into the given entries array).
static void gdt_set_gate_in(gdt_entry_t* e, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    e->base_low = (base & 0xFFFF);
    e->base_middle = (base >> 16) & 0xFF;
    e->base_high = (base >> 24) & 0xFF;
    e->limit_low = (limit & 0xFFFF);
    e->granularity = (limit >> 16) & 0x0F;
    e->granularity |= gran & 0xF0;
    e->access = access;
}

// Set TSS descriptor (into the given 16-byte descriptor).
static void gdt_set_tss_in(gdt_tss_entry_t* d, uint64_t base, uint32_t limit) {
    d->limit_low = limit & 0xFFFF;
    d->base_low = base & 0xFFFF;
    d->base_middle = (base >> 16) & 0xFF;
    d->base_high = (base >> 24) & 0xFF;
    d->base_upper = (base >> 32) & 0xFFFFFFFF;
    d->access = 0x89;          // Present, DPL=0, Available TSS
    d->granularity = 0x00;
    d->reserved = 0;
}

// Build the standard 5-entry + TSS GDT into the given buffers and load it (GDT +
// TR). Shared by the BSP (tss_init) and the APs (tss_setup_ap).
static void build_and_load_gdt(gdt_entry_t* entries, gdt_tss_entry_t* tssd,
                               uint8_t* raw, gdt_ptr_t* ptr, tss_t* tssp) {
    gdt_set_gate_in(&entries[0], 0, 0, 0, 0);                 // Null
    gdt_set_gate_in(&entries[1], 0, 0x000FFFFF, 0x9A, 0xA0);  // Kernel code
    gdt_set_gate_in(&entries[2], 0, 0x000FFFFF, 0x92, 0xC0);  // Kernel data
    gdt_set_gate_in(&entries[3], 0, 0x000FFFFF, 0xFA, 0xA0);  // User code  (0x1B)
    gdt_set_gate_in(&entries[4], 0, 0x000FFFFF, 0xF2, 0xC0);  // User data  (0x23)
    gdt_set_tss_in(tssd, (uint64_t)tssp, sizeof(tss_t) - 1);

    for (int i = 0; i < 5; i++) {
        const uint8_t* src = (const uint8_t*)&entries[i];
        for (int b = 0; b < (int)sizeof(gdt_entry_t); b++) raw[i * sizeof(gdt_entry_t) + b] = src[b];
    }
    const uint8_t* tss_src = (const uint8_t*)tssd;
    int base_off = 5 * sizeof(gdt_entry_t);
    for (int b = 0; b < (int)sizeof(gdt_tss_entry_t); b++) raw[base_off + b] = tss_src[b];

    ptr->limit = GDT_RAW_SZ - 1;
    ptr->base = (uint64_t)&raw[0];
    gdt_flush((uint64_t)ptr);
    tss_flush(0x28);
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

    // --- Build + load the BSP GDT and TSS ---
    terminal_writestring("[M2] TSS addr: "); print_hex((uint64_t)&tss);
    terminal_writestring(" rsp0: "); print_hex(tss.rsp0); terminal_writestring("\n");
    build_and_load_gdt(gdt_entries, &gdt_tss, gdt_raw, &gdt_ptr, &tss);

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[M2][OK] TSS loaded with guarded IST stacks\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

// [M29] Bring up an application processor's GDT + TSS. Each AP gets its own TSS
// (rsp0 = its kernel stack) and its own IST stacks, then loads them. Called from
// ap_entry() on the AP itself. Records the TSS in this CPU's percpu block so
// tss_set_kernel_stack() updates the right one.
void tss_setup_ap(uint32_t idx, uint64_t kstack_top) {
    if (idx >= SMP_MAX_CPUS) return;
    extern void* kmalloc(unsigned long);
    tss_t* t = &ap_tss[idx];
    for (int i = 0; i < (int)sizeof(tss_t); i++) ((uint8_t*)t)[i] = 0;
    t->rsp0 = kstack_top;
    // Per-AP IST stacks (heap-backed; grow down from the top of each block).
    #define AP_IST_SZ 0x2000ull
    uint64_t i1 = (uint64_t)kmalloc(AP_IST_SZ);
    uint64_t i2 = (uint64_t)kmalloc(AP_IST_SZ);
    uint64_t i3 = (uint64_t)kmalloc(AP_IST_SZ);
    t->ist1 = i1 ? i1 + AP_IST_SZ : kstack_top;
    t->ist2 = i2 ? i2 + AP_IST_SZ : kstack_top;
    t->ist3 = i3 ? i3 + AP_IST_SZ : kstack_top;
    t->iomap_base = sizeof(tss_t);
    build_and_load_gdt(ap_gdt_entries[idx], &ap_gdt_tss[idx], ap_gdt_raw[idx],
                       &ap_gdt_ptr[idx], t);
    this_cpu()->tss = t;
}

void tss_set_kernel_stack(uint64_t stack) {
    // [M29] Update THIS CPU's TSS (the BSP falls back to the global tss before its
    // percpu tss pointer is set).
    cpu_t* c = this_cpu();
    tss_t* t = c->tss ? (tss_t*)c->tss : &tss;
    t->rsp0 = stack;
}

// M2: returns virtual top addresses (not physical identity pointers)
void tss_get_ist_bases(uint64_t* out_ist1, uint64_t* out_ist2, uint64_t* out_ist3) {
    if (out_ist1) *out_ist1 = m2_ist1_top;
    if (out_ist2) *out_ist2 = m2_ist2_top;
    if (out_ist3) *out_ist3 = m2_ist3_top;
}
