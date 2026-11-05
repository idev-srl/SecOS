/* SecOS Window Protocol - client helper library.
 * SPDX-License-Identifier: MIT
 *
 * A program started by secwm (launcher button or `secwm --run`) talks to the
 * compositor through fds 3/4 (swp.h). Typical use:
 *
 *   swpc_t c;
 *   if (swpc_open(&c, 320, 200, "Hello") < 0) return 1;   // HELLO + CREATE + CONFIGURE
 *   gfx_fill(&c.surf, 0, 0, c.w, c.h, 0x203040);            // draw into the local buffer
 *   swpc_commit(&c, 0, 0, c.w, c.h);                        // send those pixels
 *   for (;;) { struct swpc_event e; if (swpc_wait(&c, &e, 1000) > 0 && e.type == SWP_EV_CLOSE) break; }
 */
#ifndef SECOS_SWPC_H
#define SECOS_SWPC_H
#include <stdint.h>
#include "swp.h"
#include "gfx.h"

typedef struct swpc {
    int ev_fd, req_fd;
    int w, h;
    uint32_t* px;           /* local copy of the window content (w * h) */
    surf_t surf;            /* drawing surface over px */
    int focused;
    uint8_t rx[256]; int rx_have;
} swpc_t;

struct swpc_event {
    uint32_t type;
    union {
        struct swp_configure cfg;
        struct swp_key key;
        struct swp_pointer ptr;
        struct swp_button btn;
        struct swp_focus focus;
        struct swp_frame frame;
        uint8_t raw[64];
    } u;
};

int  swpc_open(swpc_t* c, int w, int h, const char* title);
/* Send the rectangle (x, y, w, h) of the local buffer to the compositor. */
int  swpc_commit(swpc_t* c, int x, int y, int w, int h);
int  swpc_fill(swpc_t* c, int x, int y, int w, int h, uint32_t color);
int  swpc_title(swpc_t* c, const char* title);
/* Low level: send one raw message (used by the security selftest). */
int  swpc_send(swpc_t* c, uint32_t type, const void* p, uint32_t len);
/* Wait up to timeout_ms for one event: 1 got one, 0 timeout, -1 the
 * compositor closed the connection. */
int  swpc_wait(swpc_t* c, struct swpc_event* e, int timeout_ms);

#endif
