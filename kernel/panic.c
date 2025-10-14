#include "panic.h"
#include "terminal.h"

// Nomi delle eccezioni CPU (INT 0-31)
const char* exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",
    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

// Converti numero in stringa esadecimale
static void print_hex64(uint64_t value) {
    char hex_chars[] = "0123456789ABCDEF";
    terminal_writestring("0x");
    
    for (int i = 60; i >= 0; i -= 4) {
        char c = hex_chars[(value >> i) & 0xF];
        terminal_putchar(c);
    }
}

// Ferma il sistema con un messaggio di panic
void kernel_panic(const char* message, const char* file, uint32_t line) {
    // Disabilita interrupt
    __asm__ volatile("cli");
    
    // Schermo rosso
    terminal_initialize();
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
    
    // Banner
    terminal_writestring("\n");
    terminal_writestring("================================================================================\n");
    terminal_writestring("                            KERNEL PANIC                                        \n");
    terminal_writestring("================================================================================\n");
    terminal_writestring("\n");
    
    // Messaggio
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_RED));
    terminal_writestring("Message: ");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
    terminal_writestring(message);
    terminal_writestring("\n\n");
    
    // Posizione nel codice
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_RED));
    terminal_writestring("File: ");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
    terminal_writestring(file);
    terminal_writestring("\n");
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_RED));
    terminal_writestring("Line: ");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
    
    // Stampa numero linea
    char buf[16];
    if (line == 0) {
        terminal_writestring("0");
    } else {
        int i = 0;
        uint32_t n = line;
        while (n > 0) {
            buf[i++] = '0' + (n % 10);
            n /= 10;
        }
        while (i > 0) {
            terminal_putchar(buf[--i]);
        }
    }
    terminal_writestring("\n\n");
    
    // Istruzioni
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_RED));
    terminal_writestring("Il sistema e' stato arrestato per prevenire danni.\n");
    terminal_writestring("Premere il tasto reset per riavviare.\n");
    terminal_writestring("\n");
    terminal_writestring("================================================================================\n");
    
    // Ferma il sistema
    while (1) {
        __asm__ volatile("hlt");
    }
}

// Handler per eccezioni CPU (unica definizione corretta)
void exception_handler(struct registers* regs) {
    __asm__ volatile("cli");
    uint64_t int_no = regs->int_no;
    uint64_t err_code = regs->err_code;

    if (int_no == 14) { // Page Fault
        uint64_t cr2; __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        extern void vmm_handle_page_fault(uint64_t fault_addr, uint64_t error_code);
        vmm_handle_page_fault(cr2, err_code);
        return;
    }

    if (int_no < 32) {
        terminal_initialize();
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
        terminal_writestring("\n=== EXCEPTION ===\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_RED));
        terminal_writestring("Tipo: ");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
        terminal_writestring(exception_messages[int_no]);
        terminal_writestring("\nCodice errore: 0x");
        char hex_chars[] = "0123456789ABCDEF";
        for (int i = 60; i >= 0; i -= 4) terminal_putchar(hex_chars[(err_code >> i) & 0xF]);
        terminal_writestring("\nRIP: "); print_hex64(regs->rip);
        terminal_writestring("\nRSP: "); print_hex64(regs->rsp);
        terminal_writestring("\nCS: "); print_hex64(regs->cs);
        terminal_writestring("\nRFLAGS: "); print_hex64(regs->rflags);
        terminal_writestring("\n\nSistema bloccato.\n");
        while (1) { __asm__ volatile("hlt"); }
    }
}