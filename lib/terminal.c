#include "terminal.h"
#include <stddef.h>
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000
static size_t terminal_row, terminal_column; static uint8_t terminal_color; static uint16_t* terminal_buffer;
static inline void outb(uint16_t port,uint8_t value){ __asm__ volatile("outb %0, %1"::"a"(value),"Nd"(port)); }
static inline uint8_t inb(uint16_t port){ uint8_t ret; __asm__ volatile("inb %1, %0":"=a"(ret):"Nd"(port)); return ret; }
static void update_cursor(void){ uint16_t pos=terminal_row * VGA_WIDTH + terminal_column; outb(0x3D4,0x0F); outb(0x3D5,(uint8_t)(pos & 0xFF)); outb(0x3D4,0x0E); outb(0x3D5,(uint8_t)((pos>>8)&0xFF)); }
static void enable_cursor(void){ outb(0x3D4,0x0A); outb(0x3D5,0x0E); outb(0x3D4,0x0B); outb(0x3D5,0x0F); }
void terminal_setcolor(uint8_t color){ terminal_color=color; }
void terminal_putentryat(char c,uint8_t color,size_t x,size_t y){ terminal_buffer[y*VGA_WIDTH + x] = vga_entry(c,color); }
void terminal_initialize(void){ terminal_row=0; terminal_column=0; terminal_color=vga_entry_color(VGA_COLOR_LIGHT_GREEN,VGA_COLOR_BLACK); terminal_buffer=(uint16_t*)VGA_MEMORY; for(size_t y=0;y<VGA_HEIGHT;y++) for(size_t x=0;x<VGA_WIDTH;x++) terminal_buffer[y*VGA_WIDTH + x]= vga_entry(' ',terminal_color); enable_cursor(); update_cursor(); }
static void terminal_scroll(void){ for(size_t y=0;y<VGA_HEIGHT-1;y++) for(size_t x=0;x<VGA_WIDTH;x++) terminal_buffer[y*VGA_WIDTH+x] = terminal_buffer[(y+1)*VGA_WIDTH + x]; for(size_t x=0;x<VGA_WIDTH;x++) terminal_buffer[(VGA_HEIGHT-1)*VGA_WIDTH + x] = vga_entry(' ',terminal_color); }
void terminal_putchar(char c){ if(c=='\n'){ terminal_column=0; if(++terminal_row==VGA_HEIGHT){ terminal_scroll(); terminal_row=VGA_HEIGHT-1; } update_cursor(); return; } if(c=='\b'){ if(terminal_column>0) terminal_column--; else if(terminal_row>0){ terminal_row--; terminal_column=VGA_WIDTH-1; } terminal_putentryat(' ',terminal_color,terminal_column,terminal_row); update_cursor(); return; } terminal_putentryat(c,terminal_color,terminal_column,terminal_row); if(++terminal_column==VGA_WIDTH){ terminal_column=0; if(++terminal_row==VGA_HEIGHT){ terminal_scroll(); terminal_row=VGA_HEIGHT-1; } } update_cursor(); }
void terminal_write(const char* data,size_t size){ for(size_t i=0;i<size;i++) terminal_putchar(data[i]); }
static size_t strlen(const char* s){ size_t l=0; while(s[l]) l++; return l; }
void terminal_writestring(const char* data){ terminal_write(data,strlen(data)); }
void print_hex(uint64_t value){ char hc[]="0123456789ABCDEF"; char buf[17]; buf[16]='\0'; for(int i=15;i>=0;i--){ buf[i]=hc[value & 0xF]; value >>=4; } terminal_writestring("0x"); terminal_writestring(buf); }
