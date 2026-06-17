/*
 * SecOS Kernel - Linker Script (higher-half)
 * The kernel runs at the canonical high-half base KERNEL_VMA = 0xFFFFFFFF80000000.
 * A small .boot section stays at its low physical load address (identity) so the
 * 32-bit Multiboot2 entry can run before paging maps the high half. All other
 * sections are linked HIGH (VMA) but LOADED low (LMA = VMA - KERNEL_VMA), so the
 * single kernel.bin still loads contiguously at physical 2 MB.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
    . = 2M;
    _boot_phys_start = .;

    /* Low-address boot section: Multiboot2 header + 32-bit setup + initial page
     * tables + boot GDT + boot stack. VMA == LMA == physical (identity). */
    .boot : ALIGN(8) {
        mb2_header_start = .;
        KEEP(*(.multiboot))
        *(.boot.text*)
        *(.boot.data*)
        . = ALIGN(4K);
        *(.boot.bss*)
        . = ALIGN(4K);
    }
    _boot_phys_end = .;

    /* Shift to the higher half for the kernel proper. LMA continues physically
     * right after .boot via AT(). */
    . += KERNEL_VMA;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
        _text_start = .;
        *(.text*)
        _text_end = .;
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        _rodata_start = .;
        *(.rodata*)
        _rodata_end = .;
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        _data_start = .;
        *(.data*)
        _data_end = .;
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        _bss_start = .;
        *(COMMON)
        *(.bss*)
        _bss_end = .;
    }

    /* High VMA end-of-kernel marker (the PMM converts it to physical). */
    _kernel_end = .;
}
