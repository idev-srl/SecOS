#include <stdint.h>
#include <stddef.h>
#include "terminal.h"
#include "idt.h"
#include "keyboard.h"
#include "timer.h"
#include "multiboot.h"
#include "pmm.h"
#include "heap.h"
#include "tss.h"
#include "panic.h"
#include "shell.h"
#include "vmm.h"
#include "config.h"
#include "multiboot2.h"
#if ENABLE_FB
#include "fb.h"
#endif

// Definizioni per VGA text mode
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000

// Funzione helper
size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len])
        len++;
    return len;
}

// Variabili globali per il terminale
static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t* terminal_buffer;

// Funzioni I/O inline
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Aggiorna il cursore hardware VGA
static void update_cursor(void) {
    uint16_t pos = terminal_row * VGA_WIDTH + terminal_column;
    
    // Invia la posizione al controller VGA
    outb(0x3D4, 0x0F);  // Low byte
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);  // High byte
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

// Abilita il cursore hardware
static void enable_cursor(void) {
    outb(0x3D4, 0x0A);  // Cursor start register
    outb(0x3D5, 0x0E);  // Start at scanline 14
    outb(0x3D4, 0x0B);  // Cursor end register
    outb(0x3D5, 0x0F);  // End at scanline 15
}

void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) {
    const size_t index = y * VGA_WIDTH + x;
    terminal_buffer[index] = vga_entry(c, color);
}

void terminal_initialize(void) {
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    terminal_buffer = (uint16_t*) VGA_MEMORY;
    
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = vga_entry(' ', terminal_color);
        }
    }
    
    enable_cursor();
    update_cursor();
}

void terminal_scroll(void) {
    // Sposta tutto verso l'alto di una riga
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_buffer[y * VGA_WIDTH + x] = 
                terminal_buffer[(y + 1) * VGA_WIDTH + x];
        }
    }
    
    // Pulisce l'ultima riga
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        terminal_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = 
            vga_entry(' ', terminal_color);
    }
}

void terminal_putchar(char c) {
    if (c == '\n') {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT) {
            terminal_scroll();
            terminal_row = VGA_HEIGHT - 1;
        }
        update_cursor();
        return;
    }
    
    if (c == '\b') {
        // Backspace: torna indietro di una posizione
        if (terminal_column > 0) {
            terminal_column--;
        } else if (terminal_row > 0) {
            terminal_row--;
            terminal_column = VGA_WIDTH - 1;
        }
        // Cancella il carattere nella posizione corrente
        terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);
        update_cursor();
        return;
    }
    
    terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
    
    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT) {
            terminal_scroll();
            terminal_row = VGA_HEIGHT - 1;
        }
    }
    
    update_cursor();
}

void terminal_write(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_writestring(const char* data) {
    terminal_write(data, strlen(data));
}

void print_hex(uint64_t value) {
    char hex_chars[] = "0123456789ABCDEF";
    char buffer[17];
    buffer[16] = '\0';
    
    for (int i = 15; i >= 0; i--) {
        buffer[i] = hex_chars[value & 0xF];
        value >>= 4;
    }
    
    terminal_writestring("0x");
    terminal_writestring(buffer);
}

// Funzione principale del kernel
void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info) {
    terminal_initialize();
    
    // Banner del kernel
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("==================================\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("   Kernel 64-bit con GRUB\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("==================================\n\n");
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("Kernel avviato in modalita' Long Mode (64-bit)!\n");
    
    // Verifica Multiboot
    if (multiboot_magic == 0x2BADB002) {
        terminal_writestring("[OK] Multiboot1 rilevato\n");
    } else if (multiboot_magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        terminal_writestring("[OK] Multiboot2 rilevato\n");
    } else {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[WARN] Magic number bootloader sconosciuto!\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    }

#if ENABLE_FB
    if (multiboot_magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        // Parse tags
        uint8_t* ptr = (uint8_t*)(uint64_t)multiboot_info;
        uint32_t total_size = *(uint32_t*)ptr; // first u32 total size
        uint32_t reserved = *(uint32_t*)(ptr+4); (void)reserved;
        uint8_t* tags = ptr + 8;
        uint8_t* end = ptr + total_size;
        while (tags < end) {
            struct multiboot2_tag* tag = (struct multiboot2_tag*)tags;
            if (tag->type == MULTIBOOT2_TAG_END) break;
            if (tag->type == MULTIBOOT2_TAG_FRAMEBUFFER) {
                struct multiboot2_tag_framebuffer* fbtag = (struct multiboot2_tag_framebuffer*)tag;
                terminal_writestring("[MB2] FB tag trovato: ");
                terminal_writestring("w=");
                // naive decimal width
                uint32_t w=fbtag->width; char dbuf[12]; int di=0; if(!w){dbuf[0]='0';dbuf[1]='\0';} else {char tmp[12]; while(w){ tmp[di++]='0'+(w%10); w/=10;} for(int j=0;j<di;j++){ dbuf[j]=tmp[di-1-j]; } dbuf[di]='\0'; }
                terminal_writestring(dbuf); terminal_writestring(" h=");
                uint32_t h=fbtag->height; di=0; if(!h){dbuf[0]='0';dbuf[1]='\0';} else {char tmp2[12]; while(h){ tmp2[di++]='0'+(h%10); h/=10;} for(int j=0;j<di;j++){ dbuf[j]=tmp2[di-1-j]; } dbuf[di]='\0'; }
                terminal_writestring(dbuf); terminal_writestring(" bpp=");
                uint32_t b=fbtag->bpp; di=0; if(!b){dbuf[0]='0';dbuf[1]='\0';} else {char tmp3[12]; while(b){ tmp3[di++]='0'+(b%10); b/=10;} for(int j=0;j<di;j++){ dbuf[j]=tmp3[di-1-j]; } dbuf[di]='\0'; }
                terminal_writestring(dbuf); terminal_writestring("\n");
                // We could copy info into our fb driver global struct directly here.
            }
            // advance to next tag 8-byte aligned
            tags += (tag->size + 7) & ~7; 
        }
    }
#endif
    
    // Inizializza Physical Memory Manager
    pmm_init((void*)(uint64_t)multiboot_info);

    // Inizializza Virtual Memory Manager (usa CR3 corrente)
    vmm_init();

    // Inizializza IDT prima della physmap/TSS per intercettare eventuali fault precoci
    terminal_writestring("[OK] Inizializzazione IDT...\n");
    idt_init();

    // Ora che l'handler Page Fault è attivo possiamo mappare la physmap
    vmm_init_physmap();

    // Inizializza TSS e GDT estesa (con IST) dopo avere handlers attivi
    tss_init();

    // Log diagnostico degli indirizzi critici
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("[DBG] Indirizzi chiave:\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("  kernel_main: "); print_hex((uint64_t)&kernel_main); terminal_writestring("\n");
    terminal_writestring("  idt_init:    "); print_hex((uint64_t)&idt_init); terminal_writestring("\n");
    terminal_writestring("  tss_init:    "); print_hex((uint64_t)&tss_init); terminal_writestring("\n");
    extern uint32_t _kernel_end; // dal linker
    terminal_writestring("  _kernel_end: "); print_hex((uint64_t)&_kernel_end); terminal_writestring("\n");
    // Leggi CR3
    uint64_t cr3_val; __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    terminal_writestring("  CR3:         "); print_hex(cr3_val); terminal_writestring("\n\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));

    // Inizializza Heap
    heap_init();
    
    // Inizializza timer (1000 Hz = 1 tick ogni 1ms)
    terminal_writestring("[OK] Inizializzazione timer PIT (1000 Hz)...\n");
    timer_init(1000);
    
    // Inizializza tastiera
    terminal_writestring("[OK] Inizializzazione tastiera PS/2...\n");
    keyboard_init();

#if ENABLE_FB
    terminal_writestring("[OK] Inizializzazione framebuffer...\n");
    if (fb_init(multiboot_info) == 0) {
        terminal_writestring("[OK] Framebuffer pronto, disegno pattern...\n");
        fb_draw_test_pattern();
    } else {
        terminal_writestring("[WARN] Framebuffer non disponibile\n");
    }
#endif
    
    terminal_writestring("[OK] Sistema pronto!\n");
    
    // Avvia la shell
    shell_init();
    shell_run();
    
    // Non dovremmo mai arrivare qui
    while (1) {
        __asm__ volatile ("hlt");
    }
}