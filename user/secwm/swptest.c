/* SecOS - SWP client isolation selftest (/bin/swptest).
 * SPDX-License-Identifier: MIT
 *
 * Started by the compositor (`secwm --run "/bin/swptest MODE"`). Signed with
 * the confined manifest (CAP_ENFORCE | CAP_FS_READ | CAP_TIME), like every GUI
 * client. Markers go to stdout (console + debugcon):
 *
 *   swptest          probe the sandbox (fds, fb0, input, ptys, signals, fork),
 *                    open a window filled with 0x12AB34, then report every
 *                    key / focus event the compositor delivers (SWT-*)
 *   swptest oob      PUT outside its own window        -> must be disconnected
 *   swptest len      PUT whose length lies about its rectangle
 *   swptest read     an unknown request (a "give me pixels" attempt)
 *   swptest early    PUT before CREATE
 *   swptest big      a small request claiming a 1 MB payload
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <linux/kd.h>
#include "libsecos.h"
#include "swpc.h"

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
    char b[200];
    va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    if (n > (int)sizeof(b) - 2) n = (int)sizeof(b) - 2;
    b[n++] = '\n';
    write(1, b, (size_t)n);
}

static void probes(void) {
    /* 1. Only stdio + the two SWP pipes are open. */
    char open_fds[128] = ""; int extra = 0;
    for (int fd = 0; fd < 32; fd++) {
        if (fcntl(fd, F_GETFD) == -1) continue;
        char t[8]; snprintf(t, sizeof(t), "%s%d", open_fds[0] ? "," : "", fd);
        strcat(open_fds, t);
        if (fd > SWP_FD_REQ) extra++;
    }
    say("SWT-FDS %s open=%s", extra == 0 ? "OK" : "FAIL", open_fds);
    /* 2. No display, no input devices, no console mode switch. */
    int fb = open("/dev/fb0", O_RDONLY);
    int fb_e = errno;
    int inputs = 0;
    for (int i = 0; i < 8; i++) {
        char p[32]; snprintf(p, sizeof(p), "/dev/input/event%d", i);
        int e = open(p, O_RDONLY);
        if (e >= 0) { inputs++; close(e); }
    }
    int con = open("/dev/console", O_RDONLY);
    int kd = con >= 0 ? ioctl(con, KDSETMODE, (void*)KD_TEXT) : -1;
    int kd_e = errno;
    if (con >= 0) close(con);
    say("SWT-NOCAP %s fb0=%d(%d) inputs=%d kd=%d(%d)",
        (fb < 0 && fb_e == EACCES && inputs == 0 && kd != 0) ? "OK" : "FAIL", fb, fb_e, inputs, kd, kd_e);
    if (fb >= 0) close(fb);
    /* 3. Other terminals' pty slaves cannot be opened (the compositor locks them). */
    int pts = 0;
    for (int i = 0; i < 16; i++) {
        char p[32]; snprintf(p, sizeof(p), "/dev/pts/%d", i);
        int e = open(p, O_RDONLY | O_NOCTTY);
        if (e >= 0) { pts++; close(e); }
    }
    say("SWT-PTS %s opened=%d", pts == 0 ? "OK" : "FAIL", pts);
    /* 4. Confined manifest: no signals to anybody, no fork, no file writes. */
    int k = kill(getppid(), SIGCONT);
    int f = fork();
    if (f == 0) _exit(0);
    int wfd = open("/tmp/swptest.out", O_WRONLY | O_CREAT, 0644);
    say("SWT-CONFINED %s kill=%d fork=%d write=%d", (k != 0 && f < 0 && wfd < 0) ? "OK" : "FAIL", k, f, wfd);
    if (wfd >= 0) close(wfd);
}

static int evil(const char* mode) {
    swpc_t c;
    memset(&c, 0, sizeof(c));
    c.ev_fd = SWP_FD_EV; c.req_fd = SWP_FD_REQ;
    if (!strcmp(mode, "early")) {
        struct swp_hello hl = { SWP_MAGIC, SWP_VERSION };
        swpc_send(&c, SWP_REQ_HELLO, &hl, sizeof(hl));
        struct { struct swp_put p; uint32_t px[4]; } m = { { 0, 0, 2, 2 }, { 0, 0, 0, 0 } };
        say("SWE-SEND early");
        swpc_send(&c, SWP_REQ_PUT, &m, sizeof(m));
    } else {
        if (swpc_open(&c, 200, 100, mode) < 0) { say("SWE-OPEN-FAIL %s", mode); return 1; }
        gfx_fill(&c.surf, 0, 0, c.w, c.h, 0x802020);
        swpc_commit(&c, 0, 0, c.w, c.h);
        say("SWE-SEND %s", mode);
        if (!strcmp(mode, "oob")) {
            /* 100 px wide at x = 150 of a 200 px window: would write past its right
             * edge into whatever sits next to it in the compositor's memory. */
            static uint32_t px[100 * 10];
            for (int i = 0; i < 100 * 10; i++) px[i] = 0xFF00FF;
            struct swp_put p = { 150, 10, 100, 10 };
            struct swp_hdr h = { SWP_REQ_PUT, (uint32_t)(sizeof(p) + sizeof(px)) };
            write(SWP_FD_REQ, &h, sizeof(h)); write(SWP_FD_REQ, &p, sizeof(p)); write(SWP_FD_REQ, px, sizeof(px));
        } else if (!strcmp(mode, "len")) {
            struct swp_put p = { 0, 0, 10, 10 };
            struct swp_hdr h = { SWP_REQ_PUT, (uint32_t)(sizeof(p) + 4) };        /* claims 1 pixel */
            write(SWP_FD_REQ, &h, sizeof(h)); write(SWP_FD_REQ, &p, sizeof(p)); write(SWP_FD_REQ, "\0\0\0\0", 4);
        } else if (!strcmp(mode, "read")) {
            struct { uint32_t win, x, y, w, h; } q = { 1, 0, 0, 64, 64 };       /* "read window 1" */
            swpc_send(&c, 99, &q, sizeof(q));
        } else if (!strcmp(mode, "big")) {
            struct swp_hdr h = { SWP_REQ_TITLE, 1u << 20 };
            write(SWP_FD_REQ, &h, sizeof(h));
        }
    }
    /* A compliant compositor drops us now: wait to be killed or see EOF. */
    struct swpc_event e;
    for (int i = 0; i < 50; i++) {
        int r = swpc_wait(&c, &e, 100);
        if (r < 0) { say("SWE-DISCONNECTED %s", mode); return 0; }
    }
    say("SWE-STILL-CONNECTED %s", mode);
    return 1;
}

int main(int argc, char** argv) {
    if (argc > 1) return evil(argv[1]);
    probes();
    swpc_t c;
    if (swpc_open(&c, 240, 140, "swptest") < 0) { say("SWT-WINDOW FAIL"); return 1; }
    gfx_fill(&c.surf, 0, 0, c.w, c.h, 0x12AB34);
    gfx_text(&c.surf, 10, 10, "swptest: isolation probe", 0xFFFFFF, 1, 0);
    swpc_commit(&c, 0, 0, c.w, c.h);
    say("SWT-WINDOW OK %dx%d", c.w, c.h);
    int keys = 0;
    for (;;) {
        struct swpc_event e;
        int r = swpc_wait(&c, &e, 120000);
        if (r <= 0) break;
        if (e.type == SWP_EV_KEY && e.u.key.value == 1) {
            keys++;
            say("SWT-KEY code=%u cp=%u", e.u.key.code, e.u.key.cp);
        } else if (e.type == SWP_EV_FOCUS) {
            say("SWT-FOCUS %u", e.u.focus.in);
        } else if (e.type == SWP_EV_BUTTON && e.u.btn.value == 1) {
            say("SWT-BUTTON %d,%d", e.u.btn.x, e.u.btn.y);
        } else if (e.type == SWP_EV_CLOSE) {
            break;
        }
    }
    say("SWT-END keys=%d", keys);
    return 0;
}
