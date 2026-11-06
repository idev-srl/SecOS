/* SecOS - framebuffer + evdev demo (/bin/fbdemo).
 * SPDX-License-Identifier: MIT
 *
 * Signed with CAP_DISPLAY | CAP_INPUT. Takes the screen over (KD_GRAPHICS),
 * maps /dev/fb0, paints a pattern and moves a mouse cursor with whatever
 * pointer is attached (PS/2 or USB mouse: relative, QEMU usb-tablet:
 * absolute). Left button paints, right button clears, the wheel cycles the
 * brush colour. The keyboards are grabbed (EVIOCGRAB) so typing does not leak
 * into the shell underneath; Esc or q quits and gives the text console back.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <termios.h>    /* ioctl() */
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include "libsecos.h"

static uint8_t* fb;
static uint32_t W, H, PITCH;

#define NBITS(x) ((((x) - 1) / (8 * sizeof(long))) + 1)
static int test_bit(int bit, const unsigned long* a) { return (a[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1; }

static inline uint32_t* px(int x, int y) { return (uint32_t*)(fb + (size_t)y * PITCH) + x; }

static uint32_t bg_at(int x, int y) {
    /* Soft diagonal gradient with a 64-pixel grid. */
    uint32_t r = (uint32_t)(x * 160 / (int)W) + 40, g = (uint32_t)(y * 160 / (int)H) + 40, b = 120;
    if (x % 64 == 0 || y % 64 == 0) { r = g = b = 220; }
    return (r << 16) | (g << 8) | b;
}

static void paint_background(void) {
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) *px((int)x, (int)y) = bg_at((int)x, (int)y);
    /* Banner box. */
    for (int y = 20; y < 60; y++)
        for (int x = 20; x < 420 && x < (int)W; x++) *px(x, y) = 0x00202830;
}

/* 12x19 arrow cursor: 'X' outline, '.' fill. */
static const char* const arrow[19] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.........X ", "X......XXXXX", "X...X..X    ", "X..XX..X    ", "X.X  X..X   ",
    "XX   X..X   ", "X     X..X  ", "      X..X  ", "       XX   ",
};
static uint32_t saved[19][12];
static int cx = -1, cy = -1;

static void cursor_hide(void) {
    if (cx < 0) return;
    for (int r = 0; r < 19; r++)
        for (int c = 0; c < 12; c++)
            if (cx + c < (int)W && cy + r < (int)H && arrow[r][c] != ' ') *px(cx + c, cy + r) = saved[r][c];
}
static void cursor_show(int x, int y) {
    cx = x; cy = y;
    for (int r = 0; r < 19; r++)
        for (int c = 0; c < 12; c++) {
            if (cx + c >= (int)W || cy + r >= (int)H || arrow[r][c] == ' ') continue;
            saved[r][c] = *px(cx + c, cy + r);
            *px(cx + c, cy + r) = arrow[r][c] == 'X' ? 0x00000000u : 0x00FFFFFFu;
        }
}

static const uint32_t brush_cols[] = { 0x00FF3030, 0x0030FF30, 0x003080FF, 0x00FFFF30, 0x00FF30FF };
static int brush;

static void dab(int x, int y) {
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            int X = x + dx, Y = y + dy;
            if (X >= 0 && Y >= 0 && X < (int)W && Y < (int)H && dx * dx + dy * dy <= 16)
                *px(X, Y) = brush_cols[brush];
        }
    /* Show the colour in the banner. */
    for (int yy = 28; yy < 52; yy++)
        for (int xx = 380; xx < 412 && xx < (int)W; xx++) *px(xx, yy) = brush_cols[brush];
}

int main(void) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { printf("fbdemo: /dev/fb0: %s (needs CAP_DISPLAY)\n", strerror(errno)); return 1; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) {
        printf("fbdemo: screeninfo failed\n"); return 1;
    }
    W = v.xres; H = v.yres; PITCH = f.line_length;
    fb = secos_mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { printf("fbdemo: mmap: %s\n", strerror(errno)); return 1; }

    int ev[8], nev = 0, abs_dev[8] = { 0 };
    struct input_absinfo ax[8], ay[8];
    for (int i = 0; i < 8; i++) {
        char p[32]; snprintf(p, sizeof(p), "/dev/input/event%d", i);
        int e = open(p, O_RDONLY | O_NONBLOCK);
        if (e < 0) continue;
        unsigned long key[NBITS(KEY_CNT)] = { 0 }, absb[NBITS(ABS_CNT)] = { 0 };
        ioctl(e, EVIOCGBIT(EV_KEY, sizeof(key)), key);
        ioctl(e, EVIOCGBIT(EV_ABS, sizeof(absb)), absb);
        if (test_bit(KEY_Q, key)) ioctl(e, EVIOCGRAB, (void*)1);   /* keyboard: keep keys from bash */
        if (test_bit(ABS_X, absb)) {
            abs_dev[nev] = 1;
            ioctl(e, EVIOCGABS(ABS_X), &ax[nev]);
            ioctl(e, EVIOCGABS(ABS_Y), &ay[nev]);
        }
        ev[nev++] = e;
    }
    if (nev == 0) { printf("fbdemo: no /dev/input/event* (needs CAP_INPUT)\n"); return 1; }
    printf("fbdemo: %ux%u, %d input devices - Esc or q quits\n", W, H, nev);
    fflush(stdout);

    if (ioctl(0, KDSETMODE, (void*)KD_GRAPHICS) != 0) printf("fbdemo: KD_GRAPHICS refused\n");
    paint_background();
    int mx = (int)W / 2, my = (int)H / 2, left = 0, quit = 0;
    cursor_show(mx, my);
    while (!quit) {
        struct pollfd pf[8];
        for (int i = 0; i < nev; i++) { pf[i].fd = ev[i]; pf[i].events = POLLIN; pf[i].revents = 0; }
        if (poll(pf, (nfds_t)nev, 1000) <= 0) continue;
        for (int i = 0; i < nev && !quit; i++) {
            if (!(pf[i].revents & POLLIN)) continue;
            struct input_event e[64];
            int n = (int)read(ev[i], e, sizeof(e));
            int nx = mx, ny = my, moved = 0;
            for (int k = 0; k < n / (int)sizeof(e[0]); k++) {
                if (e[k].type == EV_KEY) {
                    if ((e[k].code == KEY_ESC || e[k].code == KEY_Q) && e[k].value == 1) quit = 1;
                    if (e[k].code == BTN_LEFT) left = e[k].value;
                    if (e[k].code == BTN_RIGHT && e[k].value == 1) {
                        cursor_hide(); paint_background(); cx = -1; moved = 1;
                    }
                } else if (e[k].type == EV_REL) {
                    if (e[k].code == REL_X) { nx += e[k].value; moved = 1; }
                    if (e[k].code == REL_Y) { ny += e[k].value; moved = 1; }
                    if (e[k].code == REL_WHEEL) brush = (brush + (e[k].value > 0 ? 1 : 4)) % 5;
                } else if (e[k].type == EV_ABS && abs_dev[i]) {
                    struct input_absinfo* a = e[k].code == ABS_X ? &ax[i] : &ay[i];
                    int span = a->maximum - a->minimum; if (span <= 0) span = 1;
                    int lim = e[k].code == ABS_X ? (int)W : (int)H;
                    int val = (int)((long)(e[k].value - a->minimum) * (lim - 1) / span);
                    if (e[k].code == ABS_X) nx = val; else ny = val;
                    moved = 1;
                } else if (e[k].type == EV_SYN && moved) {
                    if (nx < 0) nx = 0;
                    if (ny < 0) ny = 0;
                    if (nx >= (int)W) nx = (int)W - 1;
                    if (ny >= (int)H) ny = (int)H - 1;
                    cursor_hide();
                    if (left) dab(nx, ny);
                    mx = nx; my = ny;
                    cursor_show(mx, my);
                    moved = 0;
                }
            }
        }
    }
    ioctl(0, KDSETMODE, (void*)KD_TEXT);          /* the kernel would do it at exit too */
    printf("fbdemo: bye\n");
    return 0;
}
