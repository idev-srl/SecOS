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
#include "debugcon.h"   // ISA debugcon boot markers (port 0xE9)
#include "block.h"      // block device registry (virtio-blk smoke check)
#include "vfs.h"        // [M23] VFS types (vfs_inode_t) for boot self-checks
#include "selftest.h"   // M4 isolation selftest
#include "process.h"    // M6 ring3 entry
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


// ---- [M8] Preemptive scheduling demo (gated; off by default) ----
// Spawns user processes that compute then SYS_EXIT, scheduled preemptively by
// the timer, with a kernel idle task that reaps zombies and checks for leaks.
#ifndef M8_SCHED_DEMO
#define M8_SCHED_DEMO 0
#endif
#if M8_SCHED_DEMO
#include "../mm/elf.h"
#include "syscall.h"
#include "trapframe.h"

#define M8_NPROC  4
#define M8_ROUNDS 4   // run several spawn-all/exit-all rounds; assert the last two
                      // leave PMM free identical (stable => no per-round leak).
static uint8_t  m8_idle_stack[16384] __attribute__((aligned(16)));
static unsigned char m8_elf[512];
static uint64_t m8_free[M8_ROUNDS + 2];
static int      m8_round = 0;

static void m8_build_elf(void) {
    for (int i = 0; i < 512; i++) m8_elf[i] = 0;
    m8_elf[0]=0x7F; m8_elf[1]='E'; m8_elf[2]='L'; m8_elf[3]='F';
    m8_elf[4]=2; m8_elf[5]=1; m8_elf[6]=1;
    *(uint16_t*)(m8_elf+16)=2; *(uint16_t*)(m8_elf+18)=0x3E; *(uint32_t*)(m8_elf+20)=1;
    *(uint64_t*)(m8_elf+24)=USER_CODE_BASE; *(uint64_t*)(m8_elf+32)=64;
    *(uint16_t*)(m8_elf+52)=64; *(uint16_t*)(m8_elf+54)=56; *(uint16_t*)(m8_elf+56)=1;
    *(uint32_t*)(m8_elf+64)=1; *(uint32_t*)(m8_elf+68)=PF_R|PF_X;
    *(uint64_t*)(m8_elf+72)=0x100ULL; *(uint64_t*)(m8_elf+80)=USER_CODE_BASE;
    *(uint64_t*)(m8_elf+88)=USER_CODE_BASE; *(uint64_t*)(m8_elf+96)=0x20ULL;
    *(uint64_t*)(m8_elf+104)=0x20ULL; *(uint64_t*)(m8_elf+112)=0x1000ULL;
    // mov rbx,0x200000 / loop: dec rbx / jnz loop / mov rax,SYS_EXIT / int 0x80
    unsigned char* c = m8_elf + 0x100;
    c[0]=0x48; c[1]=0xC7; c[2]=0xC3; c[3]=0x00; c[4]=0x00; c[5]=0x20; c[6]=0x00; // mov rbx,0x200000
    c[7]=0x48; c[8]=0xFF; c[9]=0xCB;                                            // dec rbx
    c[10]=0x75; c[11]=0xFB;                                                     // jnz -5
    c[12]=0x48; c[13]=0xC7; c[14]=0xC0; c[15]=SYS_EXIT; c[16]=0; c[17]=0; c[18]=0; // mov rax,SYS_EXIT
    c[19]=0xCD; c[20]=0x80;                                                     // int 0x80
}

static void m8_spawn_round(void) {
    for (int i = 0; i < M8_NPROC; i++) {
        process_t* p = process_create_from_elf(m8_elf, 512);
        if (p) p->state = PROC_READY;
    }
}

__attribute__((noreturn)) static void m8_idle_entry(void) {
    for (;;) {
        // Critical section: the idle task is restarted from the top each time
        // it is scheduled, so spawn+bookkeeping must be atomic w.r.t. timer
        // preemption (otherwise a mid-spawn preempt loses progress).
        __asm__ volatile("cli");
        sched_reap_zombies();
        int alive = sched_count_alive_user();
        if (m8_round == 0) {
            debugcon_writestring("[M8] round 1: spawning\n");
            m8_spawn_round(); m8_round = 1;
        } else if (alive == 0 && m8_round <= M8_ROUNDS) {
            m8_free[m8_round] = pmm_get_free_memory();
            debugcon_writestring("[M8] round "); debugcon_print_hex((uint64_t)m8_round);
            debugcon_writestring(" complete, free="); debugcon_print_hex(m8_free[m8_round]);
            debugcon_writestring("\n");
            if (m8_round < M8_ROUNDS) {
                m8_round++;
                debugcon_writestring("[M8] round "); debugcon_print_hex((uint64_t)m8_round);
                debugcon_writestring(": spawning\n");
                m8_spawn_round();
            } else {
                // Compare the last two rounds: equal => no per-round leak.
                if (m8_free[M8_ROUNDS] == m8_free[M8_ROUNDS - 1])
                    debugcon_writestring("[M8] PMM stable across rounds: NO LEAK\n");
                else
                    debugcon_writestring("[M8] PMM MISMATCH across rounds: possible leak\n");
                debugcon_writestring("[M8] DONE\n");
                m8_round = M8_ROUNDS + 1; // sentinel: idle quietly hereafter
            }
        }
        __asm__ volatile("sti; hlt");
    }
}

static void m8_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    static process_t  idle;
    static trapframe_t idle_tf;
    m8_build_elf();
    debugcon_writestring("[M8] preemptive scheduling demo, nproc=");
    debugcon_print_hex((uint64_t)M8_NPROC); debugcon_writestring("\n");

    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0;
    idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m8_idle_stack + sizeof(m8_idle_stack));
    idle.tf = &idle_tf;
    idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m8_idle_entry;
    idle_tf.cs = 0x08; idle_tf.ss = 0x10; idle_tf.rflags = 0x202;
    idle_tf.rsp = idle.kstack_top;

    sched_set_idle(&idle);
    sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[M8] entering idle/scheduler\n");
    arch_iret_to_tf(&idle_tf); // does not return
}
#endif /* M8_SCHED_DEMO */

// ---- [M9] Signed userland demo (gated; off by default) ----
// Loads a toolchain-built, Ed25519-signed `hello` from the VFS, verifies its
// signature (the loader gate), and runs it in ring-3. Also proves a tampered
// copy is refused. Built WITHOUT -DDEV_ALLOW_UNSIGNED so the gate is enforced.
#ifndef M9_USER_DEMO
#define M9_USER_DEMO 0
#endif
#if M9_USER_DEMO
#include "trapframe.h"
#include "../crypto/user_hello_elf.h"
extern int  vfs_create(const char* path, const void* data, size_t size);
extern int  vfs_read_all(const char* path, void* buf, size_t bufsize);

static uint8_t  m9_idle_stack[16384] __attribute__((aligned(16)));
static uint8_t  m9_loadbuf[8192];
static int      m9_spawned = 0, m9_done = 0;

__attribute__((noreturn)) static void m9_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (!m9_spawned) {
            m9_spawned = 1;
            int n = vfs_read_all("/bin/hello", m9_loadbuf, sizeof(m9_loadbuf));
            debugcon_writestring("[M9] loaded /bin/hello from VFS, bytes=");
            debugcon_print_hex((uint64_t)(uint32_t)n); debugcon_writestring("\n");
            process_t* p = (n > 0) ? process_create_from_elf(m9_loadbuf, (size_t)n) : 0;
            if (p) { p->state = PROC_READY; debugcon_writestring("[M9] spawned signed hello\n"); }
            else   { debugcon_writestring("[M9] FAIL: signed hello not loaded/refused\n"); }
        } else if (sched_count_alive_user() == 0 && !m9_done) {
            m9_done = 1;
            debugcon_writestring("[M9] user program exited; DONE\n");
        }
        __asm__ volatile("sti; hlt");
    }
}

static void m9_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[M9] signed userland demo\n");

    // Enforcement check: a tampered copy must be REFUSED by the loader gate.
    static uint8_t tampered[8192];
    for (size_t i = 0; i < user_hello_elf_len && i < sizeof(tampered); i++) tampered[i] = user_hello_elf[i];
    tampered[0x100] ^= 0x01; // flip a code byte -> signature no longer matches
    process_t* bad = process_create_from_elf(tampered, user_hello_elf_len);
    debugcon_writestring(bad ? "[M9] FAIL: tampered ELF accepted\n"
                             : "[M9] tampered ELF REFUSED (good)\n");

    // Publish the signed hello into the VFS, to be loaded back from there.
    if (vfs_create("/bin/hello", user_hello_elf, user_hello_elf_len) != 0)
        debugcon_writestring("[M9] WARN: vfs_create /bin/hello failed\n");

    static process_t  idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m9_idle_stack + sizeof(m9_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m9_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[M9] entering scheduler\n");
    arch_iret_to_tf(&idle_tf); // does not return
}
#endif /* M9_USER_DEMO */

// ---- [M11] Driver Space demo (gated; off by default) ----
// Runs two signed ring-3 programs back-to-back to prove the capability boundary
// is rooted in the signature:
//   1. /bin/driver   — manifest declares PROC_TYPE_DRIVER (dev 0, caps
//      READ|WRITE|GET_INFO). Granted ops succeed via SYS_DRIVER; an un-granted
//      op (MAP_MEM) is refused (DRV_ERR_PERM) even though the device supports it.
//   2. /bin/userprobe — manifest is PROC_TYPE_USER. Every SYS_DRIVER call is
//      refused (DRV_ERR_NOTDRV): a plain user process has no driver rights.
#ifndef M11_DRIVER_DEMO
#define M11_DRIVER_DEMO 0
#endif
#if M11_DRIVER_DEMO
#include "trapframe.h"
#include "../crypto/user_driver_elf.h"
#include "../crypto/user_userprobe_elf.h"
extern int vfs_create(const char* path, const void* data, size_t size);
extern int vfs_read_all(const char* path, void* buf, size_t bufsize);

static uint8_t m11_idle_stack[16384] __attribute__((aligned(16)));
static uint8_t m11_loadbuf[16384];
static int     m11_step = 0, m11_done = 0;

// Load the signed ELF back from the VFS (ramfs). Since M12 the heap serves
// multi-frame allocations, so ramfs can store a ~8 KB ELF intact — this loads
// the same path that silently corrupted under the M11-era one-frame heap.
static void m11_spawn(const char* path, const char* what) {
    int n = vfs_read_all(path, m11_loadbuf, sizeof(m11_loadbuf));
    debugcon_writestring("[M11] loaded "); debugcon_writestring(path);
    debugcon_writestring(" bytes="); debugcon_print_hex((uint64_t)(uint32_t)n);
    debugcon_writestring("\n");
    process_t* p = (n > 0) ? process_create_from_elf(m11_loadbuf, (size_t)n) : 0;
    if (p) {
        p->state = PROC_READY;
        debugcon_writestring("[M11] spawned "); debugcon_writestring(what);
        debugcon_writestring(" proc_type="); debugcon_print_hex((uint64_t)(uint32_t)p->proc_type);
        debugcon_writestring("\n");
    } else {
        debugcon_writestring("[M11] FAIL: "); debugcon_writestring(what);
        debugcon_writestring(" not loaded/refused\n");
    }
}

__attribute__((noreturn)) static void m11_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m11_step == 0) {
            m11_step = 1;
            debugcon_writestring("[M11] --- driver-type probe (granted dev0) ---\n");
            m11_spawn("/bin/driver", "driver");
        } else if (m11_step == 1 && sched_count_alive_user() == 0) {
            m11_step = 2;
            debugcon_writestring("[M11] --- user-type probe (no driver rights) ---\n");
            m11_spawn("/bin/userprobe", "userprobe");
        } else if (m11_step == 2 && sched_count_alive_user() == 0 && !m11_done) {
            m11_done = 1;
            debugcon_writestring("[M11] DONE\n");
        }
        __asm__ volatile("sti; hlt");
    }
}

static void m11_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[M11] driver space demo\n");
    // [M12] Publish the signed ELFs into the VFS; the M12 heap makes the
    // large-file ramfs copy reliable (closes the M11 gotcha).
    if (vfs_create("/bin/driver", user_driver_elf, user_driver_elf_len) != 0)
        debugcon_writestring("[M11] WARN: vfs_create /bin/driver failed\n");
    if (vfs_create("/bin/userprobe", user_userprobe_elf, user_userprobe_elf_len) != 0)
        debugcon_writestring("[M11] WARN: vfs_create /bin/userprobe failed\n");

    static process_t  idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m11_idle_stack + sizeof(m11_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m11_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[M11] entering scheduler\n");
    arch_iret_to_tf(&idle_tf); // does not return
}
#endif /* M11_DRIVER_DEMO */

// ---- [M13] Usability & policy demo (gated; off by default) ----
// Proves, non-interactively: (1) manifest max_mem enforcement — a signed program
// whose manifest declares a too-small max_mem is REFUSED at load; (2) minimal IPC
// — a producer sends a message on kernel channel 0 (and reports its uptime via
// SYS_GETTICKS) then exits; a separately-spawned consumer reads the queued
// message. The channel is kernel-owned, so it survives the producer's exit.
#ifndef M13_DEMO
#define M13_DEMO 0
#endif
#if M13_DEMO
#include "trapframe.h"
#include "../crypto/user_maxmem_elf.h"
#include "../crypto/user_ipc_send_elf.h"
#include "../crypto/user_ipc_recv_elf.h"

static uint8_t m13_idle_stack[16384] __attribute__((aligned(16)));
static int     m13_step = 0, m13_done = 0;

static void m13_spawn(const uint8_t* img, size_t len, const char* what) {
    process_t* p = process_create_from_elf(img, len);
    if (p) { p->state = PROC_READY;
        debugcon_writestring("[M13] spawned "); debugcon_writestring(what); debugcon_writestring("\n");
    } else {
        debugcon_writestring("[M13] FAIL: "); debugcon_writestring(what); debugcon_writestring(" not loaded\n");
    }
}

__attribute__((noreturn)) static void m13_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m13_step == 0) {
            m13_step = 1;
            // (1) max_mem enforcement: this signed ELF declares max_mem=4KB and
            // must be refused at load (process_create_from_elf returns NULL).
            debugcon_writestring("[M13] --- manifest max_mem enforcement ---\n");
            process_t* bad = process_create_from_elf(user_maxmem_elf, user_maxmem_elf_len);
            debugcon_writestring(bad ? "[M13] FAIL: over-limit program accepted\n"
                                     : "[M13] max_mem program REFUSED at load (good)\n");
            // (2) IPC: spawn the producer first; it sends to channel 0 and exits.
            debugcon_writestring("[M13] --- IPC channel 0 (producer/consumer) ---\n");
            m13_spawn(user_ipc_send_elf, user_ipc_send_elf_len, "ipc_send");
        } else if (m13_step == 1 && sched_count_alive_user() == 0) {
            m13_step = 2;
            // Consumer reads the message the producer left queued on channel 0.
            m13_spawn(user_ipc_recv_elf, user_ipc_recv_elf_len, "ipc_recv");
        } else if (m13_step == 2 && sched_count_alive_user() == 0 && !m13_done) {
            m13_done = 1;
            debugcon_writestring("[M13] DONE\n");
        }
        __asm__ volatile("sti; hlt");
    }
}

static void m13_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[M13] usability & policy demo\n");
    static process_t  idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m13_idle_stack + sizeof(m13_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m13_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[M13] entering scheduler\n");
    arch_iret_to_tf(&idle_tf); // does not return
}
#endif /* M13_DEMO */

// ---- [M14] Demand paging demo (gated; off by default) ----
// Proves, non-interactively, that user pages are NOT eagerly mapped: a signed
// program is loaded (process_create_from_elf) and BEFORE it runs we count the
// present leaf pages in its address space — it must be 0, while the reserved
// footprint (code+data+stack VMAs) is non-zero. The program is then scheduled;
// its code/data/stack pages materialize one at a time via the #PF handler
// ([PF] demand page markers), it prints and exits. A final count is > 0.
#ifndef M14_DEMAND_DEMO
#define M14_DEMAND_DEMO 0
#endif
#if M14_DEMAND_DEMO
#include "trapframe.h"
#include "../crypto/user_hello_elf.h"

static uint8_t m14_idle_stack[16384] __attribute__((aligned(16)));
static int m14_step = 0, m14_done = 0;
static process_t* m14_proc = 0;

__attribute__((noreturn)) static void m14_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m14_step == 0) {
            m14_step = 1;
            debugcon_writestring("[M14] --- demand paging: lazy load ---\n");
            m14_proc = process_create_from_elf(user_hello_elf, user_hello_elf_len);
            if (!m14_proc) {
                debugcon_writestring("[M14] FAIL: hello not loaded\n");
                m14_done = 1; debugcon_writestring("[M14] DONE\n");
            } else {
                // Headline proof: reserved footprint > 0, but nothing mapped yet.
                uint32_t mapped = vmm_count_user_pages(m14_proc->space);
                debugcon_writestring("[M14] reserved footprint=");
                debugcon_print_hex(m14_proc->user_mem_bytes);
                debugcon_writestring(" mapped at load=");
                debugcon_print_hex((uint64_t)mapped);
                debugcon_writestring("\n");
                m14_proc->state = PROC_READY; // let the scheduler run it
            }
        } else if (m14_step == 1 && m14_proc && sched_count_alive_user() == 0 && !m14_done) {
            m14_step = 2; m14_done = 1;
            debugcon_writestring("[M14] DONE\n");
        }
        __asm__ volatile("sti; hlt");
    }
}

static void m14_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[M14] demand paging demo\n");
    static process_t  idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m14_idle_stack + sizeof(m14_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m14_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[M14] entering scheduler\n");
    arch_iret_to_tf(&idle_tf); // does not return
}
#endif /* M14_DEMAND_DEMO */

// ---- [M15] Fault-kill demo (gated; off by default) ----
// Proves, non-interactively, that a ring-3 fault terminates only the offending
// process and the kernel keeps running: (1) a signed `crashtest` does a wild
// write to NULL — the kernel kills it ([KILL] pid=...) and it never prints
// "STILL ALIVE"; (2) afterwards a signed `hello` is spawned and runs normally,
// proving the scheduler/kernel survived the fault.
#ifndef M15_KILL_DEMO
#define M15_KILL_DEMO 0
#endif
#if M15_KILL_DEMO
#include "trapframe.h"
#include "../crypto/user_crash_elf.h"
#include "../crypto/user_hello_elf.h"

static uint8_t m15_idle_stack[16384] __attribute__((aligned(16)));
static int m15_step = 0, m15_done = 0;

static void m15_spawn(const uint8_t* img, size_t len, const char* what) {
    process_t* p = process_create_from_elf(img, len);
    if (p) { p->state = PROC_READY;
        debugcon_writestring("[M15] spawned "); debugcon_writestring(what); debugcon_writestring("\n");
    } else {
        debugcon_writestring("[M15] FAIL: "); debugcon_writestring(what); debugcon_writestring(" not loaded\n");
    }
}

__attribute__((noreturn)) static void m15_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m15_step == 0) {
            m15_step = 1;
            debugcon_writestring("[M15] --- ring-3 fault must kill only the process ---\n");
            m15_spawn(user_crash_elf, user_crash_elf_len, "crashtest");
        } else if (m15_step == 1 && sched_count_alive_user() == 0) {
            m15_step = 2;
            debugcon_writestring("[M15] crashtest gone; kernel alive -> spawning hello\n");
            m15_spawn(user_hello_elf, user_hello_elf_len, "hello");
        } else if (m15_step == 2 && sched_count_alive_user() == 0 && !m15_done) {
            m15_done = 1;
            debugcon_writestring("[M15] DONE\n");
        }
        __asm__ volatile("sti; hlt");
    }
}

static void m15_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[M15] fault-kill demo\n");
    static process_t  idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m15_idle_stack + sizeof(m15_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m15_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[M15] entering scheduler\n");
    arch_iret_to_tf(&idle_tf); // does not return
}
#endif /* M15_KILL_DEMO */

// ---- [M16] Exec model demo (gated; off by default) ----
// Proves argv delivery + blocking wait + exit-status: a signed `parent` is
// spawned; it writes nothing — the kernel first drops the signed `child` ELF into
// the VFS, then `parent` SYS_SPAWNs "/m16_child.elf" with argv {alpha,beta},
// blocks in SYS_WAIT, and prints the child's exit status (== argc == 3).
#ifndef M16_EXEC_DEMO
#define M16_EXEC_DEMO 0
#endif
#if M16_EXEC_DEMO
#include "trapframe.h"
#include "../crypto/user_m16_child_elf.h"
#include "../crypto/user_m16_parent_elf.h"

static uint8_t m16_idle_stack[16384] __attribute__((aligned(16)));
static int m16_step = 0, m16_done = 0;

__attribute__((noreturn)) static void m16_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m16_step == 0) {
            m16_step = 1;
            extern int vfs_create(const char*, const void*, size_t);
            extern int vfs_remove(const char*);
            debugcon_writestring("[M16] --- argv + blocking wait + exit status ---\n");
            vfs_remove("/m16_child.elf");
            if (vfs_create("/m16_child.elf", user_m16_child_elf, user_m16_child_elf_len) == 0)
                debugcon_writestring("[M16] child ELF placed in VFS\n");
            else
                debugcon_writestring("[M16] FAIL: could not place child ELF\n");
            process_t* p = process_create_from_elf(user_m16_parent_elf, user_m16_parent_elf_len);
            if (p) { p->state = PROC_READY; debugcon_writestring("[M16] spawned parent\n"); }
            else   debugcon_writestring("[M16] FAIL: parent not loaded\n");
        } else if (m16_step == 1 && sched_count_alive_user() == 0 && !m16_done) {
            m16_done = 1;
            debugcon_writestring("[M16] DONE\n");
        }
        __asm__ volatile("sti; hlt");
    }
}

static void m16_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[M16] exec model demo\n");
    static process_t  idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m16_idle_stack + sizeof(m16_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m16_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[M16] entering scheduler\n");
    arch_iret_to_tf(&idle_tf);
}
#endif /* M16_EXEC_DEMO */

// ---- [M17] Blocking primitives demo (gated; off by default) ----
// Proves blocking SYS_SLEEP and blocking SYS_MSG_RECV: a sleeper blocks 10 ticks
// and verifies the uptime advanced; a consumer is spawned FIRST and blocks on the
// empty channel 0, then a producer is spawned and its send WAKES the blocked
// consumer (consumer-before-producer ordering can only deliver via a real block).
#ifndef M17_BLOCK_DEMO
#define M17_BLOCK_DEMO 0
#endif
#if M17_BLOCK_DEMO
#include "trapframe.h"
#include "../crypto/user_m17_sleeper_elf.h"
#include "../crypto/user_ipc_send_elf.h"
#include "../crypto/user_ipc_recv_elf.h"

static uint8_t m17_idle_stack[16384] __attribute__((aligned(16)));
static int m17_step = 0, m17_done = 0;
static process_t* m17_consumer = 0;

static process_t* m17_spawn(const uint8_t* img, size_t len, const char* what) {
    process_t* p = process_create_from_elf(img, len);
    if (p) { p->state = PROC_READY; debugcon_writestring("[M17] spawned "); debugcon_writestring(what); debugcon_writestring("\n"); }
    else   { debugcon_writestring("[M17] FAIL: "); debugcon_writestring(what); debugcon_writestring("\n"); }
    return p;
}

__attribute__((noreturn)) static void m17_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m17_step == 0) {
            m17_step = 1;
            debugcon_writestring("[M17] --- blocking sleep + blocking recv ---\n");
            m17_spawn(user_m17_sleeper_elf, user_m17_sleeper_elf_len, "sleeper");
            m17_consumer = m17_spawn(user_ipc_recv_elf, user_ipc_recv_elf_len, "consumer (will block on ch0)");
        } else if (m17_step == 1) {
            // Wait until the consumer has actually BLOCKED on the empty channel,
            // then release the producer — proving the recv blocked, not polled.
            if (m17_consumer && m17_consumer->state == PROC_BLOCKED) {
                m17_step = 2;
                debugcon_writestring("[M17] consumer is BLOCKED on recv -> spawning producer\n");
                m17_spawn(user_ipc_send_elf, user_ipc_send_elf_len, "producer");
            }
        } else if (m17_step == 2 && sched_count_alive_user() == 0 && !m17_done) {
            m17_done = 1;
            debugcon_writestring("[M17] DONE\n");
        }
        __asm__ volatile("sti; hlt");
    }
}

static void m17_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[M17] blocking primitives demo\n");
    static process_t  idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m17_idle_stack + sizeof(m17_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m17_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[M17] entering scheduler\n");
    arch_iret_to_tf(&idle_tf);
}
#endif /* M17_BLOCK_DEMO */

// ---- [M18] Dynamic memory demo (gated; off by default) ----
// Runs the signed `m18_mem`: malloc/free/reuse over sbrk, anonymous mmap, W^X
// rejection, mprotect read-only. It prints [m18] DONE-USER, then writes to a
// read-only page — which terminates it ([KILL]), proving mprotect enforces. The
// kernel survives (M15 mechanism).
#ifndef M18_MEM_DEMO
#define M18_MEM_DEMO 0
#endif
#if M18_MEM_DEMO
#include "trapframe.h"
#include "../crypto/user_m18_mem_elf.h"

static uint8_t m18_idle_stack[16384] __attribute__((aligned(16)));
static int m18_step = 0, m18_done = 0;

__attribute__((noreturn)) static void m18_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m18_step == 0) {
            m18_step = 1;
            debugcon_writestring("[M18] --- malloc/mmap/mprotect ---\n");
            process_t* p = process_create_from_elf(user_m18_mem_elf, user_m18_mem_elf_len);
            if (p) { p->state = PROC_READY; debugcon_writestring("[M18] spawned m18_mem\n"); }
            else   debugcon_writestring("[M18] FAIL: m18_mem not loaded\n");
        } else if (m18_step == 1 && sched_count_alive_user() == 0 && !m18_done) {
            m18_done = 1;
            debugcon_writestring("[M18] DONE\n");
        }
        __asm__ volatile("sti; hlt");
    }
}

static void m18_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[M18] dynamic memory demo\n");
    static process_t  idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m18_idle_stack + sizeof(m18_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m18_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[M18] entering scheduler\n");
    arch_iret_to_tf(&idle_tf);
}
#endif /* M18_MEM_DEMO */

// ---- [M19] Copy-on-write fork demo (gated; off by default) ----
// Runs the signed `m19_fork`: it fills a static buffer, fork()s, the child
// mutates the buffer (COW -> private copy) and exits 7; the parent waitpid()s and
// confirms its own buffer is untouched. Proves fork, COW isolation, and inherited
// memory. [FORK]/[COW] markers appear on debugcon.
#ifndef M19_FORK_DEMO
#define M19_FORK_DEMO 0
#endif
#if M19_FORK_DEMO
#include "trapframe.h"
#include "../crypto/user_m19_fork_elf.h"

static uint8_t m19_idle_stack[16384] __attribute__((aligned(16)));
static int m19_step = 0, m19_done = 0;

__attribute__((noreturn)) static void m19_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m19_step == 0) {
            m19_step = 1;
            debugcon_writestring("[M19] --- copy-on-write fork ---\n");
            process_t* p = process_create_from_elf(user_m19_fork_elf, user_m19_fork_elf_len);
            if (p) { p->state = PROC_READY; debugcon_writestring("[M19] spawned m19_fork\n"); }
            else   debugcon_writestring("[M19] FAIL: m19_fork not loaded\n");
        } else if (m19_step == 1 && sched_count_alive_user() == 0 && !m19_done) {
            m19_done = 1;
            debugcon_writestring("[M19] DONE\n");
        }
        __asm__ volatile("sti; hlt");
    }
}

static void m19_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[M19] fork/COW demo\n");
    static process_t  idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m19_idle_stack + sizeof(m19_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m19_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[M19] entering scheduler\n");
    arch_iret_to_tf(&idle_tf);
}
#endif /* M19_FORK_DEMO */

// ---- [M25] Anonymous pipes demo (gated; off by default) ----
#ifndef M25_PIPE_DEMO
#define M25_PIPE_DEMO 0
#endif
#if M25_PIPE_DEMO
#include "trapframe.h"
#include "../crypto/user_m25_pipe_elf.h"

static uint8_t m25_idle_stack[16384] __attribute__((aligned(16)));
static int m25_step = 0, m25_done = 0;

__attribute__((noreturn)) static void m25_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m25_step == 0) {
            m25_step = 1;
            debugcon_writestring("[M25] --- anonymous pipes ---\n");
            process_t* p = process_create_from_elf(user_m25_pipe_elf, user_m25_pipe_elf_len);
            if (p) { p->state = PROC_READY; debugcon_writestring("[M25] spawned m25_pipe\n"); }
            else   debugcon_writestring("[M25] FAIL: m25_pipe not loaded\n");
        } else if (m25_step == 1 && sched_count_alive_user() == 0 && !m25_done) {
            m25_done = 1;
            debugcon_writestring("[M25] DONE\n");
        }
        __asm__ volatile("sti; hlt");
    }
}

static void m25_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[M25] pipes demo\n");
    static process_t  idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m25_idle_stack + sizeof(m25_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m25_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[M25] entering scheduler\n");
    arch_iret_to_tf(&idle_tf);
}
#endif /* M25_PIPE_DEMO */

// ---- [M20] Page cache / file-backed mmap demo (gated; off by default) ----
// Writes a known file into the VFS, then runs the signed `m20_mmap`: it maps the
// file read-only (MAP_PRIVATE via the page cache), checks the mapped bytes, then
// read()s the same file and confirms read() and mmap return identical bytes
// (they share cache pages -> coherent).
#ifndef M20_MMAP_DEMO
#define M20_MMAP_DEMO 0
#endif
#if M20_MMAP_DEMO
#include "trapframe.h"
#include "../crypto/user_m20_mmap_elf.h"

static uint8_t m20_idle_stack[16384] __attribute__((aligned(16)));
static int m20_step = 0, m20_done = 0;

__attribute__((noreturn)) static void m20_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m20_step == 0) {
            m20_step = 1;
            extern int vfs_create(const char*, const void*, size_t);
            extern int vfs_remove(const char*);
            debugcon_writestring("[M20] --- file-backed mmap via page cache ---\n");
            static const char content[] = "M20-PAGE-CACHE-OK";
            vfs_remove("/m20.txt");
            if (vfs_create("/m20.txt", content, sizeof(content) - 1) == 0)
                debugcon_writestring("[M20] wrote /m20.txt to VFS\n");
            else
                debugcon_writestring("[M20] FAIL: could not write /m20.txt\n");
            process_t* p = process_create_from_elf(user_m20_mmap_elf, user_m20_mmap_elf_len);
            if (p) { p->state = PROC_READY; debugcon_writestring("[M20] spawned m20_mmap\n"); }
            else   debugcon_writestring("[M20] FAIL: m20_mmap not loaded\n");
        } else if (m20_step == 1 && sched_count_alive_user() == 0 && !m20_done) {
            m20_done = 1;
            debugcon_writestring("[M20] DONE\n");
        }
        __asm__ volatile("sti; hlt");
    }
}

static void m20_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[M20] page cache demo\n");
    static process_t  idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m20_idle_stack + sizeof(m20_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m20_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[M20] entering scheduler\n");
    arch_iret_to_tf(&idle_tf);
}
#endif /* M20_MMAP_DEMO */

// [M23] POSIX FS personality demo: run the signed `m23_fs`, which opens /dev/zero,
// /dev/null, /proc/uptime and stat()/lseek()s a /dev block node — proving a
// signed ring-3 program uses the Linux-style filesystem.
#ifndef M23_FS_DEMO
#define M23_FS_DEMO 0
#endif
#if M23_FS_DEMO
#include "trapframe.h"
#include "../crypto/user_m23_fs_elf.h"
static uint8_t m23_idle_stack[16384] __attribute__((aligned(16)));
static int m23_step = 0, m23_done = 0;
__attribute__((noreturn)) static void m23_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m23_step == 0) {
            m23_step = 1;
            debugcon_writestring("[M23] --- POSIX FS personality demo ---\n");
            process_t* p = process_create_from_elf(user_m23_fs_elf, user_m23_fs_elf_len);
            if (p) { p->state = PROC_READY; debugcon_writestring("[M23] spawned m23_fs\n"); }
            else   debugcon_writestring("[M23] FAIL: m23_fs not loaded\n");
        } else if (m23_step == 1 && sched_count_alive_user() == 0 && !m23_done) {
            m23_done = 1;
            debugcon_writestring("[M23] DONE\n");
        }
        __asm__ volatile("sti; hlt");
    }
}
static void m23_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    static process_t idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m23_idle_stack + sizeof(m23_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m23_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    arch_iret_to_tf(&idle_tf);
}
#endif /* M23_FS_DEMO */

// ---- [M24] Networking CAP_NET demo (gated; off by default) ----
// Proves, non-interactively, that the socket syscalls are gated by CAP_NET in
// the signed manifest: a program signed WITH the CAP_NET manifest gets a socket
// (prints "[m24net] socket OK"); the same program signed WITHOUT it is refused
// the very first socket() call ("[m24net] socket DENIED"). No NIC required — the
// gate is enforced in the syscall dispatcher before any device I/O. The live
// data path (DHCP/DNS/TCP/UDP) is exercised interactively from the shell.
#ifndef M24_NET_DEMO
#define M24_NET_DEMO 0
#endif
#if M24_NET_DEMO
#include "trapframe.h"
#include "../crypto/user_m24net_elf.h"
#include "../crypto/user_m24nonet_elf.h"
static uint8_t m24_idle_stack[16384] __attribute__((aligned(16)));
static int m24_step = 0, m24_done = 0;
__attribute__((noreturn)) static void m24_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m24_step == 0) {
            m24_step = 1;
            debugcon_writestring("[M24] --- CAP_NET granted (socket allowed) ---\n");
            process_t* p = process_create_from_elf(user_m24net_elf, user_m24net_elf_len);
            if (p) { p->state = PROC_READY; debugcon_writestring("[M24] spawned m24_net (CAP_NET)\n"); }
            else   debugcon_writestring("[M24] FAIL: m24_net not loaded\n");
        } else if (m24_step == 1 && sched_count_alive_user() == 0) {
            m24_step = 2;
            debugcon_writestring("[M24] --- no CAP_NET (socket denied) ---\n");
            process_t* p = process_create_from_elf(user_m24nonet_elf, user_m24nonet_elf_len);
            if (p) { p->state = PROC_READY; debugcon_writestring("[M24] spawned m24_nonet (no cap)\n"); }
            else   debugcon_writestring("[M24] FAIL: m24_nonet not loaded\n");
        } else if (m24_step == 2 && sched_count_alive_user() == 0 && !m24_done) {
            m24_done = 1;
            debugcon_writestring("[M24] DONE\n");
        }
        __asm__ volatile("sti; hlt");
    }
}
static void m24_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    static process_t idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m24_idle_stack + sizeof(m24_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m24_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    arch_iret_to_tf(&idle_tf);
}
#endif /* M24_NET_DEMO */

// ---- [M10] Interactive shell as the scheduler idle task ----
// Running the shell as the idle task lets `run <path>` spawn a ring-3 process:
// the timer preempts idle (shell) into the user task; on SYS_EXIT the scheduler
// returns to the saved idle context, so the shell regains control.
#if M10_RUN_DEMO
#include "../crypto/user_hello_elf.h"
#endif
#if M31_LIBC_DEMO
#include "../crypto/user_m31_libc_elf.h"
#endif
extern void shell_init(void);
extern void shell_run(void);
extern void shell_run_line(const char* line);
static uint8_t   m10_idle_stack[32768] __attribute__((aligned(16)));
static process_t m10_idle;
static trapframe_t m10_idle_tf;

__attribute__((noreturn)) static void m10_shell_idle_entry(void) {
    shell_init();
#if ENABLE_FB
    extern void fb_console_draw_logo(void); fb_console_draw_logo();
    extern void fb_console_flush(void); fb_console_flush();
#endif
#if M10_RUN_DEMO
    // Non-interactive gate: write the embedded signed hello onto the disk FS,
    // then run it back from disk (full chain: write->read->verify->ring3->exit).
    {
        extern int vfs_create(const char*, const void*, size_t);
        extern int vfs_remove(const char*);
        vfs_remove("/mnt/hello.elf");
        if (vfs_create("/mnt/hello.elf", user_hello_elf, user_hello_elf_len) == 0)
            debugcon_writestring("[M10] wrote signed hello.elf to disk\n");
        else
            debugcon_writestring("[M10] FAIL: could not write hello.elf to disk\n");
        // Negative test: a tampered copy on disk must be REFUSED by the gate.
        {
            extern int ksys_spawn(const char*);
            static uint8_t bad[8192];
            for (size_t i = 0; i < user_hello_elf_len && i < sizeof(bad); i++) bad[i] = user_hello_elf[i];
            bad[0x100] ^= 0x01; // flip a code byte -> signature mismatch
            vfs_remove("/mnt/bad.elf");
            vfs_create("/mnt/bad.elf", bad, user_hello_elf_len);
            int br = ksys_spawn("/mnt/bad.elf");
            debugcon_writestring(br < 0 ? "[M10] tampered disk ELF REFUSED (good)\n"
                                        : "[M10] FAIL: tampered disk ELF accepted\n");
        }
        shell_run_line("run /mnt/hello.elf");
    }
#endif
#if M31_LIBC_DEMO
    // [M31] Run the signed libc test (printf/string/stdlib/ctype) in ring 3. Its
    // stdout (fd 1) mirrors to debugcon, so the harness can assert on [m31] lines.
    {
        extern int vfs_create(const char*, const void*, size_t);
        extern int vfs_remove(const char*);
        vfs_remove("/bin/m31");
        if (vfs_create("/bin/m31", user_m31_libc_elf, user_m31_libc_elf_len) == 0)
            debugcon_writestring("[M31] wrote signed libc test to /bin/m31\n");
        shell_run_line("run /bin/m31");
    }
#endif
    shell_run();
    for (;;) __asm__ volatile("hlt");
}

// Set up the shell idle task and enter the scheduler (does not return).
static void m10_enter_shell(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    for (unsigned i=0;i<sizeof(m10_idle);i++)    ((uint8_t*)&m10_idle)[i]=0;
    for (unsigned i=0;i<sizeof(m10_idle_tf);i++) ((uint8_t*)&m10_idle_tf)[i]=0;
    m10_idle.pid = 0;
    m10_idle.space = vmm_get_kernel_space();
    m10_idle.kstack_top = (uint64_t)(m10_idle_stack + sizeof(m10_idle_stack));
    m10_idle.tf = &m10_idle_tf;
    m10_idle.state = PROC_RUNNING;
    m10_idle_tf.rip = (uint64_t)m10_shell_idle_entry;
    m10_idle_tf.cs = 0x08; m10_idle_tf.ss = 0x10; m10_idle_tf.rflags = 0x202;
    m10_idle_tf.rsp = m10_idle.kstack_top;
    sched_set_idle(&m10_idle);
    sched_set_current(&m10_idle);
    tss_set_kernel_stack(m10_idle.kstack_top);
    arch_iret_to_tf(&m10_idle_tf); // does not return
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
    // Crash-signature: also send to debugcon so smoke log captures it.
    debugcon_writestring("[M2] Stack switch ok. RSP=");
    debugcon_print_hex(rsp);
    debugcon_writestring("\n");

    // NOTE(M2-B): We are now past the narrow window where no IDT/IST was
    // present.  vmm_init_physmap() and vmm_alloc_kernel_stack() ran with
    // interrupts disabled and no IDT loaded.  If either faulted, the result
    // would have been a triple fault (same risk exists for vmm_init() in M1).
    // A minimal no-STI fallback IDT could be added in M3 for safer debugging.
    idt_init();

    heap_init();
    sched_init();
    driver_registry_init();
    { extern void ipc_init(void); ipc_init(); }  // [M13] kernel IPC channels
    { // [M28-1] discover CPUs/LAPIC/IOAPIC (read-only). UEFI hands the RSDP over;
      // MB2 falls back to a BIOS-area scan.
      extern int acpi_init(uint64_t);
      extern uint32_t g_multiboot_magic; extern uint64_t g_multiboot_info;
      uint64_t rsdp_hint = 0;
      if (g_multiboot_magic == 0 && g_multiboot_info) {
          struct secos_boot_info* bi = (struct secos_boot_info*)g_multiboot_info;
          rsdp_hint = bi->acpi_rsdp;
      }
      acpi_init(rsdp_hint);
    }
    timer_init(1000);
    keyboard_init();

    // [M28-2] Retire the 8259 PIC + PIT IRQ0 onto the LAPIC timer + IOAPIC when
    // ACPI gave us a MADT/IOAPIC. timer_init() above already programmed the PIT
    // and timer_frequency=1000 (still used for uptime math); the switchover masks
    // the PIC and drives the same vector 0x20 (isr_timer) from the LAPIC timer at
    // 1 kHz, so timer_ticks keeps advancing. Falls back to PIC/PIT on no MADT.
    {
        extern int apic_switchover(uint32_t);
        if (apic_switchover(1000) == 0) {
            // [M29] LAPIC is up: record the BSP's real LAPIC ID so this_cpu()
            // resolves CPU 0 correctly and APs later get distinct slots.
            { extern void smp_set_bsp_lapic_id(uint32_t); extern uint32_t lapic_get_id(void);
              smp_set_bsp_lapic_id(lapic_get_id()); }
            // Prove the LAPIC timer actually fires (and IRQ delivery works) by
            // confirming timer_ticks advances. Bounded spin so a dead timer can't
            // hang boot — on failure we log and continue (PIC was already masked,
            // but a non-ticking system is a loud, visible failure in the log).
            uint64_t t0 = timer_get_ticks();
            int ticking = 0;
            for (volatile uint64_t i = 0; i < 300000000ull; i++) {
                if (timer_get_ticks() != t0) { ticking = 1; break; }
            }
            debugcon_writestring(ticking ? "[APIC] timer tick verified\n"
                                         : "[APIC] FAIL: LAPIC timer not ticking\n");
        }
    }

    // [M28-3] TSC timekeeping: calibrate rdtsc against PIT channel 2 (independent
    // of the 8259/APIC) for a monotonic nanosecond clock. The 1 kHz scheduler tick
    // is unchanged. Self-check: ktime_ns() must be non-zero and strictly advance.
    {
        extern void tsc_init(void);
        extern uint64_t ktime_ns(void); extern uint64_t tsc_hz(void);
        tsc_init();
        uint64_t a = ktime_ns();
        for (volatile int i = 0; i < 100000; i++) { __asm__ volatile(""); }
        uint64_t b = ktime_ns();
        int ok = (tsc_hz() > 100000000ull) && (b > a);  // >100 MHz and monotonic
        debugcon_writestring(ok ? "[TSC] monotonic OK\n"
                                : "[TSC] FAIL: clock not advancing\n");
    }

    // [M29-2] Bring up the application processors (INIT-SIPI-SIPI). Each AP
    // enables its LAPIC, registers online, and parks (M29-3 enters the scheduler).
    // No-op on a single-CPU system or without ACPI/APIC.
    { extern void smp_init(void); smp_init(); }

#if M4_SELFTEST_ENABLE
    m4_run_selftests();
#endif

    // [M9] Crypto known-answer self-tests (SHA-256, Ed25519 verify). Cheap; the
    // signing trust root depends on these being correct.
    { extern int crypto_selftest(void); crypto_selftest(); }

    // [M12] W^X runtime gate self-test: a W+X mapping request must be refused.
    vmm_wx_selftest();

    // [M12] Heap large-allocation self-test: kmalloc > one frame must return a
    // correctly-sized, writable region (multi-frame contiguous backing) — the
    // M11 gotcha. Write a pattern across the whole block and read it back.
    {
        extern void* kmalloc(size_t); extern void kfree(void*);
        size_t big = 64 * 1024; // 16 frames
        uint8_t* p = (uint8_t*)kmalloc(big);
        int ok = (p != 0);
        if (ok) {
            for (size_t i = 0; i < big; i++) p[i] = (uint8_t)(i * 31 + 7);
            for (size_t i = 0; i < big && ok; i++) if (p[i] != (uint8_t)(i * 31 + 7)) ok = 0;
            kfree(p);
        }
        debugcon_writestring(ok ? "[HEAP] large kmalloc(64K) OK\n"
                                : "[HEAP] FAIL: large kmalloc(64K)\n");
    }

    // [M7] Cooperative scheduling demo — two ring3 processes yielding to each
    // other via SYS_YIELD.  Disabled by default so normal boot reaches the
    // shell; the self-test harness builds with -DM7_RING3_DEMO=1.
    // NOTE: arch_enter_user_mode() does not return, so when enabled this demo
    // takes over the CPU (the cooperative yield loop runs forever).
#ifndef M7_RING3_DEMO
#define M7_RING3_DEMO 0
#endif
#if M7_RING3_DEMO
    // User code: mov rax,0 / int 0x80 / jmp loop  (SYS_YIELD=0)
    //   48 C7 C0 00 00 00 00   mov rax, 0
    //   CD 80                  int 0x80
    //   EB F5                  jmp -11
    {
        extern void arch_enter_user_mode(process_t* p);
        #include "../mm/elf.h"

        unsigned char elf_buf[512];
        for (int i = 0; i < 512; i++) elf_buf[i] = 0;
        // ELF header
        elf_buf[0]=0x7F; elf_buf[1]='E'; elf_buf[2]='L'; elf_buf[3]='F';
        elf_buf[4]=2; elf_buf[5]=1; elf_buf[6]=1;
        *(uint16_t*)(elf_buf+16) = 2;       // ET_EXEC
        *(uint16_t*)(elf_buf+18) = 0x3E;    // EM_X86_64
        *(uint32_t*)(elf_buf+20) = 1;       // EV_CURRENT
        *(uint64_t*)(elf_buf+24) = USER_CODE_BASE; // e_entry
        *(uint64_t*)(elf_buf+32) = 64;      // e_phoff
        *(uint16_t*)(elf_buf+52) = 64;      // e_ehsize
        *(uint16_t*)(elf_buf+54) = 56;      // e_phentsize
        *(uint16_t*)(elf_buf+56) = 1;       // e_phnum
        // PHDR: PT_LOAD, RX
        *(uint32_t*)(elf_buf+64)  = 1;              // p_type = PT_LOAD
        *(uint32_t*)(elf_buf+68)  = PF_R | PF_X;    // p_flags
        *(uint64_t*)(elf_buf+72)  = 0x100ULL;       // p_offset
        *(uint64_t*)(elf_buf+80)  = USER_CODE_BASE; // p_vaddr
        *(uint64_t*)(elf_buf+88)  = USER_CODE_BASE; // p_paddr
        *(uint64_t*)(elf_buf+96)  = 0x20ULL;        // p_filesz
        *(uint64_t*)(elf_buf+104) = 0x20ULL;        // p_memsz
        *(uint64_t*)(elf_buf+112) = 0x1000ULL;      // p_align
        // User code at offset 0x100: mov rax,0 / int 0x80 / jmp loop
        elf_buf[0x100] = 0x48; elf_buf[0x101] = 0xC7; elf_buf[0x102] = 0xC0;
        elf_buf[0x103] = 0x00; elf_buf[0x104] = 0x00; elf_buf[0x105] = 0x00; elf_buf[0x106] = 0x00;
        elf_buf[0x107] = 0xCD; elf_buf[0x108] = 0x80;
        elf_buf[0x109] = 0xEB; elf_buf[0x10A] = 0xF5; // jmp rel8 -11

        terminal_writestring("[M7] Creating two ring3 yield-loop processes...\n");
        debugcon_writestring("[M7] Creating two ring3 yield-loop processes\n");

        process_t* p1 = process_create_from_elf(elf_buf, 512);
        process_t* p2 = process_create_from_elf(elf_buf, 512);

        if (!p1 || !p2) {
            terminal_writestring("[M7] FAILED to create ring3 processes\n");
            debugcon_writestring("[M7] FAILED to create ring3 processes\n");
        } else {
            p1->state = PROC_RUNNING;
            p2->state = PROC_READY;
            sched_set_current(p1);

            debugcon_writestring("[M7] p1 pid=");
            debugcon_print_hex(p1->pid);
            debugcon_writestring(" p2 pid=");
            debugcon_print_hex(p2->pid);
            debugcon_writestring("\n");

            terminal_writestring("[M7] Entering ring3 (p1) — cooperative yield loop\n");
            debugcon_writestring("[M7] Entering ring3\n");
            arch_enter_user_mode(p1);
            // NOT REACHED
        }
    }
#endif /* M7_RING3_DEMO */

#if M8_SCHED_DEMO
    m8_run_demo(); // preemptive scheduling demo — does not return
#endif

    // Initialize native RAMFS (fallback)
    extern int ramfs_init(void); ramfs_init();
    // Initialize VFS
    extern void vfs_init(void); vfs_init();
    // Probe for a virtio-blk disk (QEMU -drive if=virtio). Optional: absent on
    // the plain ISO boot. Read sector 0 as a smoke check (logged on debugcon).
    {
        extern int virtio_blk_init(void);
        extern int ahci_init(void); // [M21] AHCI/SATA — for VMware / physical PCs
        extern int nvme_init(void); // [M22] NVMe — NVMe-only laptops / VMware NVMe
        extern block_dev_t* block_find(const char* name);
        int have_virtio = (virtio_blk_init() == 0);
        // [M21] Probe AHCI too (registers block dev "sda"). On QEMU q35 a SATA
        // disk attaches to the built-in AHCI; on VMware/real PCs this is the path.
        // [M22] Probe NVMe too (registers "nvme0n1"). Each probe is a no-op when
        // its controller is absent, so all storage back-ends can coexist.
        if (!have_virtio) { ahci_init(); nvme_init(); }
        // [M22] Bring up USB (xHCI): enumerates a HID keyboard and a Mass Storage
        // device (registers block dev "usb0"). Always probed — independent of the
        // disk back-end above.
        extern int usb_init(void);
        usb_init();
        if (have_virtio) {
            block_dev_t* vda = block_find("vda");
            if (vda) {
                static uint8_t s0[512];
                int r = vda->read(vda, 0, s0, 1);
                debugcon_writestring("[M10] virtio-blk read sector0 r=");
                debugcon_print_hex((uint64_t)r);
                debugcon_writestring(" bytes[0..3]=");
                debugcon_print_hex(((uint64_t)s0[0]<<24)|((uint64_t)s0[1]<<16)|((uint64_t)s0[2]<<8)|s0[3]);
                debugcon_writestring("\n");
#if M10_BLK_WRITE_TEST
                /* Write a pattern to a high scratch sector, read it back. */
                if (vda->write) {
                    static uint8_t wb[512], rb[512];
                    for (int i = 0; i < 512; i++) wb[i] = (uint8_t)(i ^ 0xA5);
                    uint64_t scratch = (vda->sector_count > 1) ? (vda->sector_count - 1) : 0;
                    int wr = vda->write(vda, scratch, wb, 1);
                    int rr = vda->read(vda, scratch, rb, 1);
                    int ok = (wr == 1 && rr == 1);
                    for (int i = 0; ok && i < 512; i++) if (rb[i] != wb[i]) ok = 0;
                    debugcon_writestring(ok ? "[M10] virtio-blk write+readback: OK\n"
                                            : "[M10] virtio-blk write+readback: FAIL\n");
                }
#endif
            }
        }
    }
    // [M23] Persistent root: if a SecOS ext2 *system* disk is present (it carries
    // the marker file /.secosroot), mount it as the VFS root "/". Plain ext2/ext4
    // *data* disks lack the marker and are never grabbed as root — so the boot
    // self-tests (no system disk) keep the RAMFS root unchanged.
    int root_is_ext2 = 0;
    {
        extern int ext2_mount_root(const char* dev_name);
        extern block_dev_t* block_find(const char* name);
        static const char* rc[] = { "vda","sda","sdb","sdc","sdd","nvme0n1","usb0" };
        for (unsigned i=0;i<sizeof(rc)/sizeof(rc[0]) && !root_is_ext2;i++) {
            if (!block_find(rc[i])) continue;
            if (ext2_mount_root(rc[i]) == 0) {
                root_is_ext2 = 1;
                terminal_writestring("[M23] persistent ext2 root mounted from ");
                terminal_writestring(rc[i]); terminal_writestring("\n");
                debugcon_writestring("[M23] persistent ext2 root on ");
                debugcon_writestring(rc[i]); debugcon_writestring("\n");
                // Refresh /VERSION on the persistent root (best-effort).
                extern int vfs_remove(const char*); extern int vfs_create(const char*, const void*, size_t);
                const char* ver = "VERSION=0.1.0-dev\nBUILD_TS=" BUILD_TS "\nGIT_HASH=" GIT_HASH "\n";
                size_t vl=0; while(ver[vl]) vl++; vfs_remove("/VERSION"); vfs_create("/VERSION", ver, vl);
            }
        }
    }
    // Root filesystem defaults to RAMFS when there is no persistent ext2 root.
    if (!root_is_ext2) {
        extern int vfs_mount_ramfs(void);
        if (vfs_mount_ramfs() == 0) terminal_writestring("[VFS] root RAMFS mounted\n");
        else terminal_writestring("[VFS] root RAMFS FAIL\n");
    }
    // Mount a disk at /mnt: try FAT32, then ext2/ext4 (multi-mount). Skipped when
    // an ext2 disk is already the persistent root (single-root model).
    if (!root_is_ext2) {
        extern int fat32_mount(const char* dev_name, const char* mount_point);
        extern int ext2_mount(const char* dev_name, const char* mount_point);
        extern int vfs_remove(const char* path);
        extern int vfs_read_all(const char* path, void* buf, size_t bufsize);
        extern int vfs_create(const char* path, const void* data, size_t size);
        const char* fsname = 0;
        // [M21] Try every disk present — virtio "vda" (QEMU) and AHCI "sda".."sdd"
        // (VMware / physical PC) — and mount the first that holds a FAT32 or extN
        // volume. On VMware the boot ESP and the data disk are both SATA, so the
        // ESP's own disk is simply skipped here (it's GPT, not a raw FS volume).
        static const char* cand[] = { "vda", "sda", "sdb", "sdc", "sdd", "nvme0n1", "usb0" };
        const char* mounted_dev = 0;        // [M26] remember which device backs /mnt
        for (unsigned i = 0; i < sizeof(cand)/sizeof(cand[0]) && !fsname; i++) {
            if (!block_find(cand[i])) continue;
            if      (fat32_mount(cand[i], "/mnt") == 0) { fsname = "FAT32"; mounted_dev = cand[i]; }
            else if (ext2_mount (cand[i], "/mnt") == 0) { fsname = "extN";  mounted_dev = cand[i]; }
        }
        (void)mounted_dev;
        if (fsname) {
            debugcon_writestring("[M10] disk mounted at /mnt fs=");
            debugcon_writestring(fsname); debugcon_writestring("\n");
            // Read a host-populated marker file (proves persisted read). FAT
            // upcases to 8.3; ext keeps the lowercase name.
            static char rdbuf[64];
            for (int i = 0; i < 64; i++) rdbuf[i] = 0;
            int n = vfs_read_all("/mnt/HELLO.TXT", rdbuf, sizeof(rdbuf));
            if (n <= 0) { for (int i=0;i<64;i++) rdbuf[i]=0; n = vfs_read_all("/mnt/hello.txt", rdbuf, sizeof(rdbuf)); }
            debugcon_writestring("[M10] read /mnt hello n=");
            debugcon_print_hex((uint64_t)n);
            if (n > 0) { debugcon_writestring(" data=\""); debugcon_writestring(rdbuf); debugcon_writestring("\""); }
            debugcon_writestring("\n");
            // Write a new file, read it back, verify byte-identical.
            const char* msg = "SECoS disk write OK\n";
            size_t mlen = 0; while (msg[mlen]) mlen++;
            vfs_remove("/mnt/secos_w.txt"); // ignore error if absent
            int cr = vfs_create("/mnt/secos_w.txt", msg, mlen);
            static char back[64]; for (int i = 0; i < 64; i++) back[i] = 0;
            int rn = vfs_read_all("/mnt/secos_w.txt", back, sizeof(back));
            int match = (cr == 0 && rn == (int)mlen);
            for (size_t i = 0; match && i < mlen; i++) if (back[i] != msg[i]) match = 0;
            debugcon_writestring(match ? "[M10] disk write+readback: OK\n"
                                       : "[M10] disk write+readback: FAIL\n");
#if M27_RECOVER_DEMO
            // [M27a] Journal replay: the disk carries a dirty JBD2 journal whose
            // committed transaction rewrites /mnt/target.bin from "OLD" to "NEW".
            // jbd2_recover() runs in ext2_mount; if it replayed, the file is "NEW".
            {
                static char tb[2048]; for(int i=0;i<2048;i++) tb[i]=0;
                int tn = vfs_read_all("/mnt/target.bin", tb, sizeof(tb));
                debugcon_writestring("[M27] target.bin n="); debugcon_print_hex((uint64_t)tn);
                debugcon_writestring(" data=\""); for(int i=0;i<3&&i<tn;i++) debugcon_putchar(tb[i]);
                debugcon_writestring("\" (expect NEW after replay)\n");
                debugcon_writestring((tn>=3 && tb[0]=='N'&&tb[1]=='E'&&tb[2]=='W')
                    ? "[M27] REPLAY OK\n" : "[M27] REPLAY FAIL\n");
            }
#endif
#if defined(M27B_CRASH)
            // [M27b] Write-side journaling crash test, RUN 1: create a file in a
            // single journalled transaction, but cut power at a precise point in
            // the commit (M27B_CRASH=1 before the commit block, =2 after s_start is
            // published). Then halt — the next boot's recovery must leave the fs
            // ATOMIC (file absent for =1, present for =2), e2fsck-clean.
            if (fsname[0]=='e') {
                extern void ext2_test_set_crash(int);
                debugcon_writestring("[M27B] run1: create /mnt/newf with crash mode\n");
                ext2_test_set_crash(M27B_CRASH);
                vfs_create("/mnt/newf", "x", 1);
                debugcon_writestring("[M27B] run1 done, halting (simulated crash)\n");
                for(;;) __asm__ volatile("cli; hlt");
            }
#endif
#if defined(M27B_VERIFY)
            // [M27b] RUN 2: the mount already ran recovery (jbd2_recover). Report
            // whether /mnt/newf survived — must be atomic (present XOR absent).
            {
                extern vfs_inode_t* vfs_lookup(const char*);
                vfs_inode_t* nf = vfs_lookup("/mnt/newf");
                debugcon_writestring(nf ? "[M27B] verify: newf PRESENT\n"
                                        : "[M27B] verify: newf ABSENT\n");
                debugcon_writestring("[M27B] verify done\n");
            }
#endif
#if M26_FS_DEMO
            // [M26] VFS maturity: metadata (chmod/chown), symlinks, mount control.
            // Needs an ext2 /mnt (FAT32 has no setattr/symlink). Run on extN only.
            if (fsname[0]=='e') {
                debugcon_writestring("[M26] --- VFS maturity (metadata + symlinks) ---\n");
                vfs_remove("/mnt/m26.txt");
                vfs_create("/mnt/m26.txt", "hi\n", 3);
                vfs_attr_t a; a.mode=0640; a.uid=0; a.gid=0; a.atime=0; a.mtime=0;
                vfs_setattr("/mnt/m26.txt", &a, VFS_ATTR_MODE);
                vfs_inode_t* v = vfs_lookup("/mnt/m26.txt");
                debugcon_writestring("[M26] chmod 0640 -> mode=");
                debugcon_print_hex(v ? (v->mode & 0xFFF) : 0); debugcon_writestring("\n");
                a.uid=1000; a.gid=1000;
                vfs_setattr("/mnt/m26.txt", &a, VFS_ATTR_UID|VFS_ATTR_GID);
                v = vfs_lookup("/mnt/m26.txt");
                debugcon_writestring("[M26] chown 1000:1000 -> uid=");
                debugcon_print_hex(v?v->uid:0); debugcon_writestring(" gid=");
                debugcon_print_hex(v?v->gid:0); debugcon_writestring("\n");
                vfs_remove("/mnt/m26.lnk");
                int sl = vfs_symlink("m26.txt", "/mnt/m26.lnk");
                char tgt[64]; for(int i=0;i<64;i++) tgt[i]=0;
                int rl = vfs_readlink("/mnt/m26.lnk", tgt, sizeof(tgt));
                debugcon_writestring("[M26] symlink rc="); debugcon_print_hex((uint64_t)(int64_t)sl);
                debugcon_writestring(" readlink="); debugcon_print_hex((uint64_t)rl);
                debugcon_writestring(" target=\""); debugcon_writestring(tgt); debugcon_writestring("\"\n");
                vfs_inode_t* lk = vfs_lookup("/mnt/m26.lnk");           // lstat: the link itself
                vfs_inode_t* tg = vfs_lookup_follow("/mnt/m26.lnk");    // stat: follows to file
                debugcon_writestring("[M26] lstat type="); debugcon_print_hex(lk?lk->type:0);
                debugcon_writestring(" stat-follow type="); debugcon_print_hex(tg?tg->type:0);
                debugcon_writestring(" (3=symlink,1=file)\n");
                // mount control: unmount /mnt then re-mount the same device.
                int um = vfs_unmount("/mnt");
                int rm = mounted_dev ? ext2_mount(mounted_dev, "/mnt") : -1;
                debugcon_writestring("[M26] umount rc="); debugcon_print_hex((uint64_t)(int64_t)um);
                debugcon_writestring(" remount rc="); debugcon_print_hex((uint64_t)(int64_t)rm); debugcon_writestring("\n");
                debugcon_writestring("[M26] DONE\n");
            }
#endif
        } else {
            debugcon_writestring("[M10] no disk mounted at /mnt (no vda/sda)\n");
        }
    }
    // [M23] POSIX-style filesystem layout: virtual filesystems + FHS skeleton.
    {
        extern int devfs_mount(const char*);
        extern int procfs_mount(const char*);
        extern int sysfs_mount(const char*);
        extern int vfs_mkdir(const char*);
        // FHS directory skeleton on the root (ramfs today; persistent ext2 root
        // is the next step). Ignore "already exists".
        static const char* fhs[] = { "/bin","/etc","/dev","/proc","/sys","/tmp",
                                     "/usr","/opt","/home","/lib","/root","/var" };
        for (unsigned i=0;i<sizeof(fhs)/sizeof(fhs[0]);i++) vfs_mkdir(fhs[i]);
        int dv = devfs_mount("/dev");
        int pr = procfs_mount("/proc");
        int sy = sysfs_mount("/sys");
        debugcon_writestring("[M23] devfs(/dev)="); debugcon_print_hex((uint64_t)(dv==0));
        debugcon_writestring(" procfs(/proc)="); debugcon_print_hex((uint64_t)(pr==0));
        debugcon_writestring(" sysfs(/sys)="); debugcon_print_hex((uint64_t)(sy==0));
        debugcon_writestring("\n");
        // Quick boot self-checks (debugcon, like the other subsystems).
        static char pb[64]; for (int i=0;i<64;i++) pb[i]=0;
        int n = vfs_read_all("/proc/version", pb, sizeof(pb));
        if (n>0) { debugcon_writestring("[M23] /proc/version: "); debugcon_writestring(pb); }
        if (block_find("vda")||block_find("sda")||block_find("nvme0n1")||block_find("usb0")) {
            extern vfs_inode_t* vfs_lookup(const char*);
            const char* bn = block_find("vda")?"/dev/vda":block_find("sda")?"/dev/sda":
                             block_find("nvme0n1")?"/dev/nvme0n1":"/dev/usb0";
            debugcon_writestring(vfs_lookup(bn)?"[M23] /dev block node present\n":"[M23] /dev block node MISSING\n");
        }
    }
    // [M24] Networking: probe NICs (e1000/e1000e/vmxnet3/igc), bring up the stack
    // with a static IP. Use the shell `ping` / `netinfo` commands to exercise it.
    { extern int net_init(void); net_init(); }
#if M9_USER_DEMO
    m9_run_demo(); // signed-userland demo (needs VFS) — does not return
#endif
#if M11_DRIVER_DEMO
    m11_run_demo(); // driver-space demo (needs VFS) — does not return
#endif
#if M13_DEMO
    m13_run_demo(); // usability & policy demo — does not return
#endif
#if M14_DEMAND_DEMO
    m14_run_demo(); // demand paging demo — does not return
#endif
#if M15_KILL_DEMO
    m15_run_demo(); // fault-kill demo — does not return
#endif
#if M16_EXEC_DEMO
    m16_run_demo(); // exec model demo — does not return
#endif
#if M17_BLOCK_DEMO
    m17_run_demo(); // blocking primitives demo — does not return
#endif
#if M18_MEM_DEMO
    m18_run_demo(); // dynamic memory demo — does not return
#endif
#if M19_FORK_DEMO
    m19_run_demo(); // fork/COW demo — does not return
#endif
#if M20_MMAP_DEMO
    m20_run_demo(); // page cache demo — does not return
#endif
#if M23_FS_DEMO
    m23_run_demo(); // POSIX FS personality demo — does not return
#endif
#if M24_NET_DEMO
    m24_run_demo(); // networking CAP_NET demo — does not return
#endif
#if M25_PIPE_DEMO
    m25_run_demo(); // anonymous pipes demo — does not return
#endif
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

    // Enter the interactive shell as the scheduler idle task (does not return).
    // This enables `run <path>` to execute ring-3 programs and return to the
    // shell on their exit.
    m10_enter_shell();
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
    // COM1 serial console: mirrors terminal output and feeds the shell input
    // path, so the interactive shell works headless over `-serial stdio`.
    extern void serial_init(void); serial_init();
    // Crash-signature marker: visible in QEMU -debugcon log even before VGA is
    // readable.  BUILD_TS and GIT_HASH are injected by the Makefile as -D macros.
    debugcon_writestring("SECoS build " BUILD_TS " git:" GIT_HASH "\n");
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
