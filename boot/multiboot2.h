#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H
#include <stdint.h>

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289

struct multiboot2_tag {
    uint32_t type;
    uint32_t size; // includes this header
};

#define MULTIBOOT2_TAG_END 0
#define MULTIBOOT2_TAG_FRAMEBUFFER 8 // standard type for framebuffer info in MB2

// Framebuffer tag layout (per spec)
struct multiboot2_tag_framebuffer {
    uint32_t type;      // =8
    uint32_t size;      // >= 24 + optional color info
    uint64_t addr;      // framebuffer physical
    uint32_t pitch;     // bytes per scanline
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  type_fb;   // 0 indexed, 1 RGB, etc.
    uint8_t  reserved;
    uint8_t  reserved2;
    // Followed by color info if type_fb==1 (RGB): 3* (uint8_t size,pos)
};

#endif