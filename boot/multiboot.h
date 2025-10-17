#ifndef MULTIBOOT_H
#define MULTIBOOT_H

/* Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

// Struttura Multiboot info
struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    // Nota: Per usare framebuffer lineare leggiamo vbe_mode_info tramite vbe_mode_info pointer
} __attribute__((packed));

// Entry della memory map
struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed));

// Tipi di memoria
#define MULTIBOOT_MEMORY_AVAILABLE        1
#define MULTIBOOT_MEMORY_RESERVED         2
#define MULTIBOOT_MEMORY_ACPI_RECLAIMABLE 3
#define MULTIBOOT_MEMORY_NVS              4
#define MULTIBOOT_MEMORY_BADRAM           5

// Struttura VBE Mode Info (parziale - campi usati)
struct vbe_mode_info {
    uint16_t attributes;            // 0
    uint8_t  winA, winB;           // 2,3
    uint16_t granularity;          // 4
    uint16_t winsize;              // 6
    uint16_t segmentA, segmentB;   // 8,10
    uint32_t realFctPtr;           // 12
    uint16_t pitch;                // 16 Bytes per scanline
    uint16_t width;                // 18
    uint16_t height;               // 20
    uint8_t  wChar;                // 22
    uint8_t  yChar;                // 23
    uint8_t  planes;               // 24
    uint8_t  bpp;                  // 25 bits per pixel
    uint8_t  banks;                // 26
    uint8_t  memoryModel;          // 27
    uint8_t  bankSize;             // 28
    uint8_t  imagePages;           // 29
    uint8_t  reserved0;            // 30
    uint8_t  redMaskSize;          // 31
    uint8_t  redMaskPos;           // 32
    uint8_t  greenMaskSize;        // 33
    uint8_t  greenMaskPos;         // 34
    uint8_t  blueMaskSize;         // 35
    uint8_t  blueMaskPos;          // 36
    uint8_t  reserved1;            // 37
    uint32_t linearFrameBuffer;    // 40 physical address
    // (Ignoriamo campi successivi per ora)
} __attribute__((packed));

#endif