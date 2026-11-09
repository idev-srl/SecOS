/* SecOS window manager / compositor (/bin/secwm, /bin/startgui).
 * SPDX-License-Identifier: MIT
 *
 * The only process holding /dev/fb0 and the input devices (signed with
 * CAP_DISPLAY | CAP_INPUT, keyboards and pointers held with EVIOCGRAB). It
 * draws the desktop, the panel, window decorations and the mouse cursor, and
 * hosts three kinds of windows:
 *   - terminals: a VT emulator (term.c) on a pty running the user's shell
 *     (from /etc/passwd, `-i`) as a session leader with the pty as its
 *     controlling terminal;
 *   - the clock, drawn by the compositor itself;
 *   - clients: separate signed programs (no GUI capabilities) speaking the
 *     SWP protocol (swp.h) over two pipes the compositor created for them.
 * Design, protocol and threat model: docs/GUI.md.
 *
 * [M43] secwm runs as the user who started it, never as root on someone's
 * behalf: /dev/fb0 (root:video 0660) and /dev/input/eventN (root:input 0660)
 * need the groups video and input on top of CAP_DISPLAY / CAP_INPUT, and the
 * terminals run that user's shell. The init hook `--auto` (started by root)
 * switches to the user named by "user=NAME" in /etc/gui-autostart first; with
 * no user there it starts as root only on a CONSOLE=root build.
 *
 *   secwm [--trace] [--no-term] [--run PROGRAM]... [--auto]
 *   Ctrl+Alt+T terminal, Ctrl+Alt+N next window, Ctrl+Alt+W close window,
 *   Ctrl+Alt+Backspace leave the GUI (back to the text console),
 *   Ctrl+Shift+Esc task manager (/bin/taskmgr).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <pty.h>
#include <termios.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include "libsecos.h"
#include <pwd.h>
#include <grp.h>
#include "gfx.h"
#include "term.h"
#include "swp.h"
#include "../../drivers/keymap.h"

/* ---- configuration ------------------------------------------------------ */
#define MAXWIN   12
#define MAXDEV   8
#define MAXDMG   32
#define PANEL_H  30
#define TITLE_H  26
#define BORDER   1
#define SHADOW   6
#define GRIP     14
#define CLIENT_READ_BUDGET (256 * 1024)
#define TERM_READ_BUDGET   (64 * 1024)

/* Palette of the "SecOS Night" theme. */
#define C_DESK_TOP   0x0B1622u
#define C_DESK_BOT   0x123A44u
#define C_PANEL_TOP  0x18202Bu
#define C_PANEL_BOT  0x0E141Bu
#define C_PANEL_LINE 0x2BB3A8u
#define C_ACCENT     0x2BB3A8u
#define C_ACCENT2    0xF0B429u
#define C_TEXT       0xE6EDF3u
#define C_TEXT_DIM   0x8A97A6u
#define C_BTN        0x243040u
#define C_BTN_HI     0x2E3D52u
#define C_TITLE_F0   0x23303Fu
#define C_TITLE_F1   0x1A2430u
#define C_TITLE_U0   0x1C232Cu
#define C_TITLE_U1   0x161B22u
#define C_BORDER_F   0x2BB3A8u
#define C_BORDER_U   0x313B47u
#define C_CLOSE      0xE05561u
#define C_CLIENT_BG  0x1A2028u

enum { WK_TERM = 1, WK_CLOCK, WK_CLIENT };

typedef struct client {
    int ev_fd, req_fd;              /* our ends: events out, requests in */
    int state;                      /* 0 want HELLO, 1 want CREATE, 2 running */
    uint8_t buf[8 + SWP_MAX_SMALL]; /* header + small payload being assembled */
    int have, need;
    int put_active;                 /* streaming PUT pixels */
    uint32_t px, py, pw, ph;
    uint64_t put_total, put_done;
    rect_t put_dmg;
    uint64_t born_ms, close_deadline;
    unsigned long dropped;
    int btn_grab;                   /* a press started inside the content */
} client_t;

typedef struct win {
    int used, id, kind;
    int x, y, ow, oh, cw, ch;       /* outer position/size, content size */
    uint32_t* buf;                  /* ow * oh: decorations + content */
    char title[64];
    char origin[48];                /* set by the compositor, never by the client */
    int pid;
    term_t* term; int master;
    client_t* cl; int visible;
    int dead;
} win_t;

typedef struct indev {
    int fd, kbd, ptr, abs;
    struct input_absinfo ax, ay;
    char name[64];
    int skip_to_syn;
} indev_t;

static uint32_t* fb; static size_t fb_len;
static int W, H, PITCH;
static uint32_t* back;              /* composed scene without the cursor */
static uint32_t* desk;              /* desktop wallpaper */
static uint32_t* panel;             /* W x PANEL_H */
static surf_t S_back, S_desk, S_panel;
static int con_fd = -1, fb_fd = -1;
static int trace, running = 1;
static int have_taskmgr;          /* [U5] /bin/taskmgr installed */
static volatile int sig_quit;

static indev_t dev[MAXDEV]; static int ndev;
static win_t wins[MAXWIN];
static int zord[MAXWIN], nz;        /* bottom .. top (indices into wins) */
static win_t* focus;
static int next_id = 1;

static rect_t dmg[MAXDMG]; static int ndmg;

/* pointer + keyboard state */
static int mx, my, cur_x = -100, cur_y = -100, buttons;
static int k_lshift, k_rshift, k_lctrl, k_rctrl, k_lalt, k_ralt, k_caps;
static enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE } drag;
static win_t* drag_win; static int drag_dx, drag_dy, drag_w0, drag_h0, drag_mx0, drag_my0;
static int pointer_moved;
static char layout[16] = "us";

/* panel hit regions */
#define MAXBTN 24
static struct { rect_t r; int action; win_t* w; } pbtn[MAXBTN]; static int npbtn;
enum { ACT_TERM = 1, ACT_CLOCK, ACT_SYSINFO, ACT_EXIT, ACT_TASK, ACT_TASKMGR };

static uint64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void logf_(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char* fmt, ...) {
    char b[300];
    va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    if (n > (int)sizeof(b) - 2) n = (int)sizeof(b) - 2;
    b[n++] = '\n';
    write(1, b, (size_t)n);         /* one write: one line on console + debugcon */
}

/* ---- damage ------------------------------------------------------------- */
static void damage(rect_t r) {
    rect_t scr = { 0, 0, W, H };
    r = rect_isect(r, scr);
    if (rect_empty(r)) return;
    for (int i = 0; i < ndmg; i++) {
        rect_t g = { dmg[i].x - 16, dmg[i].y - 16, dmg[i].w + 32, dmg[i].h + 32 };
        if (!rect_empty(rect_isect(g, r))) {
            dmg[i] = rect_union(dmg[i], r);
            return;
        }
    }
    if (ndmg == MAXDMG) {
        for (int i = 1; i < ndmg; i++) dmg[0] = rect_union(dmg[0], dmg[i]);
        ndmg = 1;
        dmg[0] = rect_union(dmg[0], r);
        return;
    }
    dmg[ndmg++] = r;
}
static void damage_all(void) { rect_t r = { 0, 0, W, H }; ndmg = 0; damage(r); }
static rect_t win_rect(const win_t* w) { rect_t r = { w->x, w->y, w->ow, w->oh }; return r; }
static rect_t win_rect_shadow(const win_t* w) { rect_t r = { w->x, w->y, w->ow + SHADOW, w->oh + SHADOW }; return r; }
static rect_t content_rect(const win_t* w) { rect_t r = { w->x + BORDER, w->y + TITLE_H, w->cw, w->ch }; return r; }
static surf_t content_surf(win_t* w) {
    surf_t s = { w->buf + (long)TITLE_H * w->ow + BORDER, w->cw, w->ch, w->ow };
    return s;
}

/* ---- cursor ------------------------------------------------------------- */
static const char* const arrow[19] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.........X ", "X......XXXXX", "X...X..X    ", "X..X X..X   ", "X.X  X..X   ",
    "XX    X..X  ", "X     X..X  ", "       X..X ", "        XX  ",
};
#define CUR_W 12
#define CUR_H 19
static rect_t cursor_rect(int x, int y) { rect_t r = { x, y, CUR_W, CUR_H }; return r; }

/* ---- compositing -------------------------------------------------------- */
static void compose(rect_t r) {
    for (int y = r.y; y < r.y + r.h; y++)
        memcpy(back + (long)y * W + r.x, desk + (long)y * W + r.x, (size_t)r.w * 4);
    for (int i = 0; i < nz; i++) {
        win_t* w = &wins[zord[i]];
        if (!w->visible) continue;
        rect_t sh = { w->x + SHADOW, w->y + SHADOW, w->ow, w->oh };
        rect_t si = rect_isect(sh, r);
        if (!rect_empty(si)) gfx_darken(&S_back, si.x, si.y, si.w, si.h, 90);
        rect_t wi = rect_isect(win_rect(w), r);
        if (rect_empty(wi)) continue;
        for (int y = wi.y; y < wi.y + wi.h; y++)
            memcpy(back + (long)y * W + wi.x, w->buf + (long)(y - w->y) * w->ow + (wi.x - w->x), (size_t)wi.w * 4);
    }
    rect_t pr = { 0, 0, W, PANEL_H };
    rect_t pi = rect_isect(pr, r);
    for (int y = pi.y; y < pi.y + pi.h; y++)
        memcpy(back + (long)y * W + pi.x, panel + (long)y * W + pi.x, (size_t)pi.w * 4);
}

static void present(rect_t r) {
    for (int y = r.y; y < r.y + r.h; y++)
        memcpy(fb + (long)y * PITCH + r.x, back + (long)y * W + r.x, (size_t)r.w * 4);
    rect_t c = rect_isect(cursor_rect(cur_x, cur_y), r);
    for (int y = c.y; y < c.y + c.h; y++)
        for (int x = c.x; x < c.x + c.w; x++) {
            char a = arrow[y - cur_y][x - cur_x];
            if (a == 'X') fb[(long)y * PITCH + x] = 0x0A0F14;
            else if (a == '.') fb[(long)y * PITCH + x] = 0xF4F8FB;
        }
}

static void flush_damage(void) {
    for (int i = 0; i < ndmg; i++) { compose(dmg[i]); present(dmg[i]); }
    ndmg = 0;
}

static void move_cursor(void) {
    if (mx == cur_x && my == cur_y) return;
    rect_t o = cursor_rect(cur_x, cur_y);
    cur_x = mx; cur_y = my;
    rect_t scr = { 0, 0, W, H };
    o = rect_isect(o, scr);
    if (!rect_empty(o)) present(o);       /* restore from the composed scene */
    rect_t n = rect_isect(cursor_rect(cur_x, cur_y), scr);
    if (!rect_empty(n)) present(n);
}

/* ---- desktop + panel ---------------------------------------------------- */
static void draw_shield(surf_t* s, int cx, int top, int w, int h, uint32_t c, int alpha) {
    for (int j = 0; j < h; j++) {
        int hw;
        if (j < h * 55 / 100) hw = w / 2;
        else hw = (int)((long)(w / 2) * (h - j) / (h - h * 55 / 100));
        int y = top + j;
        if (y < 0 || y >= s->h) continue;
        for (int x = cx - hw; x < cx + hw; x++) {
            if (x < 0 || x >= s->w) continue;
            uint32_t* p = gfx_at(s, x, y);
            *p = gfx_mix(*p, c, alpha);
        }
    }
}

static void paint_desktop(void) {
    surf_t* s = &S_desk;
    for (int y = 0; y < H; y++) {
        uint32_t* row = gfx_at(s, 0, y);
        for (int x = 0; x < W; x++) {
            /* Diagonal gradient + a dotted grid, the SecOS "circuit" texture. */
            int t = (int)(((long)y * 200 / H) + ((long)x * 55 / W));
            uint32_t c = gfx_mix(C_DESK_TOP, C_DESK_BOT, t > 255 ? 255 : t);
            if ((x % 24) == 12 && (y % 24) == 12) c = gfx_mix(c, 0x3FD8CC, 60);
            row[x] = c;
        }
    }
    /* Faint emblem: shield with a keyhole, bottom right. */
    int ex = W - 170, ey = H - 250;
    draw_shield(s, ex, ey, 180, 210, 0x2BB3A8, 34);
    draw_shield(s, ex, ey + 12, 150, 180, 0x0B1622, 40);
    gfx_disc(s, ex, ey + 80, 18, gfx_mix(*gfx_at(s, ex, ey + 80), 0x2BB3A8, 70));
    gfx_fill(s, ex - 6, ey + 92, 12, 40, gfx_mix(*gfx_at(s, ex, ey + 100), 0x2BB3A8, 70));
    const char* word = "SecOS";
    int wx = ex - gfx_text_width(word) * 2 / 2;
    /* 2x scaled wordmark */
    for (int i = 0; word[i]; i++) {
        surf_t tmp; uint32_t tb[16 * 8]; tmp.px = tb; tmp.w = 8; tmp.h = 16; tmp.stride = 8;
        for (int k = 0; k < 128; k++) tb[k] = 0;
        gfx_glyph(&tmp, 0, 0, (uint32_t)word[i], 1, 0, 1);
        for (int gy = 0; gy < 16; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (tb[gy * 8 + gx]) {
                    int px = wx + i * 16 + gx * 2, py = ey + 222 + gy * 2;
                    for (int a = 0; a < 2; a++)
                        for (int b = 0; b < 2; b++)
                            if (px + a < W && py + b < H) {
                                uint32_t* p = gfx_at(s, px + a, py + b);
                                *p = gfx_mix(*p, 0x9FE8E1, 90);
                            }
                }
    }
    const char* hint = "Ctrl+Alt+T terminal   Ctrl+Alt+N next window   Ctrl+Alt+Backspace leave the GUI   Ctrl+Shift+Esc task manager";
    gfx_text(s, 16, H - 24, hint, 0x6F8796, 0, 0);
}

static int panel_button(surf_t* s, int x, const char* label, int action, win_t* w, int hi) {
    int bw = gfx_text_width(label) + 20;
    if (x + bw > W - 260) return x;
    gfx_rrect(s, x, 4, bw, PANEL_H - 8, 5, hi ? C_BTN_HI : C_BTN);
    if (hi) gfx_fill(s, x + 6, PANEL_H - 6, bw - 12, 2, C_ACCENT);
    gfx_text(s, x + 10, 7, label, hi ? C_TEXT : 0xC5D0DB, hi, 0);
    if (npbtn < MAXBTN) {
        rect_t r = { x, 4, bw, PANEL_H - 8 };
        pbtn[npbtn].r = r; pbtn[npbtn].action = action; pbtn[npbtn].w = w; npbtn++;
    }
    return x + bw + 6;
}

static void paint_panel(void) {
    surf_t* s = &S_panel;
    npbtn = 0;
    gfx_vgrad(s, 0, 0, W, PANEL_H, C_PANEL_TOP, C_PANEL_BOT);
    gfx_fill(s, 0, PANEL_H - 1, W, 1, gfx_mix(C_PANEL_BOT, C_PANEL_LINE, 120));
    /* brand: small shield + name */
    draw_shield(s, 18, 6, 16, 18, C_ACCENT, 255);
    gfx_disc(s, 18, 12, 2, C_PANEL_BOT);
    gfx_fill(s, 17, 13, 2, 5, C_PANEL_BOT);
    gfx_text(s, 32, 7, "SecOS", C_TEXT, 1, 0);
    int x = 86;
    gfx_fill(s, x - 6, 7, 1, PANEL_H - 14, 0x34404E);
    x = panel_button(s, x, "\xE2\x96\xB6 Terminal", ACT_TERM, 0, 0);
    x = panel_button(s, x, "\xE2\x97\x8B Clock", ACT_CLOCK, 0, 0);
    x = panel_button(s, x, "\xE2\x96\xA0 System", ACT_SYSINFO, 0, 0);
    if (have_taskmgr) x = panel_button(s, x, "\xE2\x96\x86 Task Manager", ACT_TASKMGR, 0, 0);
    gfx_fill(s, x, 7, 1, PANEL_H - 14, 0x34404E);
    x += 8;
    for (int i = 0; i < nz; i++) {
        win_t* w = &wins[zord[i]];
        if (!w->visible) continue;
        char lab[24];
        snprintf(lab, sizeof(lab), "%.18s", w->title);
        x = panel_button(s, x, lab, ACT_TASK, w, w == focus);
    }
    /* right: clock + exit */
    time_t now = time(0);
    struct tm tmv; gmtime_r(&now, &tmv);
    char clk[48];
    strftime(clk, sizeof(clk), "%a %d %b  %H:%M:%S UTC", &tmv);
    int ew = gfx_text_width("\xC3\x97 Exit") + 20;
    int ex = W - ew - 6;
    int cx = ex - gfx_text_width(clk) - 14;
    gfx_text(s, cx, 7, clk, 0xC5D0DB, 0, 0);
    gfx_rrect(s, ex, 4, ew, PANEL_H - 8, 5, 0x3A2630);
    gfx_text(s, ex + 10, 7, "\xC3\x97 Exit", 0xFFB3BA, 0, 0);
    if (npbtn < MAXBTN) {
        rect_t r = { ex, 4, ew, PANEL_H - 8 };
        pbtn[npbtn].r = r; pbtn[npbtn].action = ACT_EXIT; pbtn[npbtn].w = 0; npbtn++;
    }
    rect_t pr = { 0, 0, W, PANEL_H };
    damage(pr);
    static int logged;
    if (trace && !logged) {
        logged = 1;
        static const char* const names[] = { "", "Terminal", "Clock", "System", "Exit", "", "TaskManager" };
        for (int i = 0; i < npbtn; i++)
            if ((pbtn[i].action >= ACT_TERM && pbtn[i].action <= ACT_EXIT) || pbtn[i].action == ACT_TASKMGR)
                logf_("SW-PANEL %s %d,%d,%d,%d", names[pbtn[i].action], pbtn[i].r.x, pbtn[i].r.y, pbtn[i].r.w, pbtn[i].r.h);
    }
}

/* ---- windows ------------------------------------------------------------ */
static void paint_decor(win_t* w) {
    surf_t s = { w->buf, w->ow, w->oh, w->ow };
    int f = (w == focus);
    gfx_vgrad(&s, 0, 0, w->ow, TITLE_H, f ? C_TITLE_F0 : C_TITLE_U0, f ? C_TITLE_F1 : C_TITLE_U1);
    uint32_t bc = f ? C_BORDER_F : C_BORDER_U;
    gfx_frame(&s, 0, 0, w->ow, w->oh, bc);
    gfx_fill(&s, 0, TITLE_H - 1, w->ow, 1, f ? gfx_mix(C_TITLE_F1, C_ACCENT, 90) : C_BORDER_U);
    if (f) gfx_fill(&s, 1, 1, 4, TITLE_H - 2, C_ACCENT);
    /* origin badge (compositor-verified), then the title */
    int x = 12;
    uint32_t badge = w->kind == WK_CLIENT ? C_ACCENT2 : (f ? C_ACCENT : C_TEXT_DIM);
    int bw = gfx_text_width(w->origin) + 10;
    gfx_rrect(&s, x, 5, bw, TITLE_H - 10, 4, gfx_mix(f ? C_TITLE_F1 : C_TITLE_U1, badge, 60));
    gfx_text(&s, x + 5, 5, w->origin, badge, 0, 0);
    x += bw + 8;
    int room = w->ow - x - 34;
    if (room > 0) gfx_text(&s, x, 5, w->title, f ? C_TEXT : C_TEXT_DIM, f, room);
    /* close button */
    int cx = w->ow - 24, cy = 4;
    gfx_rrect(&s, cx, cy, 18, 18, 4, f ? C_CLOSE : 0x3A4350);
    for (int i = 0; i < 8; i++) {
        uint32_t c = 0xFFFFFF;
        *gfx_at(&s, cx + 5 + i, cy + 5 + i) = c; *gfx_at(&s, cx + 6 + i, cy + 5 + i) = c;
        *gfx_at(&s, cx + 12 - i, cy + 5 + i) = c; *gfx_at(&s, cx + 11 - i, cy + 5 + i) = c;
    }
    /* bottom border row */
    gfx_fill(&s, 0, w->oh - 1, w->ow, 1, bc);
}

static void paint_grip(win_t* w) {
    if (w->kind != WK_TERM) return;
    surf_t s = { w->buf, w->ow, w->oh, w->ow };
    uint32_t c = (w == focus) ? C_ACCENT : 0x4A5563;
    for (int i = 0; i < 3; i++)
        for (int k = 0; k <= i; k++)
            gfx_fill(&s, w->ow - 4 - (2 - k) * 3 - 1, w->oh - 4 - (i - k) * 3 - 1 - (2 - i) * 0, 2, 2, c);
}

static win_t* win_alloc(int kind, int cw, int ch) {
    win_t* w = 0;
    for (int i = 0; i < MAXWIN; i++) if (!wins[i].used) { w = &wins[i]; break; }
    if (!w) return 0;
    memset(w, 0, sizeof(*w));
    w->used = 1; w->id = next_id++; w->kind = kind;
    w->cw = cw; w->ch = ch;
    w->ow = cw + 2 * BORDER; w->oh = ch + TITLE_H + BORDER;
    w->buf = malloc((size_t)w->ow * w->oh * 4);
    if (!w->buf) { w->used = 0; return 0; }
    w->master = -1; w->pid = -1;
    int k = (w->id - 1) % 8;
    w->x = 40 + k * 34; w->y = PANEL_H + 18 + k * 30;
    if (w->x + w->ow > W) w->x = W - w->ow > 0 ? W - w->ow : 0;
    if (w->y + w->oh > H) w->y = H - w->oh > PANEL_H ? H - w->oh : PANEL_H;
    return w;
}

static int win_index(win_t* w) { return (int)(w - wins); }

static void z_remove(win_t* w) {
    int idx = win_index(w), j = 0;
    for (int i = 0; i < nz; i++) if (zord[i] != idx) zord[j++] = zord[i];
    nz = j;
}

static void send_ev(client_t* c, uint32_t type, const void* p, uint32_t len);

static void set_focus(win_t* w) {
    if (w == focus) return;
    win_t* old = focus;
    focus = w;
    if (old && old->used) {
        paint_decor(old); paint_grip(old);
        if (old->term) term_invalidate(old->term);
        if (old->cl) { struct swp_focus f = { 0 }; send_ev(old->cl, SWP_EV_FOCUS, &f, sizeof(f)); }
        damage(win_rect(old));
    }
    if (w) {
        paint_decor(w); paint_grip(w);
        if (w->term) term_invalidate(w->term);
        if (w->cl) { struct swp_focus f = { 1 }; send_ev(w->cl, SWP_EV_FOCUS, &f, sizeof(f)); }
        damage(win_rect(w));
        if (trace) logf_("SW-FOCUS %d", w->id);
    }
    paint_panel();
}

static void raise_win(win_t* w) {
    z_remove(w);
    zord[nz++] = win_index(w);
    damage(win_rect_shadow(w));
}

static void show_win(win_t* w) {
    w->visible = 1;
    zord[nz++] = win_index(w);
    paint_decor(w);
    set_focus(w);
    paint_decor(w); paint_grip(w);
    damage(win_rect_shadow(w));
    paint_panel();
}

static void win_destroy(win_t* w) {
    if (!w->used) return;
    damage(win_rect_shadow(w));
    if (w->visible) z_remove(w);
    if (w->term) { term_free(w->term); w->term = 0; }
    if (w->master >= 0) { close(w->master); w->master = -1; }
    if (w->pid > 0) kill(w->pid, w->kind == WK_CLIENT ? SIGKILL : SIGHUP);
    if (w->cl) {
        close(w->cl->ev_fd); close(w->cl->req_fd);
        free(w->cl); w->cl = 0;
    }
    free(w->buf);
    if (trace) logf_("SW-CLOSE %d", w->id);
    w->used = 0; w->visible = 0;
    if (focus == w) {
        focus = 0;
        if (nz) set_focus(&wins[zord[nz - 1]]);
    }
    paint_panel();
}

/* ---- child processes ---------------------------------------------------- */
static char** child_env(void) {
    static char* env[40];
    static char term_s[] = "TERM=xterm";
    int n = 0;
    env[n++] = term_s;
    for (int i = 0; environ && environ[i] && n < 38; i++) {
        if (!strncmp(environ[i], "TERM=", 5) || !strncmp(environ[i], "COLUMNS=", 8) ||
            !strncmp(environ[i], "LINES=", 6)) continue;
        env[n++] = environ[i];
    }
    env[n] = 0;
    return env;
}

/* In a freshly forked child: default signal dispositions, empty mask, and
 * every fd above `keep` closed (the compositor's fb0, input devices, ptys and
 * client pipes are also FD_CLOEXEC; this is the second line). */
static void child_sanitize(int keep) {
    for (int s = 1; s < 32; s++) if (s != SIGKILL && s != 19) signal(s, SIG_DFL);
    unsigned long none = 0;
    sigprocmask(2 /* SIG_SETMASK */, &none, 0);
    for (int fd = keep + 1; fd < 32; fd++) close(fd);
}

static void trace_line(void* ctx, const char* line) {
    win_t* w = ctx;
    if (trace) logf_("SW-TTY %d: %s", w->id, line);
}

static void term_set_winsize(win_t* w) {
    struct winsize ws;
    ws.ws_col = (unsigned short)term_cols(w->term); ws.ws_row = (unsigned short)term_rows(w->term);
    ws.ws_xpixel = (unsigned short)w->cw; ws.ws_ypixel = (unsigned short)w->ch;
    ioctl(w->master, TIOCSWINSZ, &ws);
}

static win_t* open_terminal(void) {
    int cols = 80, rows = 24;
    win_t* w = win_alloc(WK_TERM, term_px_w(cols), term_px_h(rows));
    if (!w) return 0;
    w->term = term_new(cols, rows);
    if (!w->term) { free(w->buf); w->used = 0; return 0; }
    term_set_line_hook(w->term, trace_line, w);
    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, (unsigned short)w->cw, (unsigned short)w->ch };
    int m, s;
    if (openpty(&m, &s, 0, 0, &ws) < 0) {
        logf_("secwm: openpty: %s", strerror(errno));
        term_free(w->term); free(w->buf); w->used = 0; return 0;
    }
    fcntl(m, F_SETFD, FD_CLOEXEC);
    fcntl(s, F_SETFD, FD_CLOEXEC);
    /* Lock the slave again now that we hold it open: nobody else can open
     * /dev/pts/N by name (read the user's keystrokes, write into the window).
     * bash and its children use the inherited fds or /dev/tty, which the
     * kernel resolves to the controlling terminal without the lock check. */
    int lk = 1;
    ioctl(m, TIOCSPTLCK, &lk);
    char pts[32] = "pty";
    unsigned ptn = 0;
    if (ioctl(m, TIOCGPTN, &ptn) == 0) snprintf(pts, sizeof(pts), "pts/%u", ptn);
    int pid = fork();
    if (pid < 0) {
        logf_("secwm: fork: %s", strerror(errno));
        close(m); close(s); term_free(w->term); free(w->buf); w->used = 0; return 0;
    }
    if (pid == 0) {
        setsid();
        ioctl(s, TIOCSCTTY, (void*)0);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        child_sanitize(2);
        const char* home = getenv("HOME");
        if (!home || chdir(home) != 0) chdir("/");
        /* The user's own shell (/etc/passwd), as that user: secwm runs with
         * the ids of whoever started it. */
        const struct passwd* pw = getpwuid(getuid());
        static char sh[128];
        snprintf(sh, sizeof(sh), "%s", pw && pw->pw_shell && pw->pw_shell[0] == '/' ? pw->pw_shell : "/bin/bash");
        const char* base = strrchr(sh, '/');
        char* argv[] = { (char*)(base ? base + 1 : sh), "-i", 0 };
        execve(sh, argv, child_env());
        argv[0] = "bash";
        execve("/bin/bash", argv, child_env());
        execve("/bin/sh", argv, child_env());
        _exit(127);
    }
    close(s);
    int one = 1;
    ioctl(m, FIONBIO, &one);
    w->master = m; w->pid = pid;
    snprintf(w->origin, sizeof(w->origin), "%s", pts);
    snprintf(w->title, sizeof(w->title), "bash");
    surf_t cs = content_surf(w);
    term_render(w->term, &cs, 1);
    show_win(w);
    logf_("SW-TERM-OPEN %d pid=%d %s at %d,%d", w->id, pid, pts, w->x, w->y);
    return w;
}

static void clock_paint(win_t* w);
static win_t* open_clock(void) {
    for (int i = 0; i < MAXWIN; i++)
        if (wins[i].used && wins[i].kind == WK_CLOCK) { raise_win(&wins[i]); set_focus(&wins[i]); return &wins[i]; }
    win_t* w = win_alloc(WK_CLOCK, 200, 228);
    if (!w) return 0;
    w->x = W - w->ow - 40; w->y = PANEL_H + 30;
    snprintf(w->origin, sizeof(w->origin), "secwm");
    snprintf(w->title, sizeof(w->title), "Clock");
    clock_paint(w);
    show_win(w);
    if (trace) logf_("SW-CLOCK-OPEN %d", w->id);
    return w;
}

static win_t* spawn_client(const char* cmd) {
    /* "PATH [ARG]..." split on spaces (launcher entries and --run). */
    static char line[128];
    char* argv[8]; int argc = 0;
    snprintf(line, sizeof(line), "%s", cmd);
    for (char* p = line; *p && argc < 7;) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    argv[argc] = 0;
    if (argc == 0) return 0;
    const char* path = argv[0];
    if (access(path, 0) != 0) { logf_("secwm: %s: not found", path); return 0; }
    int ev[2], rq[2];
    if (pipe2(ev, O_CLOEXEC) < 0) return 0;
    if (pipe2(rq, O_CLOEXEC) < 0) { close(ev[0]); close(ev[1]); return 0; }
    if (ev[0] <= SWP_FD_REQ || rq[1] <= SWP_FD_REQ) {   /* fds 3/4 are ours: never true in practice */
        close(ev[0]); close(ev[1]); close(rq[0]); close(rq[1]); return 0;
    }
    fcntl(ev[1], F_SETFL, O_NONBLOCK);
    fcntl(rq[0], F_SETFL, O_NONBLOCK);
    win_t* w = 0;
    for (int i = 0; i < MAXWIN; i++) if (!wins[i].used) { w = &wins[i]; break; }
    client_t* c = malloc(sizeof(client_t));
    if (!w || !c) { free(c); close(ev[0]); close(ev[1]); close(rq[0]); close(rq[1]); return 0; }
    int pid = fork();
    if (pid < 0) { free(c); close(ev[0]); close(ev[1]); close(rq[0]); close(rq[1]); return 0; }
    if (pid == 0) {
        setsid();                         /* no shared process group with the compositor */
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) dup2(nul, 0);
        dup2(ev[0], SWP_FD_EV);
        dup2(rq[1], SWP_FD_REQ);
        child_sanitize(SWP_FD_REQ);
        const char* base = strrchr(path, '/');
        argv[0] = (char*)(base ? base + 1 : path);
        execve(path, argv, child_env());
        _exit(127);
    }
    close(ev[0]); close(rq[1]);
    /* The window slot stays unused (and invisible) until CREATE arrives. */
    memset(w, 0, sizeof(*w));
    memset(c, 0, sizeof(*c));
    w->used = 1; w->id = next_id++; w->kind = WK_CLIENT; w->pid = pid; w->master = -1;
    c->ev_fd = ev[1]; c->req_fd = rq[0];
    c->born_ms = now_ms();
    w->cl = c;
    const char* base = strrchr(path, '/');
    snprintf(w->origin, sizeof(w->origin), "%s pid %d", base ? base + 1 : path, pid);
    logf_("SW-CLIENT %d pid=%d %s", w->id, pid, path);
    return w;
}

/* The System launcher: the LVGL monitor when installed, else the plain one. */
static void open_system(void) {
    spawn_client(access("/bin/sysmon", 0) == 0 ? "/bin/sysmon" : "/bin/sysinfo");
}

/* [U5] The task manager (panel button, Ctrl+Shift+Esc): one instance. A second
 * request raises the open one, as on Windows. It learns the largest window the
 * screen takes from --max (CREATE larger than that is a protocol violation). */
static void open_taskmgr(void) {
    if (!have_taskmgr) return;
    for (int i = 0; i < MAXWIN; i++) {
        win_t* w = &wins[i];
        if (!w->used || w->dead || w->kind != WK_CLIENT || strncmp(w->origin, "taskmgr ", 8) != 0) continue;
        if (w->visible) { raise_win(w); set_focus(w); }
        return;                           /* starting: its window comes by itself */
    }
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "/bin/taskmgr --max %dx%d%s", W - 2 * BORDER, H - PANEL_H - TITLE_H - BORDER,
             trace ? " --trace" : "");
    spawn_client(cmd);
}

/* ---- SWP: client protocol (all input untrusted) ------------------------- */
static void send_ev(client_t* c, uint32_t type, const void* p, uint32_t len) {
    uint8_t b[8 + 64];
    if (len > 64) return;
    struct swp_hdr h = { type, len };
    memcpy(b, &h, 8);
    if (len) memcpy(b + 8, p, len);
    /* <= PIPE_BUF: the write is atomic, all or EAGAIN. Never block on a client. */
    if (write(c->ev_fd, b, 8 + len) != (ssize_t)(8 + len)) c->dropped++;
}

static void violation(win_t* w, const char* why) {
    logf_("SW-VIOLATION client %d pid=%d: %s - disconnected", w->id, w->pid, why);
    w->dead = 1;
}

static void sanitize_title(char* dst, int dmax, const char* src, int smax) {
    int k = 0;
    for (int i = 0; i < smax && src[i] && k < dmax - 1; i++) {
        unsigned char ch = (unsigned char)src[i];
        dst[k++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
    }
    dst[k] = 0;
}

static void client_request(win_t* w, uint32_t type, const uint8_t* p, uint32_t len) {
    client_t* c = w->cl;
    if (c->state == 0) {
        struct swp_hello hl;
        if (type != SWP_REQ_HELLO || len != sizeof(hl)) { violation(w, "first message is not HELLO"); return; }
        memcpy(&hl, p, sizeof(hl));
        if (hl.magic != SWP_MAGIC || hl.version != SWP_VERSION) { violation(w, "bad HELLO magic/version"); return; }
        c->state = 1;
        return;
    }
    switch (type) {
    case SWP_REQ_CREATE: {
        struct swp_create cr;
        if (c->state != 1 || len != sizeof(cr)) { violation(w, "CREATE out of order or wrong size"); return; }
        memcpy(&cr, p, sizeof(cr));
        if (cr.w < SWP_MIN_W || cr.h < SWP_MIN_H || cr.w > SWP_MAX_W || cr.h > SWP_MAX_H ||
            cr.w + 2 * BORDER > (uint32_t)W || cr.h + TITLE_H + BORDER > (uint32_t)(H - PANEL_H)) {
            violation(w, "CREATE size out of range"); return;
        }
        w->cw = (int)cr.w; w->ch = (int)cr.h;
        w->ow = w->cw + 2 * BORDER; w->oh = w->ch + TITLE_H + BORDER;
        w->buf = malloc((size_t)w->ow * w->oh * 4);
        if (!w->buf) { violation(w, "out of memory"); return; }
        surf_t cs = content_surf(w);
        gfx_fill(&cs, 0, 0, w->cw, w->ch, C_CLIENT_BG);
        sanitize_title(w->title, sizeof(w->title), cr.title, SWP_TITLE);
        int k = (w->id - 1) % 6;
        w->x = 120 + k * 40; w->y = PANEL_H + 60 + k * 30;
        if (w->x + w->ow > W) w->x = W - w->ow;
        if (w->y + w->oh > H) w->y = H - w->oh;
        if (w->y < PANEL_H) w->y = PANEL_H;
        c->state = 2;
        struct swp_configure cf = { cr.w, cr.h };
        send_ev(c, SWP_EV_CONFIGURE, &cf, sizeof(cf));
        show_win(w);
        if (trace) logf_("SW-CLIENT-WINDOW %d %ux%u at %d,%d", w->id, cr.w, cr.h, w->x, w->y);
        return;
    }
    case SWP_REQ_FILL: {
        struct swp_fill f;
        if (c->state != 2 || len != sizeof(f)) { violation(w, "FILL before CREATE or wrong size"); return; }
        memcpy(&f, p, sizeof(f));
        if (f.x >= (uint32_t)w->cw || f.y >= (uint32_t)w->ch || f.w == 0 || f.h == 0 ||
            f.w > (uint32_t)w->cw - f.x || f.h > (uint32_t)w->ch - f.y) { violation(w, "FILL outside the window"); return; }
        surf_t cs = content_surf(w);
        gfx_fill(&cs, (int)f.x, (int)f.y, (int)f.w, (int)f.h, f.color & 0xFFFFFF);
        rect_t r = { w->x + BORDER + (int)f.x, w->y + TITLE_H + (int)f.y, (int)f.w, (int)f.h };
        damage(r);
        return;
    }
    case SWP_REQ_TITLE: {
        if (c->state != 2 || len != SWP_TITLE) { violation(w, "TITLE before CREATE or wrong size"); return; }
        sanitize_title(w->title, sizeof(w->title), (const char*)p, SWP_TITLE);
        paint_decor(w);
        damage(win_rect(w));
        paint_panel();
        return;
    }
    default:
        violation(w, "unknown request type");
        return;
    }
}

/* Start a PUT once its 16-byte rectangle is in: validate, then stream pixels. */
static void put_begin(win_t* w, const uint8_t* p, uint32_t len) {
    client_t* c = w->cl;
    struct swp_put pu;
    memcpy(&pu, p, sizeof(pu));
    if (c->state != 2) { violation(w, "PUT before CREATE"); return; }
    if (pu.x >= (uint32_t)w->cw || pu.y >= (uint32_t)w->ch || pu.w == 0 || pu.h == 0 ||
        pu.w > (uint32_t)w->cw - pu.x || pu.h > (uint32_t)w->ch - pu.y) { violation(w, "PUT outside the window"); return; }
    uint64_t want = (uint64_t)sizeof(pu) + (uint64_t)pu.w * pu.h * 4;
    if ((uint64_t)len != want) { violation(w, "PUT length does not match its rectangle"); return; }
    c->put_active = 1;
    c->px = pu.x; c->py = pu.y; c->pw = pu.w; c->ph = pu.h;
    c->put_total = (uint64_t)pu.w * pu.h * 4; c->put_done = 0;
    rect_t z = { 0, 0, 0, 0 }; c->put_dmg = z;
}

static void put_bytes(win_t* w, const uint8_t* p, int n) {
    client_t* c = w->cl;
    while (n > 0 && c->put_done < c->put_total) {
        uint64_t pix = c->put_done / 4, byte = c->put_done % 4;
        uint32_t row = (uint32_t)(pix / c->pw), col = (uint32_t)(pix % c->pw);
        uint64_t room = ((uint64_t)(c->pw - col)) * 4 - byte;     /* rest of this row */
        int k = (int)(room < (uint64_t)n ? room : (uint64_t)n);
        uint8_t* dst = (uint8_t*)(w->buf + (long)(TITLE_H + c->py + row) * w->ow + BORDER + c->px + col) + byte;
        memcpy(dst, p, (size_t)k);
        rect_t r = { w->x + BORDER + (int)c->px, w->y + TITLE_H + (int)(c->py + row), (int)c->pw, 1 };
        c->put_dmg = rect_union(c->put_dmg, r);
        c->put_done += (uint64_t)k; p += k; n -= k;
    }
    if (c->put_done == c->put_total) {
        c->put_active = 0;
        if (w->visible) damage(c->put_dmg);
    }
}

static void client_read(win_t* w) {
    client_t* c = w->cl;
    uint8_t buf[16384];
    int budget = CLIENT_READ_BUDGET;
    while (budget > 0 && !w->dead) {
        ssize_t n = read(c->req_fd, buf, sizeof(buf));
        if (n == 0) { w->dead = 1; if (trace) logf_("SW-CLIENT-EOF %d", w->id); return; }
        if (n < 0) return;                /* EAGAIN */
        budget -= (int)n;
        const uint8_t* p = buf;
        while (n > 0 && !w->dead) {
            if (c->put_active) {
                uint64_t left = c->put_total - c->put_done;
                int k = (int)(left < (uint64_t)n ? left : (uint64_t)n);
                put_bytes(w, p, k);
                if (w->visible && c->put_active) { damage(c->put_dmg); rect_t z = { 0, 0, 0, 0 }; c->put_dmg = z; }
                p += k; n -= k;
                continue;
            }
            /* 1. the 8-byte header, checked as soon as it is complete */
            if (c->have < 8) {
                int k = 8 - c->have;
                if (k > n) k = (int)n;
                memcpy(c->buf + c->have, p, (size_t)k);
                c->have += k; p += k; n -= k;
                if (c->have < 8) continue;
                struct swp_hdr h; memcpy(&h, c->buf, 8);
                if (h.type < SWP_REQ_HELLO || h.type > SWP_REQ_TITLE) { violation(w, "unknown request type"); return; }
                if (h.type == SWP_REQ_PUT) {
                    if (h.len < sizeof(struct swp_put) || h.len > 16u + (uint32_t)SWP_MAX_W * SWP_MAX_H * 4) {
                        violation(w, "PUT length out of range"); return;
                    }
                    c->need = 8 + (int)sizeof(struct swp_put);
                } else {
                    if (h.len > SWP_MAX_SMALL) { violation(w, "request too large"); return; }
                    c->need = 8 + (int)h.len;
                }
            }
            /* 2. the payload (for PUT only its rectangle; the pixels stream) */
            if (c->have < c->need) {
                int k = c->need - c->have;
                if (k > n) k = (int)n;
                memcpy(c->buf + c->have, p, (size_t)k);
                c->have += k; p += k; n -= k;
                if (c->have < c->need) continue;
            }
            struct swp_hdr h; memcpy(&h, c->buf, 8);
            c->have = 0;
            if (h.type == SWP_REQ_PUT) put_begin(w, c->buf + 8, h.len);
            else client_request(w, h.type, c->buf + 8, h.len);
        }
    }
}

/* ---- clock window ------------------------------------------------------- */
#include <math.h>
static void clock_paint(win_t* w) {
    surf_t s = content_surf(w);
    gfx_vgrad(&s, 0, 0, w->cw, w->ch, 0x16202B, 0x0F151C);
    int cx = w->cw / 2, cy = 98, R = 84;
    gfx_disc(&s, cx, cy, R, 0x1E2A37);
    gfx_disc(&s, cx, cy, R - 3, 0x121A23);
    for (int i = 0; i < 60; i++) {
        double a = i * 3.14159265358979 / 30.0;
        int r0 = (i % 5 == 0) ? R - 14 : R - 8;
        int x0 = cx + (int)(sin(a) * r0), y0 = cy - (int)(cos(a) * r0);
        int x1 = cx + (int)(sin(a) * (R - 5)), y1 = cy - (int)(cos(a) * (R - 5));
        gfx_line(&s, x0, y0, x1, y1, (i % 5 == 0) ? C_ACCENT : 0x3B4A5A);
    }
    time_t now = time(0);
    struct tm tmv; gmtime_r(&now, &tmv);
    double hs = ((tmv.tm_hour % 12) + tmv.tm_min / 60.0) * 3.14159265358979 / 6.0;
    double ms = (tmv.tm_min + tmv.tm_sec / 60.0) * 3.14159265358979 / 30.0;
    double ss = tmv.tm_sec * 3.14159265358979 / 30.0;
    for (int d = -1; d <= 1; d++) {
        gfx_line(&s, cx + d, cy, cx + d + (int)(sin(hs) * (R - 40)), cy - (int)(cos(hs) * (R - 40)), C_TEXT);
        gfx_line(&s, cx, cy + d, cx + (int)(sin(ms) * (R - 22)), cy + d - (int)(cos(ms) * (R - 22)), C_TEXT);
    }
    gfx_line(&s, cx, cy, cx + (int)(sin(ss) * (R - 14)), cy - (int)(cos(ss) * (R - 14)), C_ACCENT2);
    gfx_disc(&s, cx, cy, 4, C_ACCENT2);
    char t[32];
    strftime(t, sizeof(t), "%H:%M:%S", &tmv);
    gfx_text(&s, cx - gfx_text_width(t) / 2, 190, t, C_TEXT, 1, 0);
    strftime(t, sizeof(t), "%Y-%m-%d UTC", &tmv);
    gfx_text(&s, cx - gfx_text_width(t) / 2, 207, t, C_TEXT_DIM, 0, 0);
    if (w->visible) damage(content_rect(w));
}

/* ---- terminals: pty output + resize ------------------------------------- */
static void term_update(win_t* w) {
    surf_t cs = content_surf(w);
    rect_t d = term_render(w->term, &cs, w == focus);
    if (!rect_empty(d)) {
        paint_grip(w);
        rect_t r = { w->x + BORDER + d.x, w->y + TITLE_H + d.y, d.w, d.h };
        damage(r);
        rect_t g = { w->x + w->ow - GRIP, w->y + w->oh - GRIP, GRIP, GRIP };
        if (!rect_empty(rect_isect(g, r))) damage(g);
    }
    if (term_title_changed(w->term)) {
        snprintf(w->title, sizeof(w->title), "%s", term_title(w->term));
        paint_decor(w);
        damage(win_rect(w));
        paint_panel();
    }
}

static void term_read(win_t* w) {
    uint8_t buf[4096];
    int budget = TERM_READ_BUDGET;
    while (budget > 0) {
        ssize_t n = read(w->master, buf, sizeof(buf));
        if (n > 0) {
            term_feed(w->term, buf, (int)n);
            budget -= (int)n;
            char rep[128];
            int k = term_take_reply(w->term, rep, sizeof(rep));
            if (k > 0) write(w->master, rep, (size_t)k);
            continue;
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) w->dead = 1;   /* EIO: bash is gone */
        break;
    }
    if (w->visible) term_update(w);
}

static void win_resize_term(win_t* w, int cw, int ch) {
    int cols = (cw - 2 * TERM_PAD) / GFX_FONT_W, rows = (ch - 2 * TERM_PAD) / GFX_FONT_H;
    if (cols < 20) cols = 20;
    if (rows < 5) rows = 5;
    if (cols == term_cols(w->term) && rows == term_rows(w->term)) return;
    int ncw = term_px_w(cols), nch = term_px_h(rows);
    int now_ = ncw + 2 * BORDER, noh = nch + TITLE_H + BORDER;
    if (w->x + now_ > W || w->y + noh > H) return;
    uint32_t* nb = malloc((size_t)now_ * noh * 4);
    if (!nb) return;
    if (term_resize(w->term, cols, rows) < 0) { free(nb); return; }
    damage(win_rect_shadow(w));
    free(w->buf);
    w->buf = nb; w->cw = ncw; w->ch = nch; w->ow = now_; w->oh = noh;
    paint_decor(w);
    term_invalidate(w->term);
    term_update(w);
    paint_grip(w);
    damage(win_rect_shadow(w));
    term_set_winsize(w);
    if (trace) logf_("SW-RESIZE %d %dx%d", w->id, cols, rows);
}

/* ---- keyboard ----------------------------------------------------------- */
static void term_send(win_t* w, const char* s, int n) {
    if (w->master >= 0 && n > 0) write(w->master, s, (size_t)n);
}

static int special_key(win_t* w, int code, int shift, int ctrl, int alt, char* out) {
    const char* s = 0;
    int app = w->term && term_app_cursor(w->term);
    int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    char arrowc = 0;
    switch (code) {
    case KEY_ENTER: case KEY_KPENTER: s = "\r"; break;
    case KEY_BACKSPACE: s = ctrl ? "\b" : "\177"; break;
    case KEY_TAB: s = shift ? "\033[Z" : "\t"; break;
    case KEY_ESC: s = "\033"; break;
    case KEY_UP: arrowc = 'A'; break;
    case KEY_DOWN: arrowc = 'B'; break;
    case KEY_RIGHT: arrowc = 'C'; break;
    case KEY_LEFT: arrowc = 'D'; break;
    case KEY_HOME: arrowc = 'H'; break;
    case KEY_END: arrowc = 'F'; break;
    case KEY_INSERT: s = "\033[2~"; break;
    case KEY_DELETE: s = "\033[3~"; break;
    case KEY_PAGEUP: s = "\033[5~"; break;
    case KEY_PAGEDOWN: s = "\033[6~"; break;
    case KEY_F1: s = "\033OP"; break;
    case KEY_F2: s = "\033OQ"; break;
    case KEY_F3: s = "\033OR"; break;
    case KEY_F4: s = "\033OS"; break;
    case KEY_F5: s = "\033[15~"; break;
    case KEY_F6: s = "\033[17~"; break;
    case KEY_F7: s = "\033[18~"; break;
    case KEY_F8: s = "\033[19~"; break;
    case KEY_F9: s = "\033[20~"; break;
    case KEY_F10: s = "\033[21~"; break;
    case KEY_F11: s = "\033[23~"; break;
    case KEY_F12: s = "\033[24~"; break;
    case KEY_KPSLASH: s = "/"; break;
    case KEY_KPASTERISK: s = "*"; break;
    case KEY_KPMINUS: s = "-"; break;
    case KEY_KPPLUS: s = "+"; break;
    case KEY_KPDOT: s = "."; break;
    case KEY_KP0: s = "0"; break;
    case KEY_KP1: s = "1"; break;
    case KEY_KP2: s = "2"; break;
    case KEY_KP3: s = "3"; break;
    case KEY_KP4: s = "4"; break;
    case KEY_KP5: s = "5"; break;
    case KEY_KP6: s = "6"; break;
    case KEY_KP7: s = "7"; break;
    case KEY_KP8: s = "8"; break;
    case KEY_KP9: s = "9"; break;
    default: return -1;
    }
    if (arrowc) {
        if (mod > 1) return snprintf(out, 16, "\033[1;%d%c", mod, arrowc);
        if (app) return snprintf(out, 16, "\033O%c", arrowc);
        return snprintf(out, 16, "\033[%c", arrowc);
    }
    int n = (int)strlen(s);
    memcpy(out, s, (size_t)n);
    return n;
}

static uint32_t mods_now(void) {
    const keymap_t* km = keymap_active();
    uint32_t m = 0;
    if (k_lshift || k_rshift) m |= SWP_MOD_SHIFT;
    if (k_lctrl || k_rctrl) m |= SWP_MOD_CTRL;
    if (k_lalt || (k_ralt && !km->has_altgr)) m |= SWP_MOD_ALT;
    if (k_ralt && km->has_altgr) m |= SWP_MOD_ALTGR;
    return m;
}

static void quit_gui(void) { running = 0; }

static void focus_next(void) {
    if (nz < 2) return;
    win_t* w = &wins[zord[0]];
    raise_win(w);
    set_focus(w);
    paint_panel();
}

static void close_win_request(win_t* w) {
    if (w->kind == WK_CLIENT && w->cl && w->cl->state == 2) {
        send_ev(w->cl, SWP_EV_CLOSE, 0, 0);
        w->cl->close_deadline = now_ms() + 2000;
        w->visible = 0;                /* hide now, kill if it does not exit */
        z_remove(w);
        damage(win_rect_shadow(w));
        if (focus == w) { focus = 0; if (nz) set_focus(&wins[zord[nz - 1]]); }
        paint_panel();
        return;
    }
    win_destroy(w);
}

static void key_event(int code, int value) {
    int down = value != 0;
    switch (code) {
    case KEY_LEFTSHIFT: k_lshift = down; break;
    case KEY_RIGHTSHIFT: k_rshift = down; break;
    case KEY_LEFTCTRL: k_lctrl = down; break;
    case KEY_RIGHTCTRL: k_rctrl = down; break;
    case KEY_LEFTALT: k_lalt = down; break;
    case KEY_RIGHTALT: k_ralt = down; break;
    case KEY_CAPSLOCK: if (value == 1) k_caps = !k_caps; break;
    default: break;
    }
    int shift = k_lshift || k_rshift, ctrl = k_lctrl || k_rctrl;
    const keymap_t* km = keymap_active();
    int altgr = k_ralt && km->has_altgr;
    int alt = k_lalt || (k_ralt && !km->has_altgr);
    /* Compositor shortcuts: Ctrl + LEFT Alt (Windows' AltGr is LeftCtrl +
     * RightAlt, so AltGr combinations never trigger them). Never forwarded. */
    if (ctrl && k_lalt && value == 1) {
        switch (code) {
        case KEY_BACKSPACE: quit_gui(); return;
        case KEY_T: open_terminal(); return;
        case KEY_N: focus_next(); return;
        case KEY_W: if (focus) close_win_request(focus); return;
        case KEY_C: open_clock(); return;
        case KEY_S: open_system(); return;
        default: break;
        }
    }
    if (ctrl && k_lalt) return;
    if (ctrl && shift && !k_lalt && code == KEY_ESC) { if (value == 1) open_taskmgr(); return; }   /* [U5] */
    if (!focus) return;
    if (focus->kind == WK_CLIENT && focus->cl && focus->cl->state == 2) {
        struct swp_key k;
        k.code = (uint32_t)code; k.value = value; k.mods = mods_now();
        k.cp = (code < KEYMAP_KEYS) ? keymap_lookup(code, shift, altgr, k_caps) : 0;
        send_ev(focus->cl, SWP_EV_KEY, &k, sizeof(k));
        return;
    }
    if (focus->kind != WK_TERM || value == 0) return;
    win_t* w = focus;
    if (shift && (code == KEY_PAGEUP || code == KEY_PAGEDOWN)) {
        term_scroll_view(w->term, code == KEY_PAGEUP ? term_rows(w->term) / 2 : -(term_rows(w->term) / 2));
        term_update(w);
        return;
    }
    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT || code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL ||
        code == KEY_LEFTALT || code == KEY_RIGHTALT || code == KEY_CAPSLOCK || code == KEY_LEFTMETA ||
        code == KEY_RIGHTMETA || code == KEY_NUMLOCK || code == KEY_SCROLLLOCK) return;
    char out[32];
    int n = special_key(w, code, shift, ctrl, alt, out);
    if (n < 0) {
        uint32_t cp = (code < KEYMAP_KEYS) ? keymap_lookup(code, shift, altgr, k_caps) : 0;
        if (!cp) return;
        n = 0;
        if (alt && !altgr) out[n++] = '\033';
        if (ctrl && !altgr) {
            uint32_t base = keymap_lookup(code, 0, 0, 0);
            uint32_t ch = base;
            if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 1;
            else if (ch == ' ' || ch == '2' || ch == '@') ch = 0;
            else if (ch == '[' || ch == '3') ch = 27;
            else if (ch == '\\' || ch == '4') ch = 28;
            else if (ch == ']' || ch == '5') ch = 29;
            else if (ch == '^' || ch == '6') ch = 30;
            else if (ch == '_' || ch == '7' || ch == '/' || ch == '-') ch = 31;
            else if (ch == '8' || ch == '?') ch = 127;
            else ch = 0xFFFFFFFF;
            if (ch != 0xFFFFFFFF) { out[n++] = (char)ch; term_send(w, out, n); term_view_live(w->term); return; }
        }
        n += keymap_utf8(cp, out + n);
    } else if (alt && n == 1) {
        out[1] = out[0]; out[0] = '\033'; n = 2;
    }
    term_view_live(w->term);
    term_send(w, out, n);
}

/* ---- pointer ------------------------------------------------------------ */
static win_t* win_at(int x, int y) {
    for (int i = nz - 1; i >= 0; i--) {
        win_t* w = &wins[zord[i]];
        if (w->visible && rect_contains(win_rect(w), x, y)) return w;
    }
    return 0;
}

static void client_pointer(win_t* w, int type, int button, int value) {
    client_t* c = w->cl;
    if (!c || c->state != 2) return;
    rect_t cr = content_rect(w);
    int inside = rect_contains(cr, mx, my);
    int lx = mx - cr.x, ly = my - cr.y;
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    if (lx >= w->cw) lx = w->cw - 1;
    if (ly >= w->ch) ly = w->ch - 1;
    if (type == SWP_EV_POINTER) {
        if (!inside && !c->btn_grab) return;
        struct swp_pointer p = { lx, ly, (uint32_t)buttons };
        send_ev(c, SWP_EV_POINTER, &p, sizeof(p));
    } else {
        struct swp_button b = { lx, ly, (uint32_t)button, value };
        send_ev(c, SWP_EV_BUTTON, &b, sizeof(b));
    }
}

static void button_event(int btn, int value) {
    int bit = btn == BTN_LEFT ? 1 : btn == BTN_RIGHT ? 2 : btn == BTN_MIDDLE ? 4 : 0;
    if (!bit) return;
    if (value) buttons |= bit; else buttons &= ~bit;
    if (btn == BTN_LEFT && !value && drag != DRAG_NONE) {
        if (trace && drag == DRAG_MOVE && drag_win && drag_win->used)
            logf_("SW-MOVED %d %d,%d", drag_win->id, drag_win->x, drag_win->y);
        drag = DRAG_NONE; drag_win = 0;
        return;
    }
    if (!value) {
        if (focus && focus->kind == WK_CLIENT && focus->cl && focus->cl->btn_grab) {
            client_pointer(focus, SWP_EV_BUTTON, btn, 0);
            if (!buttons) focus->cl->btn_grab = 0;
        }
        return;
    }
    if (my < PANEL_H) {
        if (btn != BTN_LEFT) return;
        for (int i = 0; i < npbtn; i++) {
            if (!rect_contains(pbtn[i].r, mx, my)) continue;
            switch (pbtn[i].action) {
            case ACT_TERM: open_terminal(); break;
            case ACT_CLOCK: open_clock(); break;
            case ACT_SYSINFO: open_system(); break;
            case ACT_TASKMGR: open_taskmgr(); break;
            case ACT_EXIT: quit_gui(); break;
            case ACT_TASK:
                if (pbtn[i].w && pbtn[i].w->used && pbtn[i].w->visible) { raise_win(pbtn[i].w); set_focus(pbtn[i].w); }
                break;
            }
            return;
        }
        return;
    }
    win_t* w = win_at(mx, my);
    if (!w) return;
    if (w != &wins[zord[nz - 1]]) raise_win(w);
    set_focus(w);
    int lx = mx - w->x, ly = my - w->y;
    if (ly < TITLE_H) {
        if (btn != BTN_LEFT) return;
        if (lx >= w->ow - 26 && lx < w->ow - 4 && ly >= 3 && ly < 24) { close_win_request(w); return; }
        drag = DRAG_MOVE; drag_win = w; drag_dx = lx; drag_dy = ly;
        return;
    }
    if (w->kind == WK_TERM && btn == BTN_LEFT && lx >= w->ow - GRIP && ly >= w->oh - GRIP) {
        drag = DRAG_RESIZE; drag_win = w; drag_w0 = w->cw; drag_h0 = w->ch; drag_mx0 = mx; drag_my0 = my;
        return;
    }
    if (w->kind == WK_CLIENT && rect_contains(content_rect(w), mx, my)) {
        w->cl->btn_grab = 1;
        client_pointer(w, SWP_EV_BUTTON, btn, 1);
    }
}

static void wheel_event(int v) {
    win_t* w = win_at(mx, my);
    if (!w || w->kind != WK_TERM) return;
    if (w != focus) return;
    term_scroll_view(w->term, v * 3);
    term_update(w);
}

static void pointer_settle(void) {
    if (!pointer_moved) return;
    pointer_moved = 0;
    if (drag == DRAG_MOVE && drag_win && drag_win->used) {
        win_t* w = drag_win;
        int nx = mx - drag_dx, ny = my - drag_dy;
        if (ny < PANEL_H) ny = PANEL_H;
        if (ny > H - TITLE_H) ny = H - TITLE_H;
        if (nx < -w->ow + 60) nx = -w->ow + 60;
        if (nx > W - 60) nx = W - 60;
        if (nx != w->x || ny != w->y) {
            damage(win_rect_shadow(w));
            w->x = nx; w->y = ny;
            damage(win_rect_shadow(w));
        }
    } else if (drag == DRAG_RESIZE && drag_win && drag_win->used) {
        win_resize_term(drag_win, drag_w0 + (mx - drag_mx0), drag_h0 + (my - drag_my0));
    } else if (focus && focus->kind == WK_CLIENT) {
        client_pointer(focus, SWP_EV_POINTER, 0, 0);
    }
}

/* ---- input devices ------------------------------------------------------ */
#define NBITS(x) ((((x) - 1) / (8 * sizeof(long))) + 1)
static int test_bit(int bit, const unsigned long* a) { return (a[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1; }

static void open_inputs(void) {
    for (int i = 0; i < 8 && ndev < MAXDEV; i++) {
        char p[32]; snprintf(p, sizeof(p), "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        indev_t* d = &dev[ndev];
        memset(d, 0, sizeof(*d));
        d->fd = fd;
        unsigned long key[NBITS(KEY_CNT)] = { 0 }, absb[NBITS(ABS_CNT)] = { 0 }, relb[NBITS(REL_CNT)] = { 0 };
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absb)), absb);
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relb)), relb);
        ioctl(fd, EVIOCGNAME(sizeof(d->name)), d->name);
        d->kbd = test_bit(KEY_A, key);
        d->ptr = test_bit(BTN_LEFT, key) && (test_bit(REL_X, relb) || test_bit(ABS_X, absb));
        if (test_bit(ABS_X, absb)) {
            d->abs = 1;
            ioctl(fd, EVIOCGABS(ABS_X), &d->ax);
            ioctl(fd, EVIOCGABS(ABS_Y), &d->ay);
        }
        if (!d->kbd && !d->ptr) { close(fd); continue; }
        if (ioctl(fd, EVIOCGRAB, (void*)1) != 0)
            logf_("secwm: warning: %s (%s) is grabbed by someone else", p, d->name);
        if (trace) logf_("SW-INPUT %s \"%s\"%s%s%s", p, d->name, d->kbd ? " kbd" : "", d->ptr ? " ptr" : "", d->abs ? " abs" : "");
        ndev++;
    }
}

static void read_input(indev_t* d) {
    struct input_event ev[64];
    for (int round = 0; round < 8; round++) {
        ssize_t n = read(d->fd, ev, sizeof(ev));
        if (n <= 0) return;
        for (int k = 0; k < (int)(n / (ssize_t)sizeof(ev[0])); k++) {
            struct input_event* e = &ev[k];
            if (e->type == EV_SYN && e->code == SYN_DROPPED) { d->skip_to_syn = 1; continue; }
            if (d->skip_to_syn) { if (e->type == EV_SYN && e->code == SYN_REPORT) d->skip_to_syn = 0; continue; }
            if (e->type == EV_KEY) {
                if (e->code == BTN_LEFT || e->code == BTN_RIGHT || e->code == BTN_MIDDLE) {
                    /* value 2 = "already down" (a USB report repeating the
                     * button state): not a new click. */
                    if (e->value != 2) button_event(e->code, e->value);
                }
                else if (e->code < 256) key_event(e->code, e->value);
            } else if (e->type == EV_REL) {
                if (e->code == REL_X) { mx += e->value; pointer_moved = 1; }
                else if (e->code == REL_Y) { my += e->value; pointer_moved = 1; }
                else if (e->code == REL_WHEEL) wheel_event(e->value);
            } else if (e->type == EV_ABS && d->abs) {
                struct input_absinfo* a = e->code == ABS_X ? &d->ax : &d->ay;
                int span = a->maximum - a->minimum; if (span <= 0) span = 1;
                int lim = e->code == ABS_X ? W : H;
                int v = (int)((long)(e->value - a->minimum) * (lim - 1) / span);
                if (e->code == ABS_X) mx = v; else if (e->code == ABS_Y) my = v;
                pointer_moved = 1;
            }
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx >= W) mx = W - 1;
            if (my >= H) my = H - 1;
            if (e->type == EV_SYN) pointer_settle();
        }
    }
}

/* ---- main loop ---------------------------------------------------------- */
static void reap_children(void) {
    for (;;) {
        int st = 0;
        long pid = secos_syscall(55 /* SYS_WAITANY */, (long)&st, 1 /* WNOHANG */, 0, 0, 0);
        if (pid <= 0) break;
        for (int i = 0; i < MAXWIN; i++) {
            win_t* w = &wins[i];
            if (!w->used || w->pid != pid) continue;
            w->pid = -1;
            if (w->kind == WK_CLIENT) {
                if (trace) logf_("SW-CLIENT-EXIT %d status=0x%x", w->id, st);
                w->dead = 1;
            }
        }
    }
}

static void on_signal(int s) { (void)s; sig_quit = 1; }

static void refresh_layout(void) {
    char cur[16] = { 0 };
    if (ioctl(con_fd, KDGKBLAYOUT, cur) == 0 && cur[0] && strcmp(cur, layout) != 0) {
        if (keymap_select(cur) == 0) {
            snprintf(layout, sizeof(layout), "%s", cur);
            if (trace) logf_("SW-LAYOUT %s", layout);
        }
    }
}

static void cleanup(void) {
    for (int i = 0; i < MAXWIN; i++) if (wins[i].used) win_destroy(&wins[i]);
    if (con_fd >= 0) ioctl(con_fd, KDSETMODE, (void*)KD_TEXT);
    for (int i = 0; i < ndev; i++) { ioctl(dev[i].fd, EVIOCGRAB, (void*)0); close(dev[i].fd); }
    if (fb) munmap(fb, fb_len);
    if (fb_fd >= 0) close(fb_fd);
}

/* `--auto` (the init hook): start only when the GUI-at-boot setting is on.
 * /etc/gui-autostart holds the setting ("1"/"yes"/"on" or "0"/"no"/"off"; an
 * empty file means on). Without the file the build default decides: the
 * kernel puts it on /proc/cmdline (secos.gui_autostart=1, make GUI_AUTOSTART=1). */
static int auto_enabled(void) {
    char b[256];
    int fd = open("/etc/gui-autostart", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        int n = (int)read(fd, b, sizeof(b) - 1);
        close(fd);
        if (n < 0) n = 0;
        b[n] = 0;
        const char* p = b;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == '0' || *p == 'n' || *p == 'N' || *p == 'f' || *p == 'F' ||
            ((p[0] == 'o' || p[0] == 'O') && (p[1] == 'f' || p[1] == 'F'))) return 0;
        return 1;
    }
    fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int n = (int)read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    return strstr(b, "secos.gui_autostart=1") != NULL;
}

static int cmdline_has(const char* what) {
    char b[512];
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int n = (int)read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    return strstr(b, what) != NULL;
}

/* [M43] `--auto` started by root (init): become the user named by "user=NAME"
 * in /etc/gui-autostart, the console included (it is the session's terminal).
 * 0 = go on (as that user, or as root on a CONSOLE=root build), -1 = stop. */
static int auto_become_user(void) {
    if (geteuid() != 0) return 0;
    char b[256], name[40] = "";
    int fd = open("/etc/gui-autostart", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        int n = (int)read(fd, b, sizeof(b) - 1);
        close(fd);
        if (n < 0) n = 0;
        b[n] = 0;
        char* u = strstr(b, "user=");
        if (u) {
            u += 5;
            int k = 0;
            while (u[k] && u[k] != '\n' && u[k] != ' ' && u[k] != '\r' && k < (int)sizeof(name) - 1) { name[k] = u[k]; k++; }
            name[k] = 0;
        }
    }
    if (!name[0]) {
        if (cmdline_has("secos.console=root")) return 0;          /* the old behaviour: root's desktop */
        printf("secwm: --auto: no user=NAME in /etc/gui-autostart; not starting the desktop as root\n");
        return -1;
    }
    const struct passwd* pw = getpwnam(name);
    if (!pw) { printf("secwm: --auto: unknown user %s\n", name); return -1; }
    int uid = pw->pw_uid, gid = pw->pw_gid;
    static char home[160], shell[160];
    snprintf(home, sizeof(home), "%s", pw->pw_dir && pw->pw_dir[0] ? pw->pw_dir : "/");
    snprintf(shell, sizeof(shell), "%s", pw->pw_shell && pw->pw_shell[0] ? pw->pw_shell : "/bin/bash");
    chown("/dev/console", (unsigned)uid, 5 /* tty */);
    chmod("/dev/console", 0620);
    if (initgroups(name, gid) != 0 || setresgid(gid, gid, gid) != 0 || setresuid(uid, uid, uid) != 0 ||
        getuid() != uid || geteuid() != uid) {
        printf("secwm: --auto: cannot switch to %s\n", name);
        return -1;
    }
    setenv("HOME", home, 1);
    setenv("USER", name, 1);
    setenv("LOGNAME", name, 1);
    setenv("SHELL", shell, 1);
    setenv("PATH", uid == 0 ? "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" : "/usr/local/bin:/usr/bin:/bin", 1);
    if (chdir(home) != 0) chdir("/");
    return 0;
}

int main(int argc, char** argv) {
    int want_term = 1;
    const char* runs[8]; int nruns = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) trace = 1;
        else if (!strcmp(argv[i], "--no-term")) want_term = 0;
        else if (!strcmp(argv[i], "--run") && i + 1 < argc) { if (nruns < 8) runs[nruns++] = argv[++i]; else i++; }
        else if (!strcmp(argv[i], "--auto")) { if (!auto_enabled()) return 0; if (auto_become_user() != 0) return 1; }
        else {
            printf("usage: %s [--trace] [--no-term] [--run PROGRAM]... [--auto]\n", argv[0]);
            return 2;
        }
    }
    fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb_fd < 0) { printf("secwm: /dev/fb0: %s (needs CAP_DISPLAY and a framebuffer)\n", strerror(errno)); return 1; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) || ioctl(fb_fd, FBIOGET_FSCREENINFO, &f) || v.bits_per_pixel != 32) {
        printf("secwm: unsupported framebuffer\n"); return 1;
    }
    W = (int)v.xres; H = (int)v.yres; PITCH = (int)(f.line_length / 4);
    fb_len = f.smem_len;
    fb = secos_mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb == MAP_FAILED) { printf("secwm: mmap fb0: %s\n", strerror(errno)); return 1; }
    back = mmap(NULL, (size_t)W * H * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    desk = mmap(NULL, (size_t)W * H * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    panel = malloc((size_t)W * PANEL_H * 4);
    if (!back || back == MAP_FAILED || !desk || desk == MAP_FAILED || !panel) { printf("secwm: out of memory\n"); return 1; }
    S_back.px = back; S_back.w = W; S_back.h = H; S_back.stride = W;
    S_desk.px = desk; S_desk.w = W; S_desk.h = H; S_desk.stride = W;
    S_panel.px = panel; S_panel.w = W; S_panel.h = PANEL_H; S_panel.stride = W;

    con_fd = open("/dev/console", O_RDWR | O_CLOEXEC);
    if (con_fd < 0) con_fd = 0;
    open_inputs();
    if (ndev == 0) { printf("secwm: no input devices (needs CAP_INPUT)\n"); return 1; }
    refresh_layout();
    have_taskmgr = access("/bin/taskmgr", 0) == 0;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    if (ioctl(con_fd, KDSETMODE, (void*)KD_GRAPHICS) != 0) {
        printf("secwm: KD_GRAPHICS refused: %s\n", strerror(errno));
        cleanup();
        return 1;
    }
    paint_desktop();
    mx = W / 2; my = H / 2;
    paint_panel();
    damage_all();
    flush_damage();
    move_cursor();
    logf_("SW-READY %dx%d devices=%d layout=%s", W, H, ndev, layout);

    if (want_term) open_terminal();
    for (int i = 0; i < nruns; i++) spawn_client(runs[i]);

    time_t last_sec = time(0);
    uint64_t last_layout = now_ms();
    while (running && !sig_quit) {
        struct pollfd pf[MAXDEV + MAXWIN];
        win_t* pw[MAXDEV + MAXWIN];
        int np = 0;
        for (int i = 0; i < ndev; i++) { pf[np].fd = dev[i].fd; pf[np].events = POLLIN; pf[np].revents = 0; pw[np] = 0; np++; }
        for (int i = 0; i < MAXWIN; i++) {
            win_t* w = &wins[i];
            if (!w->used || w->dead) continue;
            int fd = w->kind == WK_TERM ? w->master : (w->kind == WK_CLIENT ? w->cl->req_fd : -1);
            if (fd < 0) continue;
            pf[np].fd = fd; pf[np].events = POLLIN; pf[np].revents = 0; pw[np] = w; np++;
        }
        int timeout = 1000 - (int)(now_ms() % 1000) + 5;
        for (int i = 0; i < MAXWIN; i++)
            if (wins[i].used && wins[i].kind == WK_CLIENT && wins[i].cl->state != 2) timeout = 200;
        poll(pf, (nfds_t)np, timeout);
        for (int i = 0; i < np; i++) {
            if (!pf[i].revents) continue;
            if (!pw[i]) { read_input(&dev[i]); continue; }
            win_t* w = pw[i];
            if (!w->used || w->dead) continue;
            if (w->kind == WK_TERM) term_read(w);
            else client_read(w);
        }
        pointer_settle();
        reap_children();
        uint64_t t = now_ms();
        for (int i = 0; i < MAXWIN; i++) {
            win_t* w = &wins[i];
            if (!w->used) continue;
            if (w->kind == WK_CLIENT && !w->dead) {
                client_t* c = w->cl;
                if (c->state != 2 && t - c->born_ms > 5000) violation(w, "no window within 5 s");
                if (c->close_deadline && t > c->close_deadline) w->dead = 1;
            }
            if (w->dead) {
                if (w->kind == WK_TERM && w->master >= 0) term_read(w);   /* drain what is left */
                win_destroy(w);
            }
        }
        time_t sec = time(0);
        if (sec != last_sec) {
            last_sec = sec;
            paint_panel();
            for (int i = 0; i < MAXWIN; i++)
                if (wins[i].used && wins[i].kind == WK_CLOCK) clock_paint(&wins[i]);
            for (int i = 0; i < MAXWIN; i++)
                if (wins[i].used && wins[i].kind == WK_CLIENT && wins[i].cl->state == 2) {
                    struct swp_frame fr = { t };
                    send_ev(wins[i].cl, SWP_EV_FRAME, &fr, sizeof(fr));
                }
        }
        if (t - last_layout > 1000) { last_layout = t; refresh_layout(); }
        flush_damage();
        move_cursor();
    }
    logf_("SW-EXIT");
    cleanup();
    printf("secwm: back to the text console\n");
    return 0;
}
