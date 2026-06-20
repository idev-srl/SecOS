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
#include "sched.h"   // [M30] reap/count for job control
#include "signal.h"  // [M30] foreground pgid + SIGCONT/SIGKILL
#include "elf.h" // PF_R PF_X
#include "mm/elf_manifest.h" // SECOS_NOTE_TYPE e flags manifest
#include "rtc.h"
#include "fb.h" // per framebuffer_info_t in fbinfo
#include "fs/ramfs.h" // RAMFS API
#include "fs/vfs.h" // VFS API
#include "fs/block.h" // [M22] block device introspection (blk/mountdev)
#include "net.h"       // [M24] networking (netinfo/ping)
#include "udp.h"       // [M24] UDP / DHCP / DNS
#include "tcp.h"       // [M24] TCP client
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
static void shell_print_help_compact(void); // forward
static void shell_print_help_long(void);    // forward
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
static void sh_poweroff(const char* a);  // [M31] ACPI poweroff
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
static void sh_blk(const char* a); static void sh_mountdev(const char* a);
static void sh_lspci(const char* a); static void sh_usbinfo(const char* a);
// [M23] POSIX-style commands over the VFS (with a real working directory).
static void sh_cd(const char* a); static void sh_pwd(const char* a); static void sh_ls(const char* a);
static void sh_cat(const char* a); static void sh_touch(const char* a); static void sh_mkdir(const char* a);
static void sh_rm(const char* a); static void sh_df(const char* a); static void sh_free(const char* a);
static void sh_uname(const char* a);
static void sh_netinfo(const char* a); static void sh_ping(const char* a);   // [M24]
static void sh_dhcp(const char* a); static void sh_nslookup(const char* a);  // [M24]
static void sh_udpsend(const char* a); static void sh_tcptest(const char* a);// [M24]
static void sh_nettest(const char* a);                                       // [M24]
const char* shell_get_cwd(void);   // [M23] current VFS working directory (for the prompt)
static void sh_run(const char* a);
static void sh_jobs(const char* a);     // [M30] list background/stopped jobs
static void sh_fg(const char* a);       // [M30] resume a job in the foreground
static void sh_bg(const char* a);       // [M30] resume a stopped job in background
static void sh_stress(const char* a);   // [M29] SMP CPU stress
static void sh_smp(const char* a);      // [M29] bring up secondary cores (opt-in)
static void sh_pkg(const char* a);      // [M32] signed package install
static void sh_verbose(const char* a);  // [M31] toggle kernel debug verbosity
static void sh_drvreg(const char* a); static void sh_drvunreg(const char* a); static void sh_drvlog(const char* a); static void sh_drvinfo(const char* a);
static void sh_drvtest(const char* a);
#if ENABLE_RTC
static void sh_date(const char* a);
#endif

// Command dispatcher types
typedef void (*shell_handler_t)(const char* args);
// [M31] desc != NULL → user-facing (shown in `help`); NULL → hidden dev/legacy
// command (still callable, e.g. from init.rc / selftests, just not advertised).
struct shell_cmd { const char* name; shell_handler_t handler; const char* desc; };

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

static const struct shell_cmd shell_cmds[] = {
    // --- user-facing (shown in `help`) ---
    {"help",      sh_help,      "list commands (help -l for descriptions)"},
    {"clear",     sh_clear,     "clear the screen"},
    {"echo",      sh_echo,      "print arguments"},
    {"ls",        sh_ls,        "list a directory"},
    {"cd",        sh_cd,        "change directory"},
    {"pwd",       sh_pwd,       "print working directory"},
    {"cat",       sh_cat,       "print a file"},
    {"touch",     sh_touch,     "create an empty file"},
    {"mkdir",     sh_mkdir,     "make a directory"},
    {"rm",        sh_rm,        "remove a file"},
    {"df",        sh_df,        "filesystem usage"},
    {"free",      sh_free,      "memory usage"},
    {"mem",       sh_mem,       "memory info"},
    {"ps",        sh_ps,        "list processes"},
    {"pinfo",     sh_pinfo,     "process details"},
    {"kill",      sh_kill,      "kill a process"},
    {"run",       sh_run,       "run a signed program"},
    {"jobs",      sh_jobs,      "list background/stopped jobs"},
    {"fg",        sh_fg,        "resume a job in the foreground"},
    {"bg",        sh_bg,        "resume a stopped job in background"},
    {"pkg",       sh_pkg,       "install a signed package (.spkg)"},
    {"stress",    sh_stress,    "CPU stress test"},
    {"smp",       sh_smp,       "bring up extra cores (experimental)"},
    {"uptime",    sh_uptime,    "time since boot"},
    {"sleep",     sh_sleep,     "sleep N seconds"},
    {"uname",     sh_uname,     "system name + build"},
    {"verbose",   sh_verbose,   "toggle kernel debug logs"},
    {"info",      sh_info,      "system information"},
    {"blk",       sh_blk,       "list block devices"},
    {"mountdev",  sh_mountdev,  "mount a block device"},
    {"lspci",     sh_lspci,     "list PCI devices (controllers)"},
    {"usbinfo",   sh_usbinfo,   "xHCI controller + port status"},
    {"netinfo",   sh_netinfo,   "network status"},
    {"ping",      sh_ping,      "ping a host"},
    {"dhcp",      sh_dhcp,      "acquire an IP via DHCP"},
    {"nslookup",  sh_nslookup,  "resolve a hostname"},
    {"nettest",   sh_nettest,   "TCP throughput test"},
    {"poweroff",  sh_poweroff,  "power off (ACPI soft-off)"},
    {"halt",      sh_halt,      "halt the CPU (no power-off)"},
    {"reboot",    sh_reboot,    "reboot the system"},
#if ENABLE_RTC
    {"date",      sh_date,      "current date/time"},
#endif
    // --- hidden dev/legacy (callable, not listed; desc = NULL) ---
    {"memtest",   sh_memtest,   0},
    {"memstress", sh_memstress, 0},
    {"usertest",  sh_usertest,  0},
    {"elfload",   sh_elfload,   0},
    {"elfload2",  sh_elfload2,  0},
    {"elfunload", sh_elfunload, 0},
    {"crash",     sh_crash,     0},
    {"colors",    sh_colors,    0},
    {"fbinfo",    sh_fbinfo,    0},
    {"color",     sh_color,     0},
    {"cursor",    sh_cursor,    0},
    {"dbuf",      sh_dbuf,      0},
    {"logo",      sh_logo,      0},
    {"fontdump",  sh_fontdump,  0},
    {"pager",     sh_pager,     0},
    {"rfls",      sh_rfls,      0},
    {"rfcat",     sh_rfcat,     0},
    {"rfinfo",    sh_rfinfo,    0},
    {"rfadd",     sh_rfadd,     0},
    {"rfwrite",   sh_rfwrite,   0},
    {"rfdel",     sh_rfdel,     0},
    {"rfmkdir",   sh_rfmkdir,   0},
    {"rfrmdir",   sh_rfrmdir,   0},
    {"rfcd",      sh_rfcd,      0},
    {"rfpwd",     sh_rfpwd,     0},
    {"rftree",    sh_rftree,    0},
    {"rfusage",   sh_rfusage,   0},
    {"rfmv",      sh_rfmv,      0},
    {"rftruncate", sh_rftruncate, 0},
    {"vls",       sh_vls,       0},
    {"vcat",      sh_vcat,      0},
    {"vinfo",     sh_vinfo,     0},
    {"vpwd",      sh_vpwd,      0},
    {"vmount",    sh_vmount,    0},
    {"vcreate",   sh_vcreate,   0},
    {"vwrite",    sh_vwrite,    0},
    {"vtruncate", sh_vtruncate2, 0},
    {"ext2mount", sh_ext2mount, 0},
    {"udpsend",   sh_udpsend,   0},
    {"tcptest",   sh_tcptest,   0},
    {"drvreg",    sh_drvreg,    0},
    {"drvunreg",  sh_drvunreg,  0},
    {"drvlog",    sh_drvlog,    0},
    {"drvinfo",   sh_drvinfo,   0},
    {"drvtest",   sh_drvtest,   0},
};

// [M31] Compact `help`: user-facing command names laid out in columns.
static void shell_print_help_compact(void){
    pager_begin();
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    pager_print("Commands (run a program by name, or 'help -l' for descriptions):");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    char line[128]; int pos=0, col=0;
    for (unsigned i=0;i<sizeof(shell_cmds)/sizeof(shell_cmds[0]); i++) {
        if(!shell_cmds[i].desc) continue;          // hidden command
        if(pager_should_stop()) break;
        const char* n=shell_cmds[i].name; int k=0;
        while(n[k] && pos<(int)sizeof(line)-2){ line[pos++]=n[k++]; }
        while(k<15 && pos<(int)sizeof(line)-2){ line[pos++]=' '; k++; } // pad to a column
        if(++col>=5){ line[pos]=0; pager_print(line); pos=0; col=0; }
    }
    if(col>0 && !pager_should_stop()){ line[pos]=0; pager_print(line); }
    pager_end();
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

// [M31] `help -l`: one command per line with a short description.
static void shell_print_help_long(void){
    pager_begin();
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    pager_print("Commands:");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    char line[128];
    for (unsigned i=0;i<sizeof(shell_cmds)/sizeof(shell_cmds[0]); i++) {
        if(!shell_cmds[i].desc) continue;
        if(pager_should_stop()) break;
        int pos=0; line[pos++]=' '; line[pos++]=' ';
        const char* n=shell_cmds[i].name; int k=0;
        while(n[k] && pos<(int)sizeof(line)-1){ line[pos++]=n[k++]; }
        while(k<11 && pos<(int)sizeof(line)-1){ line[pos++]=' '; k++; }
        const char* d=shell_cmds[i].desc;
        while(*d && pos<(int)sizeof(line)-1){ line[pos++]=*d++; }
        line[pos]=0; pager_print(line);
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
    extern void acpi_reboot(void);
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("\nSystem reboot...\n");
    // Tries the FADT reset register, 0xCF9, the 8042 pulse, then a triple fault.
    // The legacy 8042-only path used before did nothing on modern UEFI laptops
    // (no wired keyboard-controller reset line) — it just printed and hung.
    acpi_reboot();             // does not return
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
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK));
    terminal_writestring(":");
    terminal_writestring(shell_get_cwd());   // [M23] show the working directory
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
        } else if (c == 0x03) {            // [M25] Ctrl-C: cancel the current line
            terminal_writestring("^C\n");
            pos = 0;
            show_prompt();
        } else if (c == '\b') {
            if (pos > 0) {
                pos--;
                terminal_putchar('\b');
            }
        } else if (c >= 0x20 && c < 0x7f && pos < MAX_COMMAND_LEN - 1) {
            command[pos++] = c;
            terminal_putchar(c);
        }
    }
}

// Correct execute_command implementation
// --- Table-driven dispatcher ---

// Wrappers to adapt existing functions that do not take args or have different signatures
static void sh_help(const char* a){
    while(*a==' ') a++;
    if(a[0]=='-' && a[1]=='l') shell_print_help_long();
    else shell_print_help_compact();
}
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
static void sh_ext2mount(const char* a){ while(*a==' ') a++; const char* mp = (*a) ? a : "/mnt"; extern int ext2_mount(const char* dev_name, const char* mount_point); if(ext2_mount("vda", mp)==0){ terminal_writestring("[ext2mount] ext2/ext4 mounted at "); terminal_writestring(mp); terminal_writestring("\n"); } else terminal_writestring("[ext2mount] mount failed\n"); }
// [M22] blk: list every detected block device (virtio/AHCI/NVMe/USB) and probe
// sector 0 for a filesystem signature. Output goes to the shell (serial/FB), so
// it works on VMware where the debugcon driver markers are not visible.
static void sh_blk(const char* a){ (void)a;
    extern int block_count(void); extern block_dev_t* block_get(int);
    int n = block_count();
    if(n==0){ terminal_writestring("[blk] NO block devices detected (no virtio/SATA/NVMe/USB disk found)\n"); return; }
    static uint8_t s[4096];
    for(int i=0;i<n;i++){
        block_dev_t* d = block_get(i);
        if(!d) continue;
        terminal_writestring(d->name);
        terminal_writestring("  sectsz="); print_dec(d->sector_size);
        terminal_writestring(" sectors="); print_dec((uint64_t)d->sector_count);
        terminal_writestring(" ("); print_dec((uint64_t)(d->sector_count * (uint64_t)d->sector_size / (1024*1024)));
        terminal_writestring(" MB)  fs=");
        uint32_t cnt = (1536 + d->sector_size - 1) / d->sector_size;
        if(cnt * d->sector_size > sizeof(s)) cnt = sizeof(s) / d->sector_size;
        if(cnt==0) cnt=1;
        for(uint32_t k=0;k<sizeof(s);k++) s[k]=0;
        int rr = d->read(d, 0, s, cnt);
        if(rr<0){ terminal_writestring("[READ FAILED]\n"); continue; }
        const char* fs;
        if(s[450]==0xEE || (s[512]=='E'&&s[513]=='F'&&s[514]=='I'&&s[515]==' ')) fs="GPT (boot disk, skipped)";
        else if(s[1080]==0x53 && s[1081]==0xEF) fs="ext2/3/4";
        else if(s[510]==0x55 && s[511]==0xAA) fs="FAT";
        else fs="unknown/raw";
        terminal_writestring(fs); terminal_writestring("\n");
    }
}
// [M22] mountdev <dev> [mp]: try to mount a named device (FAT32 then ext2/4) at a
// mount point (default /mnt2 to avoid clashing with the boot /mnt). Diagnostic.
static void sh_mountdev(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("Usage: mountdev <dev> [mountpoint]\n  e.g. mountdev nvme0n1 /mnt2 ; then: vls /mnt2\n"); return; }
    char dev[32]; size_t i=0; while(*a && *a!=' ' && i<sizeof(dev)-1) dev[i++]=*a++; dev[i]=0;
    while(*a==' ') a++;
    const char* mp = (*a) ? a : "/mnt2";
    extern int fat32_mount(const char*, const char*); extern int ext2_mount(const char*, const char*);
    if(!block_find(dev)){ terminal_writestring("[mountdev] no such device: "); terminal_writestring(dev); terminal_writestring(" (run blk to list)\n"); return; }
    if(fat32_mount(dev, mp)==0){ terminal_writestring("[mountdev] FAT32 mounted "); terminal_writestring(dev); terminal_writestring(" -> "); terminal_writestring(mp); terminal_writestring("\n"); return; }
    if(ext2_mount(dev, mp)==0){ terminal_writestring("[mountdev] ext2/4 mounted "); terminal_writestring(dev); terminal_writestring(" -> "); terminal_writestring(mp); terminal_writestring("\n"); return; }
    terminal_writestring("[mountdev] mount FAILED (device not FAT32/extN, or already mounted there)\n");
}

// Print v as `digits` uppercase hex nibbles (no 0x), for compact diagnostics.
static void prhex(uint64_t v, int digits){
    char buf[17]; const char* hc="0123456789ABCDEF";
    if(digits>16) digits=16;
    for(int i=digits-1;i>=0;i--){ buf[i]=hc[v & 0xF]; v>>=4; }
    buf[digits]=0; terminal_writestring(buf);
}

// Friendly name for a PCI (class/subclass/progif) triple — enough to spot the
// storage/USB controllers we care about on real hardware.
static const char* pci_class_name(uint8_t c, uint8_t s, uint8_t p){
    if(c==0x0C && s==0x03){ if(p==0x30)return "USB xHCI"; if(p==0x20)return "USB EHCI"; if(p==0x10)return "USB OHCI"; if(p==0x00)return "USB UHCI"; return "USB"; }
    if(c==0x01 && s==0x06) return "SATA AHCI";
    if(c==0x01 && s==0x08) return "NVMe";
    if(c==0x01 && s==0x01) return "IDE";
    if(c==0x01 && s==0x05) return "ATA";
    if(c==0x08 && s==0x05) return "SD/eMMC host";
    if(c==0x02 && s==0x00) return "Ethernet";
    if(c==0x03)            return "Display";
    if(c==0x06)            return "Bridge";
    if(c==0x01)            return "Storage";
    return "";
}

// lspci: enumerate every PCI function (legacy CF8/CFC) and print its address,
// vendor:device, class and a friendly name. Output to the FB console so it works
// on real hardware with no serial — tells us which storage/USB controllers exist.
static void sh_lspci(const char* a){ (void)a;
    extern uint16_t pci_config_read16(uint8_t,uint8_t,uint8_t,uint8_t);
    extern uint8_t  pci_config_read8 (uint8_t,uint8_t,uint8_t,uint8_t);
    int found=0;
    for(uint32_t bus=0; bus<256; bus++){
        for(uint8_t slot=0; slot<32; slot++){
            if(pci_config_read16((uint8_t)bus,slot,0,0x00)==0xFFFF) continue;
            uint8_t nf = (pci_config_read8((uint8_t)bus,slot,0,0x0E)&0x80)?8:1;
            for(uint8_t fn=0; fn<nf; fn++){
                uint16_t v = pci_config_read16((uint8_t)bus,slot,fn,0x00);
                if(v==0xFFFF) continue;
                uint16_t d = pci_config_read16((uint8_t)bus,slot,fn,0x02);
                uint8_t pif=pci_config_read8((uint8_t)bus,slot,fn,0x09);
                uint8_t sub=pci_config_read8((uint8_t)bus,slot,fn,0x0A);
                uint8_t cls=pci_config_read8((uint8_t)bus,slot,fn,0x0B);
                print_dec(bus); terminal_writestring(":"); print_dec(slot); terminal_writestring("."); print_dec(fn);
                terminal_writestring("  "); prhex(v,4); terminal_writestring(":"); prhex(d,4);
                terminal_writestring("  cls="); prhex(cls,2); terminal_writestring("/"); prhex(sub,2); terminal_writestring("/"); prhex(pif,2);
                terminal_writestring("  "); terminal_writestring(pci_class_name(cls,sub,pif));
                terminal_writestring("\n");
                found++;
            }
        }
    }
    if(!found) terminal_writestring("[lspci] NO PCI devices found — legacy CF8/CFC config access may be unavailable on this firmware\n");
    else { terminal_writestring("[lspci] "); print_dec((uint64_t)found); terminal_writestring(" functions\n"); }
}

// usbinfo: report what the xHCI driver saw — controller presence, root ports, how
// many devices enumerated, and the LIVE PORTSC of each port. Lets us tell apart
// "no controller on PCI", "nothing connected", and "connected but enum failed".
static void sh_usbinfo(const char* a){ (void)a;
    extern int xhci_present(void); extern uint32_t xhci_num_ports(void);
    extern int xhci_ndev(void); extern uint32_t xhci_portsc_live(uint32_t);
    if(!xhci_present()){
        terminal_writestring("[usbinfo] xHCI NOT initialized: no controller found on PCI, or BAR/init failed.\n");
        terminal_writestring("          Run 'lspci' and look for a class 0C/03/30 (USB xHCI) function.\n");
        return;
    }
    uint32_t np = xhci_num_ports();
    terminal_writestring("[usbinfo] xHCI OK  ports="); print_dec((uint64_t)np);
    terminal_writestring("  enumerated_devices="); print_dec((uint64_t)xhci_ndev()); terminal_writestring("\n");
    for(uint32_t p=1;p<=np;p++){
        uint32_t sc = xhci_portsc_live(p);
        terminal_writestring("  port "); print_dec((uint64_t)p);
        terminal_writestring((sc&0x1)?"  CONNECTED":"  (empty)  ");
        if(sc&0x1){
            terminal_writestring(((sc>>1)&0x1)?" enabled":" NOT-enabled");
            terminal_writestring(" speed="); print_dec((uint64_t)((sc>>10)&0xF));
            terminal_writestring(((sc>>9)&0x1)?" pwr":" NOPWR");
            if((sc>>4)&0x1) terminal_writestring(" resetting");
        }
        terminal_writestring("  portsc="); prhex(sc,8); terminal_writestring("\n");
    }
}

// ===================== [M23] POSIX-style shell commands =====================
// A real VFS working directory (the legacy rf*/v* commands keep using absolute
// paths / the RAMFS cwd; these operate on the unified VFS: root, /mnt, /dev,
// /proc, /sys).
static char g_cwd[256] = "/";

// Resolve `in` against the working directory into a normalized absolute path
// (handles ".", "..", "//"). Empty `in` resolves to the cwd.
static void cwd_resolve(const char* in, char* out, size_t osz){
    char raw[512]; size_t r=0;
    if(in && in[0]=='/'){ raw[r++]='/'; }
    else { size_t i=0; while(g_cwd[i] && r<sizeof(raw)-1) raw[r++]=g_cwd[i++]; if(r==0) raw[r++]='/'; }
    if(in && in[0]){ if(r==0||raw[r-1]!='/'){ if(r<sizeof(raw)-1) raw[r++]='/'; } size_t i=0; while(in[i] && r<sizeof(raw)-1) raw[r++]=in[i++]; }
    raw[r]=0;
    char comps[24][64]; int nc=0; size_t i=0;
    while(raw[i]){
        while(raw[i]=='/') i++;
        if(!raw[i]) break;
        char c[64]; int k=0; while(raw[i] && raw[i]!='/' && k<63) c[k++]=raw[i++]; c[k]=0;
        if(k==1 && c[0]=='.') continue;
        if(k==2 && c[0]=='.' && c[1]=='.'){ if(nc>0) nc--; continue; }
        if(nc<24){ int j=0; while(c[j] && j<63){ comps[nc][j]=c[j]; j++; } comps[nc][j]=0; nc++; }
    }
    size_t o=0; out[o++]='/';
    for(int x=0;x<nc;x++){ if(o>1 && o<osz-1) out[o++]='/'; for(int j=0;comps[x][j] && o<osz-1;j++) out[o++]=comps[x][j]; }
    if(o>=osz) o=osz-1; out[o]=0;
}

const char* shell_get_cwd(void){ return g_cwd; }

static void sh_pwd(const char* a){ (void)a; terminal_writestring(g_cwd); terminal_writestring("\n"); }

static void sh_cd(const char* a){
    while(*a==' ') a++;
    char p[256]; cwd_resolve(*a?a:"/", p, sizeof(p));
    extern vfs_inode_t* vfs_lookup(const char*);
    vfs_inode_t* ino = vfs_lookup(p);
    if(!ino){ terminal_writestring("cd: no such file or directory: "); terminal_writestring(p); terminal_writestring("\n"); return; }
    if(ino->type!=VFS_NODE_DIR){ terminal_writestring("cd: not a directory: "); terminal_writestring(p); terminal_writestring("\n"); return; }
    size_t i=0; while(p[i] && i<sizeof(g_cwd)-1){ g_cwd[i]=p[i]; i++; } g_cwd[i]=0;
}

// readdir callback: print just the basename (with trailing '/' for dirs).
static void ls_cb(const vfs_inode_t* c, void* u){ (void)u;
    const char* b=c->path; for(const char* q=c->path;*q;q++) if(*q=='/') b=q+1;
    char line[280]; int k=0; for(int j=0;b[j] && k<260;j++) line[k++]=b[j];
    if(c->type==VFS_NODE_DIR && k<278) line[k++]='/';
    line[k]=0; pager_print(line);
}

static void sh_ls(const char* a){
    while(*a==' ') a++;
    char p[256]; cwd_resolve(a, p, sizeof(p));
    extern vfs_inode_t* vfs_lookup(const char*);
    extern int vfs_readdir(const char*, void(*)(const vfs_inode_t*, void*), void*);
    vfs_inode_t* ino = vfs_lookup(p);
    if(!ino){ terminal_writestring("ls: cannot access '"); terminal_writestring(p); terminal_writestring("'\n"); return; }
    if(ino->type!=VFS_NODE_DIR){ terminal_writestring(p); terminal_writestring("\n"); return; }
    pager_begin(); vfs_readdir(p, ls_cb, NULL); pager_end();
}

static void sh_cat(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("usage: cat <file>\n"); return; }
    char p[256]; cwd_resolve(a, p, sizeof(p));
    extern vfs_inode_t* vfs_lookup(const char*);
    vfs_inode_t* ino = vfs_lookup(p);
    if(!ino || ino->type!=VFS_NODE_FILE){ terminal_writestring("cat: "); terminal_writestring(p); terminal_writestring(": no such file\n"); return; }
    // Stream up to a sane cap directly via the FS read op (works for generated
    // /proc files and refuses to dump a whole disk).
    static char buf[1024];
    size_t off=0, cap=64*1024;
    while(off<cap){
        int r = ino->ops && ino->ops->read ? ino->ops->read(ino, off, buf, sizeof(buf)) : -1;
        if(r<=0) break;
        for(int i=0;i<r;i++) terminal_putchar(buf[i]);
        off += (size_t)r;
        if((size_t)r < sizeof(buf) && ino->size && off>=ino->size) break;
    }
}

static void sh_touch(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("usage: touch <file>\n"); return; }
    char p[256]; cwd_resolve(a, p, sizeof(p));
    extern vfs_inode_t* vfs_lookup(const char*); extern int vfs_create(const char*, const void*, size_t);
    if(vfs_lookup(p)) return;                       // exists: nothing to do
    if(vfs_create(p, "", 0)!=0){ terminal_writestring("touch: cannot create "); terminal_writestring(p); terminal_writestring("\n"); }
}

static void sh_mkdir(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("usage: mkdir <dir>\n"); return; }
    char p[256]; cwd_resolve(a, p, sizeof(p));
    extern int vfs_mkdir(const char*);
    if(vfs_mkdir(p)!=0){ terminal_writestring("mkdir: cannot create "); terminal_writestring(p); terminal_writestring("\n"); }
}

static void sh_rm(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("usage: rm <path>\n"); return; }
    char p[256]; cwd_resolve(a, p, sizeof(p));
    extern int vfs_remove(const char*);
    if(vfs_remove(p)!=0){ terminal_writestring("rm: cannot remove "); terminal_writestring(p); terminal_writestring("\n"); }
}

static void sh_df(const char* a){ (void)a;
    terminal_writestring("Type\tMounted on\n");
    int n=vfs_mount_count();
    for(int i=0;i<n;i++){ const char* mp=0,*fs=0; if(vfs_mount_info(i,&mp,&fs)) continue;
        terminal_writestring(fs?fs:"?"); terminal_writestring("\t");
        terminal_writestring(mp?mp:"?"); terminal_writestring("\n"); }
    terminal_writestring("\nBlock devices:\n");
    int bn=block_count();
    if(bn==0) terminal_writestring("  (none)\n");
    for(int i=0;i<bn;i++){ block_dev_t* b=block_get(i); if(!b) continue;
        terminal_writestring("  "); terminal_writestring(b->name); terminal_writestring("  ");
        print_dec(b->sector_count*(uint64_t)b->sector_size/(1024*1024)); terminal_writestring(" MB\n"); }
}

static void sh_free(const char* a){ (void)a;
    extern uint64_t pmm_get_total_memory(void); extern uint64_t pmm_get_used_memory(void); extern uint64_t pmm_get_free_memory(void);
    terminal_writestring("           total      used      free   (kB)\n");
    terminal_writestring("Mem:    ");
    print_dec(pmm_get_total_memory()/1024); terminal_writestring("   ");
    print_dec(pmm_get_used_memory()/1024);  terminal_writestring("   ");
    print_dec(pmm_get_free_memory()/1024);  terminal_writestring("\n");
}

static void sh_uname(const char* a){ (void)a; terminal_writestring("SecOS " GIT_HASH " x86_64\n"); }

// [M24] networking shell commands.
static void print_ip(uint32_t ip_net){    // ip_net in network byte order
    for(int b=0;b<4;b++){ print_dec((ip_net >> (8*b)) & 0xFF); if(b<3) terminal_writestring("."); }
}
static void sh_netinfo(const char* a){ (void)a;
    net_dev_t* d = net_primary();
    if(!d){ terminal_writestring("netinfo: no NIC\n"); return; }
    static const char hx[] = "0123456789abcdef";
    terminal_writestring(d->name); terminal_writestring("  MAC ");
    for(int i=0;i<6;i++){ terminal_putchar(hx[(d->mac[i]>>4)&0xF]); terminal_putchar(hx[d->mac[i]&0xF]); if(i<5) terminal_putchar(':'); }
    terminal_writestring("\n  IP ");   print_ip(d->ip);
    terminal_writestring("  GW ");      print_ip(d->gateway);
    terminal_writestring("  link ");    terminal_writestring(d->link_up?"up":"down");
    terminal_writestring("  rx ");
    terminal_writestring(d->irq_mode==NET_IRQ_MSIX?"msix(irq)":d->irq_mode==NET_IRQ_INTX?"intx(irq)":d->irq_mode==NET_IRQ_POLL?"poll":"none");
    terminal_writestring("\n");
}
static void sh_ping(const char* a){
    while(*a==' ') a++;
    net_dev_t* d = net_primary();
    if(!d){ terminal_writestring("ping: no NIC\n"); return; }
    uint32_t ip = d->gateway;   // default: gateway
    if(*a && *a>='0' && *a<='9'){   // parse a.b.c.d -> network byte order
        uint32_t parts[4]={0,0,0,0}; int pi=0;
        while(*a && pi<4){ uint32_t v=0; while(*a>='0'&&*a<='9'){ v=v*10+(*a-'0'); a++; } parts[pi++]=v&0xFF; if(*a=='.') a++; }
        ip = parts[0] | (parts[1]<<8) | (parts[2]<<16) | (parts[3]<<24);
    }
    // Optional count after the IP (default 4). "ping <ip> <count>".
    while(*a==' ') a++;
    int count=4; if(*a>='0'&&*a<='9'){ count=0; while(*a>='0'&&*a<='9'){ count=count*10+(*a-'0'); a++; } }
    if(count<1) count=1; if(count>100) count=100;

    terminal_writestring("PING "); print_ip(ip); terminal_writestring("  ("); print_dec((uint32_t)count); terminal_writestring(" packets)\n");
    uint64_t tpu = net_tsc_per_us();                 // cycles per microsecond
    uint64_t mn=~0ULL, mx=0, sum=0; int got=0;
    for(int i=0;i<count;i++){
        uint64_t rtt=0;
        if(net_ping_rtt(ip, (uint16_t)(i+1), &rtt)==0){
            uint64_t us = rtt / tpu;                  // cycles -> microseconds
            got++; sum+=us; if(us<mn) mn=us; if(us>mx) mx=us;
            terminal_writestring("  reply seq="); print_dec((uint32_t)(i+1));
            terminal_writestring(" time="); print_dec((uint32_t)(us/1000)); terminal_writestring(".");
            uint32_t frac=(uint32_t)(us%1000); // 3-digit ms fraction
            if(frac<100) terminal_writestring("0"); if(frac<10) terminal_writestring("0");
            print_dec(frac); terminal_writestring(" ms\n");
        } else {
            terminal_writestring("  seq="); print_dec((uint32_t)(i+1)); terminal_writestring(" timeout\n");
        }
    }
    terminal_writestring("--- "); print_ip(ip); terminal_writestring(" statistics ---\n");
    terminal_writestring("  "); print_dec((uint32_t)count); terminal_writestring(" sent, ");
    print_dec((uint32_t)got); terminal_writestring(" received, ");
    print_dec((uint32_t)(count-got)); terminal_writestring(" lost\n");
    if(got){
        uint64_t avg=sum/got;
        terminal_writestring("  rtt min/avg/max = ");
        print_dec((uint32_t)(mn/1000)); terminal_writestring("."); { uint32_t f=(uint32_t)(mn%1000); if(f<100)terminal_writestring("0"); if(f<10)terminal_writestring("0"); print_dec(f); }
        terminal_writestring(" / ");
        print_dec((uint32_t)(avg/1000)); terminal_writestring("."); { uint32_t f=(uint32_t)(avg%1000); if(f<100)terminal_writestring("0"); if(f<10)terminal_writestring("0"); print_dec(f); }
        terminal_writestring(" / ");
        print_dec((uint32_t)(mx/1000)); terminal_writestring("."); { uint32_t f=(uint32_t)(mx%1000); if(f<100)terminal_writestring("0"); if(f<10)terminal_writestring("0"); print_dec(f); }
        terminal_writestring(" ms\n");
    }
}
// Parse "a.b.c.d" -> network-order uint32 (octet0 at LSB). Returns 0 on parse.
static int parse_ip(const char* a, uint32_t* out){
    uint32_t parts[4]={0,0,0,0}; int pi=0;
    while(*a==' ') a++;
    while(*a && pi<4){ if(*a<'0'||*a>'9') break; uint32_t v=0; while(*a>='0'&&*a<='9'){ v=v*10+(*a-'0'); a++; } parts[pi++]=v&0xFF; if(*a=='.') a++; }
    if(pi!=4) return -1;
    *out = parts[0] | (parts[1]<<8) | (parts[2]<<16) | (parts[3]<<24);
    return 0;
}
static void sh_dhcp(const char* a){ (void)a;
    net_dev_t* d = net_primary();
    if(!d){ terminal_writestring("dhcp: no NIC\n"); return; }
    terminal_writestring("dhcp: requesting lease...\n");
    if(dhcp_configure(d)==0){
        terminal_writestring("dhcp: OK  IP "); print_ip(d->ip);
        terminal_writestring("  GW "); print_ip(d->gateway);
        terminal_writestring("  DNS "); print_ip(d->dns); terminal_writestring("\n");
    } else terminal_writestring("dhcp: no lease (timeout)\n");
}
static void sh_nslookup(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("Usage: nslookup <hostname>\n"); return; }
    net_dev_t* d = net_primary();
    if(!d){ terminal_writestring("nslookup: no NIC\n"); return; }
    if(!d->dns){ terminal_writestring("nslookup: no DNS server (run dhcp)\n"); return; }
    uint32_t ip=0;
    if(dns_resolve(d, a, &ip)==0){
        terminal_writestring(a); terminal_writestring(" -> "); print_ip(ip); terminal_writestring("\n");
    } else { terminal_writestring("nslookup: not resolved\n"); }
}
static void sh_udpsend(const char* a){
    // Usage: udpsend <ip> <port> <text>
    while(*a==' ') a++;
    uint32_t ip; if(parse_ip(a,&ip)!=0){ terminal_writestring("Usage: udpsend <ip> <port> <text>\n"); return; }
    while(*a && *a!=' ') a++; while(*a==' ') a++;          // skip ip
    uint32_t port=0; while(*a>='0'&&*a<='9'){ port=port*10+(*a-'0'); a++; }
    while(*a==' ') a++;                                     // text starts here
    net_dev_t* d = net_primary();
    if(!d){ terminal_writestring("udpsend: no NIC\n"); return; }
    uint16_t sport = udp_ephemeral_port();
    int n=0; const char* t=a; while(t[n]) n++;
    if(udp_send(d, ip, sport, (uint16_t)port, a, (uint32_t)n)==0)
        terminal_writestring("udpsend: sent\n");
    else terminal_writestring("udpsend: failed (ARP pending? retry)\n");
}
// tcptest <ip> <port> [host] — open a TCP connection, send an HTTP GET, print
// the first chunk of the reply. With no host, uses the dotted IP as Host:.
static void sh_tcptest(const char* a){
    while(*a==' ') a++;
    uint32_t ip; if(parse_ip(a,&ip)!=0){ terminal_writestring("Usage: tcptest <ip> <port> [path]\n"); return; }
    while(*a && *a!=' ') a++; while(*a==' ') a++;
    uint32_t port=0; while(*a>='0'&&*a<='9'){ port=port*10+(*a-'0'); a++; }
    if(port==0) port=80;
    while(*a==' ') a++;
    const char* path = *a ? a : "/";          // 3rd arg = path (consistent with nettest)
    net_dev_t* d = net_primary();
    if(!d){ terminal_writestring("tcptest: no NIC\n"); return; }
    terminal_writestring("tcptest: connecting to "); print_ip(ip); terminal_writestring("...\n");
    tcp_conn_t* c = tcp_connect(d, ip, (uint16_t)port);
    if(!c){ terminal_writestring("tcptest: connect failed\n"); return; }
    terminal_writestring("tcptest: GET "); terminal_writestring(path); terminal_writestring("\n");
    static char req[256]; int rl=0;
    const char* g="GET ";
    for(const char* p=g; *p; p++) req[rl++]=*p;
    for(const char* p=path; *p && rl<180; p++) req[rl++]=*p;
    const char* e=" HTTP/1.0\r\nHost: secos\r\nConnection: close\r\n\r\n";
    for(const char* p=e; *p; p++) req[rl++]=*p;
    tcp_send_all(c, req, (uint32_t)rl);
    static char buf[1024]; int total=0;
    for(;;){
        int n = tcp_recv_block(c, buf, sizeof(buf)-1, 3000);
        if(n<=0) break;
        buf[n]=0; terminal_writestring(buf); total+=n;
        if(total>8000) break;
    }
    terminal_writestring("\ntcptest: received "); print_dec((uint32_t)total); terminal_writestring(" bytes\n");
    tcp_close(c);
}
// nettest <ip> <port> [path] — TCP throughput: GET a (large) resource, drain ALL
// of it to EOF without printing the body, and report bytes / time / KB-s. Serve a
// big file on the host, e.g. `python3 -m http.server`, then `nettest <host> 8000 /big.bin`.
static void sh_nettest(const char* a){
    while(*a==' ') a++;
    uint32_t ip; if(parse_ip(a,&ip)!=0){ terminal_writestring("Usage: nettest <ip> <port> [path]\n"); return; }
    while(*a && *a!=' ') a++; while(*a==' ') a++;
    uint32_t port=0; while(*a>='0'&&*a<='9'){ port=port*10+(*a-'0'); a++; }
    if(port==0) port=80;
    while(*a==' ') a++;
    const char* path = *a ? a : "/";
    net_dev_t* d = net_primary();
    if(!d){ terminal_writestring("nettest: no NIC\n"); return; }
    extern uint64_t timer_get_ticks(void);

    terminal_writestring("nettest: GET "); terminal_writestring(path);
    terminal_writestring(" from "); print_ip(ip); terminal_writestring("\n");
    tcp_conn_t* c = tcp_connect(d, ip, (uint16_t)port);
    if(!c){ terminal_writestring("nettest: connect failed\n"); return; }
    static char req[256]; int rl=0;
    const char* g="GET ";
    for(const char* p=g; *p; p++) req[rl++]=*p;
    for(const char* p=path; *p && rl<180; p++) req[rl++]=*p;
    const char* h=" HTTP/1.0\r\nHost: secos\r\nConnection: close\r\n\r\n";
    for(const char* p=h; *p; p++) req[rl++]=*p;

    uint64_t t0 = timer_get_ticks();
    tcp_send_all(c, req, (uint32_t)rl);
    static char buf[2048];
    uint64_t total=0;
    for(;;){
        int n = tcp_recv_block(c, buf, sizeof(buf), 5000);   // 5 s idle timeout
        if(n<=0) break;
        total += (uint64_t)n;
    }
    uint64_t ms = timer_get_ticks() - t0;                    // ~ms at 1 kHz
    tcp_close(c);

    terminal_writestring("nettest: "); print_dec((uint32_t)total); terminal_writestring(" bytes in ");
    print_dec((uint32_t)ms); terminal_writestring(" ms");
    if(ms>0){
        // KB/s = bytes/1024 / (ms/1000) = bytes*1000 / (ms*1024)
        uint64_t kbs = (total*1000ULL) / (ms*1024ULL);
        uint64_t mbits = (total*8ULL) / (ms*125ULL);         // megabit/s = bytes*8 / (ms*1000) *1000 ... = bytes*8/(ms*125)
        terminal_writestring(" -> "); print_dec((uint32_t)kbs); terminal_writestring(" KB/s (~");
        print_dec((uint32_t)mbits); terminal_writestring(" Mbit/s)");
    }
    terminal_writestring("\n");
}
// =========================================================================

// [M10] run <path>: load+signature-verify a signed ELF from the VFS, spawn it
// ring-3 and wait for it to exit. Works because the shell runs as the scheduler
// [M29] stress: launch N CPU-bound ring-3 spinners at once to saturate the
// vCPUs. Each process is pinned round-robin to an online CPU (cpu_affinity), so
// the cores run them in parallel — watch the host CPU graph or the per-CPU
// [SMP] cpu=<idx> run markers on debugcon. The embedded spinner is signed.
static void sh_stress(const char* a){
    #include "../crypto/user_spin_elf.h"
    extern process_t* process_create_from_elf_args(const void*, size_t, int, const char* const*);
    extern int sched_count_alive_user(void);
    extern void sched_reap_zombies(void);
    extern uint32_t smp_online_count(void);
    while(*a==' ') a++;
    int n=0; while(*a>='0'&&*a<='9'){ n=n*10+(*a-'0'); a++; }
    if(n<=0) n = (int)smp_online_count() * 2;   // default: 2x the online cores
    if(n<1) n=2; if(n>32) n=32;                 // bounded (proc table is 32)
    const char* av[1] = {"spin"};
    int base = sched_count_alive_user();
    int spawned=0;
    for(int i=0;i<n;i++){
        process_t* p = process_create_from_elf_args(user_spin_elf, user_spin_elf_len, 1, av);
        if(p) spawned++;
    }
    terminal_writestring("[stress] launched "); print_dec(spawned);
    terminal_writestring(" spinners across "); print_dec((int)smp_online_count());
    terminal_writestring(" cpu(s); waiting...\n");
    // Wait until they all finish (we run as idle; the scheduler runs them on every
    // core in parallel). Reap zombies so the proc table frees up.
    while(sched_count_alive_user() > base){ __asm__ volatile("sti; hlt"); sched_reap_zombies(); }
    sched_reap_zombies();
    terminal_writestring("[stress] all "); print_dec(spawned); terminal_writestring(" done\n");
}

// [M29] smp: bring up the secondary cores on demand (AP bring-up is opt-in —
// experimental, can freeze under heavy load on real hardware). Reports the online
// CPU count. Safe to call once; a second call is a no-op for already-online cores.
static void sh_smp(const char* a){ (void)a;
    extern void smp_init(void);
    extern unsigned int smp_online_count(void);
    terminal_writestring("Bringing up application processors (experimental)...\n");
    smp_init();
    terminal_writestring("Online CPUs: "); print_dec((int)smp_online_count()); terminal_writestring("\n");
}

// [M32] pkg install <file.spkg>: read a signed package from the VFS and install
// it (verify Ed25519 signature, then unpack). A forged package installs nothing.
static void sh_pkg(const char* a){
    while(*a==' ') a++;
    if(strncmp(a,"install",7)==0){ a+=7; while(*a==' ') a++; }
    if(!*a){ terminal_writestring("Usage: pkg install <file.spkg>\n"); return; }
    extern vfs_inode_t* vfs_lookup(const char*);
    extern int vfs_read_all(const char*, void*, size_t);
    extern int pkg_install(const void*, size_t);
    extern void* kmalloc(unsigned long); extern void kfree(void*);
    vfs_inode_t* ino = vfs_lookup(a);
    if(!ino || ino->type!=VFS_NODE_FILE){ terminal_writestring("pkg: not found\n"); return; }
    size_t sz = ino->size; if(sz==0){ terminal_writestring("pkg: empty\n"); return; }
    uint8_t* buf = (uint8_t*)kmalloc(sz);
    if(!buf){ terminal_writestring("pkg: out of memory\n"); return; }
    int r = vfs_read_all(a, buf, sz);
    if(r<0){ terminal_writestring("pkg: read fail\n"); kfree(buf); return; }
    int n = pkg_install(buf, (size_t)r);
    if(n>=0){ terminal_writestring("pkg: installed "); print_dec(n); terminal_writestring(" entries\n"); }
    else terminal_writestring("pkg: REFUSED (bad signature or format)\n");
    kfree(buf);
}

// =========================== [M30] Job control ===========================
// The shell runs as the scheduler idle task, so it cannot itself be a ring-3
// process group member — instead it tracks the pipelines it launches. A
// pipeline is one process group (pgid); the kernel's foreground_pgid routes
// Ctrl-C (SIGINT) / Ctrl-Z (SIGTSTP) to whichever group is in the foreground.
// `jobs`/`fg`/`bg` list and resume stopped/background groups.
#define JOB_MAX   8
#define JOB_NPROC 8
typedef struct {
    int      used;
    int      stopped;                 // 1 = SIGTSTP'd, 0 = running in background
    uint32_t pgid;
    int      npid;
    uint32_t pids[JOB_NPROC];
    char     cmd[64];
} job_t;
static job_t g_jobs[JOB_MAX];

extern process_t* process_find_by_pid(uint32_t);
extern void* pipe_alloc(void);

// Drop jobs whose every process has exited (reaped/gone).
static void job_gc(void){
    for(int j=0;j<JOB_MAX;j++){
        if(!g_jobs[j].used) continue;
        int alive=0;
        for(int i=0;i<g_jobs[j].npid;i++){
            process_t* p=process_find_by_pid(g_jobs[j].pids[i]);
            if(p && p->state!=PROC_ZOMBIE) alive++;
        }
        if(!alive) g_jobs[j].used=0;
    }
}
static int job_add(uint32_t pgid, const uint32_t* pids, int npid, const char* cmd, int stopped){
    job_gc();
    for(int j=0;j<JOB_MAX;j++){
        if(g_jobs[j].used) continue;
        g_jobs[j].used=1; g_jobs[j].stopped=stopped; g_jobs[j].pgid=pgid; g_jobs[j].npid=npid;
        for(int i=0;i<npid && i<JOB_NPROC;i++) g_jobs[j].pids[i]=pids[i];
        int k=0; while(cmd && cmd[k] && k<63){ g_jobs[j].cmd[k]=cmd[k]; k++; } g_jobs[j].cmd[k]=0;
        return j+1;                   // job numbers are 1-based
    }
    return -1;
}

// Wait for the foreground process group to finish or stop. Returns 0 when every
// process has exited, 1 when the group stopped (Ctrl-Z). Reaps as it goes.
static int job_wait_foreground(uint32_t pgid, const uint32_t* pids, int npid){
    signal_set_foreground_pgid(pgid);
    for(;;){
        __asm__ volatile("cli");
        sched_reap_zombies();
        int running=0, stopped=0;
        for(int i=0;i<npid;i++){
            process_t* p=process_find_by_pid(pids[i]);
            if(!p || p->state==PROC_ZOMBIE) continue;     // gone
            if(p->state==PROC_STOPPED) stopped++; else running++;
        }
        if(running==0){
            sched_reap_zombies();
            signal_set_foreground_pgid(0);
            __asm__ volatile("sti");   // re-enable IRQs before returning: the shell
            return stopped>0 ? 1 : 0;  // hlt-waits for the keyboard with IF set
        }
        __asm__ volatile("sti; hlt");
    }
}

// Launch a (possibly piped) job: `cmd1 args | cmd2 | ...`, optionally in the
// background (trailing &, stripped by the caller). 'line' is mutated in place.
static uint8_t g_job_spawn_buf[65536];
static void run_job(char* line, int background){
    // Split on '|' into pipeline stages.
    char* stages[JOB_NPROC]; int ns=0;
    stages[ns++]=line;
    for(char* s=line; *s; s++){ if(*s=='|'){ *s=0; if(ns<JOB_NPROC) stages[ns++]=s+1; } }

    // Tokenize each stage into argv and resolve its program path.
    static char store[JOB_NPROC][JOB_NPROC][96];
    const char* av[JOB_NPROC][JOB_NPROC];
    int argc[JOB_NPROC];
    static char paths[JOB_NPROC][160];
    for(int i=0;i<ns;i++){
        char* a=stages[i]; int ac=0;
        while(*a && ac<JOB_NPROC){
            while(*a==' '||*a=='\t') a++;
            if(!*a) break;
            int j=0; while(*a && *a!=' ' && *a!='\t' && j<95){ store[i][ac][j++]=*a++; }
            store[i][ac][j]=0; av[i][ac]=store[i][ac]; ac++;
        }
        argc[i]=ac;
        if(ac==0){ terminal_writestring("syntax error near '|'\n"); return; }
        const char* c0=av[i][0]; int p=0;
        if(c0[0]=='/'){ for(const char* s=c0; *s && p<159; s++) paths[i][p++]=*s; }
        else { const char* pre="/bin/"; for(int k=0;pre[k];k++) paths[i][p++]=pre[k];
               for(const char* s=c0; *s && p<159; s++) paths[i][p++]=*s; }
        paths[i][p]=0;
    }

    extern process_t* process_create_from_elf_args(const void*, size_t, int, const char* const*);
    extern int vfs_read_all(const char*, void*, size_t);
    process_t* procs[JOB_NPROC]; uint32_t pids[JOB_NPROC]; int created=0;
    uint32_t pgid=0; void* prev_read=NULL;
    for(int i=0;i<ns;i++){
        int n=vfs_read_all(paths[i], g_job_spawn_buf, sizeof(g_job_spawn_buf));
        if(n<=0){ terminal_writestring("run: cannot launch "); terminal_writestring(paths[i]);
                  terminal_writestring(" (missing or unsigned)\n"); goto fail; }
        process_t* p=process_create_from_elf_args(g_job_spawn_buf,(size_t)n,argc[i],av[i]);
        if(!p){ terminal_writestring("run: cannot launch "); terminal_writestring(paths[i]);
                terminal_writestring(" (missing or unsigned)\n"); goto fail; }
        // Wire stdin from the previous stage's pipe, stdout to a fresh pipe.
        if(prev_read){ p->fds[0].used=1; p->fds[0].is_pipe=1; p->fds[0].pipe_w=0; p->fds[0].inode=prev_read; prev_read=NULL; }
        if(i < ns-1){
            void* pp=pipe_alloc();
            if(!pp){ terminal_writestring("pipe: out of pipes\n"); procs[created]=p; pids[created]=p->pid; created++; goto fail; }
            p->fds[1].used=1; p->fds[1].is_pipe=1; p->fds[1].pipe_w=1; p->fds[1].inode=pp;
            prev_read=pp;
        }
        if(i==0) pgid=p->pid;
        p->pgid=pgid; p->ppid=0;
        procs[i]=p; pids[i]=p->pid; created++;
    }
    for(int i=0;i<created;i++) procs[i]->state=PROC_READY;

    if(background){
        int jn=job_add(pgid,pids,created,line,0);
        terminal_writestring("["); print_dec(jn); terminal_writestring("] "); print_dec((int)pgid); terminal_writestring("\n");
        return;
    }
    int stopped=job_wait_foreground(pgid,pids,created);
    if(stopped){
        int jn=job_add(pgid,pids,created,line,1);
        terminal_writestring("\n["); print_dec(jn); terminal_writestring("]+ Stopped\n");
    }
    return;
fail:
    for(int i=0;i<created;i++) if(procs[i]){ signal_post(procs[i],9/*SIGKILL*/); procs[i]->state=PROC_READY; }
    for(int spin=0; spin<2000 && sched_count_alive_user()>0; spin++){ __asm__ volatile("sti; hlt"); sched_reap_zombies(); }
}

// jobs: list tracked background/stopped pipelines.
static void sh_jobs(const char* a){ (void)a;
    job_gc();
    int any=0;
    for(int j=0;j<JOB_MAX;j++){
        if(!g_jobs[j].used) continue; any=1;
        terminal_writestring("["); print_dec(j+1); terminal_writestring("] ");
        terminal_writestring(g_jobs[j].stopped ? "Stopped  " : "Running  ");
        terminal_writestring(g_jobs[j].cmd); terminal_writestring("\n");
    }
    if(!any) terminal_writestring("no jobs\n");
}

// Resolve a "%n" / "n" job spec (or the most recent job if empty) to a slot.
static int job_pick(const char* a){
    while(*a==' ') a++;
    if(*a=='%') a++;
    if(*a>='1'&&*a<='9'){ int n=0; while(*a>='0'&&*a<='9'){ n=n*10+(*a-'0'); a++; }
        if(n>=1 && n<=JOB_MAX && g_jobs[n-1].used) return n-1; return -1; }
    for(int j=JOB_MAX-1;j>=0;j--) if(g_jobs[j].used) return j;   // most recent
    return -1;
}

// fg [%n]: resume a job in the foreground (sends SIGCONT, then waits).
static void sh_fg(const char* a){
    job_gc();
    int j=job_pick(a);
    if(j<0){ terminal_writestring("fg: no such job\n"); return; }
    terminal_writestring(g_jobs[j].cmd); terminal_writestring("\n");
    signal_post_pgid(g_jobs[j].pgid, SIGCONT);
    uint32_t pgid=g_jobs[j].pgid; uint32_t pids[JOB_NPROC]; int npid=g_jobs[j].npid;
    for(int i=0;i<npid;i++) pids[i]=g_jobs[j].pids[i];
    g_jobs[j].used=0;                              // detach; re-added if it stops again
    int stopped=job_wait_foreground(pgid,pids,npid);
    if(stopped){ int jn=job_add(pgid,pids,npid,"(resumed)",1);
                 terminal_writestring("\n["); print_dec(jn); terminal_writestring("]+ Stopped\n"); }
}

// bg [%n]: resume a stopped job in the background (SIGCONT, no wait).
static void sh_bg(const char* a){
    job_gc();
    int j=job_pick(a);
    if(j<0){ terminal_writestring("bg: no such job\n"); return; }
    signal_post_pgid(g_jobs[j].pgid, SIGCONT);
    g_jobs[j].stopped=0;
    terminal_writestring("["); print_dec(j+1); terminal_writestring("] "); terminal_writestring(g_jobs[j].cmd); terminal_writestring(" &\n");
}

// [M30] run <path> [args]: now a thin wrapper over run_job, so a foreground
// program gets job control (Ctrl-C terminates it, Ctrl-Z stops it). run_job
// resolves an absolute path as-is, or /bin/<name> for a bare name.
static void sh_run(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("Usage: run <path> [args...]\n"); return; }
    char line[256]; int q=0; for(const char* s=a; *s && q<255; s++) line[q++]=*s; line[q]=0;
    run_job(line, 0);
}

// [M31] Toggle kernel verbosity (the per-spawn [PROC]/[ELF]/[M5]/... debug noise).
static void sh_verbose(const char* a){
    extern int g_kverbose;
    while(*a==' ') a++;
    if(strncmp(a,"on",2)==0) g_kverbose=1;
    else if(strncmp(a,"off",3)==0) g_kverbose=0;
    else if(*a==0) { /* toggle */ g_kverbose = !g_kverbose; }
    terminal_writestring("verbose: "); terminal_writestring(g_kverbose?"on\n":"off\n");
}
// --- Driver space commands ---
static void sh_drvreg(const char* a){ while(*a==' ') a++; if(!*a){ terminal_writestring("Usage: drvreg <device_id>\n"); return; } int dev=0; while(*a>='0'&&*a<='9'){ dev = dev*10 + (*a-'0'); a++; } extern int driver_register_binding(process_t*, int); extern const device_desc_t* driver_get_device(int); extern process_t* sched_get_current(void); extern process_t* process_get_last(void);
    process_t* target = sched_get_current(); if(!target){ // fallback: last created process
        target = process_get_last();
        if(target){ terminal_writestring("[drvreg] (fallback: using last process, no current)\n"); }
    }
    if(!target){ terminal_writestring("[drvreg] no process: run 'elfload' first\n"); return; }
    if(!driver_get_device(dev)){ terminal_writestring("[drvreg] device not found\n"); return; }
    int r = driver_register_binding(target, dev);
    // [M11] Manual/dev binding path: also mark the target a driver so SYS_DRIVER
    // (which now requires PROC_TYPE_DRIVER) accepts it. The signed-manifest path
    // is the production route; this is the interactive harness.
    if(r==DRV_OK){ target->proc_type = PROC_TYPE_DRIVER; target->drv_dev_id = dev; target->drv_caps = driver_binding_caps(target, dev); terminal_writestring("[drvreg] OK\n"); }
    else if(r==DRV_ERR_DEVICE) terminal_writestring("[drvreg] FAIL device\n"); else if(r==DRV_ERR_PERM) terminal_writestring("[drvreg] FAIL no slot\n"); else terminal_writestring("[drvreg] FAIL\n"); }
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
// [M31] poweroff: ACPI soft-off (works on QEMU/VMware/real HW). If ACPI is
// unavailable, fall back to halting the CPU.
static void sh_poweroff(const char* a){ (void)a;
    extern void acpi_poweroff(void);
    terminal_writestring("\nPowering off...\n");
    __asm__ volatile("cli");
    acpi_poweroff();                 // does not return on success
    terminal_writestring("ACPI poweroff unavailable; halting.\n");
    for(;;) __asm__ volatile("hlt");
}
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
// [M30] kill [-SIG] <pid> | kill [-SIG] %job — send a signal (default SIGTERM).
// Now signal-based (was a forced process_destroy): it works on blocked/stopped
// targets and respects user handlers, like a real shell `kill`.
static void sh_kill(const char* a){
    while(*a==' ') a++;
    if(!*a){ terminal_writestring("Usage: kill [-SIG] <pid> | kill [-SIG] %job\n"); return; }
    int sig=15; // SIGTERM
    if(*a=='-'){ a++; int n=0; while(*a>='0'&&*a<='9'){ n=n*10+(*a-'0'); a++; } if(n>0) sig=n; while(*a==' ') a++; }
    if(*a=='%'){
        job_gc(); int j=job_pick(a);
        if(j<0){ terminal_writestring("kill: no such job\n"); return; }
        int n=signal_post_pgid(g_jobs[j].pgid, sig);
        terminal_writestring("[kill] signaled "); print_dec(n); terminal_writestring(" proc(s)\n");
        return;
    }
    uint32_t pid=0; while(*a>='0'&&*a<='9'){ pid=pid*10+(*a-'0'); a++; }
    process_t* t=process_find_by_pid(pid);
    if(!t){ terminal_writestring("[kill] PID not found\n"); return; }
    if(signal_post(t,sig)==0) terminal_writestring("[kill] OK\n"); else terminal_writestring("[kill] FAIL\n");
}
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

    // [M30] Trailing '&' => run in the background. Strip it (and trailing space).
    int background=0;
    { int L=0; while(line[L]) L++;
      while(L>0 && (line[L-1]==' '||line[L-1]=='\t')) L--;
      if(L>0 && line[L-1]=='&'){ background=1; L--; while(L>0 && (line[L-1]==' '||line[L-1]=='\t')) L--; }
      line[L]=0; }

    // [M30] A pipeline (`a | b | ...`) is always launched as external programs
    // through the job-control path — builtins do not participate in pipelines.
    for(char* s=line; *s; s++) if(*s=='|'){ run_job(line, background); return; }

    char* args=line; while(*args && *args!=' ') args++; if(*args){ *args='\0'; args++; while(*args==' ') args++; }
    const char* cmd=line;
    // Ricerca lineare (pochi comandi, costo trascurabile). In futuro: ordinare e binary search.
    for (unsigned i=0;i<sizeof(shell_cmds)/sizeof(shell_cmds[0]); i++) {
        if (strcmp(cmd, shell_cmds[i].name)==0) { shell_cmds[i].handler(args); return; }
    }
    // [M31] Not a builtin → try to launch a signed program: an absolute path as-is,
    // otherwise /bin/<cmd> (so `hexdump f`, `uname -a`, `seq 1 5` run by name).
    // [M30] Launched through run_job so it gets job control (Ctrl-C/Ctrl-Z, &).
    {
        extern vfs_inode_t* vfs_lookup(const char*);
        char path[160]; int p=0;
        if (cmd[0]=='/') { for(const char* s=cmd; *s && p<159; s++) path[p++]=*s; }
        else { const char* pre="/bin/"; for(int k=0;pre[k];k++) path[p++]=pre[k];
               for(const char* s=cmd; *s && p<159; s++) path[p++]=*s; }
        path[p]=0;
        if (vfs_lookup(path)) {
            char rl[256]; int q=0;
            for(int k=0; path[k] && q<255; k++) rl[q++]=path[k];
            if (*args){ rl[q++]=' '; for(const char* s=args; *s && q<255; s++) rl[q++]=*s; }
            rl[q]=0;
            run_job(rl, background);
            return;
        }
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