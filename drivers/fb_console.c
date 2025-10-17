/*
 * SecOS Kernel - Framebuffer Console
 * Text console rendering on 32-bpp linear framebuffer with glyph drawing,
 * cursor blinking, logo glow animation, and optional double buffering.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "fb_console.h"
#include "terminal.h" // VGA color enum
#include "fb.h"
#include "vmm.h" // phys_to_virt
#include "timer.h" // timer callback registration for blink
#include <stddef.h>
#include <stdint.h>
#if ENABLE_FB

// Standard VGA 8x16 font (ASCII 32..126). bit7 = leftmost pixel.
static const uint8_t font8x16[95][16] = {
 /* 0x20 ' ' */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
 /* 0x21 '!' */ {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0,0,0,0,0,0},
 /* 0x22 '"'*/ {0x36,0x36,0x36,0x12,0x24,0,0,0,0,0,0,0,0,0,0,0},
 /* 0x23 '#' */ {0x36,0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x36,0,0,0,0,0,0,0},
 /* 0x24 '$' */ {0x0C,0x3E,0x03,0x03,0x1E,0x30,0x30,0x1F,0x0C,0x0C,0,0,0,0,0,0},
 /* 0x25 '%' */ {0x00,0x63,0x73,0x18,0x0C,0x06,0x67,0x63,0,0,0,0,0,0,0,0},
 /* 0x26 '&' */ {0x1C,0x36,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0,0,0,0,0,0,0,0},
 /* 0x27 '\''*/ {0x0C,0x0C,0x0C,0x06,0x06,0,0,0,0,0,0,0,0,0,0,0},
 /* 0x28 '(' */ {0x18,0x0C,0x06,0x06,0x06,0x06,0x06,0x0C,0x18,0,0,0,0,0,0,0},
 /* 0x29 ')' */ {0x06,0x0C,0x18,0x18,0x18,0x18,0x18,0x0C,0x06,0,0,0,0,0,0,0},
 /* 0x2A '*' */ {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0,0,0,0,0,0,0,0},
 /* 0x2B '+' */ {0x00,0x0C,0x0C,0x7F,0x0C,0x0C,0x00,0x00,0,0,0,0,0,0,0,0},
 /* 0x2C ',' */ {0x00,0,0,0,0,0,0,0x0C,0x0C,0x06,0,0,0,0,0,0},
 /* 0x2D '-' */ {0x00,0,0,0x7F,0,0,0,0,0,0,0,0,0,0,0,0},
 /* 0x2E '.' */ {0x00,0,0,0,0,0,0,0x0C,0x0C,0,0,0,0,0,0,0},
 /* 0x2F '/' */ {0x60,0x70,0x18,0x0C,0x06,0x03,0x01,0,0,0,0,0,0,0,0,0},
 /* 0x30 '0' */ {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x63,0x3E,0,0,0,0,0,0,0,0},
 /* 0x31 '1' */ {0x18,0x1C,0x18,0x18,0x18,0x18,0x18,0x7E,0,0,0,0,0,0,0,0},
 /* 0x32 '2' */ {0x3E,0x63,0x60,0x30,0x18,0x0C,0x06,0x7F,0,0,0,0,0,0,0,0},
 /* 0x33 '3' */ {0x3E,0x63,0x60,0x3C,0x60,0x60,0x63,0x3E,0,0,0,0,0,0,0,0},
 /* 0x34 '4' */ {0x30,0x38,0x3C,0x36,0x33,0x7F,0x30,0x30,0,0,0,0,0,0,0,0},
 /* 0x35 '5' */ {0x7F,0x03,0x03,0x3F,0x60,0x60,0x63,0x3E,0,0,0,0,0,0,0,0},
 /* 0x36 '6' */ {0x3C,0x06,0x03,0x3F,0x63,0x63,0x63,0x3E,0,0,0,0,0,0,0,0},
 /* 0x37 '7' */ {0x7F,0x63,0x60,0x30,0x18,0x0C,0x0C,0x0C,0,0,0,0,0,0,0,0},
 /* 0x38 '8' */ {0x3E,0x63,0x63,0x3E,0x63,0x63,0x63,0x3E,0,0,0,0,0,0,0,0},
 /* 0x39 '9' */ {0x3E,0x63,0x63,0x7E,0x60,0x60,0x30,0x1E,0,0,0,0,0,0,0,0},
 /* 0x3A ':' */ {0,0x0C,0x0C,0,0,0,0,0x0C,0x0C,0,0,0,0,0,0,0},
 /* 0x3B ';' */ {0,0x0C,0x0C,0,0,0,0,0x0C,0x0C,0x06,0,0,0,0,0,0},
 /* 0x3C '<' */ {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0,0,0,0,0,0,0,0,0},
 /* 0x3D '=' */ {0,0,0x7F,0,0x7F,0,0,0,0,0,0,0,0,0,0,0},
 /* 0x3E '>' */ {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0,0,0,0,0,0,0,0,0},
 /* 0x3F '?' */ {0x3E,0x63,0x60,0x30,0x18,0,0x18,0x18,0,0,0,0,0,0,0,0},
 /* 0x40 '@' */ {0x3E,0x41,0x5D,0x55,0x5D,0x1D,0x01,0x3E,0,0,0,0,0,0,0,0},
 /* 0x41 'A' */ {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0,0,0,0,0,0,0,0},
 /* 0x42 'B' */ {0x3F,0x66,0x66,0x3E,0x66,0x66,0x66,0x3F,0,0,0,0,0,0,0,0},
 /* 0x43 'C' */ {0x3C,0x66,0x03,0x03,0x03,0x03,0x66,0x3C,0,0,0,0,0,0,0,0},
 /* 0x44 'D' */ {0x1F,0x36,0x66,0x66,0x66,0x66,0x36,0x1F,0,0,0,0,0,0,0,0},
 /* 0x45 'E' */ {0x7F,0x46,0x16,0x1E,0x16,0x46,0x46,0x7F,0,0,0,0,0,0,0,0},
 /* 0x46 'F' */ {0x7F,0x46,0x16,0x1E,0x16,0x06,0x06,0x0F,0,0,0,0,0,0,0,0},
 /* 0x47 'G' */ {0x3C,0x66,0x03,0x03,0x73,0x63,0x66,0x7C,0,0,0,0,0,0,0,0},
 /* 0x48 'H' */ {0x63,0x63,0x63,0x7F,0x63,0x63,0x63,0x63,0,0,0,0,0,0,0,0},
 /* 0x49 'I' */ {0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0,0,0},
 /* 0x4A 'J' */ {0x78,0x30,0x30,0x30,0x30,0x33,0x33,0x1E,0,0,0,0,0,0,0,0},
 /* 0x4B 'K' */ {0x67,0x66,0x36,0x1E,0x1E,0x36,0x66,0x67,0,0,0,0,0,0,0,0},
 /* 0x4C 'L' */ {0x0F,0x06,0x06,0x06,0x06,0x46,0x66,0x7F,0,0,0,0,0,0,0,0},
 /* 0x4D 'M' */ {0x41,0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0,0,0,0,0,0,0,0},
 /* 0x4E 'N' */ {0x63,0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0,0,0,0,0,0,0,0},
 /* 0x4F 'O' */ {0x3E,0x63,0x63,0x63,0x63,0x63,0x63,0x3E,0,0,0,0,0,0,0,0},
 /* 0x50 'P' */ {0x3F,0x66,0x66,0x66,0x3E,0x06,0x06,0x0F,0,0,0,0,0,0,0,0},
 /* 0x51 'Q' */ {0x3E,0x63,0x63,0x63,0x63,0x6B,0x73,0x5E,0,0,0,0,0,0,0,0},
 /* 0x52 'R' */ {0x3F,0x66,0x66,0x3E,0x1E,0x36,0x66,0x67,0,0,0,0,0,0,0,0},
 /* 0x53 'S' */ {0x3C,0x66,0x06,0x1C,0x30,0x60,0x66,0x3C,0,0,0,0,0,0,0,0},
 /* 0x54 'T' */ {0x7E,0x5A,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0,0,0},
 /* 0x55 'U' */ {0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x3E,0,0,0,0,0,0,0,0},
 /* 0x56 'V' */ {0x63,0x63,0x63,0x63,0x63,0x36,0x1C,0x08,0,0,0,0,0,0,0,0},
 /* 0x57 'W' */ {0x63,0x63,0x63,0x6B,0x7F,0x7F,0x77,0x63,0,0,0,0,0,0,0,0},
 /* 0x58 'X' */ {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x63,0,0,0,0,0,0,0,0},
 /* 0x59 'Y' */ {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x3C,0,0,0,0,0,0,0,0},
 /* 0x5A 'Z' */ {0x7F,0x63,0x31,0x18,0x0C,0x46,0x63,0x7F,0,0,0,0,0,0,0,0},
 /* 0x5B '[' */ {0x1E,0x06,0x06,0x06,0x06,0x06,0x06,0x1E,0,0,0,0,0,0,0,0},
 /* 0x5C '\\'*/ {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0,0,0,0,0,0,0,0,0},
 /* 0x5D ']' */ {0x1E,0x18,0x18,0x18,0x18,0x18,0x18,0x1E,0,0,0,0,0,0,0,0},
 /* 0x5E '^' */ {0x08,0x1C,0x36,0x63,0,0,0,0,0,0,0,0,0,0,0,0},
 /* 0x5F '_' */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0xFF,0},
 /* 0x60 '`' */ {0x0C,0x0C,0x18,0,0,0,0,0,0,0,0,0,0,0,0,0},
 /* 0x61 'a' */ {0,0,0x3C,0x60,0x7C,0x66,0x66,0x7C,0,0,0,0,0,0,0,0},
 /* 0x62 'b' */ {0x07,0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0,0,0,0,0,0,0,0},
 /* 0x63 'c' */ {0,0,0x3C,0x66,0x06,0x06,0x66,0x3C,0,0,0,0,0,0,0,0},
 /* 0x64 'd' */ {0x70,0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0,0,0,0,0,0,0,0},
 /* 0x65 'e' */ {0,0,0x3C,0x66,0x7E,0x06,0x66,0x3C,0,0,0,0,0,0,0,0},
 /* 0x66 'f' */ {0x38,0x0C,0x0C,0x3E,0x0C,0x0C,0x0C,0x1E,0,0,0,0,0,0,0,0},
 /* 0x67 'g' */ {0,0,0x3C,0x66,0x66,0x66,0x3C,0x60,0x3C,0,0,0,0,0,0,0},
 /* 0x68 'h' */ {0x07,0x06,0x06,0x3E,0x66,0x66,0x66,0x66,0,0,0,0,0,0,0,0},
 /* 0x69 'i' */ {0x18,0,0x1C,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0,0,0},
 /* 0x6A 'j' */ {0x30,0,0x38,0x30,0x30,0x30,0x30,0x33,0x1E,0,0,0,0,0,0,0},
 /* 0x6B 'k' */ {0x07,0x06,0x66,0x36,0x1E,0x36,0x66,0x67,0,0,0,0,0,0,0,0},
 /* 0x6C 'l' */ {0x1C,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0,0,0},
 /* 0x6D 'm' */ {0,0,0x63,0x77,0x7F,0x6B,0x63,0x63,0,0,0,0,0,0,0,0},
 /* 0x6E 'n' */ {0,0,0x3E,0x66,0x66,0x66,0x66,0x66,0,0,0,0,0,0,0,0},
 /* 0x6F 'o' */ {0,0,0x3C,0x66,0x66,0x66,0x66,0x3C,0,0,0,0,0,0,0,0},
 /* 0x70 'p' */ {0,0,0x3E,0x66,0x66,0x66,0x3E,0x06,0x0F,0,0,0,0,0,0,0},
 /* 0x71 'q' */ {0,0,0x7C,0x66,0x66,0x66,0x7C,0x60,0x78,0,0,0,0,0,0,0},
 /* 0x72 'r' */ {0,0,0x36,0x1E,0x06,0x06,0x06,0x0F,0,0,0,0,0,0,0,0},
 /* 0x73 's' */ {0,0,0x3C,0x06,0x1C,0x30,0x06,0x3C,0,0,0,0,0,0,0,0},
 /* 0x74 't' */ {0x08,0x0C,0x3E,0x0C,0x0C,0x0C,0x2C,0x18,0,0,0,0,0,0,0,0},
 /* 0x75 'u' */ {0,0,0x66,0x66,0x66,0x66,0x66,0x3E,0,0,0,0,0,0,0,0},
 /* 0x76 'v' */ {0,0,0x66,0x66,0x66,0x66,0x3C,0x18,0,0,0,0,0,0,0,0},
 /* 0x77 'w' */ {0,0,0x63,0x63,0x6B,0x7F,0x7F,0x36,0,0,0,0,0,0,0,0},
 /* 0x78 'x' */ {0,0,0x66,0x3C,0x18,0x3C,0x66,0x66,0,0,0,0,0,0,0,0},
 /* 0x79 'y' */ {0,0,0x66,0x66,0x66,0x66,0x3C,0x18,0x30,0,0,0,0,0,0,0},
 /* 0x7A 'z' */ {0,0,0x7E,0x30,0x18,0x0C,0x06,0x7E,0,0,0,0,0,0,0,0},
 /* 0x7B '{' */ {0x38,0x0C,0x0C,0x06,0x0C,0x0C,0x38,0,0,0,0,0,0,0,0,0},
 /* 0x7C '|' */ {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0,0,0,0,0,0,0,0,0},
 /* 0x7D '}' */ {0x07,0x0C,0x0C,0x18,0x0C,0x0C,0x07,0,0,0,0,0,0,0,0,0},
 /* 0x7E '~' */ {0x00,0,0x32,0x4C,0,0,0,0,0,0,0,0,0,0,0,0},
};

static int fb_enabled = 0;
static uint32_t fb_width, fb_height, fb_pitch;
static uint64_t fb_phys_addr = 0;
static uint8_t current_fg = VGA_COLOR_LIGHT_GREEN;
static uint8_t current_bg = VGA_COLOR_BLACK;
static uint32_t cursor_x = 0, cursor_y = 0;
static const uint32_t glyph_w = 8, glyph_h = 16;
static int cursor_visible = 1; // logical state
static int cursor_blink_phase = 0; // 0 shown, 1 hidden (inverted)
static uint32_t blink_counter = 0; // measured in ticks
static uint32_t blink_interval_ticks = 0; // derived from timer frequency (~500ms)
static int cursor_blink_enabled = 0;
static void fb_console_cursor_toggle(void);
// Double buffering
static uint8_t* dbuf = NULL; // secondary (virtual) buffer when enabled
static int dbuf_enabled = 0;
static int dbuf_auto_flush = 0; // automatic flush flag
// (Scaling removed)

// VGA 16-color palette -> RGB
static uint32_t vga_palette[16] = {
    0x000000,0x0000AA,0x00AA00,0x00AAAA,0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,
    0x555555,0x5555FF,0x55FF55,0x55FFFF,0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF
};

static uint32_t blend(uint32_t a,uint32_t b,float t){
    if(t<0) t=0; if(t>1) t=1;
    uint8_t ar=(a>>16)&0xFF, ag=(a>>8)&0xFF, ab=a&0xFF;
    uint8_t br=(b>>16)&0xFF, bg=(b>>8)&0xFF, bb=b&0xFF;
    uint8_t rr = (uint8_t)(ar + (int)((br-ar)*t));
    uint8_t rg = (uint8_t)(ag + (int)((bg-ag)*t));
    uint8_t rb = (uint8_t)(ab + (int)((bb-ab)*t));
    return (rr<<16)|(rg<<8)|rb;
}

int fb_console_init(void){
    framebuffer_info_t info; if(!fb_get_info(&info)) return -1; if (info.bpp != 32) return -1;
    // If virt_addr unset but physical address is high, attempt mapping: physmap may already cover.
    if (info.virt_addr == 0 && info.addr >= (16ULL*1024*1024)) {
    // Attempt phys_to_virt lookup directly; assume vmm_extend_physmap was called earlier.
        extern uint64_t phys_to_virt(uint64_t phys);
        uint64_t tentative = phys_to_virt(info.addr);
    // Cannot easily verify mapping here without page-walk; assume valid post vmm_init_physmap.
        info.virt_addr = tentative;
    }
    fb_width = info.width; fb_height = info.height; fb_pitch = info.pitch; fb_phys_addr = (info.virt_addr ? info.virt_addr : info.addr);
    fb_enabled = 1; fb_clear(0x000000); cursor_x=0; cursor_y=0; dbuf_enabled=0; dbuf=NULL;
    // Draw SecOS logo on top-right
    extern void fb_console_draw_logo(void); fb_console_draw_logo();
    // Set a more legible default color (white on black)
    extern void terminal_setcolor(uint8_t color);
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    return 0;
}

// Internal helper to draw blinking cursor underline
static void fb_console_draw_cursor(void){
    if(!fb_enabled || !cursor_blink_enabled) return;
    uint32_t cell_w = glyph_w;
    uint32_t cell_h = glyph_h;
    uint32_t gx = cursor_x * cell_w; uint32_t gy = cursor_y * cell_h;
    uint8_t* base=(uint8_t*)(uint64_t)fb_phys_addr;
    uint8_t* target = (dbuf_enabled && dbuf)? dbuf : base;
    uint32_t* row; uint32_t fg_rgb = vga_palette[current_fg & 0xF];
    uint32_t blink_h = 2; if(blink_h>cell_h) blink_h=cell_h; // underline thickness
    for(uint32_t y=0;y<blink_h;y++){
        row=(uint32_t*)(target + (gy + cell_h - 1 - y) * fb_pitch);
        for(uint32_t x=0;x<cell_w;x++) row[gx + x] = fg_rgb;
    }
    if(dbuf_enabled && !dbuf_auto_flush) fb_console_flush();
}
static void fb_console_clear_cursor(void){
    if(!fb_enabled) return;
    uint32_t cell_w = glyph_w;
    uint32_t cell_h = glyph_h;
    uint32_t gx = cursor_x * cell_w; uint32_t gy = cursor_y * cell_h;
    uint8_t* base=(uint8_t*)(uint64_t)fb_phys_addr;
    uint8_t* target = (dbuf_enabled && dbuf)? dbuf : base;
    uint32_t* row; uint32_t bg_rgb = vga_palette[current_bg & 0xF];
    uint32_t blink_h = 2; if(blink_h>cell_h) blink_h=cell_h;
    for(uint32_t y=0;y<blink_h;y++){
        row=(uint32_t*)(target + (gy + cell_h - 1 - y) * fb_pitch);
        for(uint32_t x=0;x<cell_w;x++) row[gx + x] = bg_rgb;
    }
    if(dbuf_enabled && !dbuf_auto_flush) fb_console_flush();
}

// Simple stylized SecOS logo (5 letters) rendered top-right
static void draw_block(int x,int y,int w,int h,uint32_t rgb){
    if(x<0||y<0) return; if(x+w>fb_width) w=fb_width-x; if(y+h>fb_height) h=fb_height-y;
    uint8_t* base=(uint8_t*)(uint64_t)fb_phys_addr;
    uint8_t* target = (dbuf_enabled && dbuf)? dbuf : base;
    for(int yy=0; yy<h; yy++){
        uint32_t* row=(uint32_t*)(target + (y+yy)*fb_pitch);
        for(int xx=0; xx<w; xx++) row[x+xx]=rgb;
    }
}
// Logo chip coordinates & dimensions (preserved during scroll)
static int logo_chip_x=0, logo_chip_y=0, logo_chip_w=0, logo_chip_h=0;
static int logo_pin_len=6; // pin length used for computing total logo area
static int logo_glow_phase=0; // glow animation phase
static int logo_glow_enabled=1; // glow effect enabled
// Kernel version fallback if macro undefined
#ifndef KERNEL_VERSION
#define KERNEL_VERSION "v0.2"
#endif
static void fb_console_clear_logo_area(void){
    if(!fb_enabled) return;
    if(logo_chip_w==0 || logo_chip_h==0) return;
    int top = logo_chip_y - (logo_pin_len+2);
    int bottom = logo_chip_y + logo_chip_h + 2 + logo_pin_len; // esclusivo
    int left = logo_chip_x - (logo_pin_len+2);
    int right = logo_chip_x + logo_chip_w + 2 + logo_pin_len; // esclusivo
    if(top < 0) top = 0; if(left < 0) left = 0; if(bottom > (int)fb_height) bottom = fb_height; if(right > (int)fb_width) right = fb_width;
    uint8_t* base=(uint8_t*)(uint64_t)fb_phys_addr; uint8_t* target = (dbuf_enabled && dbuf)? dbuf : base;
    for(int y=top; y<bottom; y++){
        uint32_t* row=(uint32_t*)(target + y*fb_pitch);
        for(int x=left; x<right; x++) row[x]=0x000000;
    }
}
void fb_console_draw_logo(void){
    if(!fb_enabled) return;
    // Chip body dimensions
    int body_w = 120; int body_h = 40; int margin = 8;
    if(body_w + margin > (int)fb_width) body_w = fb_width - margin;
    logo_chip_w = body_w; logo_chip_h = body_h; logo_chip_x = fb_width - body_w - margin; logo_chip_y = 4;
    uint32_t body_top = 0x303030; uint32_t body_bot = 0x202020; uint32_t pin_col = 0x909090; uint32_t pin_shadow = 0x404040; uint32_t outline = 0xAAAAAA;
    // Vertical gradient fill
    for(int y=0;y<body_h;y++){
        float t=(float)y/(float)(body_h-1);
        uint32_t col = blend(body_top, body_bot, t);
        draw_block(logo_chip_x, logo_chip_y + y, body_w, 1, col);
    }
    // Outline rectangle
    draw_block(logo_chip_x, logo_chip_y, body_w, 1, outline);
    draw_block(logo_chip_x, logo_chip_y + body_h -1, body_w, 1, outline);
    draw_block(logo_chip_x, logo_chip_y, 1, body_h, outline);
    draw_block(logo_chip_x + body_w -1, logo_chip_y, 1, body_h, outline);
    // Pins: top & bottom rows
    int pin_len = logo_pin_len; int pin_spacing = 10;
    for(int px=logo_chip_x + 6; px < logo_chip_x + body_w - 6; px += pin_spacing){
    // top side pin
        draw_block(px, logo_chip_y - (pin_len+2), 4, pin_len, pin_col);
        draw_block(px+1, logo_chip_y - (pin_len+2), 2, pin_len, pin_shadow); // centrale leggero scuro
    // bottom side pin
        draw_block(px, logo_chip_y + body_h +2, 4, pin_len, pin_col);
        draw_block(px+1, logo_chip_y + body_h +2, 2, pin_len, pin_shadow);
    }
    // Side pins
    for(int py=logo_chip_y + 6; py < logo_chip_y + body_h - 6; py += pin_spacing){
    // left side pin
        draw_block(logo_chip_x - (pin_len+2), py, pin_len, 4, pin_col);
        draw_block(logo_chip_x - (pin_len+2), py+1, pin_len, 2, pin_shadow);
    // right side pin
        draw_block(logo_chip_x + body_w +2, py, pin_len, 4, pin_col);
        draw_block(logo_chip_x + body_w +2, py+1, pin_len, 2, pin_shadow);
    }
    // Label "SecOS" centered inside chip (scale 1, bright color)
    const char* lbl="SecOS"; int len=5; int gw=glyph_w; int gh=glyph_h; int spacing=1; int text_w=len*gw + (len-1)*spacing; int tx = logo_chip_x + (body_w - text_w)/2; int ty = logo_chip_y + (body_h - gh)/2;
    uint32_t txt_col = 0x66CCFF; uint32_t txt_shadow = 0x000000;
    for(int i=0;i<len;i++){
        char c = lbl[i]; if(c<' '||c>'~') c='?'; const uint8_t* g = font8x16[c-32];
        int gx = tx + i*(gw+spacing);
    // shadow layer
        for(int r=0;r<gh;r++){ uint8_t line=g[r]; for(int col=0; col<gw; col++){ if((line>>col)&1) draw_block(gx+col+1, ty+r+1,1,1, txt_shadow); } }
    // glyph layer
        for(int r=0;r<gh;r++){ uint8_t line=g[r]; for(int col=0; col<gw; col++){ uint32_t bit=(line>>col)&1; draw_block(gx+col, ty+r,1,1, bit?txt_col: (uint32_t)0x000000); } }
    }
    // Version string below (centered)
    const char* ver = KERNEL_VERSION; int vlen=0; while(ver[vlen]) vlen++; int vtext_w = vlen*gw + (vlen-1)*spacing; int vtx = logo_chip_x + (body_w - vtext_w)/2; int vty = logo_chip_y + body_h - gh - 2;
    uint32_t ver_col = 0x88FFAA;
    for(int i=0;i<vlen;i++){
        char c = ver[i]; if(c<' '||c>'~') c='?'; const uint8_t* g = font8x16[c-32]; int gx = vtx + i*(gw+spacing);
        for(int r=0;r<gh;r++){ uint8_t line=g[r]; for(int col=0; col<gw; col++){ uint32_t bit=(line>>col)&1; if(bit) draw_block(gx+col, vty+r,1,1, ver_col); } }
    }
    // Outer glow: pulsating ring (color varies with logo_glow_phase)
    if(logo_glow_enabled){
        float phase = (logo_glow_phase % 200) / 200.0f; // ciclo lento
        uint32_t glow_a = 0x003366; uint32_t glow_b = 0x00AAFF; uint32_t glow_c = 0x33DDFF;
    // Two-stage interpolation
        uint32_t gcol1 = blend(glow_a, glow_b, phase);
        uint32_t gcol2 = blend(glow_b, glow_c, phase);
        uint32_t edge_top = blend(gcol1, gcol2, 0.5f);
        int pad = 3; int gx0 = logo_chip_x - pad; int gy0 = logo_chip_y - pad; int gw0 = logo_chip_w + pad*2; int gh0 = logo_chip_h + pad*2;
    // Faded border rectangle
        // Top & Bottom lines
        for(int x=0;x<gw0;x++){ draw_block(gx0 + x, gy0, 1,1, edge_top); draw_block(gx0 + x, gy0 + gh0 -1,1,1, edge_top); }
        for(int y=0;y<gh0;y++){ draw_block(gx0, gy0 + y,1,1, gcol1); draw_block(gx0 + gw0 -1, gy0 + y,1,1, gcol2); }
    }
}
// Redraw only glow ring to reduce flicker
static void fb_console_draw_logo_glow(void){
    if(!fb_enabled || !logo_glow_enabled) return;
    if(logo_chip_w==0) return;
    float phase = (logo_glow_phase % 200) / 200.0f;
    uint32_t glow_a = 0x003366; uint32_t glow_b = 0x00AAFF; uint32_t glow_c = 0x33DDFF;
    uint32_t gcol1 = blend(glow_a, glow_b, phase);
    uint32_t gcol2 = blend(glow_b, glow_c, phase);
    uint32_t edge_top = blend(gcol1, gcol2, 0.5f);
    int pad = 3; int gx0 = logo_chip_x - pad; int gy0 = logo_chip_y - pad; int gw0 = logo_chip_w + pad*2; int gh0 = logo_chip_h + pad*2;
    uint8_t* base=(uint8_t*)(uint64_t)fb_phys_addr; uint8_t* target = (dbuf_enabled && dbuf)? dbuf : base;
    // Top & Bottom
    for(int x=0;x<gw0;x++){ ((uint32_t*)(target + gy0*fb_pitch))[gx0 + x] = edge_top; ((uint32_t*)(target + (gy0+gh0-1)*fb_pitch))[gx0 + x] = edge_top; }
    // Left & Right
    for(int y=0;y<gh0;y++){ ((uint32_t*)(target + (gy0+y)*fb_pitch))[gx0] = gcol1; ((uint32_t*)(target + (gy0+y)*fb_pitch))[gx0 + gw0 -1] = gcol2; }
    if(dbuf_enabled && !dbuf_auto_flush) fb_console_flush();
}

// Timer tick callback
static void fb_console_tick(void){
    if(!cursor_blink_enabled || !fb_enabled) return;
    blink_counter++;
    // Update glow slowly every 5 ticks independent of cursor blink
    if(logo_glow_enabled && (blink_counter % 5)==0){ logo_glow_phase++; fb_console_draw_logo_glow(); }
    if (blink_counter >= blink_interval_ticks){
        blink_counter = 0;
    // Toggle visibility: erase then redraw
        if(cursor_blink_phase==0){ fb_console_clear_cursor(); cursor_blink_phase=1; }
        else { fb_console_draw_cursor(); cursor_blink_phase=0; }
    }
}

// Enable cursor blinking (called after timer_init)
int fb_console_enable_cursor_blink(uint32_t timer_freq){
    if(!fb_enabled) return -1;
    if (timer_freq==0) return -1;
    blink_interval_ticks = (timer_freq / 2); // ~500ms
    if (blink_interval_ticks==0) blink_interval_ticks=1;
    if (timer_register_tick_callback(fb_console_tick)!=0) return -1;
    cursor_blink_enabled = 1; blink_counter=0; cursor_blink_phase=0; fb_console_draw_cursor();
    return 0;
}

void fb_console_disable_cursor_blink(void){ if(!cursor_blink_enabled) return; cursor_blink_enabled=0; fb_console_clear_cursor(); }

void fb_console_set_color(uint8_t fg, uint8_t bg){ current_fg=fg; current_bg=bg; }
static void fb_console_flush_if_auto(void){ if(dbuf_enabled && dbuf_auto_flush) fb_console_flush(); }
// Scaling helpers removed

static void putpixel_raw(int x,int y,uint32_t rgb){ if(x<0||y<0||x>=(int)fb_width||y>=(int)fb_height) return; if(!fb_enabled) return; uint8_t* base=(uint8_t*)(uint64_t)fb_phys_addr; uint32_t* row=(uint32_t*)( (dbuf_enabled && dbuf)? (dbuf + y*fb_pitch) : (base + y*fb_pitch) ); row[x]=rgb; }
static void putpixel(int x,int y,uint32_t rgb){ putpixel_raw(x,y,rgb); }

static void draw_glyph(char c, uint32_t gx, uint32_t gy){
    // Basic support for CP437 box-drawing chars: ─ (0xC4), │ (0xB3), ┌ (0xDA), ┐ (0xBF), └ (0xC0), ┘ (0xD9), ├ (0xC3), ┤ (0xB4), ┬ (0xC2), ┴ (0xC1), ┼ (0xC5)
    // They may arrive as bytes >126 if shell allows extended 8-bit input; drawn manually.
    uint32_t fg_rgb = vga_palette[current_fg & 0xF];
    uint32_t bg_rgb = vga_palette[current_bg & 0xF];
    unsigned char uc = (unsigned char)c;
    if (uc >= 0xB3) {
        switch(uc) {
            case 0xC4: { // ─ horizontal line
                for(uint32_t row=0; row<glyph_h; row++) {
                    for(uint32_t col=0; col<glyph_w; col++) {
                        uint32_t rgb = bg_rgb;
                        // Linea centrale spessore 2 pixel (rows 7 e 8)
                        if (row==7 || row==8) rgb = fg_rgb;
                        putpixel(gx+col, gy+row, rgb);
                    }
                }
                return; }
            case 0xB3: { // │ vertical line
                for(uint32_t row=0; row<glyph_h; row++) {
                    for(uint32_t col=0; col<glyph_w; col++) {
                        uint32_t rgb = bg_rgb;
                        if (col==3 || col==4) rgb = fg_rgb; // col centrale spessore 2
                        putpixel(gx+col, gy+row, rgb);
                    }
                }
                return; }
            case 0xDA: { // ┌ top-left corner
                for(uint32_t row=0; row<glyph_h; row++) {
                    for(uint32_t col=0; col<glyph_w; col++) {
                        uint32_t rgb = bg_rgb;
                        if ((row==7 || row==8) && col < glyph_w/2) rgb = fg_rgb; // tratto orizzontale metà
                        if ((col==3 || col==4) && row < glyph_h/2) rgb = fg_rgb; // tratto verticale metà
                        putpixel(gx+col, gy+row, rgb);
                    }
                }
                return; }
            case 0xBF: { // ┐ top-right corner
                for(uint32_t row=0; row<glyph_h; row++) {
                    for(uint32_t col=0; col<glyph_w; col++) {
                        uint32_t rgb = bg_rgb;
                        if ((row==7 || row==8) && col >= glyph_w/2) rgb = fg_rgb;
                        if ((col==3 || col==4) && row < glyph_h/2) rgb = fg_rgb;
                        putpixel(gx+col, gy+row, rgb);
                    }
                }
                return; }
            case 0xC0: { // └ bottom-left corner
                for(uint32_t row=0; row<glyph_h; row++) {
                    for(uint32_t col=0; col<glyph_w; col++) {
                        uint32_t rgb = bg_rgb;
                        if ((row==7 || row==8) && col < glyph_w/2) rgb = fg_rgb;
                        if ((col==3 || col==4) && row >= glyph_h/2) rgb = fg_rgb;
                        putpixel(gx+col, gy+row, rgb);
                    }
                }
                return; }
            case 0xD9: { // ┘ bottom-right corner
                for(uint32_t row=0; row<glyph_h; row++) {
                    for(uint32_t col=0; col<glyph_w; col++) {
                        uint32_t rgb = bg_rgb;
                        if ((row==7 || row==8) && col >= glyph_w/2) rgb = fg_rgb;
                        if ((col==3 || col==4) && row >= glyph_h/2) rgb = fg_rgb;
                        putpixel(gx+col, gy+row, rgb);
                    }
                }
                return; }
            case 0xC3: { // ├ left junction
                for(uint32_t row=0; row<glyph_h; row++) {
                    for(uint32_t col=0; col<glyph_w; col++) {
                        uint32_t rgb = bg_rgb;
                        if (col==3 || col==4) rgb = fg_rgb; // verticale piena
                        if ((row==7 || row==8) && col >= glyph_w/2) rgb = fg_rgb; // orizzontale da metà
                        putpixel(gx+col, gy+row, rgb);
                    }
                }
                return; }
            case 0xB4: { // ┤ right junction
                for(uint32_t row=0; row<glyph_h; row++) {
                    for(uint32_t col=0; col<glyph_w; col++) {
                        uint32_t rgb = bg_rgb;
                        if (col==3 || col==4) rgb = fg_rgb;
                        if ((row==7 || row==8) && col < glyph_w/2) rgb = fg_rgb;
                        putpixel(gx+col, gy+row, rgb);
                    }
                }
                return; }
            case 0xC2: { // ┬ top junction
                for(uint32_t row=0; row<glyph_h; row++) {
                    for(uint32_t col=0; col<glyph_w; col++) {
                        uint32_t rgb = bg_rgb;
                        if ((row==7 || row==8)) rgb = fg_rgb; // orizzontale completa
                        if ((col==3 || col==4) && row < glyph_h/2) rgb = fg_rgb; // verticale metà
                        putpixel(gx+col, gy+row, rgb);
                    }
                }
                return; }
            case 0xC1: { // ┴ bottom junction
                for(uint32_t row=0; row<glyph_h; row++) {
                    for(uint32_t col=0; col<glyph_w; col++) {
                        uint32_t rgb = bg_rgb;
                        if ((row==7 || row==8)) rgb = fg_rgb;
                        if ((col==3 || col==4) && row >= glyph_h/2) rgb = fg_rgb;
                        putpixel(gx+col, gy+row, rgb);
                    }
                }
                return; }
            case 0xC5: { // ┼ full junction
                for(uint32_t row=0; row<glyph_h; row++) {
                    for(uint32_t col=0; col<glyph_w; col++) {
                        uint32_t rgb = bg_rgb;
                        if ((row==7 || row==8) || (col==3 || col==4)) rgb = fg_rgb;
                        putpixel(gx+col, gy+row, rgb);
                    }
                }
                return; }
            default: break; // others >126 unsupported
        }
    }
    if(c < 32 || c > 126) c='?';
    const uint8_t* glyph = font8x16[c-32];
    // Detect if glyph is completely empty (incomplete font) except for space
    int empty = 1;
    if (c == ' ') empty = 0; // space legitimately empty
    else {
        for (int i=0;i<16;i++) { if (glyph[i] != 0) { empty = 0; break; } }
    }
    if (empty) {
    // Draw placeholder 8x16 box with border
        for(uint32_t row=0; row<glyph_h; row++) {
            for(uint32_t col=0; col<glyph_w; col++) {
                int border = (row==0 || row==glyph_h-1 || col==0 || col==glyph_w-1);
                putpixel(gx+col, gy+row, border ? fg_rgb : bg_rgb);
            }
        }
        return;
    }
    for(uint32_t row=0; row<glyph_h; row++) {
        uint8_t line = glyph[row];
    // Interpret LSB-left (bit0 left pixel) to avoid visual inversion given current font encoding
        for(uint32_t col=0; col<glyph_w; col++) {
            uint32_t bit_on = (line >> col) & 1;
            putpixel(gx+col, gy+row, bit_on ? fg_rgb : bg_rgb);
        }
    }
}

static void scroll_if_needed(void){
    uint32_t cell_h = glyph_h;
    if ((cursor_y+1) * cell_h <= fb_height) return;
    uint8_t* base=(uint8_t*)(uint64_t)fb_phys_addr;
    uint8_t* target = dbuf_enabled && dbuf ? dbuf : base;
    size_t line_bytes = fb_pitch * cell_h;
    // First: clear logo area to avoid phantom copy upward
    fb_console_clear_logo_area();
    size_t copy_bytes = fb_pitch * (fb_height - cell_h);
    for(size_t i=0;i<copy_bytes;i++) target[i] = target[i+line_bytes];
    // Clear last row
    for(uint32_t y=fb_height - cell_h; y<fb_height; y++) {
        uint32_t* row=(uint32_t*)(target + y*fb_pitch);
        for(uint32_t x=0;x<fb_width;x++) row[x]=0x000000; }
    cursor_y--;
    fb_console_draw_logo(); // redraw in original position
    if(dbuf_enabled && !dbuf_auto_flush) fb_console_flush();
}

void fb_console_putc(char c){
    if(!fb_enabled){ return; }
    if(c=='\n'){
        if(cursor_blink_enabled && cursor_blink_phase==0){ fb_console_clear_cursor(); }
        cursor_x=0; cursor_y++; scroll_if_needed();
        if(cursor_blink_enabled){ fb_console_draw_cursor(); cursor_blink_phase=0; }
        fb_console_flush_if_auto();
        return;
    }
    if(c=='\b'){
        if(cursor_blink_enabled && cursor_blink_phase==0){ fb_console_clear_cursor(); }
        if(cursor_x>0){ cursor_x--; }
    else if(cursor_y>0){ cursor_y--; cursor_x = (fb_width / glyph_w) - 1; }
    uint32_t gx = cursor_x * glyph_w; uint32_t gy = cursor_y * glyph_h;
        uint32_t bg_rgb = vga_palette[current_bg & 0xF];
        for(uint32_t row=0; row<glyph_h; row++) {
            for(uint32_t col=0; col<glyph_w; col++) putpixel(gx+col, gy+row, bg_rgb);
        }
        if(cursor_blink_enabled){ fb_console_draw_cursor(); cursor_blink_phase=0; }
        fb_console_flush_if_auto();
        return;
    }
    uint32_t cells_per_row = fb_width / glyph_w;
    if(cursor_x >= cells_per_row){ cursor_x=0; cursor_y++; scroll_if_needed(); }
    draw_glyph(c, cursor_x*glyph_w, cursor_y*glyph_h);
    cursor_x++;
    if(cursor_blink_enabled){ fb_console_draw_cursor(); cursor_blink_phase=0; }
    if(cursor_x >= cells_per_row){ cursor_x=0; cursor_y++; scroll_if_needed(); }
    fb_console_flush_if_auto();
}
void fb_console_write(const char* s){ while(*s) fb_console_putc(*s++); }

// Double buffer API
int fb_console_enable_dbuf(void){
    if(!fb_enabled) return -1;
    if(dbuf_enabled) return 0;
    // Simple allocation via kmalloc (requires working heap)
    extern void* kmalloc(size_t sz); extern void kfree(void* p);
    size_t sz = fb_pitch * fb_height; dbuf = (uint8_t*)kmalloc(sz);
    if(!dbuf) return -1;
    // Copy existing content
    uint8_t* base=(uint8_t*)(uint64_t)fb_phys_addr;
    for(size_t i=0;i<sz;i++) dbuf[i]=base[i];
    dbuf_enabled=1;
    return 0;
}
void fb_console_disable_dbuf(void){
    if(!dbuf_enabled) return;
    extern void kfree(void* p);
    // Final flush before freeing
    fb_console_flush();
    kfree(dbuf); dbuf=NULL; dbuf_enabled=0;
}
void fb_console_flush(void){
    if(!dbuf_enabled || !dbuf) return; uint8_t* base=(uint8_t*)(uint64_t)fb_phys_addr;
    size_t sz = fb_pitch * fb_height; for(size_t i=0;i<sz;i++) base[i]=dbuf[i];
}
void fb_console_set_dbuf_auto(int on){ dbuf_auto_flush = on?1:0; if(dbuf_auto_flush) fb_console_flush(); }
// Removed scale APIs
void fb_console_fontdump(char c){
    if(c < 32 || c > 126){ terminal_writestring("[fontdump] range 32-126\n"); return; }
    const uint8_t* glyph = font8x16[c-32];
    terminal_writestring("Glyph "); terminal_putchar(c); terminal_writestring("\nHex:");
    for(int i=0;i<16;i++){ terminal_writestring(" "); print_hex(glyph[i]); }
    terminal_writestring("\nBits:\n");
    for(int row=0; row<16; row++){
        uint8_t line=glyph[row];
        for(int col=0; col<8; col++){ uint32_t bit=(line >> col)&1; terminal_putchar(bit?'#':'.'); }
        terminal_putchar('\n');
    }
}

#endif
