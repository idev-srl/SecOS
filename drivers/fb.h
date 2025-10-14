#ifndef FB_H
#define FB_H

#include <stdint.h>
#include "config.h"

#if ENABLE_FB

typedef struct framebuffer_info {
    uint64_t addr;      // Physical address
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

#endif // ENABLE_FB

#endif // FB_H
