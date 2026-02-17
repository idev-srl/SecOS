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
// M2: split into Phase 1 (old .bss stack) and Phase 2 (new guarded stack).
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
#if CONFIG_UEFI
#include "bootinfo.h"
#endif
#if ENABLE_FB
#include "fb.h"
#include "fb_console.h"
#endif

// Assembly trampoline: switches RSP to new_rsp and tail-calls fn.
// Defined in arch/x86/idt_asm.asm.
extern void trampoline_switch_stack(uint64_t new_rsp, void (*fn)(void));

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


// ---- Phase 2: runs on the new guarded kernel stack ----
// Called by trampoline_switch_stack after RSP has been moved to M2_KSTACK_TOP.
// Interrupts are still disabled; idt_init() will enable them.
static void kernel_main_phase2(void) {
    // Confirm the stack switch succeeded
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    terminal_writestring("[M2] Stack switch complete. RSP= ");
    print_hex(rsp);
    terminal_writestring("\n");

    // NOTE(M2-B): We are now past the narrow window where no IDT/IST was
    // present.  vmm_init_physmap() and vmm_alloc_kernel_stack() ran with
    // interrupts disabled and no IDT loaded.  If either faulted, the result
    // would have been a triple fault (same risk exists for vmm_init() in M1).
    // A minimal no-STI fallback IDT could be added in M3 for safer debugging.
    idt_init();

    heap_init();
    sched_init();
    driver_registry_init();
    timer_init(1000);
    keyboard_init();

    // Initialize native RAMFS (fallback)
    extern int ramfs_init(void); ramfs_init();
    // Initialize VFS
    extern void vfs_init(void); vfs_init();
    // Register ext2ram block device and attempt ext2 mount
    extern int ext2ramdev_register(void); ext2ramdev_register();
    extern int ext2_mount(const char* dev_name);
    int ext2_res = ext2_mount("ext2ram");
    if (ext2_res == 0) {
        terminal_writestring("[EXT2] mount succeeded (stub, root replaced)\n");
    } else {
        extern int vfs_mount_ramfs(void);
        if (vfs_mount_ramfs() == 0) terminal_writestring("[VFS] root RAMFS fallback mounted\n");
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
    if (initrc) {
        terminal_writestring("[INIT] Executing init.rc\n");
        size_t pos = 0;
        while (pos < initrc->size) {
            char line[128]; size_t li = 0;
            while (pos < initrc->size && initrc->data[pos] != '\n' && li < sizeof(line)-1)
                line[li++] = (char)initrc->data[pos++];
            line[li] = 0;
            if (pos < initrc->size && initrc->data[pos] == '\n') pos++;
            char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == 0) continue;
            extern void shell_run_line(const char* line); shell_run_line(p);
        }
        terminal_writestring("[INIT] Script completed\n");
    } else {
        terminal_writestring("[INIT] init.rc not found\n");
    }

#if ENABLE_FB
    // Boot magic is saved in a local variable in kernel_main; phase2 does not
    // have access to it.  For framebuffer init, use the saved global below.
    extern uint32_t g_multiboot_magic;
    extern uint64_t g_multiboot_info;
    if (g_multiboot_magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        if (fb_init((uint32_t)g_multiboot_info) == 0) {
            fb_finalize_mapping();
            framebuffer_info_t info;
            if (fb_get_info(&info)) {
                uint64_t base = info.virt_addr ? info.virt_addr : info.addr;
                volatile uint32_t* p = (uint32_t*)base;
                p[0]=0x0000FF; p[1]=0x00FF00; p[2]=0xFF0000;
                if (terminal_try_enable_fb()) {
                    fb_clear(0x000000);
                    extern void fb_console_draw_logo(void); fb_console_draw_logo();
                    print_banner();
                    terminal_writestring("[FB] w="); print_hex(info.width);
                    terminal_writestring(" h="); print_hex(info.height);
                    terminal_writestring(" bpp="); print_hex(info.bpp);
                    terminal_writestring(" pitch="); print_hex(info.pitch);
                    terminal_writestring(" addr="); print_hex(info.addr);
                    terminal_writestring(" virt="); print_hex(info.virt_addr);
                    terminal_writestring("\n");
                    extern int fb_console_enable_cursor_blink(uint32_t timer_freq);
                    fb_console_enable_cursor_blink(timer_get_frequency());
                }
            }
        }
    }
#if CONFIG_UEFI
    else if (g_multiboot_magic == 0 && g_multiboot_info != 0) {
        struct secos_boot_info* bi = (struct secos_boot_info*)g_multiboot_info;
        if (bi->fb_addr && (bi->flags & (1ULL<<0))) {
            extern int fb_init_uefi(struct secos_boot_info*);
            if (fb_init_uefi(bi) == 0) {
                fb_finalize_mapping();
                framebuffer_info_t info;
                if (fb_get_info(&info)) {
                    if (terminal_try_enable_fb()) {
                        fb_clear(0x000000);
                        extern void fb_console_draw_logo(void); fb_console_draw_logo();
                        print_banner();
                        terminal_writestring("[UEFI-FB] w="); print_hex(info.width);
                        terminal_writestring(" h="); print_hex(info.height);
                        terminal_writestring(" bpp="); print_hex(info.bpp);
                        terminal_writestring("\n");
                        extern int fb_console_enable_cursor_blink(uint32_t timer_freq);
                        fb_console_enable_cursor_blink(timer_get_frequency());
                    }
                }
            }
        }
    }
#endif
#endif

    shell_init();
#if ENABLE_FB
    extern void fb_console_draw_logo(void); fb_console_draw_logo();
    extern void fb_console_flush(void); fb_console_flush();
#endif
    shell_run();
    while (1) { __asm__ volatile ("hlt"); }
}

// ---- Phase 1: runs on the old .bss stack from boot.asm ----
//
// M2 initialization order:
//   1. pmm_init*()           — parse memory map, set up frame bitmap
//   2. vmm_init()            — build kernel-owned PML4, identity 0–512MB, load CR3
//   3. vmm_init_physmap()    — map all physical memory at 0xFFFF888000000000
//                              (moved before tss/idt; interrupts still disabled)
//   4. vmm_alloc_kernel_stack() — allocate + map 16KB guarded kernel stack
//                                 returns M2_KSTACK_TOP as RSP_INIT
//   5. tss_init(rsp0)        — allocate + map guarded IST stacks,
//                              load GDT + TSS; TSS.rsp0 = RSP_INIT
//   6. trampoline_switch_stack(rsp, phase2) — switch RSP to new stack,
//                                             tail-call kernel_main_phase2
//   --- phase2: ---
//   7. idt_init()            — set up IDT, enable interrupts (STI)
//   8. heap / sched / drivers / fs / shell
//
// NOTE(M2-B): Steps 3–6 execute without a valid IDT or TSS.  Any CPU fault
// in that window causes a triple fault.  These functions are deterministic
// and do not fault under correct operation.  A minimal fallback IDT (no STI)
// for early-boot debugging can be added in M3 if needed.

// Globals saved in phase1 for use in phase2 (framebuffer init)
uint32_t g_multiboot_magic = 0;
uint64_t g_multiboot_info  = 0;

void kernel_main(uint32_t multiboot_magic, uint64_t multiboot_info) {
    // --- Phase 1 begins (old .bss stack) ---
    terminal_initialize();
    print_banner();

    // Save for phase2 (framebuffer)
    g_multiboot_magic = multiboot_magic;
    g_multiboot_info  = multiboot_info;

    terminal_writestring("Multiboot magic: "); print_hex(multiboot_magic);
    terminal_writestring("  info: "); print_hex(multiboot_info); terminal_writestring("\n");
    if (multiboot_magic == 0x2BADB002) {
        terminal_writestring("[OK] Multiboot1 detected\n");
    } else if (multiboot_magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        terminal_writestring("[OK] Multiboot2 detected\n");
    } else {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[WARN] Unknown bootloader magic number!\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    }

    // Step 1: PMM init
#if CONFIG_UEFI
    if (multiboot_magic == 0 && multiboot_info != 0) {
        struct secos_boot_info* bi = (struct secos_boot_info*)multiboot_info;
        terminal_writestring("[UEFI] Boot info flags= "); print_hex(bi->flags); terminal_writestring("\n");
        if (bi->flags & (1ULL<<1)) {
            pmm_init_uefi(bi->mem_descs, bi->mem_desc_count, bi->mem_desc_size, bi->mem_desc_version);
        } else {
            terminal_writestring("[UEFI][WARN] Memory map absent, fallback synthetic PMM\n");
            pmm_init_uefi(NULL, 0, 0, 0);
        }
    } else
#endif
    if (multiboot_magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        pmm_init_mb2((void*)multiboot_info);
    } else {
        pmm_init((void*)multiboot_info);
    }

    // Step 2: VMM — build kernel page tables (identity 0–512MB), load CR3
    vmm_init();

    // Step 3: Physmap — map all physical memory at VMM_PHYSMAP_BASE.
    // Moved before tss_init so that vmm_map() in tss_init uses physmap-aware
    // page table walks.  Interrupts are disabled; no IDT/TSS needed yet.
    vmm_init_physmap();

    // Step 4: Allocate new kernel stack (16KB + guard pages) in dedicated VA region
    uint64_t new_rsp = vmm_alloc_kernel_stack();

    // Step 5: TSS — allocate guarded IST stacks, build GDT, load TSS.
    // Must run after physmap (vmm_alloc_ist_stack uses vmm_map).
    tss_init(new_rsp);

    // Step 6: Switch RSP to the new guarded kernel stack and enter phase2.
    // trampoline_switch_stack does NOT return.
    trampoline_switch_stack(new_rsp, kernel_main_phase2);

    // UNREACHABLE — the trampoline jumps to phase2 and never returns.
    while (1) { __asm__ volatile ("hlt"); }
}
