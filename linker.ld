/*
 * SecOS Kernel - Linker Script (higher-half)
 * The kernel is LINKED at the canonical high-half base KERNEL_VMA =
 * 0xFFFFFFFF80000000; at boot it is slid by a random 2 MiB multiple (KASLR, M45,
 * boot/kaslr.h) and runs at KERNEL_VMA + slide.
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
        *(.boot.rodata*)
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
        /* Exception table: {faulting insn, fixup} pairs for the user-access
         * primitives (arch/x86/uaccess.asm). Read-only, searched on a ring-0
         * #PF on a user address (kernel/panic.c -> uaccess_fixup_lookup). */
        . = ALIGN(8);
        __ex_table_start = .;
        KEEP(*(.ex_table))
        __ex_table_end = .;
        _rodata_end = .;
    }

    /* Code-signing note (QSEC manifest + QSIG signature). Its own SHT_NOTE
     * section so ld emits a PT_NOTE phdr; page-aligned + R/NX at runtime
     * (vmm_protect_kernel_sections). Excluded from the measured-boot hash,
     * which covers only .text+.rodata. */
    .note.secos ALIGN(4K) : AT(ADDR(.note.secos) - KERNEL_VMA) {
        _note_start = .;
        KEEP(*(.note.secos))
        . = ALIGN(4K);
        _note_end = .;
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        _data_start = .;
        *(.data*)
        _data_end = .;
    }

    /* [M45] KASLR relocation table (boot/kaslr.h, tools/kaslr-relocs.py). It
     * is produced from a first link and linked back in here, AFTER every
     * section that carries relocations and BEFORE .bss, so adding it moves
     * nothing the table describes (the Makefile re-extracts and compares).
     * Loaded with the image and covered by the kernel signature. The early
     * relocators find it by its physical load address _kaslr_phys. */
    .kaslr ALIGN(4K) : AT(ADDR(.kaslr) - KERNEL_VMA) {
        _kaslr_start = .;
        KEEP(*(.kaslr))
        . = ALIGN(8);
        _kaslr_end = .;
    }
    _kaslr_phys = LOADADDR(.kaslr);

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        _bss_start = .;
        *(COMMON)
        *(.bss)
        /* [M55] The DMA region (.dmabss): every static driver buffer a device
         * may reach, tagged __dma (kernel/dma.h) = input section .bss.dma.
         * Page-aligned at both ends so the strict kernel DMA domain maps
         * exactly these pages; after the boot stack (stack_bottom must stay
         * the first .bss symbol, see vmm_setup_kernel_guard_pages). */
        . = ALIGN(4K);
        __dma_start = .;
        *(.bss.dma)
        . = ALIGN(4K);
        __dma_end = .;
        *(.bss.*)
        _bss_end = .;
    }

    /* High VMA end-of-kernel marker (the PMM converts it to physical). */
    _kernel_end = .;

    /* Drop unwind/CFI tables: the freestanding kernel never unwinds, and an
     * orphan-placed .eh_frame inside the LOAD segment would leave an
     * unprotected (RWX) gap between sections — a W^X hole. */
    /DISCARD/ : { *(.eh_frame) *(.eh_frame_hdr) *(.comment) *(.note.GNU-stack) }
}
