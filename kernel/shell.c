// SPDX-License-Identifier: MIT
/*
 * SecOS Kernel - Interactive Shell
 * Original Author: Luigi De Astis <l.deastis@idev-srl.com>
 * License: MIT
 */
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
#include "fs/ramfs.h" // RAMFS API
#include "fs/vfs.h" // VFS API
#include "driver_if.h" // driver space API
#include <stdint.h>
#include <stddef.h>

#define MAX_COMMAND_LEN 256
static char shell_cwd[RAMFS_NAME_MAX] = ""; // empty cwd = root
static void path_print_cwd(void){ terminal_writestring(shell_cwd[0]?"/":"/"); if(shell_cwd[0]) terminal_writestring(shell_cwd); terminal_writestring("\n"); }
static void path_resolve(const char* in, char* out){ if(in && in[0]=='/') in++; if(!in||!in[0]){ size_t i=0; while(shell_cwd[i]){ out[i]=shell_cwd[i]; i++; } out[i]=0; return; } const char* src=in; char temp[RAMFS_NAME_MAX]; size_t tp=0; if(shell_cwd[0]){ size_t i=0; while(shell_cwd[i]) temp[tp++]=shell_cwd[i++]; }
    char comp[RAMFS_NAME_MAX]; while(*src){ // salta duplicati '/'
        while(*src=='/') src++; size_t ci=0; while(src[0]&&src[0]!='/') comp[ci++]=*src++; comp[ci]=0; if(src[0]=='/') src++; if(ci==0) continue; if(comp[0]=='.'&&comp[1]==0){} else if(comp[0]=='.'&&comp[1]=='.'&&comp[2]==0){ if(tp){ int k=(int)tp-1; while(k>=0 && temp[k]!='/') k--; tp = (k>=0)? (size_t)k : 0; if(tp && temp[tp-1]=='/') tp--; } } else { if(tp && temp[tp-1]!='/') temp[tp++]='/'; for(size_t j=0;j<ci && tp<RAMFS_NAME_MAX-1;j++) temp[tp++]=comp[j]; } }
    // rimuovi eventuale trailing '/'
    if(tp>1 && temp[tp-1]=='/') tp--; temp[tp]=0; size_t k=0; while(temp[k]){ out[k]=temp[k]; k++; } out[k]=0; }

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

// Convert number to string
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

// Parse number from string
static uint32_t atoi(const char* str) {
    uint32_t result = 0;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result;
}
// Forward declarations of missing commands (ported from previous version)
static void cmd_help(void);
static void cmd_clear(void);
static void sh_help(const char* a); // forward wrapper
static void sh_clear(const char* a); // forward wrapper
static void execute_command(char* line); // forward dispatcher
static void shell_ps_list(void); // forward ps listing
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
static void sh_rfls(const char* a);
static void sh_rfcat(const char* a);
static void sh_rfinfo(const char* a);
static void sh_rfadd(const char* a);
static void sh_rfwrite(const char* a);
static void sh_rfdel(const char* a);
static void sh_rfmkdir(const char* a);
static void sh_rfrmdir(const char* a);
static void sh_rfcd(const char* a);
static void sh_rfpwd(const char* a);
static void sh_rftree(const char* a);
static void sh_rfusage(const char* a);
static void sh_rfmv(const char* a);
static void sh_rftruncate(const char* a);
static void sh_vls(const char* a); static void sh_vcat(const char* a); static void sh_vinfo(const char* a); static void sh_vpwd(const char* a); static void sh_vmount(const char* a);
static void sh_vcreate(const char* a); static void sh_vwrite(const char* a); static void sh_vtruncate2(const char* a);
static void sh_ext2mount(const char* a);
static void sh_drvreg(const char* a); static void sh_drvunreg(const char* a); static void sh_drvlog(const char* a); static void sh_drvinfo(const char* a);
static void sh_drvtest(const char* a);
#if ENABLE_RTC
static void sh_date(const char* a);
#endif

// Command dispatcher types
typedef void (*shell_handler_t)(const char* args);
struct shell_cmd { const char* name; shell_handler_t handler; };

// Pager subsystem (generic)
static int pager_enabled = 1;           // enabled by default
static unsigned pager_page_lines = 22;  // fits typical 25-line VGA (minus prompt/header)
static int pager_line_budget;           // remaining lines
static int pager_quit;                  // user aborted
static void sh_pager(const char* a);    // forward command handler
static void pager_begin(void){ if(pager_enabled){ pager_line_budget = (int)pager_page_lines; pager_quit=0; } else { pager_line_budget = 0x7FFFFFFF; pager_quit=0; } }
static int pager_should_stop(void){ return pager_quit; }
static void pager_prompt_more(void){
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("--More--(SPACE=page, ENTER=line, q=quit)");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    while(1){ char c = keyboard_getchar(); if(c=='q'||c=='Q'){ pager_quit=1; break; } if(c==' '){ pager_line_budget = (int)pager_page_lines; break; } if(c=='\n'){ pager_line_budget = 1; break; } }
    terminal_writestring("\n");
}
static void pager_print(const char* line){ if(pager_quit) return; terminal_writestring(line); terminal_writestring("\n"); if(--pager_line_budget <=0 && pager_enabled) pager_prompt_more(); }
static void pager_end(void){ (void)0; }
static void shell_print_help_paged(void); // forward after generic pager

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
    {"rfls",      sh_rfls},
    {"rfcat",     sh_rfcat},
    {"rfinfo",    sh_rfinfo},
    {"rfadd",     sh_rfadd},
    {"rfwrite",   sh_rfwrite},
    {"rfdel",     sh_rfdel},
    {"rfmkdir",   sh_rfmkdir},
    {"rfrmdir",   sh_rfrmdir},
    {"rfcd",      sh_rfcd},
    {"rfpwd",     sh_rfpwd},
    {"rftree",    sh_rftree},
    {"rfusage",   sh_rfusage},
    {"rfmv",      sh_rfmv},
    {"rftruncate", sh_rftruncate},
    {"vls",       sh_vls},
    {"vcat",      sh_vcat},
    {"vinfo",     sh_vinfo},
    {"vpwd",      sh_vpwd},
    {"vmount",    sh_vmount},
    {"vcreate",   sh_vcreate},
    {"vwrite",    sh_vwrite},
    {"vtruncate", sh_vtruncate2},
    {"ext2mount", sh_ext2mount},
    {"drvreg",    sh_drvreg},
    {"drvunreg",  sh_drvunreg},
    {"drvlog",    sh_drvlog},
    {"drvinfo",   sh_drvinfo},
    {"drvtest",   sh_drvtest},
    {"fontdump",  sh_fontdump},
    {"halt",      sh_halt},
    {"reboot",    sh_reboot},
    {"pager",    sh_pager},
#if ENABLE_RTC
    {"date",      sh_date},
#endif
};

static void cmd_help(void) { shell_print_help_paged(); }

static void shell_print_help_paged(void){
    pager_begin();
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    pager_print("");
    pager_print("Available commands:");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    for (unsigned i=0;i<sizeof(shell_cmds)/sizeof(shell_cmds[0]); i++) { if(pager_should_stop()) break; char namebuf[64]; int k=0; namebuf[k++]=' '; namebuf[k++]=' '; const char* n=shell_cmds[i].name; while(*n && k < (int)sizeof(namebuf)-1) namebuf[k++]=*n++; namebuf[k]=0; pager_print(namebuf); }
    if(!pager_should_stop()){
        pager_print("");
        pager_print("RAMFS: rfls rfcat rfinfo rfadd rfwrite rfdel rfmkdir rfrmdir rfcd rfpwd rftree rfusage rfmv rftruncate");
        pager_print("VFS: vls vcat vinfo vpwd vmount vcreate vwrite vtruncate");
        pager_print("Drivers: drvinfo drvreg drvunreg drvlog drvtest");
        pager_print("System: help clear info uptime sleep mem memtest memstress colors color fbinfo fontdump halt reboot crash");
        pager_print("Other: elfload elfload2 elfunload ps pinfo kill ext2mount usertest logo date (if enabled)");
        pager_print("");
        pager_print("Use 'pager off' to disable paging or 'pager lines N' to change page size.");
    }
    pager_end();
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

static void sh_pager(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("Pager is "); terminal_writestring(pager_enabled?"ON":"OFF"); terminal_writestring(", lines="); print_dec(pager_page_lines); terminal_writestring("\n"); return; }
    if(a[0]=='o'&&a[1]=='n'&& (a[2]==0||a[2]==' ')){ pager_enabled=1; terminal_writestring("Pager enabled\n"); return; }
    if(a[0]=='o'&&a[1]=='f'&&a[2]=='f'&& (a[3]==0||a[3]==' ')){ pager_enabled=0; terminal_writestring("Pager disabled\n"); return; }
    if(strncmp(a, "lines",5)==0){ a+=5; while(*a==' ') a++; unsigned v=0; while(*a>='0'&&*a<='9'){ v=v*10+(*a-'0'); a++; } if(v>=5 && v<=100){ pager_page_lines=v; terminal_writestring("Pager lines updated\n"); } else terminal_writestring("Invalid lines (5-100)\n"); return; }
    terminal_writestring("Usage: pager [on|off|lines <n>]\n");
}

static void cmd_clear(void) { terminal_initialize(); }

static void cmd_echo(const char* args) { if (*args) terminal_writestring(args); terminal_writestring("\n"); }

static void cmd_info(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("\n=== System Information ===\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("Name:        SecOS Kernel\n");
    terminal_writestring("Version:     0.2.0\n");
    terminal_writestring("Architecture: x86-64 (Long Mode)\n");
    terminal_writestring("Bootloader:  GRUB Multiboot\n");
    terminal_writestring("Timer:       PIT @ ");
    char freq_str[16]; itoa(timer_get_frequency(), freq_str, 10); terminal_writestring(freq_str); terminal_writestring(" Hz\n");
    terminal_writestring("Keyboard:    PS/2 Driver\n");
    terminal_writestring("Video:       VGA Text Mode 80x25\n\n");
}

static void cmd_mem(void) { terminal_writestring("\n"); pmm_print_stats(); heap_print_stats(); }

static void cmd_memtest(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("\nMemory allocation test...\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("Test 1: Allocating 256 bytes...\n");
    void* test_ptr = kmalloc(256);
    if (!test_ptr) { terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK)); terminal_writestring("  [FAIL] Allocation failed\n"); terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK)); return; }
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK)); terminal_writestring("  [OK] Allocated\n"); terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("Test 2: Freeing...\n"); kfree(test_ptr); terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK)); terminal_writestring("  [OK] Freed\n");
    terminal_writestring("\nTest 3: Allocating 5 blocks of 1KB each...\n"); void* blocks[5]; for(int i=0;i<5;i++){ blocks[i]=kmalloc(1024); if(!blocks[i]){ terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK)); terminal_writestring("  [FAIL] Allocation failed\n"); terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK)); return; } }
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK)); terminal_writestring("  [OK] All blocks allocated\n");
    terminal_writestring("Test 4: Freeing blocks...\n"); for(int i=0;i<5;i++) kfree(blocks[i]); terminal_writestring("  [OK] All blocks freed\n");
    terminal_writestring("\nTest completed successfully!\n\n"); terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
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
    terminal_writestring("Total ticks: "); itoa(ticks, buffer, 10); terminal_writestring(buffer); terminal_writestring("\n\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}
static void cmd_sleep(const char* args) {
    if (*args == '\0') {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    terminal_writestring("Usage: sleep <milliseconds>\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        return;
    }
    
    uint32_t ms = atoi(args);
    
    if (ms == 0 || ms > 10000) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    terminal_writestring("Invalid value (1-10000 ms)\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        return;
    }
    
    char buffer[16];
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("Waiting ");
    itoa(ms, buffer, 10);
    terminal_writestring(buffer);
    terminal_writestring(" ms...\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    timer_sleep_ms(ms);
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("Done!\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

static void cmd_colors(void) {
    terminal_writestring("\nVGA color test:\n");

    const char* color_names[] = {
        "Black", "Blue", "Green", "Cyan", "Red", "Magenta", "Brown", "Light grey",
        "Dark grey", "Light blue", "Light green", "Light cyan", 
        "Light red", "Light magenta", "Yellow", "White"
    };
    
    for (int i = 0; i < 16; i++) {
        terminal_setcolor(vga_entry_color(i, VGA_COLOR_BLACK));
        terminal_writestring(color_names[i]);
        terminal_writestring("  ");
    }
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("\n\n");
}

// (Old execute_command version removed; final implementation used at end of file)

static void cmd_reboot(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("\nSystem reboot...\n");
    
    // Reboot using keyboard controller
    uint8_t temp;
    __asm__ volatile ("cli");  // Disable interrupts
    
    do {
        __asm__ volatile ("inb $0x64, %0" : "=a"(temp));
    } while (temp & 0x02);  // Wait until buffer is empty
    
    __asm__ volatile ("outb %0, $0x64" : : "a"((uint8_t)0xFE));  // Pulse reset line
    
    __asm__ volatile ("hlt");
}

static void cmd_halt(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("\nSystem halt...\n\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("System halted safely.\n");
    terminal_writestring("You can power off the machine.\n\n");
    
    // Disabilita interrupt
    __asm__ volatile ("cli");
    
    // ACPI shutdown (funziona su QEMU e alcuni PC reali)
    // Prova prima con QEMU
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    
    // Se ACPI non funziona, entra in un loop HLT
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("(If the system does not power off automatically, press the power button)\n");
    
    while (1) {
        __asm__ volatile ("hlt");
    }
}

static void cmd_crash(const char* args) {
    // Strip leading spaces
    while (*args == ' ') args++;
    
    if (*args == '\0') {
        terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("\nUsage: crash <type>\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("Available types:\n");
        terminal_writestring("  div0  - Division by zero (INT 0)\n");
        terminal_writestring("  pf    - Page Fault (INT 14)\n");
        terminal_writestring("  gpf   - General Protection Fault (INT 13)\n");
        terminal_writestring("  df    - Double Fault (INT 8)\n");
        terminal_writestring("  inv   - Invalid Opcode (INT 6)\n\n");
        terminal_writestring("  stk   - Stack Fault (INT 12)\n\n");
        return;
    }
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    terminal_writestring("\n!!! WARNING: Intentional exception generation !!!\n\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    if (strcmp(args, "div0") == 0) {
    terminal_writestring("Generating Division by Zero...\n");
        // Usa assembly per evitare ottimizzazioni
        __asm__ volatile (
            "mov $5, %%eax\n"
            "xor %%ebx, %%ebx\n"
            "div %%ebx\n"
            ::: "eax", "ebx", "edx"
        );
    } else if (strcmp(args, "pf") == 0) {
    terminal_writestring("Generating Page Fault...\n");
        uint64_t* ptr = (uint64_t*)0x5000000;  // Fuori memoria mappata
        *ptr = 42;
    } else if (strcmp(args, "gpf") == 0) {
    terminal_writestring("Generating General Protection Fault...\n");
        // Carica un valore invalido nel registro DS
        __asm__ volatile (
            "mov $0x1234, %%ax\n"
            "mov %%ax, %%ds\n"
            ::: "ax"
        );
    } else if (strcmp(args, "df") == 0) {
    terminal_writestring("Generating Double Fault...\n");
    terminal_writestring("(Corrupting stack and causing exception)\n");
        // Simulazione: forziamo un'istruzione invalida dopo manipolazione registro generale (evita clobber rsp warning)
        __asm__ volatile ("xor %%eax, %%eax; ud2" ::: "eax", "memory");
    } else if (strcmp(args, "inv") == 0) {
    terminal_writestring("Generating Invalid Opcode...\n");
        __asm__ volatile ("ud2");  // Undefined instruction
    } else if (strcmp(args, "stk") == 0) {
    terminal_writestring("Generating Stack Fault...\n");
    // Simulated fault: force use of an unmapped page
    volatile uint64_t* p = (uint64_t*)0x10; // very low unmapped address in long mode
        *p = 0xDEADBEEF;
    } else {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    terminal_writestring("Invalid crash type!\n");
    terminal_writestring("Use 'crash' with no args to see supported types.\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    }
}

// Stress test heap: perform many allocations to test expansion and coalescing
static void cmd_memstress(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("\nStarting memstress (repeated allocations)...\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));

    const int small_count = 128; // small blocks
    const size_t small_size = 64;
    void* small_ptrs[small_count];

    // Small allocations to fragment
    int allocated_small = 0;
    for (int i=0; i<small_count; i++) {
        small_ptrs[i] = kmalloc(small_size);
        if (!small_ptrs[i]) break;
        allocated_small++;
    }
    terminal_writestring("[memstress] Small allocs: ");
    char buf[32];
    // Reuse itoa from above (itoa present globally) -> use base 10
    itoa(allocated_small, buf, 10);
    terminal_writestring(buf);
    terminal_writestring("\n");

    // Medium allocations to force expansion (512 bytes)
    const int mid_cap = 64;
    void* mid_ptrs[mid_cap];
    int mid_count = 0;
    for (int i=0; i<mid_cap; i++) {
        mid_ptrs[i] = kmalloc(512);
        if (!mid_ptrs[i]) break;
        mid_count++;
    }
    itoa(mid_count, buf, 10);
    terminal_writestring("[memstress] Medium allocs 512B: ");
    terminal_writestring(buf);
    terminal_writestring("\n");

    // Free alternating small blocks to test coalescing
    for (int i=0; i<allocated_small; i+=2) {
        kfree(small_ptrs[i]);
    }
    terminal_writestring("[memstress] Freed alternating small blocks\n");

    // Large allocations to push further expansion (2048 bytes)
    const int big_cap = 32;
    void* big_ptrs[big_cap];
    int big_count = 0;
    for (int i=0; i<big_cap; i++) {
        big_ptrs[i] = kmalloc(2048);
        if (!big_ptrs[i]) break;
        big_count++;
    }
    itoa(big_count, buf, 10);
    terminal_writestring("[memstress] Large allocs 2KB: ");
    terminal_writestring(buf);
    terminal_writestring("\n");

    // Free everything
    for (int i=1; i<allocated_small; i+=2) kfree(small_ptrs[i]); // free remaining small blocks
    for (int i=0; i<mid_count; i++) kfree(mid_ptrs[i]);
    for (int i=0; i<big_count; i++) kfree(big_ptrs[i]);
    terminal_writestring("[memstress] Freed all blocks\n");

    heap_print_stats();
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[memstress] Completed\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}


// Shell prompt
static void show_prompt(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("secos");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("$ ");
}

// Initialize shell
void shell_init(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("\n==================================\n");
    terminal_writestring("   Welcome to SecOS Shell!\n");
    terminal_writestring("==================================\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("\nType 'help' to see available commands.\n\n");
}

// Main shell loop
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

// Correct execute_command implementation
// --- Table-driven dispatcher ---

// Wrappers to adapt existing functions that do not take args or have different signatures
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
    if (!fb_get_info(&info)) { terminal_writestring("[FBINFO] Framebuffer not initialized\n"); return; }
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("\n[FBINFO] Framebuffer parameters:\n");
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
    terminal_writestring("[FBINFO] Framebuffer disabled in configuration\n");
#endif
}
// Implement color command: color <fg> <bg> names or numbers
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
    if(!*args){ terminal_writestring("Usage: color <fg> <bg> | color list | color <fg> <bg> clear\n"); return; }
    // Supporta 'list'
    if(args[0]=='l'&&args[1]=='i'&&args[2]=='s'&&args[3]=='t'&& (args[4]=='\0'||args[4]==' ')){
    terminal_writestring("Color list (fg/bg):\n");
        const char* names[]={"black","blue","green","cyan","red","magenta","brown","grey","darkgrey","lightblue","lightgreen","lightcyan","lightred","lightmagenta","yellow","white"};
        for(int i=0;i<16;i++){ terminal_writestring("  "); print_dec(i); terminal_writestring(" = "); terminal_writestring(names[i]); terminal_writestring("\n"); }
        return;
    }
    char fg_tok[16]; char bg_tok[16]; int i=0;
    while(*args && *args!=' ' && i<15){ fg_tok[i++]=*args++; } fg_tok[i]='\0';
    while(*args==' ') args++; i=0; while(*args && *args!=' ' && i<15){ bg_tok[i++]=*args++; } bg_tok[i]='\0';
    if(bg_tok[0]=='\0'){ terminal_writestring("Usage: color <fg> <bg>\n"); return; }
    int fg = parse_color_token(fg_tok);
    int bg = parse_color_token(bg_tok);
    if(fg<0||fg>15||bg<0||bg>15){ terminal_writestring("Invalid color\n"); return; }
    // Registra come colore utente persistente
    extern void terminal_setcolor(uint8_t color); extern void terminal_restore_user_color(void);
    terminal_setcolor(vga_entry_color((enum vga_color)fg,(enum vga_color)bg));
    extern uint8_t user_fg; extern uint8_t user_bg; extern int user_color_set; user_fg=fg; user_bg=bg; user_color_set=1;
    // Optional 'clear' to redraw background
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
// Command cursor on|off (underline blink only for now)
static void sh_cursor(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("Usage: cursor on|off\n"); return; }
    if(a[0]=='o' && a[1]=='n' && (a[2]=='\0' || a[2]==' ')){
        extern int fb_console_enable_cursor_blink(uint32_t timer_freq);
        if (fb_console_enable_cursor_blink(timer_get_frequency())==0) terminal_writestring("Cursor blink ON\n"); else terminal_writestring("[cursor] unable to enable\n");
        return;
    }
    if(a[0]=='o' && a[1]=='f' && a[2]=='f' && (a[3]=='\0' || a[3]==' ')){
        extern void fb_console_disable_cursor_blink(void);
        fb_console_disable_cursor_blink();
        terminal_writestring("Cursor blink OFF\n");
        return;
    }
    terminal_writestring("Usage: cursor on|off\n");
}

// Command dbuf on|off|flush
static void sh_dbuf(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("Usage: dbuf on|off|flush|auto|manual\n"); return; }
#if ENABLE_FB
    if(a[0]=='o'&&a[1]=='n'&&(a[2]=='\0'||a[2]==' ')){
        extern int fb_console_enable_dbuf(void); if(fb_console_enable_dbuf()==0) terminal_writestring("[dbuf] enabled\n"); else terminal_writestring("[dbuf] FAIL alloc\n"); return; }
    if(a[0]=='o'&&a[1]=='f'&&a[2]=='f'&&(a[3]=='\0'||a[3]==' ')){
        extern void fb_console_disable_dbuf(void); fb_console_disable_dbuf(); terminal_writestring("[dbuf] disabled\n"); return; }
    if(a[0]=='f'&&a[1]=='l'&&a[2]=='u'&&a[3]=='s'&&a[4]=='h'&&(a[5]=='\0'||a[5]==' ')){
        extern void fb_console_flush(void); fb_console_flush(); terminal_writestring("[dbuf] flush\n"); return; }
    if(a[0]=='a'&&a[1]=='u'&&a[2]=='t'&&a[3]=='o'&&(a[4]=='\0'||a[4]==' ')){
        extern void fb_console_set_dbuf_auto(int on); fb_console_set_dbuf_auto(1); terminal_writestring("[dbuf] auto flush ON\n"); return; }
    if(a[0]=='m'&&a[1]=='a'&&a[2]=='n'&&a[3]=='u'&&a[4]=='a'&&a[5]=='l'&&(a[6]=='\0'||a[6]==' ')){
        extern void fb_console_set_dbuf_auto(int on); fb_console_set_dbuf_auto(0); terminal_writestring("[dbuf] auto flush OFF\n"); return; }
#endif
    terminal_writestring("Usage: dbuf on|off|flush|auto|manual\n");
}
// Command fontdump <char>
static void sh_fontdump(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("Usage: fontdump <char>\n"); return; }
#if ENABLE_FB
    extern void fb_console_fontdump(char c); fb_console_fontdump(a[0]);
#else
    terminal_writestring("[fontdump] framebuffer not enabled\n");
#endif
}
static void sh_logo(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("Usage: logo on|off|redraw\n"); return; }
#if ENABLE_FB
    if(a[0]=='o'&&a[1]=='n'&&(a[2]=='\0'||a[2]==' ')){
        extern void fb_console_draw_logo(void); fb_console_draw_logo();
        extern void fb_console_flush(void); fb_console_flush();
        terminal_writestring("[logo] redrawn\n"); return; }
    if(a[0]=='o'&&a[1]=='f'&&a[2]=='f'&&(a[3]=='\0'||a[3]==' ')){
        // Clear logo area (rectangle top-right) assuming same dimension used in draw
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
        terminal_writestring("[logo] hidden\n"); return; }
    if(a[0]=='r'&&a[1]=='e'&&a[2]=='d'&&a[3]=='r'&&a[4]=='a'&&a[5]=='w'&&(a[6]=='\0'||a[6]==' ')){
        extern void fb_console_draw_logo(void); fb_console_draw_logo(); extern void fb_console_flush(void); fb_console_flush(); terminal_writestring("[logo] redraw\n"); return; }
#else
    terminal_writestring("[logo] framebuffer not enabled\n"); return;
#endif
    terminal_writestring("Usage: logo on|off|redraw\n");
}
// RAMFS: lista file
static void sh_rfls(const char* a){ while(*a==' ') a++; char abs[RAMFS_NAME_MAX]; if(*a) path_resolve(a,abs); else path_resolve("",abs); const ramfs_entry_t* arr[RAMFS_MAX_FILES]; size_t n; if(abs[0]==0){ n=ramfs_list_path("",arr,RAMFS_MAX_FILES); terminal_writestring("RAMFS root ("); } else { n=ramfs_list_path(abs,arr,RAMFS_MAX_FILES); terminal_writestring("RAMFS list '"); terminal_writestring(abs); terminal_writestring("' ("); } print_dec(n); terminal_writestring("):\n"); pager_begin(); for(size_t i=0;i<n;i++){ if(pager_should_stop()) break; const ramfs_entry_t* e=arr[i]; char line[RAMFS_NAME_MAX+32]; int k=0; line[k++]=' '; line[k++]=' '; const char* nm=e->name; while(*nm && k < (int)sizeof(line)-10) line[k++]=*nm++; if(e->flags & 2 && k < (int)sizeof(line)-2) { line[k++]='/'; line[k++]=' '; } else { line[k++]=' '; line[k++]=' '; } // size
        line[k]=0; terminal_writestring(line); print_dec(e->size); terminal_writestring(" bytes\n"); if(--pager_line_budget <=0 && pager_enabled && !pager_should_stop()) pager_prompt_more(); }
    pager_end(); }
// RAMFS: crea directory
static void sh_rfmkdir(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: rfmkdir <path>\n"); return; } char abs[RAMFS_NAME_MAX]; path_resolve(a,abs); if(ramfs_mkdir(abs)==0) terminal_writestring("[rfmkdir] OK\n"); else terminal_writestring("[rfmkdir] FAIL\n"); }
// RAMFS: rimuovi directory (vuota)
static void sh_rfrmdir(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: rfrmdir <path>\n"); return; } char abs[RAMFS_NAME_MAX]; path_resolve(a,abs); if(ramfs_rmdir(abs)==0) terminal_writestring("[rfrmdir] OK\n"); else terminal_writestring("[rfrmdir] FAIL (not empty / missing)\n"); }
// RAMFS: cat file
static void sh_rfcat(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: rfcat <name>\n"); return; } char abs[RAMFS_NAME_MAX]; path_resolve(a,abs); const ramfs_entry_t* e = ramfs_find(abs); if(!e){ terminal_writestring("[rfcat] file not found\n"); return; }
    for(size_t i=0;i<e->size;i++) terminal_putchar((char)e->data[i]);
    if(e->size==0) terminal_writestring("[rfcat] (empty)\n");
}
// RAMFS: info file
static void sh_rfinfo(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: rfinfo <name>\n"); return; } char abs[RAMFS_NAME_MAX]; path_resolve(a,abs); const ramfs_entry_t* e = ramfs_find(abs); if(!e){ terminal_writestring("[rfinfo] file not found\n"); return; }
    terminal_writestring("Name: "); terminal_writestring(e->name); terminal_writestring("\nSize: "); print_dec(e->size); terminal_writestring(" bytes\n");
    // Show first 32 bytes hex
    terminal_writestring("First bytes: "); size_t show = e->size < 32 ? e->size : 32; for(size_t i=0;i<show;i++){ uint8_t b=e->data[i]; char hx[]="0123456789ABCDEF"; terminal_putchar(hx[b>>4]); terminal_putchar(hx[b&0xF]); terminal_putchar(' '); } terminal_writestring("\n");
}
// RAMFS: aggiungi file (rfadd nome contenuto)
static void sh_rfadd(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: rfadd <name> <content>\n"); return; } char name[RAMFS_NAME_MAX]; size_t ni=0; while(a[0] && a[0]!=' ' && ni<RAMFS_NAME_MAX-1){ name[ni++]=*a++; } name[ni]=0; while(*a==' ') a++; if(name[0]==0){ terminal_writestring("[rfadd] empty name\n"); return; } if(!*a){ terminal_writestring("[rfadd] missing content\n"); return; } size_t len=0; while(a[len]) len++; char abs[RAMFS_NAME_MAX]; path_resolve(name,abs); if(ramfs_find(abs)){ terminal_writestring("[rfadd] already exists\n"); return; } if(ramfs_add(abs,a,len)==0){ terminal_writestring("[rfadd] OK\n"); } else terminal_writestring("[rfadd] FAIL\n"); }
// RAMFS: scrivi (rfwrite nome offset dati)
static void sh_rfwrite(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: rfwrite <name> <offset> <data>\n"); return; } char name[RAMFS_NAME_MAX]; size_t ni=0; while(a[0] && a[0]!=' ' && ni<RAMFS_NAME_MAX-1){ name[ni++]=*a++; } name[ni]=0; while(*a==' ') a++; if(name[0]==0){ terminal_writestring("[rfwrite] empty name\n"); return; } uint64_t off=0; if(*a<'0'||*a>'9'){ terminal_writestring("[rfwrite] missing offset\n"); return; } while(*a>='0'&&*a<='9'){ off = off*10 + (*a-'0'); a++; } while(*a==' ') a++; if(!*a){ terminal_writestring("[rfwrite] missing data\n"); return; } const char* data_str=a; size_t len=0; while(data_str[len]) len++; char abs[RAMFS_NAME_MAX]; path_resolve(name,abs); int written = ramfs_write(abs,(size_t)off,data_str,len); if(written>=0){ terminal_writestring("[rfwrite] wrote "); print_dec(written); terminal_writestring(" bytes\n"); } else terminal_writestring("[rfwrite] FAIL\n"); }
// RAMFS: elimina file (rfdel nome)
static void sh_rfdel(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: rfdel <name>\n"); return; } char name[RAMFS_NAME_MAX]; size_t ni=0; while(a[0] && a[0]!=' ' && ni<RAMFS_NAME_MAX-1){ name[ni++]=*a++; } name[ni]=0; char abs[RAMFS_NAME_MAX]; path_resolve(name,abs); if(ramfs_remove(abs)==0){ terminal_writestring("[rfdel] OK\n"); } else terminal_writestring("[rfdel] FAIL (immutable or missing)\n"); }
// RAMFS: rename
static void sh_rfmv(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: rfmv <old> <new>\n"); return; } char oldn[RAMFS_NAME_MAX]; size_t oi=0; while(a[0] && a[0]!=' ' && oi<RAMFS_NAME_MAX-1){ oldn[oi++]=*a++; } oldn[oi]=0; while(*a==' ') a++; if(!*a){ terminal_writestring("[rfmv] missing new name\n"); return; } char newn[RAMFS_NAME_MAX]; size_t ni=0; while(a[0] && a[0]!=' ' && ni<RAMFS_NAME_MAX-1){ newn[ni++]=*a++; } newn[ni]=0; char old_abs[RAMFS_NAME_MAX]; char new_abs[RAMFS_NAME_MAX]; path_resolve(oldn,old_abs); path_resolve(newn,new_abs); if(ramfs_rename(old_abs,new_abs)==0) terminal_writestring("[rfmv] OK\n"); else terminal_writestring("[rfmv] FAIL\n"); }
// RAMFS: truncate
static void sh_rftruncate(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: rftruncate <file> <size>\n"); return; } char name[RAMFS_NAME_MAX]; size_t ni=0; while(a[0] && a[0]!=' ' && ni<RAMFS_NAME_MAX-1){ name[ni++]=*a++; } name[ni]=0; while(*a==' ') a++; if(!*a){ terminal_writestring("[rftruncate] missing size\n"); return; } uint64_t sz=0; while(*a>='0'&&*a<='9'){ sz=sz*10+(*a-'0'); a++; } char abs[RAMFS_NAME_MAX]; path_resolve(name,abs); if(ramfs_truncate(abs,(size_t)sz)==0) terminal_writestring("[rftruncate] OK\n"); else terminal_writestring("[rftruncate] FAIL\n"); }
// RAMFS: cambia directory corrente
static void sh_rfcd(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: rfcd <path>\n"); return; } char abs[RAMFS_NAME_MAX]; path_resolve(a,abs); int d=ramfs_is_dir(abs); if(d==1){ size_t i=0; while(abs[i] && i<RAMFS_NAME_MAX-1){ shell_cwd[i]=abs[i]; i++; } shell_cwd[i]=0; terminal_writestring("[rfcd] OK\n"); } else if(d==0){ terminal_writestring("[rfcd] not a directory\n"); } else terminal_writestring("[rfcd] not found\n"); }
// RAMFS: mostra working directory
static void sh_rfpwd(const char* a){ (void)a; terminal_writestring("CWD: "); path_print_cwd(); }
// RAMFS: stampa albero ricorsivo
static void rftree_print(const char* path,int depth){ const ramfs_entry_t* arr[RAMFS_MAX_FILES]; size_t n = (path && path[0])? ramfs_list_path(path,arr,RAMFS_MAX_FILES) : ramfs_list_path("",arr,RAMFS_MAX_FILES); for(size_t i=0;i<n;i++){ if(pager_should_stop()) return; const ramfs_entry_t* e=arr[i]; const char* last=e->name; for(const char* p=e->name; *p; p++){ if(*p=='/') last=p+1; } char line[RAMFS_NAME_MAX+16]; int k=0; for(int d=0; d<depth && k < (int)sizeof(line)-4; d++){ line[k++]=' '; line[k++]=' '; line[k++]='|'; line[k++]=' '; } if(i+1<n){ line[k++]='|'; line[k++]='-'; line[k++]=' '; } else { line[k++]='`'; line[k++]='-'; line[k++]=' '; } const char* q=last; while(*q && k < (int)sizeof(line)-2) line[k++]=*q++; if(e->flags & 2 && k < (int)sizeof(line)-2) line[k++]='/'; line[k]=0; pager_print(line); if((e->flags & 2) && !pager_should_stop()) rftree_print(e->name, depth+1); } }
static void sh_rftree(const char* a){ while(*a==' ') a++; char abs[RAMFS_NAME_MAX]; if(*a) path_resolve(a,abs); else abs[0]=0; if(abs[0] && ramfs_is_dir(abs)!=1){ terminal_writestring("[rftree] not a directory\n"); return; } terminal_writestring("[rftree] tree:\n"); pager_begin(); rftree_print(abs,0); pager_end(); }
// RAMFS: uso totale
static void sh_rfusage(const char* a){ (void)a; const ramfs_entry_t* arr[RAMFS_MAX_FILES]; size_t n = ramfs_list(arr,RAMFS_MAX_FILES); size_t bytes=0; size_t files=0; size_t dirs=0; for(size_t i=0;i<n;i++){ if(arr[i]->flags & 2) dirs++; else { files++; bytes += arr[i]->size; } } terminal_writestring("[rfusage] files="); print_dec(files); terminal_writestring(" dirs="); print_dec(dirs); terminal_writestring(" total_bytes="); print_dec(bytes); terminal_writestring(" slots_used="); print_dec(n); terminal_writestring(" slots_free="); print_dec(RAMFS_MAX_FILES - n); terminal_writestring("\n"); }
// ---- VFS commands ----
static void vls_cb(const vfs_inode_t* child, void* user){
    (void)user;
    terminal_writestring("  ");
    terminal_writestring(child->path);
    if(child->type==VFS_NODE_DIR){
        terminal_writestring("/\n");
    } else {
        terminal_writestring("  ");
        print_dec(child->size);
        terminal_writestring(" bytes\n");
    }
}
static void sh_vls(const char* a){ while(*a==' ') a++; char path[256]; size_t pi=0; while(*a && pi<sizeof(path)-1) path[pi++]=*a++; path[pi]=0; if(pi==0){ path[0]='/'; path[1]=0; } extern int vfs_readdir(const char*, void(*)(const vfs_inode_t*, void*), void*); extern vfs_inode_t* vfs_lookup(const char*); vfs_inode_t* dir = vfs_lookup(path); if(dir && dir->type!=VFS_NODE_DIR){ terminal_writestring("[vls] not a directory\n"); return; } terminal_writestring("[vls] "); terminal_writestring(path); terminal_writestring("\n"); pager_begin(); vfs_readdir(path, vls_cb, NULL); pager_end(); }
static void sh_vcat(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: vcat <path>\n"); return; } extern vfs_inode_t* vfs_lookup(const char*); extern int vfs_read_all(const char*, void*, size_t); vfs_inode_t* ino=vfs_lookup(a); if(!ino || ino->type!=VFS_NODE_FILE){ terminal_writestring("[vcat] file not found\n"); return; } char buf[1024]; if(ino->size >= sizeof(buf)){ terminal_writestring("[vcat] file too large for buffer\n"); return; } int r=vfs_read_all(a,buf,sizeof(buf)); if(r<0){ terminal_writestring("[vcat] read fail\n"); return; } for(int i=0;i<r;i++) terminal_putchar(buf[i]); if(r==0) terminal_writestring("(empty)\n"); }
static void sh_vinfo(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: vinfo <path>\n"); return; } extern vfs_inode_t* vfs_lookup(const char*); vfs_inode_t* ino=vfs_lookup(a); if(!ino){ terminal_writestring("[vinfo] not found\n"); return; } terminal_writestring("Path: "); terminal_writestring(ino->path); terminal_writestring("\nType: "); terminal_writestring(ino->type==VFS_NODE_DIR?"DIR":"FILE"); terminal_writestring("\nSize: "); print_dec(ino->size); terminal_writestring(" bytes\n"); }
static void sh_vpwd(const char* a){ (void)a; terminal_writestring("(vpwd uses RAMFS CWD) "); path_print_cwd(); }
static void sh_vmount(const char* a){ (void)a; terminal_writestring("[vmount] root already mounted (RAMFS)\n"); }
static void sh_vcreate(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: vcreate <path> <content>\n"); return; } char name[256]; size_t ni=0; while(*a && *a!=' ' && ni<sizeof(name)-1){ name[ni++]=*a++; } name[ni]=0; while(*a==' ') a++; if(!*a){ terminal_writestring("[vcreate] missing content\n"); return; } const char* data=a; size_t len=0; while(data[len]) len++; extern int vfs_create(const char*, const void*, size_t); if(vfs_create(name,data,len)==0) terminal_writestring("[vcreate] OK\n"); else terminal_writestring("[vcreate] FAIL\n"); }
static void sh_vwrite(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: vwrite <path> <offset> <data>\n"); return; } char name[256]; size_t ni=0; while(*a && *a!=' ' && ni<sizeof(name)-1){ name[ni++]=*a++; } name[ni]=0; while(*a==' ') a++; if(*a<'0'||*a>'9'){ terminal_writestring("[vwrite] missing offset\n"); return; } size_t off=0; while(*a>='0'&&*a<='9'){ off=off*10+(*a-'0'); a++; } while(*a==' ') a++; if(!*a){ terminal_writestring("[vwrite] missing data\n"); return; } const char* data=a; size_t len=0; while(data[len]) len++; extern int vfs_write(const char*, size_t, const void*, size_t); int r=vfs_write(name,off,data,len); if(r>=0){ terminal_writestring("[vwrite] wrote "); print_dec(r); terminal_writestring(" bytes\n"); } else terminal_writestring("[vwrite] FAIL\n"); }
static void sh_vtruncate2(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: vtruncate <path> <size>\n"); return; } char name[256]; size_t ni=0; while(*a && *a!=' ' && ni<sizeof(name)-1){ name[ni++]=*a++; } name[ni]=0; while(*a==' ') a++; if(*a<'0'||*a>'9'){ terminal_writestring("[vtruncate] missing size\n"); return; } size_t sz=0; while(*a>='0'&&*a<='9'){ sz=sz*10+(*a-'0'); a++; } extern int vfs_truncate(const char*, size_t); if(vfs_truncate(name,sz)==0) terminal_writestring("[vtruncate] OK\n"); else terminal_writestring("[vtruncate] FAIL\n"); }
static void sh_ext2mount(const char* a){ (void)a; extern int ext2_mount(const char* dev_name); if(ext2_mount("ext2ram")==0) terminal_writestring("[ext2mount] EXT2 mounted as root (stub)\n"); else terminal_writestring("[ext2mount] mount failed\n"); }
// --- Driver space commands ---
static void sh_drvreg(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: drvreg <device_id>\n"); return; } int dev=0; while(*a>='0'&&*a<='9'){ dev = dev*10 + (*a-'0'); a++; } extern int driver_register_binding(process_t*, int); extern const device_desc_t* driver_get_device(int); extern process_t* sched_get_current(void); extern process_t* process_get_last(void);
    process_t* target = sched_get_current(); if(!target){ // fallback: last created process
        target = process_get_last();
        if(target){ terminal_writestring("[drvreg] (fallback: using last process, no current)\n"); }
    }
    if(!target){ terminal_writestring("[drvreg] no process: run 'elfload' first\n"); return; }
    if(!driver_get_device(dev)){ terminal_writestring("[drvreg] device not found\n"); return; }
    int r = driver_register_binding(target, dev); if(r==DRV_OK) terminal_writestring("[drvreg] OK\n"); else if(r==DRV_ERR_DEVICE) terminal_writestring("[drvreg] FAIL device\n"); else if(r==DRV_ERR_PERM) terminal_writestring("[drvreg] FAIL no slot\n"); else terminal_writestring("[drvreg] FAIL\n"); }
static void sh_drvunreg(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: drvunreg <device_id>\n"); return; } int dev=0; while(*a>='0'&&*a<='9'){ dev=dev*10+(*a-'0'); a++; } extern int driver_remove_binding(process_t*, int); extern process_t* sched_get_current(void); extern process_t* process_get_last(void); process_t* target = sched_get_current(); if(!target) target = process_get_last(); if(!target){ terminal_writestring("[drvunreg] no process (create with elfload)\n"); return; } int r=driver_remove_binding(target, dev); if(r==DRV_OK) terminal_writestring("[drvunreg] OK (removed)\n"); else if(r==DRV_ERR_DEVICE) terminal_writestring("[drvunreg] FAIL binding not found\n"); else terminal_writestring("[drvunreg] FAIL\n"); }
static void sh_drvlog(const char* a){
    extern int driver_audit_dump(driver_audit_entry_t*, int);
    int only_errors=0; int filter_dev=-1; int filter_op=-1; int limit=32;
    // Simple parsing: space separated tokens: errors dev=ID op=OP limit=N
    const char* s=a; while(*s==' ') s++; char token[32];
    while(*s){ while(*s==' ') s++; int ti=0; while(*s && *s!=' ' && ti< (int)sizeof(token)-1){ token[ti++]=*s++; } token[ti]=0; if(ti==0) break;
        if(strcmp(token,"errors")==0) only_errors=1; else if(strncmp(token,"dev=",4)==0){ filter_dev=0; const char* p=token+4; while(*p>='0'&&*p<='9'){ filter_dev = filter_dev*10 + (*p-'0'); p++; } }
        else if(strncmp(token,"op=",3)==0){ filter_op=0; const char* p=token+3; while(*p>='0'&&*p<='9'){ filter_op = filter_op*10 + (*p-'0'); p++; } }
        else if(strncmp(token,"limit=",6)==0){ limit=0; const char* p=token+6; while(*p>='0'&&*p<='9'){ limit = limit*10 + (*p-'0'); p++; } if(limit<=0) limit=32; if(limit>128) limit=128; }
    }
    if(limit>128) limit=128;
    driver_audit_entry_t buf[128]; int n = driver_audit_dump(buf, limit);
    if(n==0){ terminal_writestring("[drvlog] (empty)\n"); return; }
    terminal_writestring("[drvlog] events (filtered):\n");
    int shown=0; pager_begin();
    for(int i=0;i<n;i++){
        if(pager_should_stop()) break;
        driver_audit_entry_t* e=&buf[i];
        if(only_errors && e->result==DRV_OK) continue;
        if(filter_dev!=-1 && e->device_id!=filter_dev) continue;
        if(filter_op!=-1 && e->opcode!=filter_op) continue;
        terminal_writestring("  pid="); print_dec(e->pid);
        terminal_writestring(" dev="); print_dec(e->device_id);
        terminal_writestring(" op="); print_dec(e->opcode);
        terminal_writestring(" res="); print_dec(e->result);
        terminal_writestring(" tgt="); print_hex(e->target);
        terminal_writestring(" val="); print_hex(e->value);
        terminal_writestring(" flags="); print_hex(e->flags);
        terminal_writestring(" tick="); print_dec(e->tick);
        terminal_writestring("\n");
        shown++;
    }
    pager_end(); if(shown==0){ terminal_writestring("[drvlog] no events after filters\n"); }
}
static void sh_drvinfo(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: drvinfo <device_id>\n"); return; } int dev=0; while(*a>='0'&&*a<='9'){ dev=dev*10+(*a-'0'); a++; } extern const device_desc_t* driver_get_device(int); const device_desc_t* d = driver_get_device(dev); if(!d){ terminal_writestring("[drvinfo] device not found\n"); return; } terminal_writestring("[drvinfo] id="); print_dec(d->device_id); terminal_writestring(" reg_base="); print_hex(d->reg_base); terminal_writestring(" reg_size="); print_hex(d->reg_size); terminal_writestring(" mem_base="); print_hex(d->mem_base); terminal_writestring(" mem_size="); print_hex(d->mem_size); terminal_writestring(" caps="); print_hex(d->caps_mask); terminal_writestring("\n"); }
static void sh_drvtest(const char* a){ (void)a; extern void user_test_driver(void); user_test_driver(); }
// Mappa nomi colori (lowercase) -> codice VGA
static void sh_halt(const char* a){ (void)a; cmd_halt(); }
static void sh_reboot(const char* a){ (void)a; cmd_reboot(); }
#if ENABLE_RTC
static void sh_date(const char* a){ (void)a; struct rtc_datetime dt; if (rtc_read(&dt)) { char buf[32]; rtc_format(&dt, buf, sizeof(buf)); terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK)); terminal_writestring("\nDate/Time: "); terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK)); terminal_writestring(buf); terminal_writestring("\n"); } else { terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK)); terminal_writestring("\n[FAIL] RTC read failed\n"); terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK)); } }
#endif
static void sh_echo(const char* a){ cmd_echo(a); }
static void sh_sleep(const char* a){ cmd_sleep(a); }
static void sh_crash(const char* a){ cmd_crash(a); }

// Comandi speciali con logica propria non semplicemente wrapper
static void sh_usertest(const char* args) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK)); terminal_writestring("\nCreating test user space...\n"); terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        extern int vmm_map_user_code(uint64_t virt); extern int vmm_map_user_data(uint64_t virt); extern uint64_t vmm_alloc_user_stack(int pages); extern vmm_space_t* vmm_space_create_user(void); extern int vmm_switch_space(vmm_space_t* space); extern vmm_space_t* vmm_get_kernel_space(void);
    vmm_space_t* us = vmm_space_create_user(); if(!us){ terminal_writestring("[FAIL] user space creation\n"); } else {
            if (vmm_map_user_code(USER_CODE_BASE) != 0) terminal_writestring("[FAIL] code page\n"); else terminal_writestring("[OK] code page RX\n");
            if (vmm_map_user_data(USER_DATA_BASE) != 0) terminal_writestring("[FAIL] data page\n"); else terminal_writestring("[OK] data page RW/NX\n");
            uint64_t st = vmm_alloc_user_stack(4);
            terminal_writestring("[OK] user stack top="); char hx[]="0123456789ABCDEF"; for(int i=60;i>=0;i-=4) terminal_putchar(hx[(st>>i)&0xF]); terminal_writestring("\n[TEST] switching to user space...\n");
            if (vmm_switch_space(us)==0) terminal_writestring("[OK] switch user CR3\n"); else terminal_writestring("[FAIL] switch user\n");
            vmm_switch_space(vmm_get_kernel_space()); terminal_writestring("[OK] returned to kernel space\n"); }
}
static void sh_elfload(const char* a) {
    extern process_t* process_create_from_elf(const void* elf_buf, size_t size); unsigned char elf_buf[512]; for(int i=0;i<512;i++) elf_buf[i]=0; elf_buf[0]=0x7F; elf_buf[1]='E'; elf_buf[2]='L'; elf_buf[3]='F'; elf_buf[4]=2; elf_buf[5]=1; elf_buf[6]=1; *(uint16_t*)(elf_buf+16)=2; *(uint16_t*)(elf_buf+18)=0x3E; *(uint32_t*)(elf_buf+20)=1; *(uint64_t*)(elf_buf+24)=USER_CODE_BASE; *(uint64_t*)(elf_buf+32)=64; *(uint16_t*)(elf_buf+52)=64; *(uint16_t*)(elf_buf+54)=56; *(uint16_t*)(elf_buf+56)=1; *(uint32_t*)(elf_buf+64)=1; *(uint32_t*)(elf_buf+68)=PF_R|PF_X; *(uint64_t*)(elf_buf+72)=0x100ULL; *(uint64_t*)(elf_buf+80)=USER_CODE_BASE; *(uint64_t*)(elf_buf+88)=USER_CODE_BASE; *(uint64_t*)(elf_buf+96)=0x80ULL; *(uint64_t*)(elf_buf+104)=0x80ULL; *(uint64_t*)(elf_buf+112)=0x1000ULL; for(int i=0;i<0x80;i++) elf_buf[0x100+i]=0x90; terminal_writestring("[ELFLOAD] Loading test ELF...\n"); process_t* p = process_create_from_elf(elf_buf, sizeof(elf_buf)); if(!p) terminal_writestring("[ELFLOAD] Failed\n"); else terminal_writestring("[ELFLOAD] OK (process created)\n"); }
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
    terminal_writestring("[ELFLOAD2] Loading multi-segment ELF with manifest...\n");
    process_t* p = process_create_from_elf(elf_buf, used_size);
    kfree(elf_buf); // buffer no longer needed
    if(!p) terminal_writestring("[ELFLOAD2] Failed\n"); else terminal_writestring("[ELFLOAD2] OK (process created)\n");
    }
static void sh_elfunload(const char* a) { extern process_t* process_get_last(void); extern process_t* process_find_by_pid(uint32_t pid); extern int process_destroy(process_t* p); uint32_t pid=0; while(*a==' ') a++; while(*a>='0'&&*a<='9'){ pid=pid*10+(*a-'0'); a++; } process_t* target = pid? process_find_by_pid(pid): process_get_last(); if(!target) terminal_writestring("[ELFUNLOAD] process not found\n"); else { int ur=process_destroy(target); if(ur==0) terminal_writestring("[ELFUNLOAD] OK (process destroyed)\n"); else terminal_writestring("[ELFUNLOAD] FAIL\n"); } }
static void sh_ps(const char* a){ (void)a; pager_begin(); shell_ps_list(); pager_end(); }
static void sh_kill(const char* a){ extern process_t* process_find_by_pid(uint32_t pid); extern int process_destroy(process_t*); while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: kill <pid>\n"); return; } uint32_t pid=0; while(*a>='0'&&*a<='9'){ pid=pid*10+(*a-'0'); a++; } process_t* t=process_find_by_pid(pid); if(!t){ terminal_writestring("[KILL] PID not found\n"); return; } int r=process_destroy(t); if(r==0) terminal_writestring("[KILL] OK\n"); else terminal_writestring("[KILL] FAIL\n"); }
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
    if(!*a){ terminal_writestring("Usage: pinfo <pid>\n"); return; }
    uint32_t pid=0; while(*a>='0'&&*a<='9'){ pid=pid*10+(*a-'0'); a++; }
    process_t* p = process_find_by_pid(pid);
    if(!p){ terminal_writestring("[PINFO] PID not found\n"); return; }
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
    terminal_writestring("Command not found: "); terminal_writestring(cmd); terminal_writestring("\nType 'help' for the command list.\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

// API pubblica per esecuzione singola linea (init script)
void shell_run_line(const char* line){ if(!line) return; // copia mutabile
    char buf[MAX_COMMAND_LEN]; size_t i=0; while(line[i] && i<MAX_COMMAND_LEN-1){ buf[i]=line[i]; i++; } buf[i]=0; execute_command(buf); }

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

static void shell_ps_list(void) {
    terminal_writestring("\n[PS] Active processes:\n");
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