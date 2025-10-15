// Consolidated kernel_main moved from root kernel.c (advanced framebuffer + MB2 + PMM2 support)
#include "config.h"
#include "terminal.h"
#include "multiboot.h"
#include "multiboot2.h"
#include "pmm.h"
#include "vmm.h"
#include "idt.h"
#include "tss.h"
#include "heap.h"
#include "keyboard.h"
#include "timer.h"
#include "shell.h"
#include "sched.h"
#include "panic.h"
#if ENABLE_FB
#include "fb.h"
#include "fb_console.h"
#endif

static void print_banner(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("==================================\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("   Kernel 64-bit con GRUB\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("==================================\n\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("Kernel avviato in modalita' Long Mode (64-bit)!\n");
}

void kernel_main(uint32_t multiboot_magic, uint64_t multiboot_info) {
    // Avvio kernel
    terminal_initialize();
    // Funzione riusabile per stampare il banner
    void print_banner(void);
    print_banner();

    // Info sintetiche boot
    terminal_writestring("Multiboot magic: "); print_hex(multiboot_magic); terminal_writestring("  info: "); print_hex(multiboot_info); terminal_writestring("\n");
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
    // (Eventuale: potremmo enumerare i tag solo in modalità debug)
#endif

    // Mark which PMM path we take
    if (multiboot_magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
    pmm_init_mb2((void*)multiboot_info);
    } else {
    pmm_init((void*)multiboot_info);
    }
    // pmm_print_stats(); // opzionale
    vmm_init();
    // terminal_writestring("[OK] Inizializzazione IDT...\n");
    idt_init();
    vmm_init_physmap();
    tss_init();

    // terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    // terminal_writestring("[DBG] Indirizzi chiave:\n");
    // terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    // terminal_writestring("  kernel_main: "); print_hex((uint64_t)&kernel_main); terminal_writestring("\n");
    // terminal_writestring("  idt_init:    "); print_hex((uint64_t)&idt_init); terminal_writestring("\n");
    // terminal_writestring("  tss_init:    "); print_hex((uint64_t)&tss_init); terminal_writestring("\n");
    // extern uint32_t _kernel_end; terminal_writestring("  _kernel_end: "); print_hex((uint64_t)&_kernel_end); terminal_writestring("\n");
    // uint64_t cr3_val; __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val)); terminal_writestring("  CR3:         "); print_hex(cr3_val); terminal_writestring("\n\n");
    // terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));

    heap_init();
    sched_init();
    // terminal_writestring("[OK] Inizializzazione timer PIT (1000 Hz)...\n");
    timer_init(1000);
    // terminal_writestring("[OK] Inizializzazione tastiera PS/2...\n");
    keyboard_init();

#if ENABLE_FB
    if (multiboot_magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        if (fb_init((uint32_t)multiboot_info) == 0) {
            fb_finalize_mapping();
            framebuffer_info_t info; if (fb_get_info(&info)) {
                uint64_t base = info.virt_addr ? info.virt_addr : info.addr;
                volatile uint32_t* p = (uint32_t*)base;
                p[0]=0x0000FF; p[1]=0x00FF00; p[2]=0xFF0000;
                if (terminal_try_enable_fb()) {
                    // Pulisce con nero per mostrare solo testo e shell
                    fb_clear(0x000000);
                    extern void fb_console_draw_logo(void); fb_console_draw_logo();
                    print_banner();
                    terminal_writestring("[FB] w="); print_hex(info.width); terminal_writestring(" h="); print_hex(info.height); terminal_writestring(" bpp="); print_hex(info.bpp); terminal_writestring(" pitch="); print_hex(info.pitch); terminal_writestring(" addr="); print_hex(info.addr); terminal_writestring(" virt="); print_hex(info.virt_addr); terminal_writestring("\n");
                    // Abilita blink cursore framebuffer
                    extern int fb_console_enable_cursor_blink(uint32_t timer_freq);
                    fb_console_enable_cursor_blink(timer_get_frequency());
                }
            }
        }
    }
#endif

    // terminal_writestring("[OK] Sistema pronto!\n");
    shell_init();
#if ENABLE_FB
    extern void fb_console_draw_logo(void); fb_console_draw_logo();
    extern void fb_console_flush(void); fb_console_flush();
#endif
    shell_run();
    while (1) { __asm__ volatile ("hlt"); }
}