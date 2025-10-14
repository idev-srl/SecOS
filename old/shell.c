#include "shell.h"
#include "keyboard.h"
#include "terminal.h"
#include "timer.h"
#include "pmm.h"
#include "heap.h"
#include <stdint.h>
#include <stddef.h>

#define MAX_COMMAND_LEN 256

// Funzioni helper
static int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

// Converti numero in stringa
static void itoa(uint64_t value, char* buffer, int base) {
    char temp[32];
    int i = 0;
    
    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }
    
    while (value > 0) {
        int digit = value % base;
        temp[i++] = (digit < 10) ? ('0' + digit) : ('A' + digit - 10);
        value /= base;
    }
    
    int j = 0;
    while (i > 0) {
        buffer[j++] = temp[--i];
    }
    buffer[j] = '\0';
}

// Parse numero da stringa
static uint32_t atoi(const char* str) {
    uint32_t result = 0;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result;
}

// Comandi della shell
static void cmd_help(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("\nComandi disponibili:\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("  help     - Mostra questo messaggio\n");
    terminal_writestring("  clear    - Pulisce lo schermo\n");
    terminal_writestring("  echo     - Stampa un messaggio\n");
    terminal_writestring("  info     - Informazioni sul sistema\n");
    terminal_writestring("  uptime   - Mostra tempo di attivita'\n");
    terminal_writestring("  sleep    - Attendi per N millisecondi\n");
    terminal_writestring("  mem      - Mostra statistiche memoria\n");
    terminal_writestring("  memtest  - Test allocazione memoria\n");
    terminal_writestring("  crash    - Test eccezioni CPU (div0/pf/gpf/df/inv)\n");
    terminal_writestring("  colors   - Test dei colori\n");
    terminal_writestring("  halt     - Arresta il sistema\n");
    terminal_writestring("  reboot   - Riavvia il sistema\n");
    terminal_writestring("\n");
}

static void cmd_clear(void) {
    terminal_initialize();
}

static void cmd_echo(const char* args) {
    if (*args) {
        terminal_writestring(args);
    }
    terminal_writestring("\n");
}

static void cmd_info(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("\n=== Informazioni Sistema ===\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("Nome:        SecOS Kernel\n");
    terminal_writestring("Versione:    0.2.0\n");
    terminal_writestring("Architettura: x86-64 (Long Mode)\n");
    terminal_writestring("Bootloader:  GRUB Multiboot\n");
    terminal_writestring("Timer:       PIT @ ");
    
    char freq_str[16];
    itoa(timer_get_frequency(), freq_str, 10);
    terminal_writestring(freq_str);
    terminal_writestring(" Hz\n");
    
    terminal_writestring("Tastiera:    PS/2 Driver\n");
    terminal_writestring("Video:       VGA Text Mode 80x25\n");
    terminal_writestring("\n");
}

static void cmd_uptime(void) {
    uint64_t seconds = timer_get_uptime_seconds();
    uint64_t ticks = timer_get_ticks();
    
    uint64_t hours = seconds / 3600;
    uint64_t minutes = (seconds % 3600) / 60;
    uint64_t secs = seconds % 60;
    
    char buffer[32];
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("\nUptime: ");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    itoa(hours, buffer, 10);
    terminal_writestring(buffer);
    terminal_writestring("h ");
    
    itoa(minutes, buffer, 10);
    terminal_writestring(buffer);
    terminal_writestring("m ");
    
    itoa(secs, buffer, 10);
    terminal_writestring(buffer);
    terminal_writestring("s\n");
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    terminal_writestring("Tick totali: ");
    itoa(ticks, buffer, 10);
    terminal_writestring(buffer);
    terminal_writestring("\n\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

static void cmd_sleep(const char* args) {
    if (*args == '\0') {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("Uso: sleep <millisecondi>\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        return;
    }
    
    uint32_t ms = atoi(args);
    
    if (ms == 0 || ms > 10000) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("Valore non valido (1-10000 ms)\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        return;
    }
    
    char buffer[16];
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("Attendo ");
    itoa(ms, buffer, 10);
    terminal_writestring(buffer);
    terminal_writestring(" ms...\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    timer_sleep_ms(ms);
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("Fatto!\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

static void cmd_colors(void) {
    terminal_writestring("\nTest colori VGA:\n");
    
    const char* color_names[] = {
        "Nero", "Blu", "Verde", "Ciano", "Rosso", "Magenta", "Marrone", "Grigio chiaro",
        "Grigio scuro", "Blu chiaro", "Verde chiaro", "Ciano chiaro", 
        "Rosso chiaro", "Magenta chiaro", "Giallo", "Bianco"
    };
    
    for (int i = 0; i < 16; i++) {
        terminal_setcolor(vga_entry_color(i, VGA_COLOR_BLACK));
        terminal_writestring(color_names[i]);
        terminal_writestring("  ");
    }
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("\n\n");
}

static void cmd_mem(void) {
    terminal_writestring("\n");
    pmm_print_stats();
    heap_print_stats();
}

static void cmd_memtest(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("\nTest allocazione memoria...\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    // Test 1: Alloca singolo blocco
    terminal_writestring("Test 1: Allocazione 256 bytes...\n");
    void* test_ptr = kmalloc(256);
    
    if (test_ptr == NULL) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("  [FAIL] Allocazione fallita\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        return;
    }
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("  [OK] Allocato\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    // Test 2: Libera
    terminal_writestring("Test 2: Liberazione...\n");
    kfree(test_ptr);
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("  [OK] Liberato\n");
    
    // Test 3: Alloca multipli blocchi
    terminal_writestring("\nTest 3: Allocazione 5 blocchi da 1KB...\n");
    void* blocks[5];
    for (int i = 0; i < 5; i++) {
        blocks[i] = kmalloc(1024);
        if (!blocks[i]) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
            terminal_writestring("  [FAIL] Allocazione fallita\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            return;
        }
    }
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("  [OK] Tutti i blocchi allocati\n");
    
    // Test 4: Libera tutti
    terminal_writestring("Test 4: Liberazione blocchi...\n");
    for (int i = 0; i < 5; i++) {
        kfree(blocks[i]);
    }
    terminal_writestring("  [OK] Tutti i blocchi liberati\n");
    
    terminal_writestring("\nTest completato con successo!\n\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

static void cmd_reboot(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("\nRiavvio del sistema...\n");
    
    // Riavvia usando la tastiera controller
    uint8_t temp;
    __asm__ volatile ("cli");  // Disabilita interrupt
    
    do {
        __asm__ volatile ("inb $0x64, %0" : "=a"(temp));
    } while (temp & 0x02);  // Attendi che il buffer sia vuoto
    
    __asm__ volatile ("outb %0, $0x64" : : "a"((uint8_t)0xFE));  // Pulse reset line
    
    __asm__ volatile ("hlt");
}

static void cmd_halt(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("\nArresto del sistema...\n\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("Il sistema e' stato arrestato in sicurezza.\n");
    terminal_writestring("E' possibile spegnere il computer.\n\n");
    
    // Disabilita interrupt
    __asm__ volatile ("cli");
    
    // ACPI shutdown (funziona su QEMU e alcuni PC reali)
    // Prova prima con QEMU
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    
    // Se ACPI non funziona, entra in un loop HLT
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("(Se il sistema non si spegne automaticamente, premere il pulsante)\n");
    
    while (1) {
        __asm__ volatile ("hlt");
    }
}

static void cmd_crash(const char* args) {
    // Rimuovi spazi iniziali
    while (*args == ' ') args++;
    
    if (*args == '\0') {
        terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
        terminal_writestring("\nUso: crash <tipo>\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        terminal_writestring("Tipi disponibili:\n");
        terminal_writestring("  div0  - Division by zero (INT 0)\n");
        terminal_writestring("  pf    - Page Fault (INT 14)\n");
        terminal_writestring("  gpf   - General Protection Fault (INT 13)\n");
        terminal_writestring("  df    - Double Fault (INT 8)\n");
        terminal_writestring("  inv   - Invalid Opcode (INT 6)\n\n");
        terminal_writestring("  stk   - Stack Fault (INT 12)\n\n");
        return;
    }
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    terminal_writestring("\n!!! ATTENZIONE: Generazione eccezione intenzionale !!!\n\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    if (strcmp(args, "div0") == 0) {
        terminal_writestring("Generazione Division by Zero...\n");
        // Usa assembly per evitare ottimizzazioni
        __asm__ volatile (
            "mov $5, %%eax\n"
            "xor %%ebx, %%ebx\n"
            "div %%ebx\n"
            ::: "eax", "ebx", "edx"
        );
    } else if (strcmp(args, "pf") == 0) {
        terminal_writestring("Generazione Page Fault...\n");
        uint64_t* ptr = (uint64_t*)0x5000000;  // Fuori memoria mappata
        *ptr = 42;
    } else if (strcmp(args, "gpf") == 0) {
        terminal_writestring("Generazione General Protection Fault...\n");
        // Carica un valore invalido nel registro DS
        __asm__ volatile (
            "mov $0x1234, %%ax\n"
            "mov %%ax, %%ds\n"
            ::: "ax"
        );
    } else if (strcmp(args, "df") == 0) {
        terminal_writestring("Generazione Double Fault...\n");
        terminal_writestring("(Corrompo lo stack e causo un'eccezione)\n");
        // Imposta RSP a un valore invalido e causa un'eccezione
        __asm__ volatile (
            "movq $0x10, %%rsp\n"    // Stack pointer invalido
            "int $0x03\n"             // Causa un'eccezione (breakpoint)
            ::: "rsp"
        );
    } else if (strcmp(args, "inv") == 0) {
        terminal_writestring("Generazione Invalid Opcode...\n");
        __asm__ volatile ("ud2");  // Undefined instruction
    } else if (strcmp(args, "stk") == 0) {
        terminal_writestring("Generazione Stack Fault...\n");
        // Metti RSP in un indirizzo invalido
        __asm__ volatile ("mov $0x10, %rsp");
    } else {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("Tipo di crash non valido!\n");
        terminal_writestring("Usa 'crash' senza argomenti per vedere i tipi disponibili.\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    }
}

// Parsing ed esecuzione comandi
static void execute_command(char* cmd) {
    // Rimuovi spazi iniziali
    while (*cmd == ' ') cmd++;
    
    if (*cmd == '\0') return;
    
    // Trova gli argomenti
    char* args = cmd;
    while (*args && *args != ' ') args++;
    if (*args) {
        *args = '\0';
        args++;
        while (*args == ' ') args++;
    }
    
    // Esegui comando
    if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "clear") == 0) {
        cmd_clear();
    } else if (strcmp(cmd, "echo") == 0) {
        cmd_echo(args);
    } else if (strcmp(cmd, "info") == 0) {
        cmd_info();
    } else if (strcmp(cmd, "uptime") == 0) {
        cmd_uptime();
    } else if (strcmp(cmd, "sleep") == 0) {
        cmd_sleep(args);
    } else if (strcmp(cmd, "mem") == 0) {
        cmd_mem();
    } else if (strcmp(cmd, "memtest") == 0) {
        cmd_memtest();
    } else if (strcmp(cmd, "crash") == 0) {
        cmd_crash(args);
    } else if (strcmp(cmd, "colors") == 0) {
        cmd_colors();
    } else if (strcmp(cmd, "halt") == 0) {
        cmd_halt();
    } else if (strcmp(cmd, "reboot") == 0) {
        cmd_reboot();
    } else {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("Comando non trovato: ");
        terminal_writestring(cmd);
        terminal_writestring("\nDigita 'help' per la lista dei comandi.\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    }
}

// Prompt della shell
static void show_prompt(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("secos");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("$ ");
}

// Inizializza la shell
void shell_init(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("\n==================================\n");
    terminal_writestring("   Benvenuto in SecOS Shell!\n");
    terminal_writestring("==================================\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("\nDigita 'help' per vedere i comandi disponibili.\n\n");
}

// Loop principale della shell
void shell_run(void) {
    char command[MAX_COMMAND_LEN];
    int pos = 0;
    
    show_prompt();
    
    while (1) {
        char c = keyboard_getchar();
        
        if (c == '\n') {
            terminal_putchar('\n');
            command[pos] = '\0';
            
            if (pos > 0) {
                execute_command(command);
            }
            
            pos = 0;
            show_prompt();
        } else if (c == '\b') {
            if (pos > 0) {
                pos--;
                terminal_putchar('\b');
            }
        } else if (pos < MAX_COMMAND_LEN - 1) {
            command[pos++] = c;
            terminal_putchar(c);
        }
    }
}