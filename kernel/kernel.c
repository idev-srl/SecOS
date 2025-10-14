#include <stdint.h>
#include <stddef.h>
#include "terminal.h"
#include "../config.h"
#include "idt.h"
#include "keyboard.h"
#include "timer.h"
#include "multiboot2.h"
#include "fb.h"
#include "pmm.h"
#include "heap.h"
#include "tss.h"
#include "panic.h"
#include "shell.h"
#include "vmm.h"

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
    
    // Verifica bootloader (supporto Multiboot1 + futuro Multiboot2)
    if (multiboot_magic == 0x2BADB002) {
        terminal_writestring("[OK] Multiboot1 rilevato\n");
    } else if (multiboot_magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        terminal_writestring("[OK] Multiboot2 rilevato\n");
    } else {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[WARN] Bootloader sconosciuto (magic!=MB1/MB2)\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    }
    
    // Inizializza Physical Memory Manager
#if ENABLE_PMM
    pmm_init((void*)(uint64_t)multiboot_info);
#else
    terminal_writestring("[SKIP] PMM disabilitato\n");
#endif

    // Inizializza VMM base (CR3) - chiamata spostata qui prima di physmap
    vmm_init();
    // Inizializza IDT
    terminal_writestring("[OK] Inizializzazione IDT...\n");
    idt_init();

    // Ora che l'handler Page Fault è attivo possiamo mappare la physmap
#if ENABLE_VMM
    vmm_init_physmap();
    vmm_protect_kernel_sections();
#endif

    // Inizializza TSS e GDT estesa (con IST) dopo avere handlers attivi
#if ENABLE_TSS
    tss_init();
#else
    terminal_writestring("[SKIP] TSS disabilitato\n");
#endif

#if ENABLE_DEBUG_LOG
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
#endif
    // Inizializza Heap

#if ENABLE_HEAP
    heap_init();
#else
    terminal_writestring("[SKIP] HEAP disabilitato\n");
#endif
    
    // Inizializza timer (1000 Hz = 1 tick ogni 1ms)
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("[OK] Inizializzazione timer PIT (1000 Hz)...\n");
    timer_init(1000);
    
    // Inizializza tastiera
    terminal_writestring("[OK] Inizializzazione tastiera PS/2...\n");
    keyboard_init();
    
    terminal_writestring("[OK] Sistema pronto!\n");

#if ENABLE_FB
    // Framebuffer attivo solo se Multiboot2 (tag runtime) - al momento Multiboot1 non gestito
    if (multiboot_magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        if (fb_init(multiboot_info) == 0) {
            terminal_writestring("[OK] Framebuffer inizializzato (boot splash)\n");
            fb_clear(0x002244);
            for (int y=100; y<300; y++) {
                for (int x=100; x<300; x++) {
                    uint32_t col = 0x66AAFF;
                    if (x==100||x==299||y==100||y==299) col = 0xFFFFFF;
                    fb_putpixel(x,y,col);
                }
            }
        } else {
            terminal_writestring("[WARN] Framebuffer non disponibile\n");
        }
    }
#endif
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    // Avvia la shell
#if ENABLE_SHELL
    shell_init();
    shell_run();
#else
    terminal_writestring("[SKIP] SHELL disabilitata\n");
#endif
    
    // Non dovremmo mai arrivare qui
    while (1) {
        __asm__ volatile ("hlt");
    }
}