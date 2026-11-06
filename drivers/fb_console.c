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
#include "vt.h"    // escape-sequence interpreter + cell grid
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
// [M40] Graphics mode (KDSETMODE KD_GRAPHICS): a client owns the screen. The vt
// keeps interpreting output into its cell grid, but nothing is drawn; leaving
// graphics mode repaints the whole console from the grid (fb_console_redraw).
static volatile int fb_gfx = 0;
static uint32_t fb_width, fb_height, fb_pitch;
static uint64_t fb_phys_addr = 0;
static const uint32_t glyph_w = 8, glyph_h = 16;
static int cursor_blink_phase = 0; // 0 shown, 1 hidden
static uint32_t blink_counter = 0; // measured in ticks
static uint32_t blink_interval_ticks = 0; // derived from timer frequency (~500ms)
static int cursor_blink_enabled = 0;
static void fb_console_flush_if_auto(void);
// Double buffering. Rendering + scrolling go to `dbuf` (normal RAM, fast); the
// flush copies only the dirty rectangle to the framebuffer (write-only, 64-bit
// words). This avoids reading VRAM (catastrophically slow over the bus on real
// hardware) and minimizes VRAM traffic — the fix for slow console scroll.
static uint8_t* dbuf = NULL; // secondary (virtual) buffer when enabled
static int dbuf_enabled = 0;
static int dbuf_auto_flush = 0; // automatic flush flag
// Dirty rectangle (pixels): [dx0,dx1) x [dy0,dy1). Empty when dx1<=dx0.
static int dx0, dy0, dx1, dy1;
static void dirty_reset(void){ dx0 = (int)fb_width; dy0 = (int)fb_height; dx1 = 0; dy1 = 0; }
static inline void dirty_rect(int x, int y, int w, int h){
    if(x<dx0)dx0=x; if(y<dy0)dy0=y; if(x+w>dx1)dx1=x+w; if(y+h>dy1)dy1=y+h;
}
static inline void dirty_all(void){ dx0=0; dy0=0; dx1=(int)fb_width; dy1=(int)fb_height; }
static inline uint8_t* fb_target(void){ return (dbuf_enabled && dbuf) ? dbuf : (uint8_t*)(uint64_t)fb_phys_addr; }

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

/* ---- glyphs -------------------------------------------------------------- */
// [M40] Accented letters = base letter + accent rows; three symbols drawn here.
static const uint8_t glyph_section[11] = {0x3C,0x66,0x06,0x1C,0x36,0x66,0x6C,0x38,0x60,0x66,0x3C};
static const uint8_t glyph_pound[9] = {0x38,0x6C,0x0C,0x0C,0x3F,0x0C,0x0C,0x4E,0x3F};
static const uint8_t glyph_euro[9] = {0x78,0xCC,0x06,0x1F,0x06,0x1F,0x06,0xCC,0x78};
static int accent_glyph(uint8_t g, uint8_t out[16]){
    static const struct { uint8_t g; char base; uint8_t kind; } tab[] = {
        { VT_G_A_GRAVE, 'a', 1 }, { VT_G_E_GRAVE, 'e', 1 }, { VT_G_E_ACUTE, 'e', 2 },
        { VT_G_I_GRAVE, 'i', 1 }, { VT_G_O_GRAVE, 'o', 1 }, { VT_G_U_GRAVE, 'u', 1 },
        { VT_G_CA_GRAVE, 'A', 1 }, { VT_G_CE_GRAVE, 'E', 1 }, { VT_G_CE_ACUTE, 'E', 2 },
        { VT_G_CI_GRAVE, 'I', 1 }, { VT_G_CO_GRAVE, 'O', 1 }, { VT_G_CU_GRAVE, 'U', 1 },
        { VT_G_C_CEDIL, 'c', 3 },
    };
    const uint8_t* sym = 0; int nsym = 0;
    if(g == VT_G_SECTION){ sym = glyph_section; nsym = (int)sizeof(glyph_section); }
    if(g == VT_G_POUND)  { sym = glyph_pound;   nsym = (int)sizeof(glyph_pound); }
    if(g == VT_G_EURO)   { sym = glyph_euro;    nsym = (int)sizeof(glyph_euro); }
    if(sym){ for(int r=0;r<nsym && r<16;r++) out[r]=sym[r]; return 1; }
    for(unsigned i=0;i<sizeof(tab)/sizeof(tab[0]);i++){
        if(tab[i].g != g) continue;
        const uint8_t* b = font8x16[tab[i].base - 32];
        int upper = (tab[i].base >= 'A' && tab[i].base <= 'Z');
        int shift = upper ? 3 : 0;               // capitals move down to make room
        for(int r=0;r+shift<16;r++) out[r+shift] = b[r];
        if(tab[i].base == 'i'){ out[0] = 0; out[1] = 0; }   // drop the dot
        if(tab[i].kind == 1){ out[0] |= 0x06; out[1] |= 0x0C; }        // grave
        else if(tab[i].kind == 2){ out[0] |= 0x30; out[1] |= 0x18; }   // acute
        else { out[8] |= 0x10; out[9] |= 0x18; }                       // cedilla
        return 1;
    }
    return 0;
}

// Fill out[16] with the bitmap of glyph code g (bit n = pixel column n, the
// font's LSB-left encoding). Covers ASCII, the CP437 box-drawing subset and the
// extra vt glyphs (ellipsis, middle dot, degree, block, shade, check, unknown).
static void glyph_rows(uint8_t g, uint8_t out[16]){
    for(int i=0;i<16;i++) out[i]=0;
    if(g >= 0x20 && g <= 0x7E){ for(int i=0;i<16;i++) out[i]=font8x16[g-32][i]; return; }
    if(g >= 0x90 && g <= 0x9F && accent_glyph(g, out)) return;
    // Box drawing: arms Left/Right/Up/Down meeting in the cell centre.
    enum { L=1, R=2, U=4, D=8 };
    int arms = 0;
    switch(g){
    case 0xC4: arms=L|R; break;       case 0xB3: arms=U|D; break;
    case 0xDA: arms=R|D; break;       case 0xBF: arms=L|D; break;
    case 0xC0: arms=R|U; break;       case 0xD9: arms=L|U; break;
    case 0xC3: arms=U|D|R; break;     case 0xB4: arms=U|D|L; break;
    case 0xC2: arms=L|R|D; break;     case 0xC1: arms=L|R|U; break;
    case 0xC5: arms=L|R|U|D; break;
    default: break;
    }
    if(arms){
        uint8_t h = (uint8_t)(((arms&L)?0x1F:0) | ((arms&R)?0xF8:0));
        out[7] |= h; out[8] |= h;
        for(int r=0;r<16;r++){
            if(((arms&U) && r<=8) || ((arms&D) && r>=7)) out[r] |= 0x18;
        }
        return;
    }
    switch(g){
    case VT_G_ELLIPSIS: out[11]=out[12]=0x92; return;
    case VT_G_MIDDOT:   out[7]=out[8]=0x18; return;
    case VT_G_DEGREE:   out[1]=0x1C; out[2]=0x22; out[3]=0x22; out[4]=0x1C; return;
    case VT_G_BLOCK:    for(int i=0;i<16;i++) out[i]=0xFF; return;
    case VT_G_SHADE:    for(int i=0;i<16;i++) out[i]=(i&1)?0xAA:0x55; return;
    case VT_G_CHECK:
        out[3]=0x80; out[4]=0xC0; out[5]=0x60; out[6]=0x30; out[7]=0x19; out[8]=0x0F; out[9]=0x06; return;
    default: break;
    }
    // Unknown code point: one hollow box per character.
    out[2]=0x7E; for(int r=3;r<12;r++) out[r]=0x42; out[12]=0x7E;
}

// Render one cell. Rows are written whole into the target buffer; the cell is
// clipped against the framebuffer (vt coordinates are already in range, the
// check guards a geometry change underneath us).
static void fb_draw_cell_px(int x, int y, const vt_cell_t* c, int with_cursor){
    if(!fb_enabled || fb_gfx || x < 0 || y < 0) return;
    uint32_t gx = (uint32_t)x * glyph_w, gy = (uint32_t)y * glyph_h;
    if(gx + glyph_w > fb_width || gy + glyph_h > fb_height) return;
    uint8_t rows[16]; glyph_rows(c->ch, rows);
    if(c->attr & VT_ATTR_UL) rows[15] = 0xFF;
    uint32_t fg = vga_palette[c->fg & 0xF], bg = vga_palette[c->bg & 0xF];
    uint8_t* target = fb_target();
    for(uint32_t r=0; r<glyph_h; r++){
        uint32_t* row = (uint32_t*)(target + (size_t)(gy + r) * fb_pitch) + gx;
        uint8_t line = rows[r];
        int cur = with_cursor && r >= glyph_h - 2;   // 2-pixel underline cursor
        for(uint32_t col=0; col<glyph_w; col++)
            row[col] = (cur || ((line >> col) & 1)) ? fg : bg;
    }
    if(dbuf_enabled && dbuf) dirty_rect((int)gx, (int)gy, (int)glyph_w, (int)glyph_h);
}

/* ---- vt backend ---------------------------------------------------------- */
static void fbvt_draw_cell(int x, int y, const vt_cell_t* c){ fb_draw_cell_px(x, y, c, 0); }

static void fb_console_clear_logo_area(void);
void fb_console_draw_logo(void);
// Fast scroll: move whole pixel rows of the text band [top, bot] in the target
// buffer (RAM back buffer when enabled, never read from VRAM otherwise).
static int fbvt_scroll(int top, int bot, int n){
    if(!fb_enabled) return -1;
    if(fb_gfx) return 0;                       // graphics mode: the grid alone moves
    int cols, rows; vt_get_size(&cols, &rows);
    (void)cols;
    if(top < 0 || bot >= rows || top >= bot) return -1;
    if((size_t)(bot + 1) * glyph_h > fb_height) return -1;
    int full = (top == 0) && (bot == rows - 1) && !vt_app_owns_screen();
    int k = n > 0 ? n : -n;
    if(k <= 0 || k > bot - top) return -1;
    uint8_t* target = fb_target();
    size_t line_bytes = (size_t)fb_pitch * glyph_h;
    size_t span = (size_t)(bot - top + 1 - k) * line_bytes;   // bytes that move
    uint8_t* region = target + (size_t)top * line_bytes;
    if(full) fb_console_clear_logo_area();     // don't drag the logo along
    if(n > 0){
        uint8_t* dst = region; uint8_t* src = region + (size_t)k * line_bytes;
        size_t i=0; for(; i+8<=span; i+=8) *(uint64_t*)(dst+i) = *(uint64_t*)(src+i);
        for(; i<span; i++) dst[i] = src[i];
    } else {
        uint8_t* dst = region + (size_t)k * line_bytes; uint8_t* src = region;
        size_t i = span;
        while(i >= 8){ i -= 8; *(uint64_t*)(dst+i) = *(uint64_t*)(src+i); }
        while(i > 0){ i--; dst[i] = src[i]; }
    }
    if(dbuf_enabled && dbuf) dirty_rect(0, top * (int)glyph_h, (int)fb_width, (bot - top + 1) * (int)glyph_h);
    if(full) fb_console_draw_logo();
    return 0;
}

static void fbvt_cursor(int x, int y, int on){
    const vt_cell_t* c = vt_cell_at(x, y);
    if(!c) return;
    if(on && !cursor_blink_enabled) on = 0;
    fb_draw_cell_px(x, y, c, on);
    if(on){ cursor_blink_phase = 0; blink_counter = 0; }
}

static void fbvt_flush(void){ fb_console_flush_if_auto(); }

static int fbvt_clear_all(uint8_t bg){
    if(!fb_enabled) return -1;
    if(fb_gfx) return 0;
    uint32_t rgb = vga_palette[bg & 0xF];
    uint8_t* target = fb_target();
    for(uint32_t y=0; y<fb_height; y++){
        uint32_t* row = (uint32_t*)(target + (size_t)y * fb_pitch);
        for(uint32_t x=0; x<fb_width; x++) row[x] = rgb;
    }
    if(dbuf_enabled && dbuf) dirty_all();
    return 0;
}

static const vt_ops_t fb_vt_ops = {
    fbvt_draw_cell, fbvt_scroll, fbvt_cursor, fbvt_flush, fbvt_clear_all
};

int fb_console_init(void){
    framebuffer_info_t info; if(!fb_get_info(&info)) return -1; if (info.bpp != 32) return -1;
    // If virt_addr is unset, reach the framebuffer through the physmap (fb_init
    // runs in phase 2, after the physmap; [M44] there is no identity fallback).
    if (info.virt_addr == 0) {
    // Attempt phys_to_virt lookup directly; assume vmm_extend_physmap was called earlier.
        extern uint64_t phys_to_virt(uint64_t phys);
        uint64_t tentative = phys_to_virt(info.addr);
    // Cannot easily verify mapping here without page-walk; assume valid post vmm_init_physmap.
        info.virt_addr = tentative;
    }
    fb_width = info.width; fb_height = info.height; fb_pitch = info.pitch; fb_phys_addr = (info.virt_addr ? info.virt_addr : info.addr);
    fb_enabled = 1; fb_gfx = 0; fb_clear(0x000000);   // a (re)init is always text mode (panic path)
    // Drop the double buffer (a re-init may change geometry). Free it first — the
    // caller (boot / clear) re-enables it; otherwise the console falls back to slow
    // direct-VRAM rendering (scroll reads VRAM). Previously this leaked dbuf.
    if(dbuf){ extern void kfree(void* p); kfree(dbuf); }
    dbuf=NULL; dbuf_enabled=0;
    // The cell grid + escape-sequence interpreter (drivers/vt.c).
    vt_attach(&fb_vt_ops, (int)(fb_width / glyph_w), (int)(fb_height / glyph_h));
    // Draw SecOS logo on top-right
    fb_console_draw_logo();
    // Set a more legible default color (white on black)
    extern void terminal_setcolor(uint8_t color);
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    return 0;
}

// Simple stylized SecOS logo (5 letters) rendered top-right
static void draw_block(int x,int y,int w,int h,uint32_t rgb){
    if(fb_gfx) return;
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
    if(!fb_enabled || fb_gfx) return;
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
    if(!fb_enabled || fb_gfx || !logo_glow_enabled) return;
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
static void fb_console_tick_locked(void);
static void fb_console_tick(void){
    if(!cursor_blink_enabled || !fb_enabled || fb_gfx) return;
    if(vt_busy()) return;   // output in progress: the cursor position may be transient
    uint64_t lf;            // [M48] another CPU may be writing: skip this blink
    if(!con_trylock(&lf)) return;
    fb_console_tick_locked();
    con_unlock(lf);
}
static void fb_console_tick_locked(void){
    blink_counter++;
    // Update glow slowly every 5 ticks independent of cursor blink (not while a
    // full-screen program owns the screen: it would paint over its text).
    if(logo_glow_enabled && !vt_app_owns_screen() && (blink_counter % 5)==0){ logo_glow_phase++; fb_console_draw_logo_glow(); }
    if (blink_counter >= blink_interval_ticks){
        blink_counter = 0;
        int x, y; int vis = vt_get_cursor(&x, &y);
        const vt_cell_t* c = vt_cell_at(x, y);
        if(!c) return;
        if(cursor_blink_phase==0 || !vis){ fb_draw_cell_px(x, y, c, 0); cursor_blink_phase=1; }
        else { fb_draw_cell_px(x, y, c, 1); cursor_blink_phase=0; }
        if(dbuf_enabled) fb_console_flush();
    }
}

// Enable cursor blinking (called after timer_init)
int fb_console_enable_cursor_blink(uint32_t timer_freq){
    if(!fb_enabled) return -1;
    if (timer_freq==0) return -1;
    blink_interval_ticks = (timer_freq / 2); // ~500ms
    if (blink_interval_ticks==0) blink_interval_ticks=1;
    if (timer_register_tick_callback(fb_console_tick)!=0) return -1;
    cursor_blink_enabled = 1; blink_counter=0; cursor_blink_phase=0;
    { int x, y; if(vt_get_cursor(&x, &y)) fbvt_cursor(x, y, 1); }
    fb_console_flush_if_auto();
    return 0;
}

void fb_console_disable_cursor_blink(void){
    if(!cursor_blink_enabled) return;
    cursor_blink_enabled=0;
    int x, y; vt_get_cursor(&x, &y); fbvt_cursor(x, y, 0);
    fb_console_flush_if_auto();
}

void fb_console_set_color(uint8_t fg, uint8_t bg){ vt_set_colors(fg, bg); }
static void fb_console_flush_if_auto(void){ if(dbuf_enabled && dbuf_auto_flush) fb_console_flush(); }

void fb_console_putc(char c){
    if(!fb_enabled){ return; }
    vt_putc(c);
}
void fb_console_write(const char* s){ size_t n=0; while(s[n]) n++; if(fb_enabled) vt_write(s, n); }
void fb_console_write_n(const char* s, size_t n){ if(fb_enabled) vt_write(s, n); }

// Double buffer API
int fb_console_enable_dbuf(void){
    if(!fb_enabled) return -1;
    if(dbuf_enabled) return 0;
    // Simple allocation via kmalloc (requires working heap)
    extern void* kmalloc(size_t sz); extern void kfree(void* p);
    size_t sz = fb_pitch * fb_height; dbuf = (uint8_t*)kmalloc(sz);
    if(!dbuf) return -1;
    // Copy existing content (one-time; 64-bit words to limit the slow VRAM read).
    uint8_t* base=(uint8_t*)(uint64_t)fb_phys_addr;
    { size_t i=0; for(; i+8<=sz; i+=8) *(uint64_t*)(dbuf+i)=*(volatile uint64_t*)(base+i);
      for(; i<sz; i++) dbuf[i]=base[i]; }
    dbuf_enabled=1;
    dirty_reset();
    return 0;
}
// Panic path: stop using the RAM back buffer WITHOUT touching the heap (no flush,
// no kfree). The heap may be the very thing that is corrupted, and the panic
// screen is painted straight to VRAM anyway; leaking the buffer is irrelevant.
void fb_console_panic_detach_dbuf(void){
    dbuf = NULL; dbuf_enabled = 0;
    fb_gfx = 0;     // [M40] the panic screen must show even over a graphics client
}
void fb_console_disable_dbuf(void){
    if(!dbuf_enabled) return;
    extern void kfree(void* p);
    // Final flush before freeing
    fb_console_flush();
    kfree(dbuf); dbuf=NULL; dbuf_enabled=0;
}
void fb_console_flush(void){
    if(!dbuf_enabled || !dbuf || fb_gfx) return;
    if(dx1<=dx0 || dy1<=dy0) return;            // nothing dirty
    uint8_t* base=(uint8_t*)(uint64_t)fb_phys_addr;
    int x0=dx0<0?0:dx0, y0=dy0<0?0:dy0;
    int x1=dx1>(int)fb_width?(int)fb_width:dx1, y1=dy1>(int)fb_height?(int)fb_height:dy1;
    // Copy only the dirty pixel rectangle, row by row, with 64-bit words (then a
    // 32-bit tail). VRAM is written sequentially and never read.
    size_t byte0 = (size_t)x0 * 4, nbytes = (size_t)(x1 - x0) * 4;
    for(int y=y0; y<y1; y++){
        uint8_t* d = dbuf + (size_t)y*fb_pitch + byte0;
        uint8_t* s = base + (size_t)y*fb_pitch + byte0;
        size_t i=0;
        for(; i+8<=nbytes; i+=8) *(volatile uint64_t*)(s+i) = *(uint64_t*)(d+i);
        for(; i+4<=nbytes; i+=4) *(volatile uint32_t*)(s+i) = *(uint32_t*)(d+i);
    }
    dirty_reset();
}
// [M40] Console handoff. on=1: stop drawing (a graphics client owns the screen).
// on=0: repaint everything from the vt grid (background, cells, logo, cursor)
// and present it, so the text console comes back exactly as it was.
void fb_console_redraw(void){
    if(!fb_enabled || fb_gfx) return;
    uint8_t fg, bg; vt_get_colors(&fg, &bg); (void)fg;
    fbvt_clear_all(bg);
    int cols, rows; vt_get_size(&cols, &rows);
    for(int y=0; y<rows; y++)
        for(int x=0; x<cols; x++){ const vt_cell_t* c = vt_cell_at(x, y); if(c) fb_draw_cell_px(x, y, c, 0); }
    if(!vt_app_owns_screen()) fb_console_draw_logo();
    { int x, y; if(vt_get_cursor(&x, &y)) fbvt_cursor(x, y, 1); }
    if(dbuf_enabled && dbuf){ dirty_all(); fb_console_flush(); }
}
void fb_console_set_graphics(int on){
    if(on){ fb_gfx = 1; return; }
    if(!fb_gfx) return;
    fb_gfx = 0;
    fb_console_redraw();
}
int fb_console_in_graphics(void){ return fb_gfx; }
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
