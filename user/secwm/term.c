/* SecOS GUI - VT100/xterm-subset terminal emulator for secwm windows.
 * SPDX-License-Identifier: MIT
 *
 * Implemented (enough for bash/readline, BusyBox vi/less/top, lua):
 *   C0: BEL BS HT LF VT FF CR SO SI CAN SUB ESC
 *   ESC: 7 8 D E M c H = > ( ) * + (G0-G3: B ASCII, 0 DEC line drawing) # 8 [ ] P \
 *   CSI: @ A B C D E F G H I J K L M P S T X Z ` a b c d e f g h l m n r s u
 *        and the private modes ?1 (DECCKM) ?6 (DECOM) ?7 (DECAWM) ?25 (DECTCEM)
 *        ?47 ?1047 ?1048 ?1049 (alternate screen / saved cursor) ?2004
 *        (bracketed paste), 4 (IRM), 20 (LNM); >c (secondary DA); !p (DECSTR)
 *   SGR: 0 1 2 3 4 5 7 8 9 21-29, 30-37 38;5;n 38;2;r;g;b 39, 40-47 48;... 49,
 *        90-97, 100-107
 *   OSC 0/1/2 (title); other OSC and DCS strings are swallowed.
 */
#include "term.h"
#include <stdlib.h>
#include <string.h>

/* Cell colours: the top byte says what the low 24 bits mean. */
#define COL_DEF   0x01000000u           /* default foreground / background */
#define COL_PAL   0x02000000u           /* palette index 0..255 */
#define COL_RGB   0x03000000u           /* 24-bit colour */
#define COL_TAG(c) ((c) & 0xFF000000u)

#define A_BOLD    0x0001
#define A_DIM     0x0002
#define A_ITALIC  0x0004
#define A_UL      0x0008
#define A_BLINK   0x0010
#define A_REV     0x0020
#define A_HIDDEN  0x0040
#define A_STRIKE  0x0080
#define A_CURSOR  0x8000                /* drawn-state only: cursor block on this cell */
#define A_CURSOR_HOLLOW 0x4000

typedef struct cell { uint32_t cp; uint32_t fg, bg; uint16_t attr; uint16_t pad; } cell_t;

enum { ST_GROUND, ST_ESC, ST_ESC_INTER, ST_CSI, ST_OSC, ST_OSC_ESC, ST_STR, ST_STR_ESC };

#define NPAR 16
#define MAXOPS 8

struct term {
    int cols, rows;
    cell_t* main;                       /* rows * cols */
    cell_t* alt;
    cell_t* scr;                        /* main or alt */
    int alt_on;
    cell_t* drawn;                      /* what the pixels show (rows * cols) */
    int drawn_valid;
    int drawn_focus;
    struct { int top, bot, n; } ops[MAXOPS];   /* pixel scrolls since the last render */
    int nops;
    cell_t* sb;                         /* scrollback ring: TERM_SB_LINES * cols */
    int sb_head, sb_count;              /* next slot, lines stored */
    int view;                           /* lines scrolled back */
    /* cursor + modes */
    int cx, cy, wrap_pending;
    int top, bot;                       /* scroll region, inclusive */
    int autowrap, insert, origin, cursor_on, app_cursor, app_keypad, lnm, bpaste;
    uint32_t fg, bg; uint16_t attr;     /* current SGR */
    int g[4], gl;                       /* charsets: 0 ASCII, 1 DEC special */
    struct { int cx, cy, wrap, origin; uint32_t fg, bg; uint16_t attr; int g0, g1, gl; } sv, sv_alt;
    uint8_t* tabs;                      /* tab stop per column */
    /* parser */
    int st;
    int par[NPAR], npar, par_has;
    char priv, inter;
    uint32_t u_cp; int u_need;
    char osc[128]; int osc_len;
    char title[64]; int title_changed;
    uint32_t last_cp;                   /* for REP */
    char reply[128]; int reply_len;
    /* trace */
    void (*line_hook)(void*, const char*); void* line_ctx;
    char line[256]; int line_len;
};

/* ---- palette ------------------------------------------------------------ */
static const uint32_t base16[16] = {
    0x20262E, 0xE05561, 0x7FBF5A, 0xE5B45A, 0x4F8FD9, 0xB07AD6, 0x3FB8B0, 0xC8CED6,
    0x5A6472, 0xFF7A85, 0xA6E07F, 0xFFD37A, 0x7AB3FF, 0xD6A3FF, 0x6EE0D6, 0xF3F6F9,
};
#define DEF_FG 0xD4DBE3u
#define DEF_BG 0x11161Du
#define CURSOR_COL 0x6EE0D6u

static uint32_t pal256(int i) {
    if (i < 16) return base16[i];
    if (i < 232) {
        static const uint8_t lv[6] = { 0, 95, 135, 175, 215, 255 };
        i -= 16;
        return gfx_rgb(lv[(i / 36) % 6], lv[(i / 6) % 6], lv[i % 6]);
    }
    int g = 8 + (i - 232) * 10;
    return gfx_rgb(g, g, g);
}

static uint32_t resolve(uint32_t c, int is_fg, int bold) {
    switch (COL_TAG(c)) {
    case COL_PAL: {
        int i = (int)(c & 0xFF);
        if (is_fg && bold && i < 8) i += 8;
        return pal256(i);
    }
    case COL_RGB: return c & 0xFFFFFF;
    default: return is_fg ? DEF_FG : DEF_BG;
    }
}

/* ---- helpers ------------------------------------------------------------ */
static inline cell_t* AT(term_t* t, int x, int y) { return &t->scr[(long)y * t->cols + x]; }
static cell_t blank(term_t* t) {
    /* Background colour erase: keep the colours, drop the other attributes. */
    cell_t c; c.cp = ' '; c.fg = t->fg; c.bg = t->bg; c.attr = 0; c.pad = 0;
    return c;
}
static void fill_cells(cell_t* p, int n, cell_t v) { for (int i = 0; i < n; i++) p[i] = v; }

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

int term_px_w(int cols) { return cols * GFX_FONT_W + 2 * TERM_PAD; }
int term_px_h(int rows) { return rows * GFX_FONT_H + 2 * TERM_PAD; }
int term_cols(const term_t* t) { return t->cols; }
int term_rows(const term_t* t) { return t->rows; }
int term_app_cursor(const term_t* t) { return t->app_cursor; }
int term_app_keypad(const term_t* t) { return t->app_keypad; }
int term_bracketed_paste(const term_t* t) { return t->bpaste; }
const char* term_title(const term_t* t) { return t->title; }
int term_title_changed(term_t* t) { int c = t->title_changed; t->title_changed = 0; return c; }
void term_invalidate(term_t* t) { t->drawn_valid = 0; }
void term_set_line_hook(term_t* t, void (*hook)(void*, const char*), void* ctx) { t->line_hook = hook; t->line_ctx = ctx; }

static void reset_state(term_t* t) {
    t->cx = t->cy = t->wrap_pending = 0;
    t->top = 0; t->bot = t->rows - 1;
    t->autowrap = 1; t->insert = 0; t->origin = 0; t->cursor_on = 1;
    t->app_cursor = 0; t->app_keypad = 0; t->lnm = 0; t->bpaste = 0;
    t->fg = COL_DEF; t->bg = COL_DEF; t->attr = 0;
    t->g[0] = t->g[1] = t->g[2] = t->g[3] = 0; t->gl = 0;
    memset(&t->sv, 0, sizeof(t->sv)); t->sv.fg = t->sv.bg = COL_DEF;
    t->sv_alt = t->sv;
    t->st = ST_GROUND;
    for (int i = 0; i < t->cols; i++) t->tabs[i] = (i % 8) == 0 && i;
}

static int alloc_grids(term_t* t, int cols, int rows) {
    long n = (long)cols * rows;
    cell_t* m = malloc(sizeof(cell_t) * n);
    cell_t* a = malloc(sizeof(cell_t) * n);
    cell_t* d = malloc(sizeof(cell_t) * n);
    cell_t* s = malloc(sizeof(cell_t) * (long)cols * TERM_SB_LINES);
    uint8_t* tb = malloc((size_t)cols);
    if (!m || !a || !d || !s || !tb) { free(m); free(a); free(d); free(s); free(tb); return -1; }
    t->main = m; t->alt = a; t->drawn = d; t->sb = s; t->tabs = tb;
    t->cols = cols; t->rows = rows;
    return 0;
}

term_t* term_new(int cols, int rows) {
    term_t* t = malloc(sizeof(term_t));
    if (!t) return 0;
    memset(t, 0, sizeof(*t));
    if (alloc_grids(t, cols, rows) < 0) { free(t); return 0; }
    t->scr = t->main;
    reset_state(t);
    cell_t b = blank(t);
    fill_cells(t->main, cols * rows, b);
    fill_cells(t->alt, cols * rows, b);
    return t;
}

void term_free(term_t* t) {
    if (!t) return;
    free(t->main); free(t->alt); free(t->drawn); free(t->sb); free(t->tabs);
    free(t);
}

int term_resize(term_t* t, int cols, int rows) {
    if (cols < 2 || rows < 2 || cols > 400 || rows > 200) return -1;
    if (cols == t->cols && rows == t->rows) return 0;
    cell_t *om = t->main, *oa = t->alt, *od = t->drawn, *os = t->sb; uint8_t* ot = t->tabs;
    int oc = t->cols, orr = t->rows;
    if (alloc_grids(t, cols, rows) < 0) { t->main = om; t->alt = oa; t->drawn = od; t->sb = os; t->tabs = ot; return -1; }
    cell_t b; b.cp = ' '; b.fg = COL_DEF; b.bg = COL_DEF; b.attr = 0; b.pad = 0;
    /* Keep the bottom of the main screen when it shrinks (the prompt). */
    int shift = 0;
    if (t->cy >= rows) shift = t->cy - rows + 1;
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            int sy = y + shift;
            t->main[(long)y * cols + x] = (sy < orr && x < oc) ? om[(long)sy * oc + x] : b;
            t->alt[(long)y * cols + x] = (y < orr && x < oc) ? oa[(long)y * oc + x] : b;
        }
    t->scr = t->alt_on ? t->alt : t->main;
    free(om); free(oa); free(od); free(os); free(ot);
    t->sb_head = t->sb_count = 0; t->view = 0;
    t->cy -= shift;
    t->cx = imin(t->cx, cols - 1); t->cy = imax(0, imin(t->cy, rows - 1));
    t->top = 0; t->bot = rows - 1; t->wrap_pending = 0;
    for (int i = 0; i < cols; i++) t->tabs[i] = (i % 8) == 0 && i;
    t->drawn_valid = 0; t->nops = 0;
    return 0;
}

/* ---- scrolling ---------------------------------------------------------- */
static void sb_push(term_t* t, const cell_t* row) {
    memcpy(&t->sb[(long)t->sb_head * t->cols], row, sizeof(cell_t) * t->cols);
    t->sb_head = (t->sb_head + 1) % TERM_SB_LINES;
    if (t->sb_count < TERM_SB_LINES) t->sb_count++;
    if (t->view > 0 && t->view < t->sb_count) { t->view++; t->drawn_valid = 0; }
}

static void note_op(term_t* t, int top, int bot, int n) {
    if (t->view > 0 || !t->drawn_valid) return;
    if (t->nops == MAXOPS) { t->drawn_valid = 0; t->nops = 0; return; }
    t->ops[t->nops].top = top; t->ops[t->nops].bot = bot; t->ops[t->nops].n = n;
    t->nops++;
}

/* Scroll rows [top, bot] up by n (n > 0) or down by -n. */
static void scroll_region(term_t* t, int top, int bot, int n) {
    int h = bot - top + 1;
    if (h <= 0 || n == 0) return;
    cell_t b = blank(t);
    if (n > 0) {
        if (n > h) n = h;
        if (top == 0 && !t->alt_on)
            for (int i = 0; i < n; i++) sb_push(t, AT(t, 0, top + i));
        memmove(AT(t, 0, top), AT(t, 0, top + n), sizeof(cell_t) * (long)t->cols * (h - n));
        fill_cells(AT(t, 0, bot - n + 1), t->cols * n, b);
        note_op(t, top, bot, n);
    } else {
        n = -n;
        if (n > h) n = h;
        memmove(AT(t, 0, top + n), AT(t, 0, top), sizeof(cell_t) * (long)t->cols * (h - n));
        fill_cells(AT(t, 0, top), t->cols * n, b);
        note_op(t, top, bot, -n);
    }
}

static void linefeed(term_t* t) {
    t->wrap_pending = 0;
    if (t->cy == t->bot) scroll_region(t, t->top, t->bot, 1);
    else if (t->cy < t->rows - 1) t->cy++;
}

static void reverse_index(term_t* t) {
    t->wrap_pending = 0;
    if (t->cy == t->top) scroll_region(t, t->top, t->bot, -1);
    else if (t->cy > 0) t->cy--;
}

static void erase(term_t* t, int y, int x0, int x1) {   /* [x0, x1) */
    x0 = imax(0, x0); x1 = imin(t->cols, x1);
    if (x1 > x0) fill_cells(AT(t, x0, y), x1 - x0, blank(t));
}

/* ---- output ------------------------------------------------------------- */
static void trace_char(term_t* t, uint32_t cp) {
    if (!t->line_hook) return;
    if (cp == '\n') {
        t->line[t->line_len] = 0;
        t->line_hook(t->line_ctx, t->line);
        t->line_len = 0;
        return;
    }
    if (cp >= 0x20 && cp < 0x7F && t->line_len < (int)sizeof(t->line) - 1) t->line[t->line_len++] = (char)cp;
}

static uint32_t dec_special(uint32_t c) {
    static const uint16_t map[32] = {
        0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1,   /* ` a b c d e f g */
        0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA,   /* h i j k l m n o */
        0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C,   /* p q r s t u v w */
        0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7, 0x0020,   /* x y z { | } ~ DEL */
    };
    if (c >= 0x60 && c <= 0x7F) return map[c - 0x60];
    if (c == 0x5F) return ' ';
    return c;
}

static void put_cp(term_t* t, uint32_t cp) {
    if (t->g[t->gl] == 1 && cp < 0x80) cp = dec_special(cp);
    if (t->wrap_pending && t->autowrap) {
        t->cx = 0;
        linefeed(t);
    }
    t->wrap_pending = 0;
    if (t->insert && t->cx < t->cols - 1)
        memmove(AT(t, t->cx + 1, t->cy), AT(t, t->cx, t->cy), sizeof(cell_t) * (t->cols - t->cx - 1));
    cell_t* c = AT(t, t->cx, t->cy);
    c->cp = cp; c->fg = t->fg; c->bg = t->bg; c->attr = t->attr; c->pad = 0;
    t->last_cp = cp;
    trace_char(t, cp);
    if (t->cx == t->cols - 1) t->wrap_pending = 1;
    else t->cx++;
}

static void clamp_cursor(term_t* t) {
    t->cx = imax(0, imin(t->cx, t->cols - 1));
    int lo = t->origin ? t->top : 0, hi = t->origin ? t->bot : t->rows - 1;
    t->cy = imax(lo, imin(t->cy, hi));
}

static void save_cursor(term_t* t) {
    t->sv.cx = t->cx; t->sv.cy = t->cy; t->sv.wrap = t->wrap_pending; t->sv.origin = t->origin;
    t->sv.fg = t->fg; t->sv.bg = t->bg; t->sv.attr = t->attr;
    t->sv.g0 = t->g[0]; t->sv.g1 = t->g[1]; t->sv.gl = t->gl;
}
static void restore_cursor(term_t* t) {
    t->cx = t->sv.cx; t->cy = t->sv.cy; t->wrap_pending = t->sv.wrap; t->origin = t->sv.origin;
    t->fg = t->sv.fg; t->bg = t->sv.bg; t->attr = t->sv.attr;
    t->g[0] = t->sv.g0; t->g[1] = t->sv.g1; t->gl = t->sv.gl;
    clamp_cursor(t);
}

static void set_alt(term_t* t, int on, int save) {
    if (on == t->alt_on) return;
    if (on) {
        if (save) save_cursor(t);
        t->alt_on = 1; t->scr = t->alt;
        fill_cells(t->alt, t->cols * t->rows, blank(t));
    } else {
        t->alt_on = 0; t->scr = t->main;
        if (save) restore_cursor(t);
    }
    t->view = 0;
    t->drawn_valid = 0;
}

static void reply(term_t* t, const char* s) {
    int n = (int)strlen(s);
    if (t->reply_len + n > (int)sizeof(t->reply)) return;
    memcpy(t->reply + t->reply_len, s, (size_t)n);
    t->reply_len += n;
}

int term_take_reply(term_t* t, char* out, int max) {
    int n = imin(max, t->reply_len);
    memcpy(out, t->reply, (size_t)n);
    memmove(t->reply, t->reply + n, (size_t)(t->reply_len - n));
    t->reply_len -= n;
    return n;
}

static void fmt_int(char* p, int v) {     /* tiny itoa (no printf dependency) */
    char tmp[12]; int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n) *p++ = tmp[--n];
    *p = 0;
}

static int P(term_t* t, int i, int def) { return (i < t->npar && t->par[i] > 0) ? t->par[i] : def; }
static int P0(term_t* t, int i) { return i < t->npar ? t->par[i] : 0; }

static void do_sgr(term_t* t) {
    if (t->npar == 0) { t->npar = 1; t->par[0] = 0; }
    for (int i = 0; i < t->npar; i++) {
        int p = t->par[i];
        if (p == 0) { t->fg = t->bg = COL_DEF; t->attr = 0; }
        else if (p == 1) t->attr |= A_BOLD;
        else if (p == 2) t->attr |= A_DIM;
        else if (p == 3) t->attr |= A_ITALIC;
        else if (p == 4) t->attr |= A_UL;
        else if (p == 5 || p == 6) t->attr |= A_BLINK;
        else if (p == 7) t->attr |= A_REV;
        else if (p == 8) t->attr |= A_HIDDEN;
        else if (p == 9) t->attr |= A_STRIKE;
        else if (p == 21) t->attr |= A_UL;
        else if (p == 22) t->attr &= (uint16_t)~(A_BOLD | A_DIM);
        else if (p == 23) t->attr &= (uint16_t)~A_ITALIC;
        else if (p == 24) t->attr &= (uint16_t)~A_UL;
        else if (p == 25) t->attr &= (uint16_t)~A_BLINK;
        else if (p == 27) t->attr &= (uint16_t)~A_REV;
        else if (p == 28) t->attr &= (uint16_t)~A_HIDDEN;
        else if (p == 29) t->attr &= (uint16_t)~A_STRIKE;
        else if (p >= 30 && p <= 37) t->fg = COL_PAL | (uint32_t)(p - 30);
        else if (p == 39) t->fg = COL_DEF;
        else if (p >= 40 && p <= 47) t->bg = COL_PAL | (uint32_t)(p - 40);
        else if (p == 49) t->bg = COL_DEF;
        else if (p >= 90 && p <= 97) t->fg = COL_PAL | (uint32_t)(p - 90 + 8);
        else if (p >= 100 && p <= 107) t->bg = COL_PAL | (uint32_t)(p - 100 + 8);
        else if (p == 38 || p == 48) {
            uint32_t col = 0; int ok = 0;
            if (i + 2 < t->npar && t->par[i + 1] == 5) {
                col = COL_PAL | (uint32_t)(t->par[i + 2] & 0xFF); i += 2; ok = 1;
            } else if (i + 4 < t->npar && t->par[i + 1] == 2) {
                col = COL_RGB | gfx_rgb(t->par[i + 2] & 0xFF, t->par[i + 3] & 0xFF, t->par[i + 4] & 0xFF);
                i += 4; ok = 1;
            }
            if (ok) { if (p == 38) t->fg = col; else t->bg = col; }
        }
    }
}

static void set_mode(term_t* t, int on) {
    for (int i = 0; i < t->npar || i == 0; i++) {
        int p = P0(t, i);
        if (t->priv == '?') {
            switch (p) {
            case 1: t->app_cursor = on; break;
            case 6: t->origin = on; t->cx = 0; t->cy = on ? t->top : 0; break;
            case 7: t->autowrap = on; if (!on) t->wrap_pending = 0; break;
            case 25: t->cursor_on = on; break;
            case 47: case 1047: set_alt(t, on, 0); break;
            case 1048: if (on) save_cursor(t); else restore_cursor(t); break;
            case 1049: set_alt(t, on, 1); break;
            case 2004: t->bpaste = on; break;
            default: break;
            }
        } else {
            if (p == 4) t->insert = on;
            else if (p == 20) t->lnm = on;
        }
        if (t->npar == 0) break;
    }
}

static void do_csi(term_t* t, char f) {
    char buf[48];
    if (t->priv == '>' || t->priv == '=') {
        if (f == 'c') reply(t, "\033[>0;10;0c");       /* secondary DA: "VT100", fw 10 */
        return;
    }
    if (t->inter == '!' && f == 'p') {                  /* DECSTR soft reset */
        t->insert = 0; t->origin = 0; t->autowrap = 1; t->cursor_on = 1; t->app_cursor = 0;
        t->top = 0; t->bot = t->rows - 1; t->fg = t->bg = COL_DEF; t->attr = 0;
        return;
    }
    if (t->inter == ' ' && f == 'q') return;            /* DECSCUSR cursor style: ignored */
    if (t->inter) return;
    switch (f) {
    case '@': {                                          /* ICH */
        int n = imin(P(t, 0, 1), t->cols - t->cx);
        memmove(AT(t, t->cx + n, t->cy), AT(t, t->cx, t->cy), sizeof(cell_t) * (t->cols - t->cx - n));
        erase(t, t->cy, t->cx, t->cx + n);
        t->wrap_pending = 0;
        break;
    }
    case 'A': t->cy = t->cy >= t->top ? imax(t->top, t->cy - P(t, 0, 1)) : imax(0, t->cy - P(t, 0, 1));
              t->wrap_pending = 0; clamp_cursor(t); break;
    case 'B': case 'e': t->cy = t->cy <= t->bot ? imin(t->bot, t->cy + P(t, 0, 1)) : imin(t->rows - 1, t->cy + P(t, 0, 1));
              t->wrap_pending = 0; clamp_cursor(t); break;
    case 'C': case 'a': t->cx += P(t, 0, 1); t->wrap_pending = 0; clamp_cursor(t); break;
    case 'D': t->cx -= P(t, 0, 1); t->wrap_pending = 0; clamp_cursor(t); break;
    case 'E': t->cy += P(t, 0, 1); t->cx = 0; t->wrap_pending = 0; clamp_cursor(t); break;
    case 'F': t->cy -= P(t, 0, 1); t->cx = 0; t->wrap_pending = 0; clamp_cursor(t); break;
    case 'G': case '`': t->cx = P(t, 0, 1) - 1; t->wrap_pending = 0; clamp_cursor(t); break;
    case 'H': case 'f':
        t->cy = P(t, 0, 1) - 1 + (t->origin ? t->top : 0);
        t->cx = P(t, 1, 1) - 1;
        t->wrap_pending = 0; clamp_cursor(t);
        break;
    case 'I': for (int n = P(t, 0, 1); n > 0; n--) { do t->cx++; while (t->cx < t->cols - 1 && !t->tabs[t->cx]); } clamp_cursor(t); break;
    case 'Z': for (int n = P(t, 0, 1); n > 0; n--) { do t->cx--; while (t->cx > 0 && !t->tabs[t->cx]); } clamp_cursor(t); break;
    case 'J': {
        int m = P0(t, 0);
        if (m == 0) { erase(t, t->cy, t->cx, t->cols); for (int y = t->cy + 1; y < t->rows; y++) erase(t, y, 0, t->cols); }
        else if (m == 1) { for (int y = 0; y < t->cy; y++) erase(t, y, 0, t->cols); erase(t, t->cy, 0, t->cx + 1); }
        else if (m == 2) { for (int y = 0; y < t->rows; y++) erase(t, y, 0, t->cols); }
        else if (m == 3) { t->sb_count = 0; t->sb_head = 0; t->view = 0; t->drawn_valid = 0; }
        break;
    }
    case 'K': {
        int m = P0(t, 0);
        if (m == 0) erase(t, t->cy, t->cx, t->cols);
        else if (m == 1) erase(t, t->cy, 0, t->cx + 1);
        else erase(t, t->cy, 0, t->cols);
        break;
    }
    case 'L': if (t->cy >= t->top && t->cy <= t->bot) { scroll_region(t, t->cy, t->bot, -P(t, 0, 1)); t->cx = 0; } break;
    case 'M': if (t->cy >= t->top && t->cy <= t->bot) { scroll_region(t, t->cy, t->bot, P(t, 0, 1)); t->cx = 0; } break;
    case 'P': {                                          /* DCH */
        int n = imin(P(t, 0, 1), t->cols - t->cx);
        memmove(AT(t, t->cx, t->cy), AT(t, t->cx + n, t->cy), sizeof(cell_t) * (t->cols - t->cx - n));
        erase(t, t->cy, t->cols - n, t->cols);
        t->wrap_pending = 0;
        break;
    }
    case 'S': scroll_region(t, t->top, t->bot, P(t, 0, 1)); break;
    case 'T': if (t->npar <= 1) scroll_region(t, t->top, t->bot, -P(t, 0, 1)); break;
    case 'X': erase(t, t->cy, t->cx, t->cx + P(t, 0, 1)); t->wrap_pending = 0; break;
    case 'b': for (int n = imin(P(t, 0, 1), 4096); n > 0; n--) put_cp(t, t->last_cp ? t->last_cp : ' '); break;
    case 'c': if (P0(t, 0) == 0) reply(t, "\033[?1;2c"); break;   /* VT100 with AVO */
    case 'd': t->cy = P(t, 0, 1) - 1 + (t->origin ? t->top : 0); t->wrap_pending = 0; clamp_cursor(t); break;
    case 'g': if (P0(t, 0) == 0) t->tabs[t->cx] = 0; else if (P0(t, 0) == 3) memset(t->tabs, 0, (size_t)t->cols); break;
    case 'h': set_mode(t, 1); break;
    case 'l': set_mode(t, 0); break;
    case 'm': if (!t->priv) do_sgr(t); break;
    case 'n':
        if (P0(t, 0) == 5) reply(t, "\033[0n");
        else if (P0(t, 0) == 6) {
            char* p = buf;
            *p++ = '\033'; *p++ = '[';
            fmt_int(p, t->cy + 1 - (t->origin ? t->top : 0)); p += strlen(p);
            *p++ = ';';
            fmt_int(p, t->cx + 1); p += strlen(p);
            *p++ = 'R'; *p = 0;
            reply(t, buf);
        }
        break;
    case 'r': {
        if (t->priv) break;
        int top = P(t, 0, 1) - 1, bot = P(t, 1, t->rows) - 1;
        bot = imin(bot, t->rows - 1);
        if (top < bot) { t->top = top; t->bot = bot; t->cx = 0; t->cy = t->origin ? top : 0; t->wrap_pending = 0; }
        break;
    }
    case 's': if (!t->priv) save_cursor(t); break;
    case 'u': if (!t->priv) restore_cursor(t); break;
    default: break;
    }
}

static void do_osc(term_t* t) {
    t->osc[t->osc_len] = 0;
    int n = 0, i = 0;
    while (t->osc[i] >= '0' && t->osc[i] <= '9') { n = n * 10 + (t->osc[i] - '0'); i++; }
    if (t->osc[i] != ';') return;
    i++;
    if (n == 0 || n == 2) {
        int k = 0;
        for (; t->osc[i] && k < (int)sizeof(t->title) - 1; i++) {
            unsigned char c = (unsigned char)t->osc[i];
            if (c >= 0x20 && c != 0x7F) t->title[k++] = (char)c;
        }
        t->title[k] = 0;
        t->title_changed = 1;
    }
}

static void esc_dispatch(term_t* t, char c) {
    switch (c) {
    case '7': save_cursor(t); break;
    case '8': restore_cursor(t); break;
    case 'D': linefeed(t); break;
    case 'E': t->cx = 0; linefeed(t); break;
    case 'M': reverse_index(t); break;
    case 'H': t->tabs[t->cx] = 1; break;
    case '=': t->app_keypad = 1; break;
    case '>': t->app_keypad = 0; break;
    case 'c': {                                          /* RIS */
        reset_state(t);
        set_alt(t, 0, 0);
        fill_cells(t->main, t->cols * t->rows, blank(t));
        t->sb_count = t->sb_head = 0; t->view = 0;
        t->drawn_valid = 0;
        break;
    }
    default: break;
    }
}

static void control(term_t* t, uint8_t c) {
    switch (c) {
    case 7: break;                                       /* BEL */
    case 8: if (t->cx > 0) t->cx--; t->wrap_pending = 0; break;
    case 9: do t->cx++; while (t->cx < t->cols - 1 && !t->tabs[t->cx]); clamp_cursor(t); break;
    case 10: case 11: case 12:
        trace_char(t, '\n');
        if (t->lnm) t->cx = 0;
        linefeed(t);
        break;
    case 13: t->cx = 0; t->wrap_pending = 0; break;
    case 14: t->gl = 1; break;                           /* SO */
    case 15: t->gl = 0; break;                           /* SI */
    default: break;
    }
}

void term_feed(term_t* t, const uint8_t* buf, int n) {
    for (int i = 0; i < n; i++) {
        uint8_t c = buf[i];
        /* C0 controls act in every state except inside strings. */
        if (t->st != ST_OSC && t->st != ST_STR && t->st != ST_OSC_ESC && t->st != ST_STR_ESC) {
            if (c == 0x1B) { t->st = ST_ESC; t->u_need = 0; continue; }
            if (c == 0x18 || c == 0x1A) { t->st = ST_GROUND; continue; }
            if (c < 0x20 && c != 0x1B) { control(t, c); continue; }
            if (c == 0x7F) continue;
        }
        switch (t->st) {
        case ST_GROUND:
            if (t->u_need) {
                if ((c & 0xC0) == 0x80) {
                    t->u_cp = (t->u_cp << 6) | (c & 0x3F);
                    if (--t->u_need == 0) put_cp(t, t->u_cp);
                    continue;
                }
                t->u_need = 0;
                put_cp(t, 0xFFFD);
            }
            if (c < 0x80) put_cp(t, c);
            else if ((c & 0xE0) == 0xC0) { t->u_cp = c & 0x1F; t->u_need = 1; }
            else if ((c & 0xF0) == 0xE0) { t->u_cp = c & 0x0F; t->u_need = 2; }
            else if ((c & 0xF8) == 0xF0) { t->u_cp = c & 0x07; t->u_need = 3; }
            else put_cp(t, 0xFFFD);
            break;
        case ST_ESC:
            if (c == '[') { t->st = ST_CSI; t->npar = 0; t->par_has = 0; t->priv = 0; t->inter = 0; memset(t->par, 0, sizeof(t->par)); }
            else if (c == ']') { t->st = ST_OSC; t->osc_len = 0; }
            else if (c == 'P' || c == '_' || c == '^' || c == 'X') t->st = ST_STR;
            else if (c >= 0x20 && c <= 0x2F) { t->inter = (char)c; t->st = ST_ESC_INTER; }
            else { esc_dispatch(t, (char)c); t->st = ST_GROUND; }
            break;
        case ST_ESC_INTER: {
            int set = (c == '0') ? 1 : 0;
            if (t->inter == '(') t->g[0] = set;
            else if (t->inter == ')') t->g[1] = set;
            else if (t->inter == '*') t->g[2] = set;
            else if (t->inter == '+') t->g[3] = set;
            else if (t->inter == '#' && c == '8') {      /* DECALN */
                cell_t e = blank(t); e.cp = 'E';
                fill_cells(t->scr, t->cols * t->rows, e);
            }
            t->inter = 0;
            t->st = ST_GROUND;
            break;
        }
        case ST_CSI:
            if (c >= '0' && c <= '9') {
                if (t->npar == 0) t->npar = 1;
                if (t->par[t->npar - 1] < 100000) t->par[t->npar - 1] = t->par[t->npar - 1] * 10 + (c - '0');
            } else if (c == ';' || c == ':') {
                if (t->npar == 0) t->npar = 1;
                if (t->npar < NPAR) t->par[t->npar++] = 0;
            } else if (c == '?' || c == '>' || c == '=' || c == '<') {
                t->priv = (char)c;
            } else if (c >= 0x20 && c <= 0x2F) {
                t->inter = (char)c;
            } else if (c >= 0x40 && c <= 0x7E) {
                do_csi(t, (char)c);
                t->st = ST_GROUND;
            } else {
                t->st = ST_GROUND;
            }
            break;
        case ST_OSC:
            if (c == 7) { do_osc(t); t->st = ST_GROUND; }
            else if (c == 0x1B) t->st = ST_OSC_ESC;
            else if (t->osc_len < (int)sizeof(t->osc) - 1) t->osc[t->osc_len++] = (char)c;
            break;
        case ST_OSC_ESC:
            if (c == '\\') do_osc(t);
            t->st = ST_GROUND;
            break;
        case ST_STR:
            if (c == 0x1B) t->st = ST_STR_ESC;
            else if (c == 7) t->st = ST_GROUND;
            break;
        case ST_STR_ESC:
            t->st = (c == '\\') ? ST_GROUND : ST_STR;
            break;
        }
    }
}

/* ---- scrollback view ---------------------------------------------------- */
void term_scroll_view(term_t* t, int delta) {
    if (t->alt_on) return;
    int v = imax(0, imin(t->view + delta, t->sb_count));
    if (v != t->view) { t->view = v; t->drawn_valid = 0; }
}
void term_view_live(term_t* t) { if (t->view) { t->view = 0; t->drawn_valid = 0; } }

/* Cell shown at display row y (scrollback-aware). */
static const cell_t* shown(term_t* t, int x, int y) {
    if (t->view > 0 && !t->alt_on) {
        if (y < t->view) {
            int idx = (t->sb_head - t->view + y + TERM_SB_LINES * 2) % TERM_SB_LINES;
            return &t->sb[(long)idx * t->cols + x];
        }
        y -= t->view;
    }
    return &t->scr[(long)y * t->cols + x];
}

/* ---- rendering ---------------------------------------------------------- */
static void draw_cell(surf_t* s, int x, int y, const cell_t* c) {
    int bold = (c->attr & A_BOLD) != 0;
    uint32_t fg = resolve(c->fg, 1, bold), bg = resolve(c->bg, 0, 0);
    if (c->attr & A_REV) { uint32_t tmp = fg; fg = bg; bg = tmp; }
    if (c->attr & A_DIM) fg = gfx_mix(bg, fg, 150);
    if (c->attr & A_HIDDEN) fg = bg;
    if (c->attr & A_CURSOR) { fg = DEF_BG; bg = CURSOR_COL; }
    int px = TERM_PAD + x * GFX_FONT_W, py = TERM_PAD + y * GFX_FONT_H;
    gfx_glyph(s, px, py, c->cp ? c->cp : ' ', fg, bg, bold);
    if (c->attr & A_UL) gfx_fill(s, px, py + 13, GFX_FONT_W, 1, fg);
    if (c->attr & A_STRIKE) gfx_fill(s, px, py + 8, GFX_FONT_W, 1, fg);
    if (c->attr & A_CURSOR_HOLLOW) gfx_frame(s, px, py, GFX_FONT_W, GFX_FONT_H, CURSOR_COL);
}

static int cell_eq(const cell_t* a, const cell_t* b) {
    return a->cp == b->cp && a->fg == b->fg && a->bg == b->bg && a->attr == b->attr;
}

rect_t term_render(term_t* t, surf_t* s, int focused) {
    rect_t dmg = { 0, 0, 0, 0 };
    int full = !t->drawn_valid || focused != t->drawn_focus;
    if (full) {
        gfx_fill(s, 0, 0, term_px_w(t->cols), term_px_h(t->rows), DEF_BG);
        t->nops = 0;
    } else if (t->nops) {
        /* Replay the scrolls on the pixels and on the drawn-state grid. */
        for (int k = 0; k < t->nops; k++) {
            int top = t->ops[k].top, bot = t->ops[k].bot, n = t->ops[k].n;
            int h = bot - top + 1, an = n > 0 ? n : -n;
            if (an >= h) {
                for (int y = top; y <= bot; y++)
                    for (int x = 0; x < t->cols; x++) t->drawn[(long)y * t->cols + x].cp = 0xFFFFFFFF;
                continue;
            }
            int pw = t->cols * GFX_FONT_W, rowpx = GFX_FONT_H;
            if (n > 0) {
                for (int y = top; y + n <= bot; y++) {
                    memcpy(&t->drawn[(long)y * t->cols], &t->drawn[(long)(y + n) * t->cols], sizeof(cell_t) * t->cols);
                    for (int r = 0; r < rowpx; r++)
                        memcpy(gfx_at(s, TERM_PAD, TERM_PAD + y * rowpx + r),
                               gfx_at(s, TERM_PAD, TERM_PAD + (y + n) * rowpx + r), (size_t)pw * 4);
                }
                for (int y = bot - an + 1; y <= bot; y++)
                    for (int x = 0; x < t->cols; x++) t->drawn[(long)y * t->cols + x].cp = 0xFFFFFFFF;
            } else {
                for (int y = bot; y - an >= top; y--) {
                    memcpy(&t->drawn[(long)y * t->cols], &t->drawn[(long)(y - an) * t->cols], sizeof(cell_t) * t->cols);
                    for (int r = 0; r < rowpx; r++)
                        memcpy(gfx_at(s, TERM_PAD, TERM_PAD + y * rowpx + r),
                               gfx_at(s, TERM_PAD, TERM_PAD + (y - an) * rowpx + r), (size_t)pw * 4);
                }
                for (int y = top; y < top + an; y++)
                    for (int x = 0; x < t->cols; x++) t->drawn[(long)y * t->cols + x].cp = 0xFFFFFFFF;
            }
            rect_t r = { 0, TERM_PAD + top * GFX_FONT_H, term_px_w(t->cols), h * GFX_FONT_H };
            dmg = rect_union(dmg, r);
        }
        t->nops = 0;
    }
    int curx = -1, cury = -1;
    if (t->cursor_on && t->view == 0) {
        curx = t->cx; cury = t->cy;
    } else if (t->cursor_on && t->view > 0 && t->cy + t->view < t->rows) {
        curx = t->cx; cury = t->cy + t->view;
    }
    for (int y = 0; y < t->rows; y++) {
        int x0 = -1, x1 = -1;
        for (int x = 0; x < t->cols; x++) {
            cell_t c = *shown(t, x, y);
            if (x == curx && y == cury) c.attr |= focused ? A_CURSOR : A_CURSOR_HOLLOW;
            cell_t* d = &t->drawn[(long)y * t->cols + x];
            if (!full && cell_eq(&c, d)) continue;
            draw_cell(s, x, y, &c);
            *d = c;
            if (x0 < 0) x0 = x;
            x1 = x;
        }
        if (x0 >= 0) {
            rect_t r = { TERM_PAD + x0 * GFX_FONT_W, TERM_PAD + y * GFX_FONT_H, (x1 - x0 + 1) * GFX_FONT_W, GFX_FONT_H };
            dmg = rect_union(dmg, r);
        }
    }
    if (full) {
        rect_t all = { 0, 0, term_px_w(t->cols), term_px_h(t->rows) };
        dmg = all;
    }
    t->drawn_valid = 1;
    t->drawn_focus = focused;
    return dmg;
}
