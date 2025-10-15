#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H
#include <stdint.h>

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289

struct multiboot2_tag {
    uint32_t type;
    uint32_t size; // includes this header
};

#define MULTIBOOT2_TAG_END 0
#define MULTIBOOT2_TAG_CMDLINE      1
#define MULTIBOOT2_TAG_BOOT_LOADER  2
#define MULTIBOOT2_TAG_MODULE       3
#define MULTIBOOT2_TAG_MEMINFO      4
#define MULTIBOOT2_TAG_BOOTDEV      5
#define MULTIBOOT2_TAG_MMAP         6
#define MULTIBOOT2_TAG_FRAMEBUFFER  8 // standard type for framebuffer info in MB2

// Generic memory map tag (type=6)
struct multiboot2_tag_mmap {
    uint32_t type;        // =6
    uint32_t size;        // total size including header + entries
    uint32_t entry_size;  // size of each entry
    uint32_t entry_version; // version (ignored)
    // followed by entries
};

// Memory map entry (after tag header). Note: MB2 adds a 'zero' / reserved field.
struct multiboot2_mmap_entry {
    uint64_t addr;   // base address
    uint64_t len;    // length in bytes
    uint32_t type;   // 1=available, others reserved/etc.
    uint32_t zero;   // reserved
};

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