/*
 * SecOS Kernel
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
// Consolidated kernel_main (advanced framebuffer + MB2 + PMM2 support)
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
#include "driver_if.h" // driver registry init
#if ENABLE_FB
#include "fb.h"
#include "fb_console.h"
#endif

static void print_banner(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("==================================\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("   SecOS 64-bit Kernel (GRUB)\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("==================================\n\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("Kernel started in Long Mode (64-bit)!\n");
}

void kernel_main(uint32_t multiboot_magic, uint64_t multiboot_info) {
    // Kernel start
    terminal_initialize();
    // Print startup banner
    void print_banner(void);
    print_banner();

    // Basic boot info
    terminal_writestring("Multiboot magic: "); print_hex(multiboot_magic); terminal_writestring("  info: "); print_hex(multiboot_info); terminal_writestring("\n");
        if (multiboot_magic == 0x2BADB002) {
            terminal_writestring("[OK] Multiboot1 detected\n");
        } else if (multiboot_magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
            terminal_writestring("[OK] Multiboot2 detected\n");
        } else {
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
            terminal_writestring("[WARN] Unknown bootloader magic number!\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
        }

#if ENABLE_FB
    // (Optional: could enumerate framebuffer tags only in debug mode)
#endif

    // Mark which PMM path we take
    if (multiboot_magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
    pmm_init_mb2((void*)multiboot_info);
    } else {
    pmm_init((void*)multiboot_info);
    }
    // pmm_print_stats(); // optional
    vmm_init();
    // terminal_writestring("[OK] IDT initialization...\n");
    idt_init();
    vmm_init_physmap();
    tss_init();

    // Debug addresses (enable if needed)
    // terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    // terminal_writestring("[DBG] Key addresses:\n");
    // terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    // terminal_writestring("  kernel_main: "); print_hex((uint64_t)&kernel_main); terminal_writestring("\n");
    // terminal_writestring("  idt_init:    "); print_hex((uint64_t)&idt_init); terminal_writestring("\n");
    // terminal_writestring("  tss_init:    "); print_hex((uint64_t)&tss_init); terminal_writestring("\n");
    // extern uint32_t _kernel_end; terminal_writestring("  _kernel_end: "); print_hex((uint64_t)&_kernel_end); terminal_writestring("\n");
    // uint64_t cr3_val; __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val)); terminal_writestring("  CR3:         "); print_hex(cr3_val); terminal_writestring("\n\n");
    // terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));

    heap_init();
    sched_init();
    // Initialize driver space device registry (required for drvreg)
    driver_registry_init();
    // terminal_writestring("[OK] PIT timer initialization (1000 Hz)...\n");
    timer_init(1000);
    // terminal_writestring("[OK] PS/2 keyboard initialization...\n");
    keyboard_init();

    // Initialize native RAMFS (fallback)
    extern int ramfs_init(void); ramfs_init();
    // Initialize VFS
    extern void vfs_init(void); vfs_init();
    // Register ext2ram block device and attempt ext2 mount
    extern int ext2ramdev_register(void); ext2ramdev_register();
    extern int ext2_mount(const char* dev_name);
    int ext2_res = ext2_mount("ext2ram");
    if(ext2_res==0){
        terminal_writestring("[EXT2] mount succeeded (stub, root replaced)\n");
    } else {
        // Fallback: mount RAMFS as root
        extern int vfs_mount_ramfs(void);
        if(vfs_mount_ramfs()==0) terminal_writestring("[VFS] root RAMFS fallback mounted\n");
        else terminal_writestring("[VFS] fallback RAMFS FAIL\n");
    }
    // Self-test VFS (basic): list root and read VERSION
    extern void shell_run_line(const char* line);
    shell_run_line("vls /");
    shell_run_line("vinfo /VERSION");
    shell_run_line("vcat /VERSION");
    // Execute init.rc script if present
    #include "fs/ramfs.h"
    const ramfs_entry_t* initrc = ramfs_find("init.rc");
    if(initrc){
        terminal_writestring("[INIT] Executing init.rc\n");
        size_t pos=0; while(pos < initrc->size){
            // Extract line
            char line[128]; size_t li=0; while(pos < initrc->size && initrc->data[pos] != '\n' && li < sizeof(line)-1){ line[li++] = (char)initrc->data[pos++]; }
            line[li]=0; if(pos < initrc->size && initrc->data[pos]=='\n') pos++;
            // Skip comments/blank lines
            char* p=line; while(*p==' '||*p=='\t') p++; if(*p=='#' || *p==0) continue;
            extern void shell_run_line(const char* line); shell_run_line(p);
        }
        terminal_writestring("[INIT] Script completed\n");
    } else {
        terminal_writestring("[INIT] init.rc not found\n");
    }

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