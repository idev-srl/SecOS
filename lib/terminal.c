/*
 * SecOS Kernel - Terminal Abstraction
 * Provides legacy VGA text mode and optional framebuffer console delegation.
 * Both render through the VT100/xterm interpreter in drivers/vt.c, so escape
 * sequences and UTF-8 work the same on either backend.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "terminal.h"
#include "fb.h"
#include "fb_console.h"
#include "serial.h"
#include "vt.h"
#include "vmm.h"   /* [M44] phys_to_virt for the VGA text buffer */
#include <stddef.h>
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000
static uint8_t terminal_color; static uint16_t* terminal_buffer;
/* [M44] The VGA text buffer is physical 0xB8000. Before the physmap exists it
 * is reached through the boot identity map; afterwards (the identity map is
 * retired) only through the physmap. */
static uint16_t* vga_mem(void){
	if (vmm_physmap_ready()) return (uint16_t*)phys_to_virt(VGA_MEMORY);
	return (uint16_t*)VGA_MEMORY;
}
void terminal_relocate_vga(void){ if (terminal_buffer) terminal_buffer = vga_mem(); }
static inline void outb(uint16_t port,uint8_t value){ __asm__ volatile("outb %0, %1"::"a"(value),"Nd"(port)); }
static void vga_set_hw_cursor(int x, int y){ uint16_t pos=(uint16_t)(y * VGA_WIDTH + x); outb(0x3D4,0x0F); outb(0x3D5,(uint8_t)(pos & 0xFF)); outb(0x3D4,0x0E); outb(0x3D5,(uint8_t)((pos>>8)&0xFF)); }
static void enable_cursor(void){ outb(0x3D4,0x0A); outb(0x3D5,0x0E); outb(0x3D4,0x0B); outb(0x3D5,0x0F); }
static void disable_cursor(void){ outb(0x3D4,0x0A); outb(0x3D5,0x20); }
static int use_fb_console = 0;
uint8_t user_fg = VGA_COLOR_WHITE; // User-selected foreground via shell color command
uint8_t user_bg = VGA_COLOR_BLACK;
int user_color_set = 0;
void terminal_restore_user_color(void){ if(user_color_set) terminal_setcolor(vga_entry_color((enum vga_color)user_fg,(enum vga_color)user_bg)); }
void terminal_setcolor(uint8_t color){
	terminal_color=color;
	uint8_t fg = color & 0x0F; uint8_t bg = (color >>4) & 0x0F;
#if ENABLE_FB
	if (use_fb_console) { fb_console_set_color(fg,bg); return; }
#endif
	vt_set_colors(fg, bg);
}
void terminal_putentryat(char c,uint8_t color,size_t x,size_t y){
	if (x >= VGA_WIDTH || y >= VGA_HEIGHT || !terminal_buffer) return;
	terminal_buffer[y*VGA_WIDTH + x] = vga_entry((unsigned char)c,color);
}

/* ---- VGA text-mode vt backend (CP437 font in hardware) ---------------------- */
static unsigned char vga_glyph(uint8_t g){
	switch (g) {
	case VT_G_ELLIPSIS: return 0xFA;     /* CP437 small middle dot (closest) */
	case VT_G_MIDDOT:   return 0xFA;
	case VT_G_DEGREE:   return 0xF8;
	case VT_G_UNKNOWN:  return 0xFE;     /* small square */
	case VT_G_BLOCK:    return 0xDB;
	case VT_G_SHADE:    return 0xB1;
	case VT_G_CHECK:    return 0xFB;     /* square-root sign */
	/* [M40] accented letters: CP437 where it has them, else the base letter */
	case VT_G_A_GRAVE:  return 0x85;  case VT_G_E_GRAVE: return 0x8A;
	case VT_G_E_ACUTE:  return 0x82;  case VT_G_I_GRAVE: return 0x8D;
	case VT_G_O_GRAVE:  return 0x95;  case VT_G_U_GRAVE: return 0x97;
	case VT_G_CA_GRAVE: return 'A';   case VT_G_CE_GRAVE: return 'E';
	case VT_G_CE_ACUTE: return 0x90;  case VT_G_CI_GRAVE: return 'I';
	case VT_G_CO_GRAVE: return 'O';   case VT_G_CU_GRAVE: return 'U';
	case VT_G_C_CEDIL:  return 0x87;  case VT_G_SECTION: return 0x15;
	case VT_G_POUND:    return 0x9C;  case VT_G_EURO:    return 'E';
	default: return g;                   /* ASCII + CP437 box codes map 1:1 */
	}
}
static void vgavt_draw_cell(int x, int y, const vt_cell_t* c){
	uint8_t attr = (uint8_t)((c->fg & 0xF) | ((c->bg & 0xF) << 4));
	terminal_putentryat((char)vga_glyph(c->ch), attr, (size_t)x, (size_t)y);
}
static int vgavt_scroll(int top, int bot, int n){
	if (!terminal_buffer || top < 0 || bot >= VGA_HEIGHT || top >= bot) return -1;
	int k = n > 0 ? n : -n;
	if (k > bot - top) return -1;
	if (n > 0) {
		for (int y = top; y <= bot - k; y++)
			for (int x = 0; x < VGA_WIDTH; x++) terminal_buffer[y*VGA_WIDTH+x] = terminal_buffer[(y+k)*VGA_WIDTH+x];
	} else {
		for (int y = bot; y >= top + k; y--)
			for (int x = 0; x < VGA_WIDTH; x++) terminal_buffer[y*VGA_WIDTH+x] = terminal_buffer[(y-k)*VGA_WIDTH+x];
	}
	return 0;
}
static void vgavt_cursor(int x, int y, int on){
	if (on) { vga_set_hw_cursor(x, y); enable_cursor(); }
	else    { int vx, vy; if (!vt_get_cursor(&vx, &vy)) disable_cursor(); }
}
static const vt_ops_t vga_vt_ops = { vgavt_draw_cell, vgavt_scroll, vgavt_cursor, 0, 0 };

void terminal_initialize(void){
	terminal_color=vga_entry_color(VGA_COLOR_WHITE,VGA_COLOR_BLACK);
#if ENABLE_FB
	// Detect framebuffer console availability (heuristic: kernel_main already invoked fb_init).
	extern int fb_console_init(void);
	// Attempt graphical console init; on failure remain in VGA text mode.
	if (fb_console_init()==0) {
		use_fb_console = 1;
	} else {
		use_fb_console = 0;
	}
#endif
	if (!use_fb_console) {
		terminal_buffer=vga_mem();
		for(size_t y=0;y<VGA_HEIGHT;y++) for(size_t x=0;x<VGA_WIDTH;x++) terminal_buffer[y*VGA_WIDTH + x]= vga_entry(' ',terminal_color);
		vt_set_colors(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
		vt_attach(&vga_vt_ops, VGA_WIDTH, VGA_HEIGHT);
		enable_cursor(); vga_set_hw_cursor(0, 0);
	}
}

/* Kernel output: '\n' means "new line" (CR+LF) for every kernel caller. */
void terminal_putchar(char c){
	uint64_t lf = con_lock();   // [M48] serial mirror + vt in one piece
	// Mirror every character to COM1 so the console is usable headless
	// (no-op until serial_init() runs; harmless when no -serial is attached).
	serial_putchar(c);
	if (c == '\n') {
		static const char crlf[2] = { '\r', '\n' };
		vt_write(crlf, 2);
		con_unlock(lf);
		return;
	}
	vt_putc(c);
	con_unlock(lf);
}

/* User (TTY) output: bytes go to the emulator as-is, except that with
 * OPOST|ONLCR a '\n' becomes CR+LF (termios output processing). The serial
 * mirror follows the same rule so a host terminal sees identical bytes. */
void terminal_write_tty(const char* s, size_t n, int onlcr){
	uint64_t lf = con_lock();   // [M48] one write() comes out whole
	char buf[256]; size_t k = 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (c == '\n' && onlcr) { buf[k++] = '\r'; serial_putchar_raw('\r'); }
		buf[k++] = c; serial_putchar_raw(c);
		if (k >= sizeof(buf) - 2) { vt_write(buf, k); k = 0; }
	}
	if (k) vt_write(buf, k);
	con_unlock(lf);
}

void terminal_write(const char* data,size_t size){
	uint64_t lf = con_lock();   // [M48] a kernel string is never split by another CPU
	for(size_t i=0;i<size;i++) terminal_putchar(data[i]);
	con_unlock(lf);
}
static size_t strlen(const char* s){ size_t l=0; while(s[l]) l++; return l; }
void terminal_writestring(const char* data){ terminal_write(data,strlen(data)); }
void print_hex(uint64_t value){ char hc[]="0123456789ABCDEF"; char buf[19]; buf[0]='0'; buf[1]='x'; buf[18]='\0'; for(int i=17;i>=2;i--){ buf[i]=hc[value & 0xF]; value >>=4; } terminal_writestring(buf); }

void print_dec(uint64_t v){ char tmp[32]; int i=0; if(v==0){ terminal_putchar('0'); return; } while(v>0){ tmp[i++]='0'+(v%10); v/=10; } while(i>0){ terminal_putchar(tmp[--i]); } }

/* Console geometry in character cells (TIOCGWINSZ). */
void terminal_get_size(int* cols, int* rows){ vt_get_size(cols, rows); }

#if ENABLE_FB
int terminal_try_enable_fb(void) {
	if (use_fb_console) return 1;
	if (fb_console_init()==0) { use_fb_console=1; return 1; }
	return 0;
}
#else
int terminal_try_enable_fb(void){ return 0; }
#endif
