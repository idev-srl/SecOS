/* SecOS GUI - VT100/xterm-subset terminal emulator for secwm windows.
 * SPDX-License-Identifier: MIT
 *
 * One term_t per terminal window. It owns a cell grid (main + alternate
 * screen), a scrollback ring and the escape-sequence parser, and renders
 * itself into a pixel surface with the 8x16 font. Rendering is incremental:
 * the term remembers what each cell looked like when it was last drawn, moves
 * pixels for scrolls, and redraws only the cells that changed.
 */
#ifndef SECWM_TERM_H
#define SECWM_TERM_H
#include <stdint.h>
#include "gfx.h"

#define TERM_PAD 4                 /* pixels of padding around the text */
#define TERM_SB_LINES 400          /* scrollback lines */

typedef struct term term_t;

term_t* term_new(int cols, int rows);
void    term_free(term_t* t);
int     term_cols(const term_t* t);
int     term_rows(const term_t* t);
/* Pixel size of the rendered area (including the padding). */
int     term_px_w(int cols);
int     term_px_h(int rows);
/* Change the grid size (contents kept top-left, scrollback cleared). */
int     term_resize(term_t* t, int cols, int rows);

/* Feed bytes written by the program on the pty slave. */
void    term_feed(term_t* t, const uint8_t* buf, int n);
/* Bytes the terminal wants to send back (DSR / DA replies); 0 if none. */
int     term_take_reply(term_t* t, char* out, int max);

/* Render into dst (top-left of the text area at dst origin). Returns the
 * damaged rectangle in dst coordinates (empty if nothing changed). */
rect_t  term_render(term_t* t, surf_t* dst, int focused);
/* Force a full redraw on the next render (focus change, new surface). */
void    term_invalidate(term_t* t);

/* Input-side state the key encoder needs. */
int     term_app_cursor(const term_t* t);     /* DECCKM: arrows send ESC O x */
int     term_app_keypad(const term_t* t);     /* DECKPAM */
int     term_bracketed_paste(const term_t* t);
/* Scrollback view: lines scrolled back from the live screen (0 = live). */
void    term_scroll_view(term_t* t, int delta);
void    term_view_live(term_t* t);
/* Window title set by OSC 0/2 (NUL-terminated, printable ASCII/UTF-8). */
const char* term_title(const term_t* t);
int     term_title_changed(term_t* t);        /* returns and clears the flag */

/* Trace hook: called with each completed output line (secwm --trace). */
void    term_set_line_hook(term_t* t, void (*hook)(void* ctx, const char* line), void* ctx);

#endif
