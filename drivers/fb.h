#ifndef FB_H
#define FB_H
/*
 * SecOS Kernel - Framebuffer Core Driver
 * Provides low-level framebuffer discovery and basic drawing primitives.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "config.h"

#if ENABLE_FB

typedef struct framebuffer_info {
    uint64_t addr;      // Physical address
    uint64_t virt_addr; // Virtual address (after physmap ready)
    uint32_t pitch;     // Bytes per scanline
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;       // Bits per pixel
    uint8_t  type;      // 0 indexed, 1 RGB, etc.
    uint32_t red_mask_size;
    uint32_t red_mask_pos;
    uint32_t green_mask_size;
    uint32_t green_mask_pos;
    uint32_t blue_mask_size;
    uint32_t blue_mask_pos;
} framebuffer_info_t;

int fb_init(uint32_t multiboot_info);
void fb_clear(uint32_t color);
void fb_putpixel(int x, int y, uint32_t color);
void fb_draw_test_pattern(void);
void fb_debug_fill(uint32_t color_rgb);
int fb_get_info(framebuffer_info_t* out); // ritorna 1 se pronto, 0 se non pronto
void fb_finalize_mapping(void); // da chiamare dopo physmap per abilitare accesso alto

#endif // ENABLE_FB

#endif // FB_H
