/*
 * SecOS Kernel - TSS & GDT Setup
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "tss.h"
#include "pmm.h"
#include "terminal.h"
// Forward declaration of print_hex defined in kernel.c
extern void print_hex(uint64_t value);

#define IST_STACK_SIZE 4096  // 4KB for each IST stack

// 64-bit GDT layout: null, kernel code, kernel data, user data, user code, TSS (2 slots)
// Build a raw buffer (5 normal entries + TSS descriptor) then load.
static gdt_entry_t gdt_entries[5];
static gdt_tss_entry_t gdt_tss; // occupies 16 bytes
static uint8_t gdt_raw[5 * sizeof(gdt_entry_t) + sizeof(gdt_tss_entry_t)];
static gdt_ptr_t gdt_ptr;
static tss_t tss;

// IST stacks
static uint8_t* ist1_stack = NULL;
static uint8_t* ist2_stack = NULL;
static uint8_t* ist3_stack = NULL;

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

void tss_init(void) {
    // Allocate IST stacks
    ist1_stack = (uint8_t*)pmm_alloc_frame();  // Double Fault
    ist2_stack = (uint8_t*)pmm_alloc_frame();  // Page Fault
    ist3_stack = (uint8_t*)pmm_alloc_frame();  // General Protection Fault
    
    if (!ist1_stack || !ist2_stack || !ist3_stack) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[ERROR] Impossibile allocare stack IST!\n");
        return;
    }
    
    // Azzera il TSS
    uint8_t* tss_ptr = (uint8_t*)&tss;
    for (int i = 0; i < sizeof(tss_t); i++) {
        tss_ptr[i] = 0;
    }
    
    // Configura IST (puntano alla fine dello stack perché cresce verso il basso)
    tss.ist1 = (uint64_t)(ist1_stack + IST_STACK_SIZE);
    tss.ist2 = (uint64_t)(ist2_stack + IST_STACK_SIZE);
    tss.ist3 = (uint64_t)(ist3_stack + IST_STACK_SIZE);
    tss.iomap_base = sizeof(tss_t);
    
    // Entry standard
    gdt_set_gate(0, 0, 0, 0, 0);                 // Null
    gdt_set_gate(1, 0, 0x000FFFFF, 0x9A, 0xA0);  // Kernel code (limit impostato a 1MB per sicurezza)
    gdt_set_gate(2, 0, 0x000FFFFF, 0x92, 0xC0);  // Kernel data
    gdt_set_gate(3, 0, 0x000FFFFF, 0xF2, 0xC0);  // User data
    gdt_set_gate(4, 0, 0x000FFFFF, 0xFA, 0xA0);  // User code

    // TSS entry (due slot consecutivi)
    gdt_set_tss((uint64_t)&tss, sizeof(tss_t) - 1);

    // Copia nella rappresentazione contigua
    uint8_t* dst = gdt_raw;
    for (int i = 0; i < 5; i++) {
        const uint8_t* src = (const uint8_t*)&gdt_entries[i];
        for (size_t b = 0; b < sizeof(gdt_entry_t); b++) {
            dst[i * sizeof(gdt_entry_t) + b] = src[b];
        }
    }
    // Copia TSS (16 byte)
    const uint8_t* tss_src = (const uint8_t*)&gdt_tss;
    size_t base_off = 5 * sizeof(gdt_entry_t);
    for (size_t b = 0; b < sizeof(gdt_tss_entry_t); b++) {
        dst[base_off + b] = tss_src[b];
    }

    gdt_ptr.limit = sizeof(gdt_raw) - 1;
    gdt_ptr.base = (uint64_t)&gdt_raw[0];

    terminal_writestring("[DBG] GDT base: "); print_hex(gdt_ptr.base); terminal_writestring(" limit: "); print_hex(gdt_ptr.limit); terminal_writestring("\n");
    terminal_writestring("[DBG] TSS addr: "); print_hex((uint64_t)&tss); terminal_writestring(" selector 0x28\n");

    // Carica GDT e TSS
    gdt_flush((uint64_t)&gdt_ptr);
    tss_flush(0x28);
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[OK] TSS inizializzato con IST\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

void tss_set_kernel_stack(uint64_t stack) {
    tss.rsp0 = stack;
}