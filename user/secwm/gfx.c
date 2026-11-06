/* SecOS GUI - small software drawing library (compositor + clients).
 * SPDX-License-Identifier: MIT
 */
#include "gfx.h"
#include "font16.h"

int rect_empty(rect_t r) { return r.w <= 0 || r.h <= 0; }

rect_t rect_isect(rect_t a, rect_t b) {
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    rect_t r = { x0, y0, x1 - x0, y1 - y0 };
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

rect_t rect_union(rect_t a, rect_t b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    rect_t r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

int rect_contains(rect_t r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

uint32_t gfx_mix(uint32_t a, uint32_t b, int al) {
    if (al <= 0) return a;
    if (al >= 255) return b;
    uint32_t ia = (uint32_t)(255 - al), ua = (uint32_t)al;
    uint32_t r = (((a >> 16) & 0xFF) * ia + ((b >> 16) & 0xFF) * ua) / 255;
    uint32_t g = (((a >> 8) & 0xFF) * ia + ((b >> 8) & 0xFF) * ua) / 255;
    uint32_t bl = ((a & 0xFF) * ia + (b & 0xFF) * ua) / 255;
    return (r << 16) | (g << 8) | bl;
}

static int clip(const surf_t* s, int* x, int* y, int* w, int* h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > s->w) *w = s->w - *x;
    if (*y + *h > s->h) *h = s->h - *y;
    return *w > 0 && *h > 0;
}

void gfx_fill(surf_t* s, int x, int y, int w, int h, uint32_t c) {
    if (!clip(s, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t* p = gfx_at(s, x, y + j);
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

void gfx_frame(surf_t* s, int x, int y, int w, int h, uint32_t c) {
    gfx_fill(s, x, y, w, 1, c);
    gfx_fill(s, x, y + h - 1, w, 1, c);
    gfx_fill(s, x, y, 1, h, c);
    gfx_fill(s, x + w - 1, y, 1, h, c);
}

void gfx_vgrad(surf_t* s, int x, int y, int w, int h, uint32_t top, uint32_t bot) {
    int h0 = h, y0 = y;
    if (h0 <= 0) return;
    int cx = x, cy = y, cw = w, ch = h;
    if (!clip(s, &cx, &cy, &cw, &ch)) return;
    for (int j = 0; j < ch; j++) {
        int t = (h0 > 1) ? ((cy + j - y0) * 255) / (h0 - 1) : 0;
        uint32_t c = gfx_mix(top, bot, t);
        uint32_t* p = gfx_at(s, cx, cy + j);
        for (int i = 0; i < cw; i++) p[i] = c;
    }
}

void gfx_rrect(surf_t* s, int x, int y, int w, int h, int r, uint32_t c) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= s->h) continue;
        int dy = 0;
        if (j < r) dy = r - j;
        else if (j >= h - r) dy = j - (h - r - 1);
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= s->w) continue;
            int dx = 0;
            if (i < r) dx = r - i;
            else if (i >= w - r) dx = i - (w - r - 1);
            uint32_t* p = gfx_at(s, xx, yy);
            if (dx && dy) {
                /* Coverage of the corner pixel: distance from the arc centre
                 * in 1/16 px, a 1 px ramp. */
                int fx = dx * 16 - 8, fy = dy * 16 - 8, rr = r * 16;
                int d2 = fx * fx + fy * fy;
                if (d2 >= (rr + 8) * (rr + 8)) continue;
                if (d2 > (rr - 8) * (rr - 8)) {
                    int lo = (rr - 8) * (rr - 8), hi = (rr + 8) * (rr + 8);
                    int a = 255 - (int)((long)(d2 - lo) * 255 / (hi - lo));
                    *p = gfx_mix(*p, c, a);
                    continue;
                }
            }
            *p = c;
        }
    }
}

void gfx_darken(surf_t* s, int x, int y, int w, int h, int amount) {
    if (!clip(s, &x, &y, &w, &h)) return;
    uint32_t k = (uint32_t)(256 - amount);
    for (int j = 0; j < h; j++) {
        uint32_t* p = gfx_at(s, x, y + j);
        for (int i = 0; i < w; i++) {
            uint32_t v = p[i];
            uint32_t rb = (((v & 0xFF00FF) * k) >> 8) & 0xFF00FF;
            uint32_t g = (((v & 0x00FF00) * k) >> 8) & 0x00FF00;
            p[i] = rb | g;
        }
    }
}

void gfx_line(surf_t* s, int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int n = 0; n < 4096; n++) {
        if (x0 >= 0 && y0 >= 0 && x0 < s->w && y0 < s->h) *gfx_at(s, x0, y0) = c;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_disc(surf_t* s, int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= s->w || y >= s->h) continue;
            int d2 = dx * dx + dy * dy;
            if (d2 <= r * r) *gfx_at(s, x, y) = c;
            else if (d2 <= (r + 1) * (r + 1)) *gfx_at(s, x, y) = gfx_mix(*gfx_at(s, x, y), c, 110);
        }
}

void gfx_blit(surf_t* s, int x, int y, const uint32_t* src, int sstride, int w, int h) {
    int sx = 0, sy = 0;
    if (x < 0) { sx = -x; w += x; x = 0; }
    if (y < 0) { sy = -y; h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        const uint32_t* a = src + (long)(sy + j) * sstride + sx;
        uint32_t* b = gfx_at(s, x, y + j);
        for (int i = 0; i < w; i++) b[i] = a[i];
    }
}

int gfx_glyph_index(uint32_t cp) {
    int lo = 0, hi = FONT16_N - 1;
    if (cp > 0xFFFF) return -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font16_cp[mid] == cp) return mid;
        if (font16_cp[mid] < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

void gfx_glyph(surf_t* s, int x, int y, uint32_t cp, uint32_t fg, uint32_t bg, int bold) {
    int gi = gfx_glyph_index(cp);
    if (gi < 0 && cp >= 0x20) gi = gfx_glyph_index(0xFFFD);
    const uint8_t* rows = gi >= 0 ? (bold ? font16_bold[gi] : font16_reg[gi]) : 0;
    for (int r = 0; r < 16; r++) {
        int yy = y + r;
        if (yy < 0 || yy >= s->h) continue;
        uint8_t line = rows ? rows[r] : 0;
        uint32_t* p = gfx_at(s, 0, yy);
        for (int c = 0; c < 8; c++) {
            int xx = x + c;
            if (xx < 0 || xx >= s->w) continue;
            if (line & (0x80 >> c)) p[xx] = fg;
            else if (bg != GFX_TRANSPARENT) p[xx] = bg;
        }
    }
}

int gfx_utf8(const char* p, uint32_t* cp) {
    const unsigned char* u = (const unsigned char*)p;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    int n = (u[0] >= 0xF0) ? 3 : (u[0] >= 0xE0) ? 2 : (u[0] >= 0xC0) ? 1 : -1;
    if (n < 0) { *cp = 0xFFFD; return 1; }
    uint32_t v = u[0] & (0x3F >> n);
    for (int i = 1; i <= n; i++) {
        if ((u[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return i; }
        v = (v << 6) | (u[i] & 0x3F);
    }
    *cp = v;
    return n + 1;
}

int gfx_text(surf_t* s, int x, int y, const char* t, uint32_t fg, int bold, int maxw) {
    int x0 = x;
    while (*t) {
        if (maxw && x + 8 - x0 > maxw) break;
        uint32_t cp; t += gfx_utf8(t, &cp);
        gfx_glyph(s, x, y, cp, fg, GFX_TRANSPARENT, bold);
        x += 8;
    }
    return x - x0;
}

int gfx_text_width(const char* t) {
    int n = 0;
    while (*t) { uint32_t cp; t += gfx_utf8(t, &cp); n++; }
    return n * 8;
}
