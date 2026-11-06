/*
 * SecOS Kernel - VT100/xterm-style terminal emulator (console output side)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Interprets the escape sequences full-screen programs and readline emit and
 * renders into a cell grid through a backend (framebuffer or VGA text). The
 * subset follows the Linux console / xterm:
 *
 *   C0     BEL BS HT LF VT FF CR, ESC
 *   ESC    7 8 (save/restore cursor), D (IND), E (NEL), M (RI), c (RIS),
 *          ( ) * + <x> (charset designation, ignored), = > (keypad, ignored)
 *   CSI    A B C D E F G ` a d e H f (cursor), J K (erase), L M (lines),
 *          @ P X (chars), S T (scroll), m (SGR), r (scroll region), s u,
 *          n (DSR 5/6), c (DA), h l (?25 cursor, ?7 autowrap, ?1049/?1047/?47
 *          alternate screen, 4 insert mode)
 *   SGR    0 1 2 4 5 7 8 22 24 25 27 28 30-37 38;5;n 38;2;r;g;b 39 40-47
 *          48;5;n 48;2;r;g;b 49 90-97 100-107
 *   OSC/DCS/APC/PM strings are consumed and ignored.
 *
 * Output is UTF-8: each code point becomes ONE cell. Code points without a
 * glyph in the 8x16 font are folded to ASCII when there is an obvious
 * equivalent (dashes, quotes, accented Latin-1 letters), mapped to the CP437
 * box-drawing glyphs, or drawn as one replacement box — never one '?' per byte.
 * A byte that is not valid UTF-8 is shown as its CP437 glyph when it has one
 * (legacy kernel prints of raw box-drawing bytes).
 *
 * Every coordinate is clamped before it reaches a backend; the pending-wrap
 * flag (cursor stays on the last column after printing there) replaces the old
 * "cursor_x == cols" state that once let the cursor draw past the back buffer.
 */
#include "vt.h"

static const vt_ops_t* g_ops;
static int g_cols, g_rows;
static vt_cell_t g_grid[VT_MAX_ROWS][VT_MAX_COLS];
static vt_cell_t g_alt[VT_MAX_ROWS][VT_MAX_COLS];   /* saved main screen */
static int g_alt_active;

static int cx, cy;              /* cursor, always in range */
static int wrap_pending;        /* printed on the last column; wrap on next glyph */
static int sc_top, sc_bot;      /* scroll region (inclusive rows) */
static int autowrap = 1, insert_mode = 0, cursor_on = 1;
static int app_screen;          /* a program addressed the screen directly */

static uint8_t def_fg = 15, def_bg = 0;   /* kernel defaults (terminal_setcolor) */
static int sgr_fg = -1, sgr_bg = -1;       /* -1 = default color */
static int sgr_bold, sgr_rev, sgr_ul, sgr_conceal;

/* Saved cursor (ESC 7 / CSI s). */
static int sv_x, sv_y, sv_fg = -1, sv_bg = -1, sv_bold, sv_rev, sv_ul, sv_wrap;

static volatile int g_busy;
static void (*g_reply)(const char* s, int n);

/* Parser. */
enum { ST_GROUND, ST_ESC, ST_ESC_SKIP1, ST_CSI, ST_STR, ST_STR_ESC };
static int st = ST_GROUND;
#define VT_NPAR 16
static int par[VT_NPAR], npar, par_has;   /* par_has: a digit was seen in the current param */
static char csi_priv;                     /* '?', '>', '=', '<' or 0 */
static char csi_inter;                    /* intermediate byte (0x20..0x2F) or 0 */
/* UTF-8 decoder. */
static int u_need;
static uint32_t u_cp;
static uint8_t u_lead;

/* ANSI color index (0..7: black red green yellow blue magenta cyan white) ->
 * VGA palette index. */
static const uint8_t ansi2vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

int vt_active(void) { return g_ops != 0; }
int vt_busy(void) { return g_busy; }
int vt_app_owns_screen(void) { return app_screen; }
void vt_release_screen(void) { app_screen = 0; }
void vt_set_reply_hook(void (*hook)(const char* s, int n)) { g_reply = hook; }
void vt_get_size(int* cols, int* rows) {
    if (cols) *cols = g_cols ? g_cols : 80;
    if (rows) *rows = g_rows ? g_rows : 25;
}
int vt_get_cursor(int* x, int* y) {
    if (x) *x = cx;
    if (y) *y = cy;
    return cursor_on;
}
const vt_cell_t* vt_cell_at(int x, int y) {
    if (x < 0 || y < 0 || x >= g_cols || y >= g_rows) return 0;
    return &g_grid[y][x];
}
void vt_set_colors(uint8_t fg, uint8_t bg) { def_fg = fg & 0xF; def_bg = bg & 0xF; }
void vt_get_colors(uint8_t* fg, uint8_t* bg) { if (fg) *fg = def_fg; if (bg) *bg = def_bg; }

/* ---- attributes ---------------------------------------------------------- */
static uint8_t eff_bg_plain(void) { return (uint8_t)(sgr_bg < 0 ? def_bg : sgr_bg); }
static vt_cell_t make_cell(uint8_t ch) {
    vt_cell_t c;
    uint8_t f = (uint8_t)(sgr_fg < 0 ? def_fg : sgr_fg);
    uint8_t b = eff_bg_plain();
    if (sgr_bold && f < 8) f = (uint8_t)(f + 8);
    if (sgr_rev) { uint8_t t = f; f = b; b = t; }
    if (sgr_conceal) f = b;
    c.ch = ch; c.fg = f; c.bg = b; c.attr = sgr_ul ? VT_ATTR_UL : 0;
    return c;
}
/* Erased cells take the current background (Linux console "bce"). */
static vt_cell_t blank_cell(void) {
    vt_cell_t c;
    c.ch = ' '; c.fg = (uint8_t)(sgr_fg < 0 ? def_fg : sgr_fg); c.bg = eff_bg_plain(); c.attr = 0;
    return c;
}

/* ---- drawing helpers ----------------------------------------------------- */
static void draw(int x, int y) {
    if (g_ops && g_ops->draw_cell && x >= 0 && y >= 0 && x < g_cols && y < g_rows)
        g_ops->draw_cell(x, y, &g_grid[y][x]);
}
static void redraw_rows(int y0, int y1) {
    for (int y = y0; y <= y1; y++)
        for (int x = 0; x < g_cols; x++) draw(x, y);
}
static void cursor_hide(void) { if (g_ops && g_ops->cursor) g_ops->cursor(cx, cy, 0); }
static void cursor_show(void) { if (g_ops && g_ops->cursor && cursor_on) g_ops->cursor(cx, cy, 1); }

static void clamp_cursor(void) {
    if (cx < 0) cx = 0;
    if (cx >= g_cols) cx = g_cols - 1;
    if (cy < 0) cy = 0;
    if (cy >= g_rows) cy = g_rows - 1;
}

/* Erase cells [x0, x1] on row y. */
static void erase_cells(int y, int x0, int x1) {
    if (y < 0 || y >= g_rows) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= g_cols) x1 = g_cols - 1;
    vt_cell_t b = blank_cell();
    for (int x = x0; x <= x1; x++) { g_grid[y][x] = b; draw(x, y); }
}

/* Scroll rows [top, bot] by n (n > 0: up, n < 0: down). */
static void scroll_region(int top, int bot, int n) {
    if (top < 0) top = 0;
    if (bot >= g_rows) bot = g_rows - 1;
    if (top >= bot || n == 0) {
        if (top == bot && n != 0) erase_cells(top, 0, g_cols - 1);
        return;
    }
    int h = bot - top + 1;
    int up = n > 0;
    int k = up ? n : -n;
    if (k > h) k = h;
    vt_cell_t b = blank_cell();
    if (up) {
        for (int y = top; y <= bot - k; y++)
            for (int x = 0; x < g_cols; x++) g_grid[y][x] = g_grid[y + k][x];
        for (int y = bot - k + 1; y <= bot; y++)
            for (int x = 0; x < g_cols; x++) g_grid[y][x] = b;
    } else {
        for (int y = bot; y >= top + k; y--)
            for (int x = 0; x < g_cols; x++) g_grid[y][x] = g_grid[y - k][x];
        for (int y = top; y < top + k; y++)
            for (int x = 0; x < g_cols; x++) g_grid[y][x] = b;
    }
    int fast = (k < h && g_ops && g_ops->scroll) ? g_ops->scroll(top, bot, up ? k : -k) : -1;
    if (fast == 0) {
        if (up) redraw_rows(bot - k + 1, bot);
        else    redraw_rows(top, top + k - 1);
    } else {
        redraw_rows(top, bot);
    }
}

static void linefeed(void) {
    if (cy == sc_bot) scroll_region(sc_top, sc_bot, 1);
    else if (cy < g_rows - 1) cy++;
}
static void reverse_index(void) {
    if (cy == sc_top) scroll_region(sc_top, sc_bot, -1);
    else if (cy > 0) cy--;
}

static void clear_screen_all(void) {
    vt_cell_t b = blank_cell();
    for (int y = 0; y < g_rows; y++)
        for (int x = 0; x < g_cols; x++) g_grid[y][x] = b;
    if (g_ops && g_ops->clear_all && g_ops->clear_all(b.bg) == 0) return;
    redraw_rows(0, g_rows - 1);
}

static void put_glyph(uint8_t g) {
    if (wrap_pending) {
        wrap_pending = 0;
        if (autowrap) { cx = 0; linefeed(); }
    }
    if (insert_mode) {
        for (int x = g_cols - 1; x > cx; x--) { g_grid[cy][x] = g_grid[cy][x - 1]; draw(x, cy); }
    }
    g_grid[cy][cx] = make_cell(g);
    draw(cx, cy);
    if (cx >= g_cols - 1) { cx = g_cols - 1; if (autowrap) wrap_pending = 1; }
    else cx++;
}

/* ---- Unicode -> glyph ---------------------------------------------------- */
/* Latin-1 Supplement letters U+00C0..U+00FF folded to their ASCII base. */
static const char latin1_fold[64] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"
    "aaaaaaaceeeeiiiidnooooo/ouuuuypy";

static uint8_t box_glyph(uint32_t cp) {
    /* Light/heavy lines U+2500..U+254B and double lines U+2550..U+256C. */
    if (cp == 0x2500 || cp == 0x2501 || cp == 0x2550 || cp == 0x2574 || cp == 0x2576 ||
        cp == 0x2578 || cp == 0x257A || cp == 0x254C || cp == 0x254D || cp == 0x2504 ||
        cp == 0x2505 || cp == 0x2508 || cp == 0x2509) return 0xC4;
    if (cp == 0x2502 || cp == 0x2503 || cp == 0x2551 || cp == 0x2575 || cp == 0x2577 ||
        cp == 0x2579 || cp == 0x257B || cp == 0x254E || cp == 0x254F || cp == 0x2506 ||
        cp == 0x2507 || cp == 0x250A || cp == 0x250B) return 0xB3;
    if (cp >= 0x250C && cp <= 0x250F) return 0xDA;
    if (cp >= 0x2510 && cp <= 0x2513) return 0xBF;
    if (cp >= 0x2514 && cp <= 0x2517) return 0xC0;
    if (cp >= 0x2518 && cp <= 0x251B) return 0xD9;
    if (cp >= 0x251C && cp <= 0x2523) return 0xC3;
    if (cp >= 0x2524 && cp <= 0x252B) return 0xB4;
    if (cp >= 0x252C && cp <= 0x2533) return 0xC2;
    if (cp >= 0x2534 && cp <= 0x253B) return 0xC1;
    if (cp >= 0x253C && cp <= 0x254B) return 0xC5;
    if (cp >= 0x2552 && cp <= 0x2554) return 0xDA;
    if (cp >= 0x2555 && cp <= 0x2557) return 0xBF;
    if (cp >= 0x2558 && cp <= 0x255A) return 0xC0;
    if (cp >= 0x255B && cp <= 0x255D) return 0xD9;
    if (cp >= 0x255E && cp <= 0x2560) return 0xC3;
    if (cp >= 0x2561 && cp <= 0x2563) return 0xB4;
    if (cp >= 0x2564 && cp <= 0x2566) return 0xC2;
    if (cp >= 0x2567 && cp <= 0x2569) return 0xC1;
    if (cp >= 0x256A && cp <= 0x256C) return 0xC5;
    if (cp == 0x256D) return 0xDA;
    if (cp == 0x256E) return 0xBF;
    if (cp == 0x256F) return 0xD9;
    if (cp == 0x2570) return 0xC0;
    return 0;
}

uint8_t vt_glyph_for(uint32_t cp) {
    if (cp >= 0x20 && cp < 0x7F) return (uint8_t)cp;
    /* Zero-width: combining marks, ZW space/joiners, BOM, variation selectors. */
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x200B && cp <= 0x200F) ||
        cp == 0xFEFF || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x00AD) return 0;
    switch (cp) {                       /* [M40] letters with a real glyph */
    case 0x00E0: return VT_G_A_GRAVE;  case 0x00E8: return VT_G_E_GRAVE;
    case 0x00E9: return VT_G_E_ACUTE;  case 0x00EC: return VT_G_I_GRAVE;
    case 0x00F2: return VT_G_O_GRAVE;  case 0x00F9: return VT_G_U_GRAVE;
    case 0x00C0: return VT_G_CA_GRAVE; case 0x00C8: return VT_G_CE_GRAVE;
    case 0x00C9: return VT_G_CE_ACUTE; case 0x00CC: return VT_G_CI_GRAVE;
    case 0x00D2: return VT_G_CO_GRAVE; case 0x00D9: return VT_G_CU_GRAVE;
    case 0x00E7: return VT_G_C_CEDIL;  case 0x00A7: return VT_G_SECTION;
    case 0x00A3: return VT_G_POUND;    case 0x20AC: return VT_G_EURO;
    default: break;
    }
    if (cp >= 0xC0 && cp <= 0xFF) return (uint8_t)latin1_fold[cp - 0xC0];
    switch (cp) {
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: return ' ';
    case 0x00A1: return '!';
    case 0x00A2: return 'c';
    case 0x00A9: return 'c';
    case 0x00AE: return 'r';
    case 0x00AB: case 0x2039: return '<';
    case 0x00BB: case 0x203A: return '>';
    case 0x00B0: case 0x00BA: return VT_G_DEGREE;
    case 0x00B1: return '+';
    case 0x00B2: return '2';
    case 0x00B3: return '3';
    case 0x00B9: return '1';
    case 0x00B5: return 'u';
    case 0x00B7: case 0x2022: case 0x2219: case 0x22C5: case 0x2027: return VT_G_MIDDOT;
    case 0x00BF: return '?';
    case 0x00D7: return 'x';
    case 0x00F7: return '/';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015:
    case 0x2212: case 0x2043: return '-';
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0x00B4: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: return '"';
    case 0x2026: return VT_G_ELLIPSIS;
    case 0x2190: return '<';
    case 0x2191: return '^';
    case 0x2192: return '>';
    case 0x2193: return 'v';
    case 0x21D2: return '>';
    case 0x2713: case 0x2714: case 0x221A: return VT_G_CHECK;
    case 0x2717: case 0x2718: return 'x';
    case 0x2588: case 0x2589: case 0x258A: case 0x258B: case 0x258C: case 0x2590:
    case 0x2580: case 0x2584: case 0x25A0: return VT_G_BLOCK;
    case 0x2591: case 0x2592: case 0x2593: return VT_G_SHADE;
    case 0x2264: return '<';
    case 0x2265: return '>';
    case 0x2260: return '#';
    case 0x221E: return '8';
    default: break;
    }
    uint8_t b = box_glyph(cp);
    if (b) return b;
    return VT_G_UNKNOWN;
}

/* A byte that is not part of valid UTF-8: CP437 box glyph if we draw one. */
static uint8_t cp437_fallback(uint8_t byte) {
    switch (byte) {
    case 0xB3: case 0xB4: case 0xBF: case 0xC0: case 0xC1: case 0xC2: case 0xC3:
    case 0xC4: case 0xC5: case 0xD9: case 0xDA: return byte;
    default: return VT_G_UNKNOWN;
    }
}

static void put_codepoint(uint32_t cp) {
    uint8_t g = vt_glyph_for(cp);
    if (g) put_glyph(g);
}

/* ---- SGR ------------------------------------------------------------------ */
/* Nearest of the 16 VGA colors for a 24-bit color. */
static int rgb_to_vga(int r, int g, int b) {
    static const uint8_t pal[16][3] = {
        {0,0,0},{0,0,170},{0,170,0},{0,170,170},{170,0,0},{170,0,170},{170,85,0},{170,170,170},
        {85,85,85},{85,85,255},{85,255,85},{85,255,255},{255,85,85},{255,85,255},{255,255,85},{255,255,255}
    };
    int best = 0; long bd = 0x7FFFFFFF;
    for (int i = 0; i < 16; i++) {
        long dr = r - pal[i][0], dg = g - pal[i][1], db = b - pal[i][2];
        long d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static int xterm256_to_vga(int n) {
    if (n < 0) return 7;
    if (n < 8) return ansi2vga[n];
    if (n < 16) return ansi2vga[n - 8] + 8;
    if (n < 232) {
        n -= 16;
        static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
        return rgb_to_vga(lv[(n / 36) % 6], lv[(n / 6) % 6], lv[n % 6]);
    }
    if (n < 256) { int v = 8 + (n - 232) * 10; return rgb_to_vga(v, v, v); }
    return 7;
}

static void do_sgr(void) {
    if (npar == 0) { npar = 1; par[0] = 0; }
    for (int i = 0; i < npar; i++) {
        int p = par[i];
        if (p == 0) { sgr_fg = sgr_bg = -1; sgr_bold = sgr_rev = sgr_ul = sgr_conceal = 0; }
        else if (p == 1) sgr_bold = 1;
        else if (p == 2) sgr_bold = 0;            /* dim: render as normal */
        else if (p == 4) sgr_ul = 1;
        else if (p == 5 || p == 6) { /* blink: ignored */ }
        else if (p == 7) sgr_rev = 1;
        else if (p == 8) sgr_conceal = 1;
        else if (p == 21 || p == 22) sgr_bold = 0;
        else if (p == 24) sgr_ul = 0;
        else if (p == 25) { }
        else if (p == 27) sgr_rev = 0;
        else if (p == 28) sgr_conceal = 0;
        else if (p >= 30 && p <= 37) sgr_fg = ansi2vga[p - 30];
        else if (p == 39) sgr_fg = -1;
        else if (p >= 40 && p <= 47) sgr_bg = ansi2vga[p - 40];
        else if (p == 49) sgr_bg = -1;
        else if (p >= 90 && p <= 97) sgr_fg = ansi2vga[p - 90] + 8;
        else if (p >= 100 && p <= 107) sgr_bg = ansi2vga[p - 100] + 8;
        else if (p == 38 || p == 48) {
            int col = -1;
            if (i + 2 < npar && par[i + 1] == 5) { col = xterm256_to_vga(par[i + 2]); i += 2; }
            else if (i + 4 < npar && par[i + 1] == 2) {
                col = rgb_to_vga(par[i + 2] & 255, par[i + 3] & 255, par[i + 4] & 255); i += 4;
            } else break;                           /* malformed: stop */
            if (p == 38) sgr_fg = col; else sgr_bg = col;
        }
    }
}

/* ---- modes ---------------------------------------------------------------- */
static void save_cursor(void) {
    sv_x = cx; sv_y = cy; sv_fg = sgr_fg; sv_bg = sgr_bg; sv_bold = sgr_bold;
    sv_rev = sgr_rev; sv_ul = sgr_ul; sv_wrap = wrap_pending;
}
static void restore_cursor(void) {
    cx = sv_x; cy = sv_y; sgr_fg = sv_fg; sgr_bg = sv_bg; sgr_bold = sv_bold;
    sgr_rev = sv_rev; sgr_ul = sv_ul; wrap_pending = sv_wrap;
    clamp_cursor();
}
static void alt_screen(int on, int save_restore_cursor) {
    if (on == g_alt_active) return;
    if (on) {
        if (save_restore_cursor) save_cursor();
        for (int y = 0; y < g_rows; y++)
            for (int x = 0; x < g_cols; x++) g_alt[y][x] = g_grid[y][x];
        g_alt_active = 1;
        app_screen = 1;
        clear_screen_all();
    } else {
        for (int y = 0; y < g_rows; y++)
            for (int x = 0; x < g_cols; x++) g_grid[y][x] = g_alt[y][x];
        g_alt_active = 0;
        if (save_restore_cursor) restore_cursor();
        redraw_rows(0, g_rows - 1);
    }
}
static void set_mode(int on) {
    for (int i = 0; i < (npar ? npar : 1); i++) {
        int p = npar ? par[i] : 0;
        if (csi_priv == '?') {
            if (p == 25) cursor_on = on;
            else if (p == 7) { autowrap = on; if (!on) wrap_pending = 0; }
            else if (p == 1049) alt_screen(on, 1);
            else if (p == 47 || p == 1047) alt_screen(on, 0);
            else if (p == 1048) { if (on) save_cursor(); else restore_cursor(); }
            /* ?1 (cursor keys), ?12 (blink), ?2004 (bracketed paste), mouse: ignored */
        } else if (csi_priv == 0) {
            if (p == 4) insert_mode = on;
        }
    }
}

static void reply(const char* s) {
    int n = 0; while (s[n]) n++;
    if (g_reply) g_reply(s, n);
}
static int fmt_dec(char* o, int v) {
    char t[12]; int n = 0, k = 0;
    if (v <= 0) { o[0] = '0'; return 1; }
    while (v > 0 && n < 11) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) o[k++] = t[--n];
    return k;
}

static void reset_all(void) {
    cx = cy = 0; wrap_pending = 0; sc_top = 0; sc_bot = g_rows - 1;
    autowrap = 1; insert_mode = 0; cursor_on = 1;
    sgr_fg = sgr_bg = -1; sgr_bold = sgr_rev = sgr_ul = sgr_conceal = 0;
    sv_x = sv_y = 0; sv_fg = sv_bg = -1; sv_bold = sv_rev = sv_ul = sv_wrap = 0;
    st = ST_GROUND; u_need = 0;
}

/* ---- CSI dispatch ---------------------------------------------------------- */
static int P(int i, int dflt) { return (i < npar && par[i] > 0) ? par[i] : dflt; }
static int P0(int i) { return (i < npar) ? par[i] : 0; }

static void do_csi(char f) {
    if (csi_inter) {
        /* CSI ... SP q (cursor style), CSI ! p (soft reset) */
        if (csi_inter == '!' && f == 'p') { reset_all(); }
        return;
    }
    if (csi_priv == '>' || csi_priv == '=') {
        if (f == 'c' && csi_priv == '>') reply("\033[>0;0;0c");
        return;
    }
    int n;
    switch (f) {
    case 'A': cy -= P(0, 1); if (cy < sc_top && cy + P(0, 1) >= sc_top) cy = sc_top; break;
    case 'B': case 'e':
        n = P(0, 1);
        if (cy <= sc_bot && cy + n > sc_bot) cy = sc_bot; else cy += n;
        break;
    case 'C': case 'a': cx += P(0, 1); break;
    case 'D': cx -= P(0, 1); break;
    case 'E': cy += P(0, 1); cx = 0; if (cy > sc_bot && cy - P(0, 1) <= sc_bot) cy = sc_bot; break;
    case 'F': cy -= P(0, 1); cx = 0; if (cy < sc_top && cy + P(0, 1) >= sc_top) cy = sc_top; break;
    case 'G': case '`': cx = P(0, 1) - 1; break;
    case 'd': cy = P(0, 1) - 1; app_screen = 1; break;
    case 'H': case 'f': cy = P(0, 1) - 1; cx = P(1, 1) - 1; app_screen = 1; break;
    case 'J':
        n = P0(0);
        if (n == 0) {
            erase_cells(cy, cx, g_cols - 1);
            for (int y = cy + 1; y < g_rows; y++) erase_cells(y, 0, g_cols - 1);
        } else if (n == 1) {
            for (int y = 0; y < cy; y++) erase_cells(y, 0, g_cols - 1);
            erase_cells(cy, 0, cx);
        } else if (n == 2 || n == 3) {
            clear_screen_all();
            app_screen = 1;
        }
        break;
    case 'K':
        n = P0(0);
        if (n == 0) erase_cells(cy, cx, g_cols - 1);
        else if (n == 1) erase_cells(cy, 0, cx);
        else if (n == 2) erase_cells(cy, 0, g_cols - 1);
        break;
    case 'L':                                   /* insert lines at the cursor */
        if (cy >= sc_top && cy <= sc_bot) scroll_region(cy, sc_bot, -P(0, 1));
        cx = 0;
        break;
    case 'M':                                   /* delete lines at the cursor */
        if (cy >= sc_top && cy <= sc_bot) scroll_region(cy, sc_bot, P(0, 1));
        cx = 0;
        break;
    case '@': {                                 /* insert blank characters */
        n = P(0, 1); if (n > g_cols - cx) n = g_cols - cx;
        for (int x = g_cols - 1; x >= cx + n; x--) g_grid[cy][x] = g_grid[cy][x - n];
        vt_cell_t b = blank_cell();
        for (int x = cx; x < cx + n; x++) g_grid[cy][x] = b;
        for (int x = cx; x < g_cols; x++) draw(x, cy);
        break; }
    case 'P': {                                 /* delete characters */
        n = P(0, 1); if (n > g_cols - cx) n = g_cols - cx;
        for (int x = cx; x < g_cols - n; x++) g_grid[cy][x] = g_grid[cy][x + n];
        vt_cell_t b = blank_cell();
        for (int x = g_cols - n; x < g_cols; x++) g_grid[cy][x] = b;
        for (int x = cx; x < g_cols; x++) draw(x, cy);
        break; }
    case 'X':                                   /* erase characters */
        n = P(0, 1);
        erase_cells(cy, cx, cx + n - 1);
        break;
    case 'S': scroll_region(sc_top, sc_bot, P(0, 1)); break;
    case 'T': if (csi_priv == 0) scroll_region(sc_top, sc_bot, -P(0, 1)); break;
    case 'm': if (csi_priv == 0) do_sgr(); break;
    case 'r': {
        int t = P(0, 1) - 1, b = P(1, g_rows) - 1;
        if (t < 0) t = 0;
        if (b >= g_rows) b = g_rows - 1;
        if (t < b) { sc_top = t; sc_bot = b; }
        else { sc_top = 0; sc_bot = g_rows - 1; }
        cx = 0; cy = 0;
        break; }
    case 's': if (csi_priv == 0) save_cursor(); break;
    case 'u': if (csi_priv == 0) restore_cursor(); break;
    case 'h': set_mode(1); break;
    case 'l': set_mode(0); break;
    case 'n':
        if (csi_priv) break;
        if (P0(0) == 5) reply("\033[0n");
        else if (P0(0) == 6) {
            char b[24]; int k = 0;
            b[k++] = '\033'; b[k++] = '[';
            k += fmt_dec(b + k, cy + 1); b[k++] = ';';
            k += fmt_dec(b + k, cx + 1); b[k++] = 'R'; b[k] = 0;
            reply(b);
        }
        break;
    case 'c': if (csi_priv == 0 && P0(0) == 0) reply("\033[?6c"); break;
    default: break;                             /* unsupported: ignored */
    }
    if (f != 'm' && f != 'n' && f != 'c' && f != 'h' && f != 'l') wrap_pending = 0;
    clamp_cursor();
}

/* ---- C0 controls ----------------------------------------------------------- */
static void do_control(uint8_t c) {
    switch (c) {
    case 0x07: break;                           /* BEL */
    case 0x08:                                  /* BS: non-destructive */
        if (cx > 0) cx--;                       /* Linux console: also cancels a pending wrap */
        wrap_pending = 0;
        break;
    case 0x09: {                                /* HT: next multiple of 8 */
        int nx = (cx / 8 + 1) * 8;
        cx = nx >= g_cols ? g_cols - 1 : nx;
        break; }
    case 0x0A: case 0x0B: case 0x0C:            /* LF/VT/FF: line feed */
        wrap_pending = 0;
        linefeed();
        break;
    case 0x0D: cx = 0; wrap_pending = 0; break; /* CR */
    default: break;
    }
}

/* ---- byte state machine ---------------------------------------------------- */
static void csi_begin(void) {
    npar = 0; par_has = 0; csi_priv = 0; csi_inter = 0;
    for (int i = 0; i < VT_NPAR; i++) par[i] = 0;
    st = ST_CSI;
}

static void feed(uint8_t c) {
    /* CAN/SUB abort any sequence; ESC always (re)starts one. */
    if (c == 0x18 || c == 0x1A) { st = ST_GROUND; u_need = 0; return; }
    if (c == 0x1B) {
        if (st == ST_STR) { st = ST_STR_ESC; return; }
        if (u_need) { u_need = 0; put_glyph(VT_G_UNKNOWN); }
        st = ST_ESC; return;
    }
    switch (st) {
    case ST_ESC:
        st = ST_GROUND;
        switch (c) {
        case '[': csi_begin(); return;
        case ']': case 'P': case '_': case '^': case 'X': st = ST_STR; return;
        case '(': case ')': case '*': case '+': case '#': case '%': st = ST_ESC_SKIP1; return;
        case '7': save_cursor(); return;
        case '8': restore_cursor(); return;
        case 'D': wrap_pending = 0; linefeed(); return;
        case 'E': wrap_pending = 0; cx = 0; linefeed(); return;
        case 'M': wrap_pending = 0; reverse_index(); return;
        case 'c': reset_all(); clear_screen_all(); app_screen = 0; return;
        default: return;                        /* = > H Z ... ignored */
        }
    case ST_ESC_SKIP1: st = ST_GROUND; return;
    case ST_STR:
        if (c == 0x07) st = ST_GROUND;          /* OSC terminated by BEL */
        return;
    case ST_STR_ESC:
        st = (c == '\\') ? ST_GROUND : ST_STR;
        return;
    case ST_CSI:
        if (c < 0x20) { do_control(c); return; }   /* C0 inside CSI executes */
        if (c >= '0' && c <= '9') {
            if (npar == 0) npar = 1;
            if (npar <= VT_NPAR) {
                int v = par[npar - 1] * 10 + (c - '0');
                par[npar - 1] = v > 9999 ? 9999 : v;
            }
            par_has = 1;
            return;
        }
        if (c == ';' || c == ':') {
            if (npar == 0) npar = 1;
            if (npar < VT_NPAR) npar++;
            par_has = 0;
            return;
        }
        if (c == '?' || c == '>' || c == '=' || c == '<') { csi_priv = (char)c; return; }
        if (c >= 0x20 && c <= 0x2F) { csi_inter = (char)c; return; }
        if (c >= 0x40 && c <= 0x7E) {
            if (npar > VT_NPAR) npar = VT_NPAR;
            st = ST_GROUND;
            do_csi((char)c);
            return;
        }
        st = ST_GROUND;                         /* invalid byte: abort */
        return;
    default: break;
    }

    /* Ground state. */
    if (c < 0x20 || c == 0x7F) {
        if (u_need) { u_need = 0; put_glyph(VT_G_UNKNOWN); }
        if (c != 0x7F) do_control(c);
        return;
    }
    if (c < 0x80) {
        if (u_need) { u_need = 0; put_glyph(VT_G_UNKNOWN); }
        put_glyph(c);
        return;
    }
    if (c < 0xC0) {                             /* continuation byte */
        if (u_need) {
            u_cp = (u_cp << 6) | (c & 0x3F);
            if (--u_need == 0) put_codepoint(u_cp);
        } else {
            put_glyph(cp437_fallback(c));
        }
        return;
    }
    if (u_need) { u_need = 0; put_glyph(cp437_fallback(u_lead)); }
    u_lead = c;
    if (c >= 0xC2 && c <= 0xDF) { u_need = 1; u_cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { u_need = 2; u_cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { u_need = 3; u_cp = c & 0x07; }
    else put_glyph(cp437_fallback(c));
}

/* ---- public entry points --------------------------------------------------- */
void vt_attach(const vt_ops_t* ops, int cols, int rows) {
    g_busy++;
    if (cols > VT_MAX_COLS) cols = VT_MAX_COLS;
    if (rows > VT_MAX_ROWS) rows = VT_MAX_ROWS;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    g_ops = ops; g_cols = cols; g_rows = rows;
    g_alt_active = 0; app_screen = 0;
    reset_all();
    vt_cell_t b = blank_cell();
    for (int y = 0; y < g_rows; y++)
        for (int x = 0; x < g_cols; x++) g_grid[y][x] = b;
    g_busy--;
}

void vt_putc(char c) {
    if (!g_ops) return;
    g_busy++;
    cursor_hide();
    feed((uint8_t)c);
    clamp_cursor();
    cursor_show();
    if (g_ops->flush) g_ops->flush();
    g_busy--;
}

void vt_write(const char* s, size_t n) {
    if (!g_ops || !s) return;
    g_busy++;
    cursor_hide();
    for (size_t i = 0; i < n; i++) feed((uint8_t)s[i]);
    clamp_cursor();
    cursor_show();
    if (g_ops->flush) g_ops->flush();
    g_busy--;
}
