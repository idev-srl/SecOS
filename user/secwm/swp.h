/* SecOS Window Protocol (SWP) v1 - secwm <-> client wire format.
 * SPDX-License-Identifier: MIT
 *
 * A client is a program the compositor starts itself. It gets exactly two
 * pipes, nothing else of the compositor's:
 *   fd 3 (SWP_FD_EV)  - events, compositor -> client (read end)
 *   fd 4 (SWP_FD_REQ) - requests, client -> compositor (write end)
 * Every message is a swp_hdr followed by `len` payload bytes, little endian,
 * no padding between messages. The connection carries one window. There is
 * no request that returns pixels, other windows or global input, and the
 * compositor treats every request as untrusted (docs/GUI.md, threat model):
 * a malformed message ends the connection and the client is killed.
 */
#ifndef SECOS_SWP_H
#define SECOS_SWP_H
#include <stdint.h>

#define SWP_FD_EV   3
#define SWP_FD_REQ  4
#define SWP_MAGIC   0x31505753u            /* "SWP1" */
#define SWP_VERSION 1

#define SWP_MAX_W   1024                   /* largest window content */
#define SWP_MAX_H   720
#define SWP_MIN_W   32
#define SWP_MIN_H   16
#define SWP_TITLE   48                     /* bytes, NUL padded */
#define SWP_MAX_SMALL 256                  /* payload limit of every request but PUT */

struct swp_hdr { uint32_t type; uint32_t len; };

/* ---- requests (client -> compositor) ----------------------------------- */
enum {
    SWP_REQ_HELLO  = 1,     /* struct swp_hello; must be the first message */
    SWP_REQ_CREATE = 2,     /* struct swp_create; once */
    SWP_REQ_PUT    = 3,     /* struct swp_put + w*h*4 bytes XRGB8888, row major */
    SWP_REQ_FILL   = 4,     /* struct swp_fill */
    SWP_REQ_TITLE  = 5,     /* char title[SWP_TITLE] */
};
struct swp_hello  { uint32_t magic, version; };
struct swp_create { uint32_t w, h; char title[SWP_TITLE]; };
struct swp_put    { uint32_t x, y, w, h; };            /* in window content coords */
struct swp_fill   { uint32_t x, y, w, h, color; };

/* ---- events (compositor -> client) ------------------------------------- */
enum {
    SWP_EV_CONFIGURE = 101, /* struct swp_configure: the window exists, this size */
    SWP_EV_KEY       = 102, /* struct swp_key: only while the window has focus */
    SWP_EV_POINTER   = 103, /* struct swp_pointer: motion inside the content */
    SWP_EV_BUTTON    = 104, /* struct swp_button: press/release inside the content */
    SWP_EV_FOCUS     = 105, /* struct swp_focus */
    SWP_EV_CLOSE     = 106, /* no payload: the user closed the window; exit now */
    SWP_EV_FRAME     = 107, /* struct swp_frame: periodic tick (1 Hz) */
};
struct swp_configure { uint32_t w, h; };
#define SWP_MOD_SHIFT 1
#define SWP_MOD_CTRL  2
#define SWP_MOD_ALT   4
#define SWP_MOD_ALTGR 8
struct swp_key     { uint32_t code; int32_t value; uint32_t mods; uint32_t cp; };   /* Linux key code, 1/0/2, mods, Unicode or 0 */
struct swp_pointer { int32_t x, y; uint32_t buttons; };
struct swp_button  { int32_t x, y; uint32_t button; int32_t value; };            /* BTN_LEFT.. , 1 press / 0 release */
struct swp_focus   { uint32_t in; };
struct swp_frame   { uint64_t ms; };

#endif
