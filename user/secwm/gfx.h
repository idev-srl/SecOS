/* SecOS GUI - small software drawing library (compositor + clients).
 * SPDX-License-Identifier: MIT
 *
 * Everything draws into a surf_t: a 32-bit XRGB8888 pixel array with a stride
 * in pixels. All primitives clip against the surface, so callers can pass any
 * coordinates. Text uses the embedded 8x16 bitmap font (font16.h, derived
 * from Terminus Font, SIL OFL 1.1).
 */
#ifndef SECOS_GFX_H
#define SECOS_GFX_H
#include <stdint.h>

typedef struct surf { uint32_t* px; int w, h, stride; } surf_t;
typedef struct rect { int x, y, w, h; } rect_t;

#define GFX_FONT_W 8
#define GFX_FONT_H 16

static inline uint32_t* gfx_at(const surf_t* s, int x, int y) { return s->px + (long)y * s->stride + x; }
static inline uint32_t gfx_rgb(int r, int g, int b) { return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; }

/* Rectangles. */
int    rect_empty(rect_t r);
rect_t rect_isect(rect_t a, rect_t b);
rect_t rect_union(rect_t a, rect_t b);
int    rect_contains(rect_t r, int x, int y);

/* Blend b over a with alpha 0..255. */
uint32_t gfx_mix(uint32_t a, uint32_t b, int alpha);

void gfx_fill(surf_t* s, int x, int y, int w, int h, uint32_t c);
void gfx_frame(surf_t* s, int x, int y, int w, int h, uint32_t c);           /* 1px outline */
void gfx_vgrad(surf_t* s, int x, int y, int w, int h, uint32_t top, uint32_t bot);
/* Filled rounded rectangle with radius r (anti-aliased corners over what is there). */
void gfx_rrect(surf_t* s, int x, int y, int w, int h, int r, uint32_t c);
/* Darken a rectangle (shadows): new = old * (256 - amount) / 256. */
void gfx_darken(surf_t* s, int x, int y, int w, int h, int amount);
void gfx_line(surf_t* s, int x0, int y0, int x1, int y1, uint32_t c);
void gfx_disc(surf_t* s, int cx, int cy, int r, uint32_t c);
/* Copy a w x h block from src (stride in pixels) to (x, y), clipped. */
void gfx_blit(surf_t* s, int x, int y, const uint32_t* src, int sstride, int w, int h);

/* Glyph index for a code point (-1 if the font has none). */
int  gfx_glyph_index(uint32_t cp);
/* Draw one glyph cell at (x, y). bg = GFX_TRANSPARENT leaves the background. */
#define GFX_TRANSPARENT 0xFF000000u
void gfx_glyph(surf_t* s, int x, int y, uint32_t cp, uint32_t fg, uint32_t bg, int bold);
/* UTF-8 text, transparent background; returns the advance in pixels. Stops
 * at maxw pixels (0 = no limit). */
int  gfx_text(surf_t* s, int x, int y, const char* utf8, uint32_t fg, int bold, int maxw);
int  gfx_text_width(const char* utf8);
/* Decode one UTF-8 sequence; returns bytes consumed (>= 1), *cp = U+FFFD on error. */
int  gfx_utf8(const char* p, uint32_t* cp);

#endif
