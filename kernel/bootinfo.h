/*
 * SecOS Boot Information Structure (UEFI Strategy B)
 * Shared handoff data passed from UEFI bootloader to kernel entry.
 * Contains framebuffer metadata and (future) memory map descriptors.
 *
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef SECOS_BOOTINFO_H
#define SECOS_BOOTINFO_H

#include <stdint.h>

struct secos_boot_info {
    uint64_t fb_addr;      // Framebuffer base physical (identity assumed early)
    uint32_t fb_width;     // Horizontal resolution
    uint32_t fb_height;    // Vertical resolution
    uint32_t fb_pitch;     // Bytes per scan line
    uint32_t fb_bpp;       // Bits per pixel (expected 32)
    // Memory map (UEFI descriptors)
    uint64_t mem_desc_count;          // Number of descriptors
    void*    mem_descs;               // Pointer to first descriptor (physical/identity mapped)
    uint64_t mem_desc_size;           // Size of each descriptor
    uint64_t mem_desc_version;        // Descriptor version
    uint64_t mem_map_key;             // Key for ExitBootServices (informativo per kernel)
    uint64_t acpi_rsdp;               // [M28] ACPI RSDP physical address (0 if not found)
    // Future extensions: SMBIOS, RNG seed, etc.
    uint64_t flags;                   // Misc flags (bit0: GOP valid, bit1: memory map valid,
                                      //  bit2: cmdline valid)
    // [M50] The loader's own UEFI load options as ASCII (e.g. `BOOTX64.EFI
    // bkl=on nokaslr` from the UEFI shell, or a boot entry's optional data):
    // the kernel treats it as its boot command line (/proc/cmdline, nosmp,
    // bkl=). NUL-terminated; non-printable characters become spaces.
    char     cmdline[128];
};
#define SECOS_BOOTINFO_F_CMDLINE (1ULL << 2)

#endif // SECOS_BOOTINFO_H
