#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>
#include <stddef.h>
#include "../config.h"

// Colori VGA
enum vga_color {
    VGA_COLOR_BLACK = 0,
    VGA_COLOR_BLUE = 1,
    VGA_COLOR_GREEN = 2,
    VGA_COLOR_CYAN = 3,
    VGA_COLOR_RED = 4,
    VGA_COLOR_MAGENTA = 5,
    VGA_COLOR_BROWN = 6,
    VGA_COLOR_LIGHT_GREY = 7,
    VGA_COLOR_DARK_GREY = 8,
    VGA_COLOR_LIGHT_BLUE = 9,
    VGA_COLOR_LIGHT_GREEN = 10,
    VGA_COLOR_LIGHT_CYAN = 11,
    VGA_COLOR_LIGHT_RED = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_YELLOW = 14,
    VGA_COLOR_WHITE = 15,
};

// Funzioni inline
static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t) uc | (uint16_t) color << 8;
}

// Funzioni del terminale
void terminal_initialize(void);
void terminal_setcolor(uint8_t color);
// Stato colore utente persistente (framebuffer console)
extern uint8_t user_fg;
extern uint8_t user_bg;
extern int user_color_set;
void terminal_restore_user_color(void);
void terminal_putchar(char c);
void terminal_writestring(const char* data);
void print_hex(uint64_t value);
void print_dec(uint64_t value);
#if ENABLE_FB
int terminal_try_enable_fb(void);
#else
static inline int terminal_try_enable_fb(void){ return 0; }
#endif

#endif