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
#include "mm/elf_manifest.h" // SECOS_NOTE_TYPE e flags manifest
#include "rtc.h"
#include "fb.h" // per framebuffer_info_t in fbinfo
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

// Definizione struttura dispatcher (anticipata per help dinamico)
typedef void (*shell_handler_t)(const char* args);
struct shell_cmd { const char* name; shell_handler_t handler; };

// Implementazioni comandi base (copiate da old/shell.c)
// La tabella dei comandi viene definita piu' avanti; per help dinamico abbiamo bisogno di conoscerla.
// Spostiamo la tabella prima della definizione di cmd_help.

// Forward declarations wrapper handler (definiti dopo)
static void sh_help(const char* a);
static void sh_clear(const char* a);
static void sh_echo(const char* a);
static void sh_info(const char* a);
static void sh_fontdump(const char* a);
static void sh_uptime(const char* a);
static void sh_sleep(const char* a);
static void sh_mem(const char* a);
static void sh_memtest(const char* a);
static void sh_memstress(const char* a);
static void sh_usertest(const char* a);
static void sh_elfload(const char* a);
static void sh_elfload2(const char* a);
static void sh_elfunload(const char* a);
static void sh_ps(const char* a);
static void sh_kill(const char* a);
static void sh_crash(const char* a);
static void sh_colors(const char* a);
static void sh_fbinfo(const char* a);
static void sh_color(const char* a);
static void sh_cursor(const char* a);
static void sh_dbuf(const char* a);
static void sh_halt(const char* a);
static void sh_reboot(const char* a);
static void sh_pinfo(const char* a);
static void sh_logo(const char* a);
#if ENABLE_RTC
static void sh_date(const char* a);
#endif

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
    {"kill",      sh_kill},
    {"pinfo",     sh_pinfo},
    {"ps",        sh_ps},
    {"crash",     sh_crash},
    {"colors",    sh_colors},
    {"fbinfo",    sh_fbinfo},
    {"color",     sh_color},
    {"cursor",    sh_cursor},
    {"dbuf",      sh_dbuf},
    {"logo",      sh_logo},
    {"fontdump",  sh_fontdump},
    {"halt",      sh_halt},
    {"reboot",    sh_reboot},
#if ENABLE_RTC
    {"date",      sh_date},
#endif
};

static void cmd_help(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("\nComandi disponibili (auto):\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    for (unsigned i=0;i<sizeof(shell_cmds)/sizeof(shell_cmds[0]); i++) {
        terminal_writestring("  ");
        terminal_writestring(shell_cmds[i].name);
        terminal_writestring("\n");
    }
    terminal_writestring("\nNuovi: fbinfo (info framebuffer), color <fg> <bg> (cambia colori).\n");
    terminal_writestring("Digita 'help' per aggiornare dopo ulteriori aggiunte.\n\n");
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

// Wrappers per adattare funzioni esistenti che non prendono args o hanno firma diversa
static void sh_help(const char* a){ (void)a; cmd_help(); }
static void sh_clear(const char* a){ (void)a; cmd_clear(); }
static void sh_info(const char* a){ (void)a; cmd_info(); }
static void sh_uptime(const char* a){ (void)a; cmd_uptime(); }
static void sh_mem(const char* a){ (void)a; cmd_mem(); }
static void sh_memtest(const char* a){ (void)a; cmd_memtest(); }
static void sh_memstress(const char* a){ (void)a; cmd_memstress(); }
static void sh_colors(const char* a){ (void)a; cmd_colors(); }
static void sh_fbinfo(const char* a){ (void)a; 
#if ENABLE_FB
    extern int fb_get_info(framebuffer_info_t* out);
    framebuffer_info_t info; 
    if (!fb_get_info(&info)) { terminal_writestring("[FBINFO] Framebuffer non inizializzato\n"); return; }
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("\n[FBINFO] Parametri Framebuffer:\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("  addr="); print_hex(info.addr);
    terminal_writestring(" virt="); print_hex(info.virt_addr);
    terminal_writestring(" pitch="); print_dec(info.pitch);
    terminal_writestring(" width="); print_dec(info.width);
    terminal_writestring(" height="); print_dec(info.height);
    terminal_writestring(" bpp="); print_dec(info.bpp);
    terminal_writestring(" type="); print_dec(info.type);
    terminal_writestring("\n");
    if (info.type == 1) { // RGB
        terminal_writestring("  RGB masks: R(size="); print_dec(info.red_mask_size); terminal_writestring(" pos="); print_dec(info.red_mask_pos);
        terminal_writestring(" G(size="); print_dec(info.green_mask_size); terminal_writestring(" pos="); print_dec(info.green_mask_pos);
        terminal_writestring(" B(size="); print_dec(info.blue_mask_size); terminal_writestring(" pos="); print_dec(info.blue_mask_pos); terminal_writestring(")\n");
    }
#else
    terminal_writestring("[FBINFO] Framebuffer disabilitato in configurazione\n");
#endif
}
// Implementazione comando color: color <fg> <bg> nomi o numeri
static int parse_color_token(const char* s){
    if(!s||!*s) return -1;
    int v=0; int digits=0; const char* p=s; while(*p>='0'&&*p<='9'){ v=v*10+(*p-'0'); p++; digits++; }
    if(digits>0 && *p=='\0') return v; // intero
    struct { const char* name; int val; } names[] = {
        {"black",0},{"blue",1},{"green",2},{"cyan",3},{"red",4},{"magenta",5},{"brown",6},{"grey",7},
        {"darkgrey",8},{"lightblue",9},{"lightgreen",10},{"lightcyan",11},{"lightred",12},{"lightmagenta",13},{"yellow",14},{"white",15}
    };
    for(unsigned k=0;k<sizeof(names)/sizeof(names[0]);k++){
        const char* n=names[k].name; const char* t=s; int eq=1; while(*n||*t){ if(*n!=*t){ eq=0; break; } if(!*n||!*t){ eq=0; break; } n++; t++; }
        if(eq) return names[k].val;
    }
    return -1;
}
static void sh_color(const char* args){
    while(*args==' ') args++;
    if(!*args){ terminal_writestring("Uso: color <fg> <bg> | color list | color <fg> <bg> clear\n"); return; }
    // Supporta 'list'
    if(args[0]=='l'&&args[1]=='i'&&args[2]=='s'&&args[3]=='t'&& (args[4]=='\0'||args[4]==' ')){
        terminal_writestring("Lista colori (fg/bg):\n");
        const char* names[]={"black","blue","green","cyan","red","magenta","brown","grey","darkgrey","lightblue","lightgreen","lightcyan","lightred","lightmagenta","yellow","white"};
        for(int i=0;i<16;i++){ terminal_writestring("  "); print_dec(i); terminal_writestring(" = "); terminal_writestring(names[i]); terminal_writestring("\n"); }
        return;
    }
    char fg_tok[16]; char bg_tok[16]; int i=0;
    while(*args && *args!=' ' && i<15){ fg_tok[i++]=*args++; } fg_tok[i]='\0';
    while(*args==' ') args++; i=0; while(*args && *args!=' ' && i<15){ bg_tok[i++]=*args++; } bg_tok[i]='\0';
    if(bg_tok[0]=='\0'){ terminal_writestring("Uso: color <fg> <bg>\n"); return; }
    int fg = parse_color_token(fg_tok);
    int bg = parse_color_token(bg_tok);
    if(fg<0||fg>15||bg<0||bg>15){ terminal_writestring("Colore non valido\n"); return; }
    // Registra come colore utente persistente
    extern void terminal_setcolor(uint8_t color); extern void terminal_restore_user_color(void);
    terminal_setcolor(vga_entry_color((enum vga_color)fg,(enum vga_color)bg));
    extern uint8_t user_fg; extern uint8_t user_bg; extern int user_color_set; user_fg=fg; user_bg=bg; user_color_set=1;
    // Opzionale 'clear' per ridisegnare sfondo
    while(*args==' ') args++;
    int do_clear = 0;
    if(args[0]=='c'&&args[1]=='l'&&args[2]=='e'&&args[3]=='a'&&args[4]=='r'&& (args[5]=='\0'||args[5]==' ')) do_clear=1;
    if(do_clear){
        // Se framebuffer attivo, pulisce
#if ENABLE_FB
        framebuffer_info_t info; if (fb_get_info(&info)) {
            extern void fb_clear(uint32_t color);
            // Traduce bg VGA in RGB dalla palette usata in fb_console (riusiamo la stessa logica locale)
            uint32_t vga_palette_local[16] = {
                0x000000,0x0000AA,0x00AA00,0x00AAAA,0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,
                0x555555,0x5555FF,0x55FF55,0x55FFFF,0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF
            };
            uint32_t rgb = vga_palette_local[bg & 0xF];
            fb_clear(rgb);
            // Dopo un clear lo sfondo è uniforme ma il cursore precedente può lasciare artefatti: ridisegna
            extern int fb_console_enable_cursor_blink(uint32_t timer_freq); extern void fb_console_disable_cursor_blink(void);
            // Forza ridisegno: disabilita e riabilita blink
            fb_console_disable_cursor_blink();
            fb_console_enable_cursor_blink(timer_get_frequency());
        }
#endif
    }
}
// Comando cursor on|off (solo underline blink per ora)
static void sh_cursor(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("Uso: cursor on|off\n"); return; }
    if(a[0]=='o' && a[1]=='n' && (a[2]=='\0' || a[2]==' ')){
        extern int fb_console_enable_cursor_blink(uint32_t timer_freq);
        if (fb_console_enable_cursor_blink(timer_get_frequency())==0) terminal_writestring("Cursor blink ON\n"); else terminal_writestring("[cursor] impossibile abilitarlo\n");
        return;
    }
    if(a[0]=='o' && a[1]=='f' && a[2]=='f' && (a[3]=='\0' || a[3]==' ')){
        extern void fb_console_disable_cursor_blink(void);
        fb_console_disable_cursor_blink();
        terminal_writestring("Cursor blink OFF\n");
        return;
    }
    terminal_writestring("Uso: cursor on|off\n");
}

// Comando dbuf on|off|flush
static void sh_dbuf(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("Uso: dbuf on|off|flush|auto|manual\n"); return; }
#if ENABLE_FB
    if(a[0]=='o'&&a[1]=='n'&&(a[2]=='\0'||a[2]==' ')){
        extern int fb_console_enable_dbuf(void); if(fb_console_enable_dbuf()==0) terminal_writestring("[dbuf] abilitato\n"); else terminal_writestring("[dbuf] FAIL alloc\n"); return; }
    if(a[0]=='o'&&a[1]=='f'&&a[2]=='f'&&(a[3]=='\0'||a[3]==' ')){
        extern void fb_console_disable_dbuf(void); fb_console_disable_dbuf(); terminal_writestring("[dbuf] disabilitato\n"); return; }
    if(a[0]=='f'&&a[1]=='l'&&a[2]=='u'&&a[3]=='s'&&a[4]=='h'&&(a[5]=='\0'||a[5]==' ')){
        extern void fb_console_flush(void); fb_console_flush(); terminal_writestring("[dbuf] flush\n"); return; }
    if(a[0]=='a'&&a[1]=='u'&&a[2]=='t'&&a[3]=='o'&&(a[4]=='\0'||a[4]==' ')){
        extern void fb_console_set_dbuf_auto(int on); fb_console_set_dbuf_auto(1); terminal_writestring("[dbuf] auto flush ON\n"); return; }
    if(a[0]=='m'&&a[1]=='a'&&a[2]=='n'&&a[3]=='u'&&a[4]=='a'&&a[5]=='l'&&(a[6]=='\0'||a[6]==' ')){
        extern void fb_console_set_dbuf_auto(int on); fb_console_set_dbuf_auto(0); terminal_writestring("[dbuf] auto flush OFF\n"); return; }
#endif
    terminal_writestring("Uso: dbuf on|off|flush|auto|manual\n");
}
// Comando fontdump <char>
static void sh_fontdump(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("Uso: fontdump <char>\n"); return; }
#if ENABLE_FB
    extern void fb_console_fontdump(char c); fb_console_fontdump(a[0]);
#else
    terminal_writestring("[fontdump] framebuffer non abilitato\n");
#endif
}
static void sh_logo(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("Uso: logo on|off|redraw\n"); return; }
#if ENABLE_FB
    if(a[0]=='o'&&a[1]=='n'&&(a[2]=='\0'||a[2]==' ')){
        extern void fb_console_draw_logo(void); fb_console_draw_logo();
        extern void fb_console_flush(void); fb_console_flush();
        terminal_writestring("[logo] ridisegnato\n"); return; }
    if(a[0]=='o'&&a[1]=='f'&&a[2]=='f'&&(a[3]=='\0'||a[3]==' ')){
        // Pulisci area logo (rectangle top-right) assumendo stessa dimensione usata nel draw
        framebuffer_info_t info; if(fb_get_info(&info)){
            uint8_t* base=(uint8_t*)(uint64_t)(info.virt_addr?info.virt_addr:info.addr);
            int letter_w=12, letter_h=20, spacing=4, len=5; int total_w=len*letter_w+(len-1)*spacing; int start_x=info.width - total_w - 8; int start_y=4; int w=total_w; int h=letter_h;
            uint8_t* target = base; extern int fb_console_enable_dbuf(void); /* no alloc here */
            for(int yy=0; yy<h; yy++){
                uint32_t* row=(uint32_t*)(target + (start_y+yy)*info.pitch);
                for(int xx=0; xx<w; xx++) row[start_x+xx]=0x000000;
            }
            extern void fb_console_flush(void); fb_console_flush();
        }
        terminal_writestring("[logo] nascosto\n"); return; }
    if(a[0]=='r'&&a[1]=='e'&&a[2]=='d'&&a[3]=='r'&&a[4]=='a'&&a[5]=='w'&&(a[6]=='\0'||a[6]==' ')){
        extern void fb_console_draw_logo(void); fb_console_draw_logo(); extern void fb_console_flush(void); fb_console_flush(); terminal_writestring("[logo] redraw\n"); return; }
#else
    terminal_writestring("[logo] framebuffer non abilitato\n"); return;
#endif
    terminal_writestring("Uso: logo on|off|redraw\n");
}
// Mappa nomi colori (lowercase) -> codice VGA
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
    // Costruzione ELF di test multi-segment con PT_NOTE manifest SECOS
    extern process_t* process_create_from_elf(const void* elf_buf, size_t size);
    size_t buf_size = 2560;
    unsigned char* elf_buf = (unsigned char*)kmalloc(buf_size);
    if (!elf_buf) { terminal_writestring("[ELFLOAD2] kmalloc fail\n"); return; }
    for (size_t i=0;i<buf_size;i++) elf_buf[i]=0;
    // Header ELF
    elf_buf[0]=0x7F; elf_buf[1]='E'; elf_buf[2]='L'; elf_buf[3]='F'; elf_buf[4]=2; elf_buf[5]=1; elf_buf[6]=1;
    *(uint16_t*)(elf_buf+16)=2; *(uint16_t*)(elf_buf+18)=0x3E; *(uint32_t*)(elf_buf+20)=1;
    *(uint64_t*)(elf_buf+24)=USER_CODE_BASE; // entry
    *(uint64_t*)(elf_buf+32)=64; // e_phoff
    *(uint16_t*)(elf_buf+52)=64; *(uint16_t*)(elf_buf+54)=56; *(uint16_t*)(elf_buf+56)=3; // 3 PHDR: code, data, note
    // PHDR0 CODE (RX) in USER_CODE_BASE
    *(uint32_t*)(elf_buf+64)=1; *(uint32_t*)(elf_buf+68)=PF_R|PF_X; *(uint64_t*)(elf_buf+72)=0x300; *(uint64_t*)(elf_buf+80)=USER_CODE_BASE; *(uint64_t*)(elf_buf+88)=USER_CODE_BASE; *(uint64_t*)(elf_buf+96)=0x100; *(uint64_t*)(elf_buf+104)=0x180; *(uint64_t*)(elf_buf+112)=0x1000;
    // PHDR1 DATA (RW) in USER_DATA_BASE (allineato) - usiamo memsz > filesz
    uint64_t data_vaddr = USER_DATA_BASE + 0x2000; // distanza per evitare conflitti future espansioni
    *(uint32_t*)(elf_buf+120)=1; *(uint32_t*)(elf_buf+124)=PF_R|PF_W; *(uint64_t*)(elf_buf+128)=0x500; *(uint64_t*)(elf_buf+136)=data_vaddr; *(uint64_t*)(elf_buf+144)=data_vaddr; *(uint64_t*)(elf_buf+152)=0x80; *(uint64_t*)(elf_buf+160)=0x200; *(uint64_t*)(elf_buf+168)=0x1000;
    // PHDR2 NOTE (non load) solo nel file
    *(uint32_t*)(elf_buf+176)=4; /* PT_NOTE */ *(uint32_t*)(elf_buf+180)=0; *(uint64_t*)(elf_buf+184)=0x700; *(uint64_t*)(elf_buf+192)=0; *(uint64_t*)(elf_buf+200)=0; *(uint64_t*)(elf_buf+208)=0x40; *(uint64_t*)(elf_buf+216)=0x40; *(uint64_t*)(elf_buf+224)=4; // p_align =4
    // Code (NOP) at 0x300
    for(int i=0;i<0x100;i++) elf_buf[0x300+i]=0x90;
    // Data pattern
    for(int i=0;i<0x80;i++) elf_buf[0x500+i]=0xAA;
    // NOTE layout: namesz(4) descsz(4) type(4) name padded, desc padded
    // name "SECOS\0" -> namesz=6 (include terminator), desc = elf_manifest_raw (size 24 bytes)
    uint32_t namesz=6; uint32_t descsz=24; uint32_t type=SECOS_NOTE_TYPE; // usa define
    *(uint32_t*)(elf_buf+0x700)=namesz; *(uint32_t*)(elf_buf+0x704)=descsz; *(uint32_t*)(elf_buf+0x708)=type;
    elf_buf[0x70C]='S'; elf_buf[0x70D]='E'; elf_buf[0x70E]='C'; elf_buf[0x70F]='O'; elf_buf[0x710]='S'; elf_buf[0x711]=0; // name
    // padding name up to multiple of 4: namesz=6 -> padded len = 8, bytes 0x712,0x713 già 0
    // desc (manifest)
    uint64_t manifest_off = 0x714; // aligned after name padding (0x70C + 8 = 0x714)
    // struct elf_manifest_raw { u32 version; u32 flags; u64 max_mem; u64 entry_hint; }
    *(uint32_t*)(elf_buf+manifest_off+0)=1; // version
    *(uint32_t*)(elf_buf+manifest_off+4)= MANIFEST_FLAG_REQUIRE_WX_BLOCK | MANIFEST_FLAG_REQUIRE_STACK_GUARD | MANIFEST_FLAG_REQUIRE_NX_DATA | MANIFEST_FLAG_REQUIRE_RX_CODE;
    *(uint64_t*)(elf_buf+manifest_off+8)= 64*1024; // max_mem 64KB
    *(uint64_t*)(elf_buf+manifest_off+16)= USER_CODE_BASE; // entry_hint
    // descsz 24 già indicato
    size_t used_size = 0x740; // fine area nota
    terminal_writestring("[ELFLOAD2] Carico ELF multi-segmento con manifest...\n");
    process_t* p = process_create_from_elf(elf_buf, used_size);
    kfree(elf_buf); // buffer non più necessario
    if(!p) terminal_writestring("[ELFLOAD2] Fallito\n"); else terminal_writestring("[ELFLOAD2] OK (process creato)\n");
    }
static void sh_elfunload(const char* a) { extern process_t* process_get_last(void); extern process_t* process_find_by_pid(uint32_t pid); extern int process_destroy(process_t* p); uint32_t pid=0; while(*a==' ') a++; while(*a>='0'&&*a<='9'){ pid=pid*10+(*a-'0'); a++; } process_t* target = pid? process_find_by_pid(pid): process_get_last(); if(!target) terminal_writestring("[ELFUNLOAD] processo non trovato\n"); else { int ur=process_destroy(target); if(ur==0) terminal_writestring("[ELFUNLOAD] OK (process distrutto)\n"); else terminal_writestring("[ELFUNLOAD] FAIL\n"); } }
static void sh_ps(const char* a){ (void)a; shell_ps_list(); }
static void sh_kill(const char* a){ extern process_t* process_find_by_pid(uint32_t pid); extern int process_destroy(process_t*); while(*a==' ') a++; if(!*a){ terminal_writestring("Uso: kill <pid>\n"); return; } uint32_t pid=0; while(*a>='0'&&*a<='9'){ pid=pid*10+(*a-'0'); a++; } process_t* t=process_find_by_pid(pid); if(!t){ terminal_writestring("[KILL] PID non trovato\n"); return; } int r=process_destroy(t); if(r==0) terminal_writestring("[KILL] OK\n"); else terminal_writestring("[KILL] FAIL\n"); }
// Helper per decodificare flags manifest
static void decode_manifest_flags(uint32_t f, char* out, size_t cap) {
    out[0]='\0';
    const struct { uint32_t bit; const char* name; } map[] = {
        { MANIFEST_FLAG_REQUIRE_WX_BLOCK, "WX_BLOCK" },
        { MANIFEST_FLAG_REQUIRE_STACK_GUARD, "STACK_GUARD" },
        { MANIFEST_FLAG_REQUIRE_NX_DATA, "NX_DATA" },
        { MANIFEST_FLAG_REQUIRE_RX_CODE, "RX_CODE" },
    };
    for (unsigned i=0;i<sizeof(map)/sizeof(map[0]);i++) {
        if (f & map[i].bit) {
            size_t len=0; while(out[len]) len++;
            if (len && len+1<cap) out[len++]='|';
            const char* s = map[i].name; while(*s && len+1<cap) out[len++]=*s++;
            if (len<cap) out[len]='\0';
        }
    }
    if (!out[0]) { // nessun flag
        const char* nf="NONE"; size_t i=0; while(nf[i] && i+1<cap){ out[i]=nf[i]; i++; } if (i<cap) out[i]='\0';
    }
}

static void sh_pinfo(const char* a){
    extern process_t* process_find_by_pid(uint32_t pid);
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("Uso: pinfo <pid>\n"); return; }
    uint32_t pid=0; while(*a>='0'&&*a<='9'){ pid=pid*10+(*a-'0'); a++; }
    process_t* p = process_find_by_pid(pid);
    if(!p){ terminal_writestring("[PINFO] PID non trovato\n"); return; }
    char hx[]="0123456789ABCDEF"; char buf[64];
    terminal_writestring("[PINFO] PID="); for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->pid>>i)&0xF]);
    terminal_writestring(" state="); const char* st="UNKNOWN"; switch(p->state){case PROC_NEW:st="NEW";break;case PROC_READY:st="READY";break;case PROC_RUNNING:st="RUN";break;case PROC_BLOCKED:st="BLK";break;case PROC_ZOMBIE:st="ZOMB";break;} terminal_writestring(st);
    terminal_writestring("\n  entry="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(p->entry>>i)&0xF]);
    terminal_writestring(" stack_top="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(p->stack_top>>i)&0xF]);
    terminal_writestring(" pages="); itoa(p->mapped_page_count, buf, 10); terminal_writestring(buf);
    uint64_t memkb = p->user_mem_bytes/1024ULL; itoa(memkb, buf, 10); terminal_writestring(" memKB="); terminal_writestring(buf);
    itoa(p->cpu_ticks, buf, 10); terminal_writestring(" cpuTicks="); terminal_writestring(buf);
    // Registri (snapshot)
    terminal_writestring("\n  RIP="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(p->regs.rip>>i)&0xF]);
    terminal_writestring(" RSP="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(p->regs.rsp>>i)&0xF]);
    terminal_writestring(" RFLAGS="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(p->regs.rflags>>i)&0xF]);
    // Manifest
    if(p->manifest){
        elf_manifest_t* mf = (elf_manifest_t*)p->manifest;
        terminal_writestring("\n  Manifest: version="); itoa(mf->version, buf, 10); terminal_writestring(buf);
        terminal_writestring(" max_mem="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(mf->max_mem>>i)&0xF]);
        terminal_writestring(" entry_hint="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(mf->entry_hint>>i)&0xF]);
        char fbuf[128]; decode_manifest_flags(mf->flags, fbuf, sizeof(fbuf));
        terminal_writestring(" flags="); terminal_writestring(fbuf);
    } else {
        terminal_writestring("\n  Manifest: <none>");
    }
    terminal_writestring("\n");
}

// (Tabella gia' definita sopra)

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
struct ps_ctx_global { int count; uint64_t total_pages; uint64_t total_cpu; int st_new, st_ready, st_run, st_blk, st_zomb; };
static void ps_cb_impl(process_t* p, void* user) {
    struct ps_ctx_global* ctx = (struct ps_ctx_global*)user;
    char hx[]="0123456789ABCDEF";
    uint64_t memkb = p->user_mem_bytes / 1024ULL;
    terminal_writestring("  PID="); for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->pid>>i)&0xF]);
    terminal_writestring(" entry="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(p->entry>>i)&0xF]);
    terminal_writestring(" pages="); for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->mapped_page_count>>i)&0xF]);
    terminal_writestring(" memKB="); char buf[32]; itoa(memkb,buf,10); terminal_writestring(buf);
    terminal_writestring(" cpuTicks="); itoa(p->cpu_ticks, buf, 10); terminal_writestring(buf);
    const char* st="UNKNOWN"; switch(p->state){case PROC_NEW:st="NEW";break;case PROC_READY:st="READY";break;case PROC_RUNNING:st="RUN";break;case PROC_BLOCKED:st="BLK";break;case PROC_ZOMBIE:st="ZOMB";break;} terminal_writestring(" state="); terminal_writestring(st); terminal_writestring("\n");
    ctx->count++; ctx->total_pages += p->mapped_page_count; ctx->total_cpu += p->cpu_ticks;
    switch(p->state){case PROC_NEW:ctx->st_new++;break;case PROC_READY:ctx->st_ready++;break;case PROC_RUNNING:ctx->st_run++;break;case PROC_BLOCKED:ctx->st_blk++;break;case PROC_ZOMBIE:ctx->st_zomb++;break;}
}

void shell_ps_list(void) {
    terminal_writestring("\n[PS] Processi attivi:\n");
    extern void process_foreach(void (*cb)(process_t*, void*), void* user);
    struct ps_ctx_global ctx; ctx.count=0; ctx.total_pages=0; ctx.total_cpu=0; ctx.st_new=ctx.st_ready=ctx.st_run=ctx.st_blk=ctx.st_zomb=0;
    process_foreach(ps_cb_impl, &ctx);
    if (ctx.count==0) {
        terminal_writestring("  <nessuno>\n");
    } else {
        terminal_writestring("[PS] Totale pagine="); char hx[]="0123456789ABCDEF"; for(int i=28;i>=0;i-=4) terminal_putchar(hx[(ctx.total_pages>>i)&0xF]);
        uint64_t tot_kb = ctx.total_pages * 4ULL; char buf[32]; itoa(tot_kb,buf,10); terminal_writestring(" memKB="); terminal_writestring(buf);
        terminal_writestring(" NEW="); itoa(ctx.st_new,buf,10); terminal_writestring(buf);
        terminal_writestring(" READY="); itoa(ctx.st_ready,buf,10); terminal_writestring(buf);
        terminal_writestring(" RUN="); itoa(ctx.st_run,buf,10); terminal_writestring(buf);
        terminal_writestring(" BLK="); itoa(ctx.st_blk,buf,10); terminal_writestring(buf);
        terminal_writestring(" ZOMB="); itoa(ctx.st_zomb,buf,10); terminal_writestring(buf);
        if (ctx.total_cpu) {
            terminal_writestring(" CPU%=(tick per proc / total via pinfo)");
        }
        terminal_writestring("\n");
    }
}