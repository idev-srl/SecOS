#ifndef FB_CONSOLE_H
#define FB_CONSOLE_H
#include <stdint.h>
#include "fb.h"

#if ENABLE_FB
int fb_console_init(void); // ritorna 0 se framebuffer pronto + font init
void fb_console_set_color(uint8_t fg, uint8_t bg); // VGA-style palette (solo memorizzazione, rendering 24/32bpp)
void fb_console_putc(char c);
void fb_console_write(const char* s);
int fb_console_enable_dbuf(void); // ritorna 0 se ok
void fb_console_disable_dbuf(void);
void fb_console_flush(void); // forza copia buffer secondario -> framebuffer
void fb_console_set_dbuf_auto(int on); // abilita flush automatico
void fb_console_fontdump(char c); // stampa su console info glyph
void fb_console_draw_logo(void); // disegna logo SecOS top-right
int fb_console_enable_cursor_blink(uint32_t timer_freq);
void fb_console_disable_cursor_blink(void);
#endif

#endif
