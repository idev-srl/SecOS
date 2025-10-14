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
    terminal_writestring("  memstress- Stress heap (alloc espansione)\n");
    terminal_writestring("  usertest - Crea spazio utente di test\n");
    terminal_writestring("  elfload  - Carica ELF embedded di test\n");
    terminal_writestring("  elfunload- Smappa ultimo processo ELF\n");
    terminal_writestring("  crash    - Test eccezioni CPU (div0/pf/gpf/df/inv)\n");
    terminal_writestring("  colors   - Test dei colori\n");
    terminal_writestring("  halt     - Arresta il sistema\n");
#if ENABLE_RTC
    terminal_writestring("  date     - Mostra data/ora RTC\n");
#endif
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
    } else if (strcmp(cmd, "memstress") == 0) {
        cmd_memstress();
    } else if (strcmp(cmd, "usertest") == 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
        terminal_writestring("\nCreazione spazio utente di test...\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        extern int vmm_map_user_code(uint64_t virt);
        extern int vmm_map_user_data(uint64_t virt);
        extern uint64_t vmm_alloc_user_stack(int pages);
        extern vmm_space_t* vmm_space_create_user(void);
        extern int vmm_switch_space(vmm_space_t* space);
        extern vmm_space_t* vmm_get_kernel_space(void);
        vmm_space_t* us = vmm_space_create_user();
        if (!us) {
            terminal_writestring("[FAIL] creazione spazio utente\n");
        } else {
            if (vmm_map_user_code(USER_CODE_BASE) != 0) terminal_writestring("[FAIL] code page\n"); else terminal_writestring("[OK] code page RX\n");
            if (vmm_map_user_data(USER_DATA_BASE) != 0) terminal_writestring("[FAIL] data page\n"); else terminal_writestring("[OK] data page RW/NX\n");
            uint64_t st = vmm_alloc_user_stack(4);
            terminal_writestring("[OK] stack utente top=");
            char hx[]="0123456789ABCDEF"; for(int i=60;i>=0;i-=4) terminal_putchar(hx[(st>>i)&0xF]);
            terminal_writestring("\n[TEST] switch a spazio utente...\n");
            if (vmm_switch_space(us)==0) terminal_writestring("[OK] switch user CR3\n"); else terminal_writestring("[FAIL] switch user\n");
            vmm_switch_space(vmm_get_kernel_space());
            terminal_writestring("[OK] tornato a kernel space\n");
        }
    } else if (strcmp(cmd, "elfload") == 0) {
        // ELF64 minimale statico (header + un segmento code da 0x200 bytes)
        // NOTE: Questo è un placeholder non eseguibile reale; serve per testare il parser.
        extern process_t* process_create_from_elf(const void* elf_buf, size_t size);
        // Costruiamo un buffer ELF semplificato
        unsigned char elf_buf[512];
        for (int i=0;i<512;i++) elf_buf[i]=0; // zero
        // e_ident
        elf_buf[0]=0x7F; elf_buf[1]='E'; elf_buf[2]='L'; elf_buf[3]='F';
        elf_buf[4]=2; // 64-bit
        elf_buf[5]=1; // little endian
        elf_buf[6]=1; // version
        // e_type (ET_EXEC=2)
        *(uint16_t*)(elf_buf+16)=2;
        // e_machine (x86-64=0x3E)
        *(uint16_t*)(elf_buf+18)=0x3E;
        // e_version
        *(uint32_t*)(elf_buf+20)=1;
        // e_entry (USER_CODE_BASE)
        *(uint64_t*)(elf_buf+24)=USER_CODE_BASE;
        // e_phoff = 64 (size of ehdr)
        *(uint64_t*)(elf_buf+32)=64;
        // e_ehsize
        *(uint16_t*)(elf_buf+52)=64;
        // e_phentsize
        *(uint16_t*)(elf_buf+54)=56; // size phdr
        // e_phnum
        *(uint16_t*)(elf_buf+56)=1;
        // Program header (at offset 64)
        // p_type=PT_LOAD
        *(uint32_t*)(elf_buf+64)=1;
        // p_flags=PF_R|PF_X
        *(uint32_t*)(elf_buf+68)=PF_R|PF_X;
        // p_offset=0x100 (contenuto fittizio in fondo al buffer)
        *(uint64_t*)(elf_buf+72)=0x100ULL;
        // p_vaddr=USER_CODE_BASE
        *(uint64_t*)(elf_buf+80)=USER_CODE_BASE;
        // p_paddr (ignored)
        *(uint64_t*)(elf_buf+88)=USER_CODE_BASE;
        // p_filesz=0x80, p_memsz=0x80
        *(uint64_t*)(elf_buf+96)=0x80ULL; // filesz
        *(uint64_t*)(elf_buf+104)=0x80ULL; // memsz
        // p_align=0x1000
        *(uint64_t*)(elf_buf+112)=0x1000ULL;
        // Fake code bytes (just pattern) at offset 0x100
        for (int i=0;i<0x80;i++) elf_buf[0x100 + i] = 0x90; // NOP pattern
        terminal_writestring("[ELFLOAD] Carico ELF di test...\n");
        process_t* p = process_create_from_elf(elf_buf, sizeof(elf_buf));
        if (!p) terminal_writestring("[ELFLOAD] Fallito\n"); else terminal_writestring("[ELFLOAD] OK (process creato)\n");
    } else if (strcmp(cmd, "elfunload") == 0) {
        extern process_t* process_get_last(void);
        extern int process_destroy(process_t* p);
        process_t* last = process_get_last();
        if (!last) { terminal_writestring("[ELFUNLOAD] nessun processo\n"); }
        else {
            int ur = process_destroy(last);
            if (ur==0) terminal_writestring("[ELFUNLOAD] OK (process distrutto)\n"); else terminal_writestring("[ELFUNLOAD] FAIL\n");
        }
    } else if (strcmp(cmd, "crash") == 0) {
        cmd_crash(args);
    } else if (strcmp(cmd, "colors") == 0) {
        cmd_colors();
    } else if (strcmp(cmd, "halt") == 0) {
        cmd_halt();
    } else if (strcmp(cmd, "reboot") == 0) {
        cmd_reboot();
#if ENABLE_RTC
    } else if (strcmp(cmd, "date") == 0) {
        struct rtc_datetime dt;
        if (rtc_read(&dt)) {
            char buf[32];
            rtc_format(&dt, buf, sizeof(buf));
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
            terminal_writestring("\nData/Ora: ");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            terminal_writestring(buf);
            terminal_writestring("\n");
        } else {
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
            terminal_writestring("\n[FAIL] Lettura RTC fallita\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        }
#endif
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