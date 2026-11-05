/* SecOS <linux/fb.h>: the Linux fbdev ABI served by /dev/fb0 (x86-64
 * layouts). Opening /dev/fb0 requires CAP_DISPLAY in the program's signed
 * manifest; map it with secos_mmap(..., MAP_SHARED, fd, 0) (see
 * docs/GUI_INPUT.md). There is no mode setting: FBIOPUT_VSCREENINFO only
 * accepts the current mode.
 * SPDX-License-Identifier: MIT */
#ifndef _SECOS_LINUX_FB_H
#define _SECOS_LINUX_FB_H
#include <stdint.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606
#define FBIOBLANK           0x4611

#define FB_TYPE_PACKED_PIXELS 0
#define FB_VISUAL_TRUECOLOR   2
#define FB_ACCEL_NONE         0
#define FB_ACTIVATE_NOW       0
#define FB_ACTIVATE_FORCE     128
#define FB_BLANK_UNBLANK      0
#define FB_BLANK_POWERDOWN    4

struct fb_bitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct fb_var_screeninfo {
    uint32_t xres, yres;
    uint32_t xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height, width;
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
};

struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;     /* reported as 0 (no physical addresses leak) */
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    unsigned long mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

#endif
