/*
 * SecOS Kernel - VT100/xterm-style terminal emulator (console output side)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * A backend-independent escape-sequence interpreter over a cell grid. The
 * framebuffer console and the VGA text-mode fallback register a small set of
 * drawing callbacks; this layer owns the cursor, attributes, scroll region and
 * the grid itself, so every coordinate handed to a backend is already clamped
 * to [0, cols) x [0, rows).
 */
#ifndef VT_H
#define VT_H
#include <stdint.h>
#include <stddef.h>

/* Largest grid kept in .bss (1920x1080 with an 8x16 font is 240x67). A bigger
 * screen just uses the top-left VT_MAX_COLS x VT_MAX_ROWS cells. */
#define VT_MAX_COLS 256
#define VT_MAX_ROWS 128

/* Glyph codes stored in a cell. 0x20..0x7E are ASCII; the CP437 box-drawing
 * codes (0xB3..0xDA subset) are kept as-is; a few extra glyphs live at 0x80+. */
#define VT_G_ELLIPSIS  0x80
#define VT_G_MIDDOT    0x81
#define VT_G_DEGREE    0x82
#define VT_G_UNKNOWN   0x83   /* one per unmapped code point (a hollow box) */
#define VT_G_BLOCK     0x84
#define VT_G_SHADE     0x85
#define VT_G_CHECK     0x86

#define VT_ATTR_UL     0x01   /* underline */

typedef struct vt_cell {
    uint8_t ch;     /* glyph code (see above) */
    uint8_t fg;     /* VGA palette index 0..15 (reverse/bold already applied) */
    uint8_t bg;     /* VGA palette index 0..15 */
    uint8_t attr;   /* VT_ATTR_* */
} vt_cell_t;

typedef struct vt_ops {
    /* Draw one cell at (x, y); always in range. */
    void (*draw_cell)(int x, int y, const vt_cell_t* c);
    /* Optional fast path: move rows [top, bot] (inclusive) up by n (n > 0) or
     * down by -n (n < 0). The vt redraws the exposed rows itself afterwards.
     * Return 0 if done, -1 to make the vt redraw the whole region. */
    int  (*scroll)(int top, int bot, int n);
    /* Draw (on=1) / remove (on=0) the cursor overlay on cell (x, y). Removing
     * normally just redraws the cell. Optional. */
    void (*cursor)(int x, int y, int on);
    /* Called after a batch of output (optional; e.g. present the back buffer). */
    void (*flush)(void);
    /* Whole-screen clear hint (optional): lets the fb backend repaint the
     * background in one go and drop its logo. Return 0 if handled. */
    int  (*clear_all)(uint8_t bg);
} vt_ops_t;

/* Attach a backend with a grid of cols x rows (clamped to the maxima). Resets
 * all state and clears the grid. */
void vt_attach(const vt_ops_t* ops, int cols, int rows);
int  vt_active(void);

/* Feed output bytes (UTF-8 + escape sequences). */
void vt_putc(char c);
void vt_write(const char* s, size_t n);

/* Default colors (kernel `color` command / terminal_setcolor). */
void vt_set_colors(uint8_t fg, uint8_t bg);
void vt_get_colors(uint8_t* fg, uint8_t* bg);

/* Geometry and cursor state, for TIOCGWINSZ and the fb cursor blink. */
void vt_get_size(int* cols, int* rows);
int  vt_get_cursor(int* x, int* y);           /* returns 1 if the cursor is visible */
const vt_cell_t* vt_cell_at(int x, int y);    /* NULL if out of range */
int  vt_busy(void);                           /* output in progress (skip blink) */
/* A full-screen program has positioned the cursor / cleared the screen: the
 * fb backend stops redrawing its decorative logo over the text. */
int  vt_app_owns_screen(void);
void vt_release_screen(void);

/* Replies to DSR/DA queries are pushed into the TTY input queue through this
 * hook (set by kernel/tty.c). */
void vt_set_reply_hook(void (*hook)(const char* s, int n));

/* Map a Unicode code point to a glyph code (0 = zero-width, skip). Exposed for
 * the self-test. */
uint8_t vt_glyph_for(uint32_t cp);

#endif
