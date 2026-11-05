/* SecOS Window Protocol - client helper library.
 * SPDX-License-Identifier: MIT
 */
#include "swpc.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>

static int write_all(int fd, const void* p, size_t n) {
    const uint8_t* b = p;
    while (n) {
        ssize_t k = write(fd, b, n);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        b += k; n -= (size_t)k;
    }
    return 0;
}

int swpc_send(swpc_t* c, uint32_t type, const void* p, uint32_t len) {
    struct swp_hdr h = { type, len };
    if (write_all(c->req_fd, &h, sizeof(h)) < 0) return -1;
    if (len && write_all(c->req_fd, p, len) < 0) return -1;
    return 0;
}

static int read_full(int fd, void* p, size_t n, int timeout_ms) {
    uint8_t* b = p;
    while (n) {
        struct pollfd pf = { fd, POLLIN, 0 };
        int r = poll(&pf, 1, timeout_ms);
        if (r <= 0) return r == 0 ? 0 : -1;
        ssize_t k = read(fd, b, n);
        if (k == 0) return -1;
        if (k < 0) { if (errno == EINTR || errno == EAGAIN) continue; return -1; }
        b += k; n -= (size_t)k;
    }
    return 1;
}

int swpc_wait(swpc_t* c, struct swpc_event* e, int timeout_ms) {
    struct swp_hdr h;
    int r = read_full(c->ev_fd, &h, sizeof(h), timeout_ms);
    if (r <= 0) return r;
    memset(e, 0, sizeof(*e));
    e->type = h.type;
    if (h.len > sizeof(e->u.raw)) return -1;            /* the compositor never does this */
    if (h.len && read_full(c->ev_fd, e->u.raw, h.len, 1000) <= 0) return -1;
    if (h.type == SWP_EV_FOCUS) c->focused = (int)e->u.focus.in;
    return 1;
}

int swpc_open(swpc_t* c, int w, int h, const char* title) {
    memset(c, 0, sizeof(*c));
    c->ev_fd = SWP_FD_EV; c->req_fd = SWP_FD_REQ;
    struct swp_hello hl = { SWP_MAGIC, SWP_VERSION };
    if (swpc_send(c, SWP_REQ_HELLO, &hl, sizeof(hl)) < 0) return -1;
    struct swp_create cr;
    memset(&cr, 0, sizeof(cr));
    cr.w = (uint32_t)w; cr.h = (uint32_t)h;
    strncpy(cr.title, title, SWP_TITLE - 1);
    if (swpc_send(c, SWP_REQ_CREATE, &cr, sizeof(cr)) < 0) return -1;
    struct swpc_event e;
    for (;;) {
        int r = swpc_wait(c, &e, 5000);
        if (r <= 0) return -1;
        if (e.type == SWP_EV_CONFIGURE) break;
    }
    c->w = (int)e.u.cfg.w; c->h = (int)e.u.cfg.h;
    c->px = malloc((size_t)c->w * c->h * 4);
    if (!c->px) return -1;
    memset(c->px, 0, (size_t)c->w * c->h * 4);
    c->surf.px = c->px; c->surf.w = c->w; c->surf.h = c->h; c->surf.stride = c->w;
    return 0;
}

int swpc_commit(swpc_t* c, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    if (w <= 0 || h <= 0) return 0;
    struct swp_put p = { (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h };
    struct swp_hdr hd = { SWP_REQ_PUT, (uint32_t)(sizeof(p) + (size_t)w * h * 4) };
    if (write_all(c->req_fd, &hd, sizeof(hd)) < 0 || write_all(c->req_fd, &p, sizeof(p)) < 0) return -1;
    if (w == c->w) return write_all(c->req_fd, c->px + (long)y * c->w, (size_t)w * h * 4);
    for (int j = 0; j < h; j++)
        if (write_all(c->req_fd, c->px + (long)(y + j) * c->w + x, (size_t)w * 4) < 0) return -1;
    return 0;
}

int swpc_fill(swpc_t* c, int x, int y, int w, int h, uint32_t color) {
    gfx_fill(&c->surf, x, y, w, h, color);
    struct swp_fill f = { (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, color };
    return swpc_send(c, SWP_REQ_FILL, &f, sizeof(f));
}

int swpc_title(swpc_t* c, const char* title) {
    char t[SWP_TITLE];
    memset(t, 0, sizeof(t));
    strncpy(t, title, SWP_TITLE - 1);
    return swpc_send(c, SWP_REQ_TITLE, t, SWP_TITLE);
}
