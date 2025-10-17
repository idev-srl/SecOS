/*
 * SecOS Kernel - Terminal Abstraction
 * Provides legacy VGA text mode and optional framebuffer console delegation.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "terminal.h"
#include "fb.h"
#include "fb_console.h"
#include <stddef.h>
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000
static size_t terminal_row, terminal_column; static uint8_t terminal_color; static uint16_t* terminal_buffer;
static inline void outb(uint16_t port,uint8_t value){ __asm__ volatile("outb %0, %1"::"a"(value),"Nd"(port)); }
static inline uint8_t inb(uint16_t port){ uint8_t ret; __asm__ volatile("inb %1, %0":"=a"(ret):"Nd"(port)); return ret; }
static void update_cursor(void){ uint16_t pos=terminal_row * VGA_WIDTH + terminal_column; outb(0x3D4,0x0F); outb(0x3D5,(uint8_t)(pos & 0xFF)); outb(0x3D4,0x0E); outb(0x3D5,(uint8_t)((pos>>8)&0xFF)); }
static void enable_cursor(void){ outb(0x3D4,0x0A); outb(0x3D5,0x0E); outb(0x3D4,0x0B); outb(0x3D5,0x0F); }
static int use_fb_console = 0;
uint8_t user_fg = VGA_COLOR_WHITE; // User-selected foreground via shell color command
uint8_t user_bg = VGA_COLOR_BLACK;
int user_color_set = 0;
void terminal_restore_user_color(void){ if(user_color_set) terminal_setcolor(vga_entry_color((enum vga_color)user_fg,(enum vga_color)user_bg)); }
void terminal_setcolor(uint8_t color){
	terminal_color=color;
#if ENABLE_FB
	if (use_fb_console) {
		uint8_t fg = color & 0x0F; uint8_t bg = (color >>4) & 0x0F;
		fb_console_set_color(fg,bg);
	}
#endif
}
void terminal_putentryat(char c,uint8_t color,size_t x,size_t y){ terminal_buffer[y*VGA_WIDTH + x] = vga_entry(c,color); }
void terminal_initialize(void){
	terminal_row=0; terminal_column=0; terminal_color=vga_entry_color(VGA_COLOR_WHITE,VGA_COLOR_BLACK);
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
		terminal_buffer=(uint16_t*)VGA_MEMORY; for(size_t y=0;y<VGA_HEIGHT;y++) for(size_t x=0;x<VGA_WIDTH;x++) terminal_buffer[y*VGA_WIDTH + x]= vga_entry(' ',terminal_color); enable_cursor(); update_cursor();
	}
}
static void terminal_scroll(void){ for(size_t y=0;y<VGA_HEIGHT-1;y++) for(size_t x=0;x<VGA_WIDTH;x++) terminal_buffer[y*VGA_WIDTH+x] = terminal_buffer[(y+1)*VGA_WIDTH + x]; for(size_t x=0;x<VGA_WIDTH;x++) terminal_buffer[(VGA_HEIGHT-1)*VGA_WIDTH + x] = vga_entry(' ',terminal_color); }
void terminal_putchar(char c){
	if (use_fb_console) {
#if ENABLE_FB
		fb_console_putc(c);
#endif
		return;
	}
	if(c=='\n'){ terminal_column=0; if(++terminal_row==VGA_HEIGHT){ terminal_scroll(); terminal_row=VGA_HEIGHT-1; } update_cursor(); return; }
	if(c=='\b'){ if(terminal_column>0) terminal_column--; else if(terminal_row>0){ terminal_row--; terminal_column=VGA_WIDTH-1; } terminal_putentryat(' ',terminal_color,terminal_column,terminal_row); update_cursor(); return; }
	terminal_putentryat(c,terminal_color,terminal_column,terminal_row);
	if(++terminal_column==VGA_WIDTH){ terminal_column=0; if(++terminal_row==VGA_HEIGHT){ terminal_scroll(); terminal_row=VGA_HEIGHT-1; } }
	update_cursor();
}
void terminal_write(const char* data,size_t size){ for(size_t i=0;i<size;i++) terminal_putchar(data[i]); }
static size_t strlen(const char* s){ size_t l=0; while(s[l]) l++; return l; }
void terminal_writestring(const char* data){ terminal_write(data,strlen(data)); }
void print_hex(uint64_t value){ char hc[]="0123456789ABCDEF"; char buf[17]; buf[16]='\0'; for(int i=15;i>=0;i--){ buf[i]=hc[value & 0xF]; value >>=4; } terminal_writestring("0x"); terminal_writestring(buf); }

void print_dec(uint64_t v){ char tmp[32]; int i=0; if(v==0){ terminal_putchar('0'); return; } while(v>0){ tmp[i++]='0'+(v%10); v/=10; } while(i>0){ terminal_putchar(tmp[--i]); } }

#if ENABLE_FB
int terminal_try_enable_fb(void) {
	if (use_fb_console) return 1;
	if (fb_console_init()==0) { use_fb_console=1; return 1; }
	return 0;
}
#else
int terminal_try_enable_fb(void){ return 0; }
#endif
