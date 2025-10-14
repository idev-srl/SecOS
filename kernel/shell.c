#include "shell.h"
#include "keyboard.h"
#include "terminal.h"
#include "timer.h"
#include "pmm.h"
#include "heap.h"
#include "../config.h"
#include "vmm.h" // user-space API types/defines
#include "process.h" // process_t
#include "elf.h" // PF_R PF_X
#include "rtc.h"
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
// Forward declarations dei comandi mancanti (portati dalla versione precedente)
static void cmd_help(void);
static void cmd_clear(void);
static void cmd_echo(const char* args);
static void cmd_info(void);
static void cmd_mem(void);
static void cmd_memtest(void);
static void execute_command(char* cmd); // prototipo per evitare implicit declaration
void shell_ps_list(void); // forward per dispatcher

// Implementazioni comandi base (copiate da old/shell.c)
static void cmd_help(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("\nComandi disponibili:\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("  help       - Mostra questo messaggio\n");
    terminal_writestring("  clear      - Pulisce lo schermo\n");
    terminal_writestring("  echo <txt> - Stampa un messaggio\n");
    terminal_writestring("  info       - Informazioni sul sistema\n");
    terminal_writestring("  uptime     - Mostra tempo di attivita'\n");
    terminal_writestring("  sleep <ms> - Attendi per N millisecondi\n");
    terminal_writestring("  mem        - Mostra statistiche memoria\n");
    terminal_writestring("  memtest    - Test allocazione memoria\n");
    terminal_writestring("  memstress  - Stress test heap\n");
    terminal_writestring("  usertest   - Test spazio utente\n");
    terminal_writestring("  elfload    - Carica ELF di test (1 segmento)\n");
    terminal_writestring("  elfload2   - Carica ELF multi-segmento di test\n");
    terminal_writestring("  elfunload [pid] - Distrugge processo (ultimo o PID)\n");
    terminal_writestring("  ps         - Elenca processi\n");
    terminal_writestring("  crash <tipo> - Genera eccezione CPU (div0/pf/gpf/df/inv/stk)\n");
    terminal_writestring("  colors     - Test dei colori VGA\n");
    terminal_writestring("  halt       - Arresta il sistema\n");
    terminal_writestring("  reboot     - Riavvia il sistema\n");
#if ENABLE_RTC
    terminal_writestring("  date       - Mostra data/ora RTC\n");
#endif
    terminal_writestring("\n");
}

static void cmd_clear(void) { terminal_initialize(); }

static void cmd_echo(const char* args) { if (*args) terminal_writestring(args); terminal_writestring("\n"); }

static void cmd_info(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("\n=== Informazioni Sistema ===\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("Nome:        SecOS Kernel\n");
    terminal_writestring("Versione:    0.2.0\n");
    terminal_writestring("Architettura: x86-64 (Long Mode)\n");
    terminal_writestring("Bootloader:  GRUB Multiboot\n");
    terminal_writestring("Timer:       PIT @ ");
    char freq_str[16]; itoa(timer_get_frequency(), freq_str, 10); terminal_writestring(freq_str); terminal_writestring(" Hz\n");
    terminal_writestring("Tastiera:    PS/2 Driver\n");
    terminal_writestring("Video:       VGA Text Mode 80x25\n\n");
}

static void cmd_mem(void) { terminal_writestring("\n"); pmm_print_stats(); heap_print_stats(); }

static void cmd_memtest(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("\nTest allocazione memoria...\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("Test 1: Allocazione 256 bytes...\n");
    void* test_ptr = kmalloc(256);
    if (!test_ptr) { terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK)); terminal_writestring("  [FAIL] Allocazione fallita\n"); terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK)); return; }
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK)); terminal_writestring("  [OK] Allocato\n"); terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("Test 2: Liberazione...\n"); kfree(test_ptr); terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK)); terminal_writestring("  [OK] Liberato\n");
    terminal_writestring("\nTest 3: Allocazione 5 blocchi da 1KB...\n"); void* blocks[5]; for(int i=0;i<5;i++){ blocks[i]=kmalloc(1024); if(!blocks[i]){ terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK)); terminal_writestring("  [FAIL] Allocazione fallita\n"); terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK)); return; } }
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK)); terminal_writestring("  [OK] Tutti i blocchi allocati\n");
    terminal_writestring("Test 4: Liberazione blocchi...\n"); for(int i=0;i<5;i++) kfree(blocks[i]); terminal_writestring("  [OK] Tutti i blocchi liberati\n");
    terminal_writestring("\nTest completato con successo!\n\n"); terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}
// Uptime
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
    itoa(hours, buffer, 10); terminal_writestring(buffer); terminal_writestring("h ");
    itoa(minutes, buffer, 10); terminal_writestring(buffer); terminal_writestring("m ");
    itoa(secs, buffer, 10); terminal_writestring(buffer); terminal_writestring("s\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    terminal_writestring("Tick totali: "); itoa(ticks, buffer, 10); terminal_writestring(buffer); terminal_writestring("\n\n");
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

// (Vecchia versione execute_command rimossa; si usa implementazione finale in fondo al file)

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
        // Simulazione: forziamo un'istruzione invalida dopo manipolazione registro generale (evita clobber rsp warning)
        __asm__ volatile ("xor %%eax, %%eax; ud2" ::: "eax", "memory");
    } else if (strcmp(args, "inv") == 0) {
        terminal_writestring("Generazione Invalid Opcode...\n");
        __asm__ volatile ("ud2");  // Undefined instruction
    } else if (strcmp(args, "stk") == 0) {
        terminal_writestring("Generazione Stack Fault...\n");
        // Genera fault simulato: forza uso di pagina non valida
        volatile uint64_t* p = (uint64_t*)0x10; // indirizzo bassissimo non mappato in long mode
        *p = 0xDEADBEEF;
    } else {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("Tipo di crash non valido!\n");
        terminal_writestring("Usa 'crash' senza argomenti per vedere i tipi disponibili.\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    }
}

// Stress test heap: forza molte allocazioni per testare espansione e coalescing
static void cmd_memstress(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("\nAvvio memstress (allocazioni ripetute)...\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));

    const int small_count = 128; // blocchi piccoli
    const size_t small_size = 64;
    void* small_ptrs[small_count];

    // Allocazioni piccole per frammentare
    int allocated_small = 0;
    for (int i=0; i<small_count; i++) {
        small_ptrs[i] = kmalloc(small_size);
        if (!small_ptrs[i]) break;
        allocated_small++;
    }
    terminal_writestring("[memstress] Alloc piccoli: ");
    char buf[32];
    // Reuse itoa from above (itoa present globally) -> use base 10
    itoa(allocated_small, buf, 10);
    terminal_writestring(buf);
    terminal_writestring("\n");

    // Allocazioni medie fino a forzare espansione (512 bytes)
    const int mid_cap = 64;
    void* mid_ptrs[mid_cap];
    int mid_count = 0;
    for (int i=0; i<mid_cap; i++) {
        mid_ptrs[i] = kmalloc(512);
        if (!mid_ptrs[i]) break;
        mid_count++;
    }
    itoa(mid_count, buf, 10);
    terminal_writestring("[memstress] Alloc medie 512B: ");
    terminal_writestring(buf);
    terminal_writestring("\n");

    // Libera un pattern alternato dei blocchi piccoli per testare coalescing
    for (int i=0; i<allocated_small; i+=2) {
        kfree(small_ptrs[i]);
    }
    terminal_writestring("[memstress] Liberati blocchi piccoli alternati\n");

    // Allocazioni grandi per spingere ulteriori espansioni (2048 bytes)
    const int big_cap = 32;
    void* big_ptrs[big_cap];
    int big_count = 0;
    for (int i=0; i<big_cap; i++) {
        big_ptrs[i] = kmalloc(2048);
        if (!big_ptrs[i]) break;
        big_count++;
    }
    itoa(big_count, buf, 10);
    terminal_writestring("[memstress] Alloc grandi 2KB: ");
    terminal_writestring(buf);
    terminal_writestring("\n");

    // Libera tutto
    for (int i=1; i<allocated_small; i+=2) kfree(small_ptrs[i]); // libera i rimanenti piccoli
    for (int i=0; i<mid_count; i++) kfree(mid_ptrs[i]);
    for (int i=0; i<big_count; i++) kfree(big_ptrs[i]);
    terminal_writestring("[memstress] Liberati tutti i blocchi\n");

    heap_print_stats();
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[memstress] Completato\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
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

// Implementazione corretta execute_command
// --- Dispatcher tabellare ---
typedef void (*shell_handler_t)(const char* args);
struct shell_cmd { const char* name; shell_handler_t handler; };

// Wrappers per adattare funzioni esistenti che non prendono args o hanno firma diversa
static void sh_help(const char* a){ (void)a; cmd_help(); }
static void sh_clear(const char* a){ (void)a; cmd_clear(); }
static void sh_info(const char* a){ (void)a; cmd_info(); }
static void sh_uptime(const char* a){ (void)a; cmd_uptime(); }
static void sh_mem(const char* a){ (void)a; cmd_mem(); }
static void sh_memtest(const char* a){ (void)a; cmd_memtest(); }
static void sh_memstress(const char* a){ (void)a; cmd_memstress(); }
static void sh_colors(const char* a){ (void)a; cmd_colors(); }
static void sh_halt(const char* a){ (void)a; cmd_halt(); }
static void sh_reboot(const char* a){ (void)a; cmd_reboot(); }
#if ENABLE_RTC
static void sh_date(const char* a){ (void)a; struct rtc_datetime dt; if (rtc_read(&dt)) { char buf[32]; rtc_format(&dt, buf, sizeof(buf)); terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK)); terminal_writestring("\nData/Ora: "); terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK)); terminal_writestring(buf); terminal_writestring("\n"); } else { terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK)); terminal_writestring("\n[FAIL] Lettura RTC fallita\n"); terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK)); } }
#endif
static void sh_echo(const char* a){ cmd_echo(a); }
static void sh_sleep(const char* a){ cmd_sleep(a); }
static void sh_crash(const char* a){ cmd_crash(a); }

// Comandi speciali con logica propria non semplicemente wrapper
static void sh_usertest(const char* args) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK)); terminal_writestring("\nCreazione spazio utente di test...\n"); terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        extern int vmm_map_user_code(uint64_t virt); extern int vmm_map_user_data(uint64_t virt); extern uint64_t vmm_alloc_user_stack(int pages); extern vmm_space_t* vmm_space_create_user(void); extern int vmm_switch_space(vmm_space_t* space); extern vmm_space_t* vmm_get_kernel_space(void);
        vmm_space_t* us = vmm_space_create_user(); if(!us){ terminal_writestring("[FAIL] creazione spazio utente\n"); } else {
            if (vmm_map_user_code(USER_CODE_BASE) != 0) terminal_writestring("[FAIL] code page\n"); else terminal_writestring("[OK] code page RX\n");
            if (vmm_map_user_data(USER_DATA_BASE) != 0) terminal_writestring("[FAIL] data page\n"); else terminal_writestring("[OK] data page RW/NX\n");
            uint64_t st = vmm_alloc_user_stack(4);
            terminal_writestring("[OK] stack utente top="); char hx[]="0123456789ABCDEF"; for(int i=60;i>=0;i-=4) terminal_putchar(hx[(st>>i)&0xF]); terminal_writestring("\n[TEST] switch a spazio utente...\n");
            if (vmm_switch_space(us)==0) terminal_writestring("[OK] switch user CR3\n"); else terminal_writestring("[FAIL] switch user\n");
            vmm_switch_space(vmm_get_kernel_space()); terminal_writestring("[OK] tornato a kernel space\n"); }
}
static void sh_elfload(const char* a) {
        extern process_t* process_create_from_elf(const void* elf_buf, size_t size); unsigned char elf_buf[512]; for(int i=0;i<512;i++) elf_buf[i]=0; elf_buf[0]=0x7F; elf_buf[1]='E'; elf_buf[2]='L'; elf_buf[3]='F'; elf_buf[4]=2; elf_buf[5]=1; elf_buf[6]=1; *(uint16_t*)(elf_buf+16)=2; *(uint16_t*)(elf_buf+18)=0x3E; *(uint32_t*)(elf_buf+20)=1; *(uint64_t*)(elf_buf+24)=USER_CODE_BASE; *(uint64_t*)(elf_buf+32)=64; *(uint16_t*)(elf_buf+52)=64; *(uint16_t*)(elf_buf+54)=56; *(uint16_t*)(elf_buf+56)=1; *(uint32_t*)(elf_buf+64)=1; *(uint32_t*)(elf_buf+68)=PF_R|PF_X; *(uint64_t*)(elf_buf+72)=0x100ULL; *(uint64_t*)(elf_buf+80)=USER_CODE_BASE; *(uint64_t*)(elf_buf+88)=USER_CODE_BASE; *(uint64_t*)(elf_buf+96)=0x80ULL; *(uint64_t*)(elf_buf+104)=0x80ULL; *(uint64_t*)(elf_buf+112)=0x1000ULL; for(int i=0;i<0x80;i++) elf_buf[0x100+i]=0x90; terminal_writestring("[ELFLOAD] Carico ELF di test...\n"); process_t* p = process_create_from_elf(elf_buf, sizeof(elf_buf)); if(!p) terminal_writestring("[ELFLOAD] Fallito\n"); else terminal_writestring("[ELFLOAD] OK (process creato)\n"); }
static void sh_elfload2(const char* a) {
        // NOTE: versione precedente usava buffer da 1024 bytes ma scriveva fino a offset 0x400+0x80 (0x480) => overflow.
        // Allochiamo buffer piu' grande e passiamo size reale utilizzata.
        extern process_t* process_create_from_elf(const void* elf_buf, size_t size);
        unsigned char elf_buf[2048];
        for (int i=0;i<2048;i++) elf_buf[i]=0;
        // ELF header base
        elf_buf[0]=0x7F; elf_buf[1]='E'; elf_buf[2]='L'; elf_buf[3]='F';
        elf_buf[4]=2; // 64-bit
        elf_buf[5]=1; // little endian
        elf_buf[6]=1; // version
        *(uint16_t*)(elf_buf+16)=2;      // e_type EXEC
        *(uint16_t*)(elf_buf+18)=0x3E;   // e_machine x86-64
        *(uint32_t*)(elf_buf+20)=1;      // e_version
        *(uint64_t*)(elf_buf+24)=USER_CODE_BASE; // e_entry
        *(uint64_t*)(elf_buf+32)=64;     // e_phoff
        *(uint16_t*)(elf_buf+52)=64;     // e_ehsize
        *(uint16_t*)(elf_buf+54)=56;     // e_phentsize
        *(uint16_t*)(elf_buf+56)=2;      // e_phnum
        // PHDR #0 (code RX)
        *(uint32_t*)(elf_buf+64)=1;                // p_type PT_LOAD
        *(uint32_t*)(elf_buf+68)=PF_R|PF_X;        // p_flags
        *(uint64_t*)(elf_buf+72)=0x200;            // p_offset (code starts at 0x200)
        *(uint64_t*)(elf_buf+80)=USER_CODE_BASE;   // p_vaddr
        *(uint64_t*)(elf_buf+88)=USER_CODE_BASE;   // p_paddr (ignored)
        *(uint64_t*)(elf_buf+96)=0x100;            // p_filesz (256 bytes code)
        *(uint64_t*)(elf_buf+104)=0x180;           // p_memsz (include 0x80 BSS)
        *(uint64_t*)(elf_buf+112)=0x1000;          // p_align
        // PHDR #1 (data RW)
        *(uint32_t*)(elf_buf+120)=1;               // p_type PT_LOAD
        *(uint32_t*)(elf_buf+124)=PF_R|PF_W;       // p_flags
        *(uint64_t*)(elf_buf+128)=0x400;           // p_offset (data at 0x400)
        *(uint64_t*)(elf_buf+136)=USER_DATA_BASE;  // p_vaddr
        *(uint64_t*)(elf_buf+144)=USER_DATA_BASE;  // p_paddr
        *(uint64_t*)(elf_buf+152)=0x80;            // p_filesz
        *(uint64_t*)(elf_buf+160)=0x200;           // p_memsz (extra zeroed)
        *(uint64_t*)(elf_buf+168)=0x1000;          // p_align
        // Riempie codice (NOP) e dati (pattern 0xAA)
        for(int i=0;i<0x100;i++) elf_buf[0x200+i]=0x90;
        for(int i=0;i<0x80;i++)  elf_buf[0x400+i]=0xAA;
        // Dimensione reale file: fine dell'ultimo byte scritto dei dati = 0x400+0x80 = 0x480
        size_t used_size = 0x480;
        terminal_writestring("[ELFLOAD2] Carico ELF multi-segmento...\n");
        process_t* p = process_create_from_elf(elf_buf, used_size);
        if(!p) terminal_writestring("[ELFLOAD2] Fallito\n"); else terminal_writestring("[ELFLOAD2] OK (process creato)\n");
    }
static void sh_elfunload(const char* a) { extern process_t* process_get_last(void); extern process_t* process_find_by_pid(uint32_t pid); extern int process_destroy(process_t* p); uint32_t pid=0; while(*a==' ') a++; while(*a>='0'&&*a<='9'){ pid=pid*10+(*a-'0'); a++; } process_t* target = pid? process_find_by_pid(pid): process_get_last(); if(!target) terminal_writestring("[ELFUNLOAD] processo non trovato\n"); else { int ur=process_destroy(target); if(ur==0) terminal_writestring("[ELFUNLOAD] OK (process distrutto)\n"); else terminal_writestring("[ELFUNLOAD] FAIL\n"); } }
static void sh_ps(const char* a){ (void)a; shell_ps_list(); }

static const struct shell_cmd shell_cmds[] = {
    {"help",      sh_help},
    {"clear",     sh_clear},
    {"echo",      sh_echo},
    {"info",      sh_info},
    {"uptime",    sh_uptime},
    {"sleep",     sh_sleep},
    {"mem",       sh_mem},
    {"memtest",   sh_memtest},
    {"memstress", sh_memstress},
    {"usertest",  sh_usertest},
    {"elfload",   sh_elfload},
    {"elfload2",  sh_elfload2},
    {"elfunload", sh_elfunload},
    {"ps",        sh_ps},
    {"crash",     sh_crash},
    {"colors",    sh_colors},
    {"halt",      sh_halt},
    {"reboot",    sh_reboot},
#if ENABLE_RTC
    {"date",      sh_date},
#endif
};

static void execute_command(char* line) {
    while(*line==' ') line++;
    if (!*line) return;
    char* args=line; while(*args && *args!=' ') args++; if(*args){ *args='\0'; args++; while(*args==' ') args++; }
    const char* cmd=line;
    // Ricerca lineare (pochi comandi, costo trascurabile). In futuro: ordinare e binary search.
    for (unsigned i=0;i<sizeof(shell_cmds)/sizeof(shell_cmds[0]); i++) {
        if (strcmp(cmd, shell_cmds[i].name)==0) { shell_cmds[i].handler(args); return; }
    }
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    terminal_writestring("Comando non trovato: "); terminal_writestring(cmd); terminal_writestring("\nDigita 'help' per la lista dei comandi.\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

// ---- Sezione PS refactor ----
struct ps_ctx_global { int count; uint64_t total_pages; };
static void ps_cb_impl(process_t* p, void* user) {
    struct ps_ctx_global* ctx = (struct ps_ctx_global*)user;
    char hx[]="0123456789ABCDEF";
    uint64_t memkb=(uint64_t)p->mapped_page_count*4ULL;
    terminal_writestring("  PID="); for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->pid>>i)&0xF]);
    terminal_writestring(" entry="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(p->entry>>i)&0xF]);
    terminal_writestring(" pages="); for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->mapped_page_count>>i)&0xF]);
    terminal_writestring(" memKB="); char buf[32]; itoa(memkb,buf,10); terminal_writestring(buf);
    const char* st="UNKNOWN"; switch(p->state){case PROC_NEW:st="NEW";break;case PROC_READY:st="READY";break;case PROC_RUNNING:st="RUN";break;case PROC_BLOCKED:st="BLK";break;case PROC_ZOMBIE:st="ZOMB";break;} terminal_writestring(" state="); terminal_writestring(st); terminal_writestring("\n");
    ctx->count++; ctx->total_pages += p->mapped_page_count;
}

void shell_ps_list(void) {
    terminal_writestring("\n[PS] Processi attivi:\n");
    extern void process_foreach(void (*cb)(process_t*, void*), void* user);
    struct ps_ctx_global ctx; ctx.count=0; ctx.total_pages=0;
    process_foreach(ps_cb_impl, &ctx);
    if (ctx.count==0) {
        terminal_writestring("  <nessuno>\n");
    } else {
        terminal_writestring("[PS] Totale pagine="); char hx[]="0123456789ABCDEF"; for(int i=28;i>=0;i-=4) terminal_putchar(hx[(ctx.total_pages>>i)&0xF]);
        uint64_t tot_kb = ctx.total_pages * 4ULL; char buf[32]; itoa(tot_kb,buf,10); terminal_writestring(" memKB="); terminal_writestring(buf); terminal_writestring("\n");
    }
}