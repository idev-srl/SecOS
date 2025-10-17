/*
 * SecOS Kernel - Linker Script
 * Defines section layout and kernel load address.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
ENTRY(_start)

SECTIONS
{
    . = 2M;
    .text : ALIGN(8) {
        mb2_header_start = .;
        KEEP(*(.multiboot))
        _text_start = .;
        *(.text*)
        _text_end = .;
    }
    
    .rodata : ALIGN(4K) {
        _rodata_start = .;
        *(.rodata*)
        _rodata_end = .;
    }
    
    .data : ALIGN(4K) {
        _data_start = .;
        *(.data*)
        _data_end = .;
    }
    
    .bss : ALIGN(4K) {
        *(COMMON)
        _bss_start = .;
        *(.bss*)
        _bss_end = .;
    }
    
    /* Symbol marking end of kernel */
    _kernel_end = .;
}