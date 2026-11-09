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
#include "klock.h"   /* [M50] cpu_wait_irq */
#include "bkl.h"     /* [M50] bkl=on|off */
#include "lockdep.h" /* [M50] lockdep_selftest */
#include "multiboot2.h"
#include "pmm.h"
#include "sysroot.h"
#include "vmm.h"
#include "idt.h"
#include "tss.h"
#include "heap.h"
#include "keyboard.h"
#include "timer.h"
#include "shell.h"
#include "sched.h"
#include "panic.h"
#include "driver_if.h" // driver teardown hook (M57: SYS_UDRV)
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

int kernel_cmdline_has(const char* word);   // [M48] boot command line (below)

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
        if (p) sched_make_ready(p);
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
        } else if (alive == 0 && process_count_all() == 0 && m8_round <= M8_ROUNDS) {
            // [M48] Measure only once every zombie is reaped. "alive == 0"
            // alone (the M29-3 flake) let a round end while zombies whose last
            // CPU had not yet switched off them were still unreaped, holding
            // their address space and kernel stack: free memory then differed
            // between rounds depending on timing.
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
        cpu_wait_irq();
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
// Sized with headroom: hello links the libc stdio (exit() flushes it), ~14 KB.
static uint8_t  m9_loadbuf[32768];
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
            if (p) { sched_make_ready(p); debugcon_writestring("[M9] spawned signed hello\n"); }
            else   { debugcon_writestring("[M9] FAIL: signed hello not loaded/refused\n"); }
        } else if (sched_count_alive_user() == 0 && !m9_done) {
            m9_done = 1;
            debugcon_writestring("[M9] user program exited; DONE\n");
        }
        cpu_wait_irq();
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
    if (sysroot_install_exec("/bin/hello", user_hello_elf, user_hello_elf_len) != 0)
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

// ---- [M11] Driver Space demo: retired in M57 with the SYS_DRIVER mock (the
// ring-3 driver path is now SYS_UDRV; its gate is tools/udrv_gate.sh). ----

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
    if (p) { sched_make_ready(p);
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
        cpu_wait_irq();
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

// ---- [M34] Lua-from-source demo (gated; off by default) ----
// Loads the embedded SIGNED lua interpreter ELF (built from lua 5.4.7 source with
// the SecOS libc/libm/setjmp + SSE), runs it in ring-3 on the demand-paged VM.
// Its built-in demo script exercises the VM, floats (%g printf), math.sqrt, and
// recursion — proving "compile real OSS from source and run it signed".
#ifndef M34_LUA_DEMO
#define M34_LUA_DEMO 0
#endif
#if M34_LUA_DEMO
#include "trapframe.h"
#include "../crypto/user_lua_elf.h"
static uint8_t m34_idle_stack[16384] __attribute__((aligned(16)));
static int m34_step = 0;
__attribute__((noreturn)) static void m34_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m34_step == 0) {
            m34_step = 1;
            debugcon_writestring("[M34] loading signed lua interpreter (lua 5.4.7)\n");
            process_t* p = process_create_from_elf(user_lua_elf, user_lua_elf_len);
            if (p) { sched_make_ready(p); debugcon_writestring("[M34] lua spawned, running demo script\n"); }
            else    debugcon_writestring("[M34] FAIL: lua ELF not loaded\n");
        } else if (m34_step == 1 && sched_count_alive_user() == 0) {
            m34_step = 2;
            debugcon_writestring("[M34] DONE (lua interpreter ran to completion)\n");
        }
        cpu_wait_irq();
    }
}
static void m34_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[M34] lua-from-source demo\n");
    static process_t idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m34_idle_stack + sizeof(m34_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m34_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[M34] entering scheduler\n");
    arch_iret_to_tf(&idle_tf); // does not return
}
#endif /* M34_LUA_DEMO */

// ---- [M35] Capability confinement demo (gated; off by default) ----
// Loads a SIGNED program with a CONFINED manifest (CAP_ENFORCE|CAP_FS_READ|
// CAP_TIME) and runs it. Allowed ops (getticks, read /proc) succeed; out-of-scope
// syscalls (create/spawn/pipe/socket) are denied (-1) and recorded in the [AUDIT]
// log — proving the kernel enforces least privilege rooted in the signature.
#ifndef M35_CAP_DEMO
#define M35_CAP_DEMO 0
#endif
#if M35_CAP_DEMO
#include "trapframe.h"
#include "cap.h"
#include "../crypto/user_m35caps_elf.h"
static uint8_t m35_idle_stack[16384] __attribute__((aligned(16)));
static int m35_step = 0;
__attribute__((noreturn)) static void m35_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m35_step == 0) {
            m35_step = 1;
            debugcon_writestring("[M35] loading signed CONFINED program\n");
            process_t* p = process_create_from_elf(user_m35caps_elf, user_m35caps_elf_len);
            if (p) { sched_make_ready(p); debugcon_writestring("[M35] confined program spawned\n"); }
            else    debugcon_writestring("[M35] FAIL: confined ELF not loaded\n");
        } else if (m35_step == 1 && sched_count_alive_user() == 0) {
            m35_step = 2;
            debugcon_writestring("[M35] audit log total events: ");
            debugcon_print_hex(cap_audit_total());
            debugcon_writestring("\n[M35] DONE (least privilege enforced + audited)\n");
        }
        cpu_wait_irq();
    }
}
static void m35_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[M35] generalized capability / confinement demo\n");
    static process_t idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m35_idle_stack + sizeof(m35_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m35_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    arch_iret_to_tf(&idle_tf); // does not return
}
#endif /* M35_CAP_DEMO */

// ---- [M46] Exec confinement demo (gated; off by default) ----
// Installs three signed builds of user/m46_confine.c (confined driver, ambient
// probe with explicit NET/INPUT/DISPLAY, SETID root 04755) and runs, through
// the real SYS_SPAWN path, first the ambient control (`m46probe ctl`) and then
// the confined driver (`m46conf`). The selftest greps the [m46]/[CAPX]/[AUDIT]
// lines: nothing started by a no_new_privs process may hold more caps than it.
#ifndef M46_CONFINE_DEMO
#define M46_CONFINE_DEMO 0
#endif
#if M46_CONFINE_DEMO
#include "trapframe.h"
#include "cap.h"
#include "syscall.h"
#include "../crypto/user_m46conf_elf.h"
#include "../crypto/user_m46probe_elf.h"
#include "../crypto/user_m46setid_elf.h"
static uint8_t m46_idle_stack[16384] __attribute__((aligned(16)));
static int m46_step = 0;
static int m46_spawn(const char* path, const char* a0, const char* a1) {
    const char* av[3] = { a0, a1, 0 };
    int pid = ksys_spawn_argv(path, a1 ? 2 : 1, av);
    debugcon_writestring("[M46] spawned "); debugcon_writestring(path);
    debugcon_writestring(pid > 0 ? " ok\n" : " FAILED\n");
    return pid;
}
__attribute__((noreturn)) static void m46_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m46_step == 0) {
            m46_step = 1;
            m46_spawn("/bin/m46probe", "m46probe", "ctl");
        } else if (m46_step == 1 && sched_count_alive_user() == 0) {
            m46_step = 2;
            m46_spawn("/bin/m46conf", "m46conf", 0);
        } else if (m46_step == 2 && sched_count_alive_user() == 0) {
            m46_step = 3;
            debugcon_writestring("[M46] DONE\n");
        }
        cpu_wait_irq();
    }
}
static void m46_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[M46] exec confinement demo\n");
    sysroot_provision();   // /bin/busybox, /bin/bash (normally done by the idle shell)
    int r = sysroot_install_exec("/bin/m46conf", user_m46conf_elf, user_m46conf_elf_len);
    r |= sysroot_install_exec("/bin/m46probe", user_m46probe_elf, user_m46probe_elf_len);
    r |= sysroot_install_exec("/bin/m46setid", user_m46setid_elf, user_m46setid_elf_len);
    vfs_attr_t a; a.mode = 04755; a.uid = 0; a.gid = 0; a.atime = a.mtime = 0;
    r |= vfs_lsetattr("/bin/m46setid", &a, VFS_ATTR_MODE | VFS_ATTR_UID | VFS_ATTR_GID);
    debugcon_writestring(r == 0 ? "[M46] installed /bin/m46{conf,probe,setid}\n" : "[M46] FAIL: install\n");
    static process_t idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m46_idle_stack + sizeof(m46_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m46_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    arch_iret_to_tf(&idle_tf); // does not return
}
#endif /* M46_CONFINE_DEMO */

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
                // Headline proof: reserved footprint > 0, but no page of the
                // ELF image (FILE VMAs) mapped yet. [U1] The initial stack
                // (argc/argv/envp + auxv) is written at load, so its top
                // page(s) are present; 'mapped at load' counts them too.
                uint32_t mapped = vmm_count_user_pages(m14_proc->space);
                uint32_t img = 0;
                for (uint32_t i = 0; i < m14_proc->mm->vmas.count; i++) {
                    const vma_t* v = &m14_proc->mm->vmas.v[i];
                    if (v->type != VMA_TYPE_FILE) continue;
                    for (uint64_t a = v->start; a < v->end; a += 0x1000)
                        if (vmm_translate_in_space(m14_proc->space, a)) img++;
                }
                debugcon_writestring("[M14] reserved footprint=");
                debugcon_print_hex(m14_proc->user_mem_bytes);
                debugcon_writestring(" image pages at load=");
                debugcon_print_hex((uint64_t)img);
                debugcon_writestring(" mapped at load=");
                debugcon_print_hex((uint64_t)mapped);
                debugcon_writestring("\n");
                sched_make_ready(m14_proc); // let the scheduler run it
            }
        } else if (m14_step == 1 && m14_proc && sched_count_alive_user() == 0 && !m14_done) {
            m14_step = 2; m14_done = 1;
            debugcon_writestring("[M14] DONE\n");
        }
        cpu_wait_irq();
    }
}

static void m14_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    extern int g_kverbose;
    g_kverbose = 1;   // [M34/M36] this demo's whole point is the [PF] demand page trace
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
    if (p) { sched_make_ready(p);
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
        cpu_wait_irq();
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
            if (sysroot_install_exec("/m16_child.elf", user_m16_child_elf, user_m16_child_elf_len) == 0)
                debugcon_writestring("[M16] child ELF placed in VFS\n");
            else
                debugcon_writestring("[M16] FAIL: could not place child ELF\n");
            process_t* p = process_create_from_elf(user_m16_parent_elf, user_m16_parent_elf_len);
            if (p) { sched_make_ready(p); debugcon_writestring("[M16] spawned parent\n"); }
            else   debugcon_writestring("[M16] FAIL: parent not loaded\n");
        } else if (m16_step == 1 && sched_count_alive_user() == 0 && !m16_done) {
            m16_done = 1;
            debugcon_writestring("[M16] DONE\n");
        }
        cpu_wait_irq();
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
    if (p) { sched_make_ready(p); debugcon_writestring("[M17] spawned "); debugcon_writestring(what); debugcon_writestring("\n"); }
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
        cpu_wait_irq();
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
            if (p) { sched_make_ready(p); debugcon_writestring("[M18] spawned m18_mem\n"); }
            else   debugcon_writestring("[M18] FAIL: m18_mem not loaded\n");
        } else if (m18_step == 1 && sched_count_alive_user() == 0 && !m18_done) {
            m18_done = 1;
            debugcon_writestring("[M18] DONE\n");
        }
        cpu_wait_irq();
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
            if (p) { sched_make_ready(p); debugcon_writestring("[M19] spawned m19_fork\n"); }
            else   debugcon_writestring("[M19] FAIL: m19_fork not loaded\n");
        } else if (m19_step == 1 && sched_count_alive_user() == 0 && !m19_done) {
            m19_done = 1;
            debugcon_writestring("[M19] DONE\n");
        }
        cpu_wait_irq();
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

// ---- In-place execve demo (gated; off by default) ----
// Installs the signed /bin/exectest plus a tampered copy (one flipped byte), an
// unsigned build and a text script, then runs exectest: failed execs must leave
// the caller running, a successful exec keeps pid/fds/mask and drops cloexec fds
// and caught handlers, and 200 fork+exec cycles must not leak PMM frames.
// [EXECVE] markers on debugcon (see tools/selftest.sh "execve" section).
#ifndef EXECVE_DEMO
#define EXECVE_DEMO 0
#endif
#if EXECVE_DEMO
#include "trapframe.h"
#include "../crypto/user_exectest_elf.h"
#include "../crypto/user_exectest_unsigned_elf.h"

static uint8_t execve_idle_stack[16384] __attribute__((aligned(16)));
static int execve_step = 0, execve_done = 0;

static void execve_install(void) {
    if (sysroot_install_exec("/bin/exectest", user_exectest_elf, user_exectest_elf_len) != 0)
        debugcon_writestring("[EXECVE] WARN: install /bin/exectest failed\n");
    uint8_t* t = (uint8_t*)kmalloc(user_exectest_elf_len);
    if (t) {
        for (unsigned i = 0; i < user_exectest_elf_len; i++) t[i] = user_exectest_elf[i];
        t[user_exectest_elf_len / 2] ^= 0x01;           // one flipped byte: signature breaks
        sysroot_install_exec("/bin/exectest_tampered", t, user_exectest_elf_len);
        kfree(t);
    }
    sysroot_install_exec("/bin/exectest_unsigned", user_exectest_unsigned_elf, user_exectest_unsigned_elf_len);
    static const char script[] = "#!/bin/bash\necho script\n";
    sysroot_install_exec("/bin/exectest_script", script, sizeof(script) - 1);
    debugcon_writestring("[EXECVE] test binaries installed\n");
}

__attribute__((noreturn)) static void execve_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (execve_step == 0) {
            execve_step = 1;
            execve_install();
            static const char* const argv[] = { "exectest", 0 };
            process_t* p = process_create_from_elf_args(user_exectest_elf, user_exectest_elf_len, 1, argv);
            if (p) { sched_make_ready(p); debugcon_writestring("[EXECVE] spawned exectest\n"); }
            else   debugcon_writestring("[EXECVE] FAIL: exectest not loaded\n");
        } else if (execve_step == 1 && sched_count_alive_user() == 0 && !execve_done) {
            execve_done = 1;
            debugcon_writestring("[EXECVE] DONE\n");
        }
        cpu_wait_irq();
    }
}

static void execve_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[EXECVE] in-place execve demo\n");
    static process_t  idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(execve_idle_stack + sizeof(execve_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)execve_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[EXECVE] entering scheduler\n");
    arch_iret_to_tf(&idle_tf);
}
#endif /* EXECVE_DEMO */

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
            if (p) { sched_make_ready(p); debugcon_writestring("[M25] spawned m25_pipe\n"); }
            else   debugcon_writestring("[M25] FAIL: m25_pipe not loaded\n");
        } else if (m25_step == 1 && sched_count_alive_user() == 0 && !m25_done) {
            m25_done = 1;
            debugcon_writestring("[M25] DONE\n");
        }
        cpu_wait_irq();
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

// ===================== M30: signals demo =====================
// A signed ring-3 program installs a SIGUSR1 handler, raise()s it (proving
// delivery + sigreturn), then SIG_IGNs SIGPIPE and writes to a reader-closed
// pipe (proving EPIPE without termination). The program's [m30] lines reach
// debugcon via SYS_WRITE fd 1, so the selftest asserts on them.
#ifndef M30_SIGNAL_DEMO
#define M30_SIGNAL_DEMO 0
#endif
#if M30_SIGNAL_DEMO
#include "trapframe.h"
#include "../crypto/user_m30_sig_elf.h"

static uint8_t m30_idle_stack[16384] __attribute__((aligned(16)));
static int m30_step = 0, m30_done = 0;

__attribute__((noreturn)) static void m30_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m30_step == 0) {
            m30_step = 1;
            debugcon_writestring("[M30] --- signals ---\n");
            process_t* p = process_create_from_elf(user_m30_sig_elf, user_m30_sig_elf_len);
            if (p) { sched_make_ready(p); debugcon_writestring("[M30] spawned m30_sig\n"); }
            else   debugcon_writestring("[M30] FAIL: m30_sig not loaded\n");
        } else if (m30_step == 1 && sched_count_alive_user() == 0 && !m30_done) {
            m30_done = 1;
            debugcon_writestring("[M30] DONE\n");
        }
        cpu_wait_irq();
    }
}

static void m30_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[M30] signals demo\n");
    static process_t  idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m30_idle_stack + sizeof(m30_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m30_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[M30] entering scheduler\n");
    arch_iret_to_tf(&idle_tf);
}
#endif /* M30_SIGNAL_DEMO */

#ifndef M39_POSIX_DEMO
#define M39_POSIX_DEMO 0
#endif
#if M39_POSIX_DEMO
#include "trapframe.h"
#include "../crypto/user_m39_posix_elf.h"
static uint8_t m39_idle_stack[16384] __attribute__((aligned(16)));
static int m39_step = 0, m39_done = 0;
__attribute__((noreturn)) static void m39_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m39_step == 0) {
            m39_step = 1;
            debugcon_writestring("[M39] --- POSIX shell foundation ---\n");
            process_t* p = process_create_from_elf(user_m39_posix_elf, user_m39_posix_elf_len);
            if (p) { sched_make_ready(p); debugcon_writestring("[M39] spawned m39_posix\n"); }
            else   debugcon_writestring("[M39] FAIL: m39_posix not loaded\n");
        } else if (m39_step == 1 && sched_count_alive_user() == 0 && !m39_done) {
            m39_done = 1;
            debugcon_writestring("[M39] DONE\n");
        }
        cpu_wait_irq();
    }
}
static void m39_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    debugcon_writestring("[M39] POSIX foundation demo\n");
    static process_t idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m39_idle_stack + sizeof(m39_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m39_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[M39] entering scheduler\n");
    arch_iret_to_tf(&idle_tf);
}
#endif /* M39_POSIX_DEMO */

#ifndef M39_BASH_DEMO
#define M39_BASH_DEMO 0
#endif
#if M39_BASH_DEMO
#include "trapframe.h"
#include "../crypto/user_bash_elf.h"
static uint8_t m39b_idle_stack[16384] __attribute__((aligned(16)));
static int m39b_step = 0, m39b_done = 0;
__attribute__((noreturn)) static void m39b_idle_entry(void) {
    for (;;) {
        __asm__ volatile("cli");
        sched_reap_zombies();
        if (m39b_step == 0) {
            m39b_step = 1;
            debugcon_writestring("[M39] --- GNU bash from source ---\n");
            extern process_t* process_create_from_elf_args(const void*, unsigned long, int, const char* const[]);
            static const char* const argv[] = { "bash", "-i", 0 };
            process_t* p = process_create_from_elf_args(user_bash_elf, user_bash_elf_len, 2, argv);
            if (p) { sched_make_ready(p); debugcon_writestring("[M39] spawned bash\n"); }
            else   debugcon_writestring("[M39] FAIL: bash not loaded\n");
        } else if (m39b_step == 1 && sched_count_alive_user() == 0 && !m39b_done) {
            m39b_done = 1;
            debugcon_writestring("[M39] bash DONE\n");
        }
        cpu_wait_irq();
    }
}
static void m39b_run_demo(void) {
    extern void tss_set_kernel_stack(uint64_t);
    extern void arch_iret_to_tf(trapframe_t*) __attribute__((noreturn));
    static process_t idle; static trapframe_t idle_tf;
    for (unsigned i=0;i<sizeof(idle);i++)    ((uint8_t*)&idle)[i]=0;
    for (unsigned i=0;i<sizeof(idle_tf);i++) ((uint8_t*)&idle_tf)[i]=0;
    idle.pid = 0; idle.space = vmm_get_kernel_space();
    idle.kstack_top = (uint64_t)(m39b_idle_stack + sizeof(m39b_idle_stack));
    idle.tf = &idle_tf; idle.state = PROC_RUNNING;
    idle_tf.rip = (uint64_t)m39b_idle_entry; idle_tf.cs = 0x08; idle_tf.ss = 0x10;
    idle_tf.rflags = 0x202; idle_tf.rsp = idle.kstack_top;
    sched_set_idle(&idle); sched_set_current(&idle);
    tss_set_kernel_stack(idle.kstack_top);
    debugcon_writestring("[M39] entering scheduler (bash)\n");
    arch_iret_to_tf(&idle_tf);
}
#endif /* M39_BASH_DEMO */

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
            if (p) { sched_make_ready(p); debugcon_writestring("[M20] spawned m20_mmap\n"); }
            else   debugcon_writestring("[M20] FAIL: m20_mmap not loaded\n");
        } else if (m20_step == 1 && sched_count_alive_user() == 0 && !m20_done) {
            m20_done = 1;
            debugcon_writestring("[M20] DONE\n");
        }
        cpu_wait_irq();
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
            if (p) { sched_make_ready(p); debugcon_writestring("[M23] spawned m23_fs\n"); }
            else   debugcon_writestring("[M23] FAIL: m23_fs not loaded\n");
        } else if (m23_step == 1 && sched_count_alive_user() == 0 && !m23_done) {
            m23_done = 1;
            debugcon_writestring("[M23] DONE\n");
        }
        cpu_wait_irq();
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
            if (p) { sched_make_ready(p); debugcon_writestring("[M24] spawned m24_net (CAP_NET)\n"); }
            else   debugcon_writestring("[M24] FAIL: m24_net not loaded\n");
        } else if (m24_step == 1 && sched_count_alive_user() == 0) {
            m24_step = 2;
            debugcon_writestring("[M24] --- no CAP_NET (socket denied) ---\n");
            process_t* p = process_create_from_elf(user_m24nonet_elf, user_m24nonet_elf_len);
            if (p) { sched_make_ready(p); debugcon_writestring("[M24] spawned m24_nonet (no cap)\n"); }
            else   debugcon_writestring("[M24] FAIL: m24_nonet not loaded\n");
        } else if (m24_step == 2 && sched_count_alive_user() == 0 && !m24_done) {
            m24_done = 1;
            debugcon_writestring("[M24] DONE\n");
        }
        cpu_wait_irq();
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
#if M32_PKG_DEMO
#include "../crypto/user_testpkg_spkg.h"
#endif
// The signed system programs (coreutils, BusyBox, bash, lua, dropbear, init,
// the GUI, ...) and the default /etc configuration are installed on the root
// by kernel/sysroot.c (refresh policy for a persistent root included).
// (sysroot.h is included at the top: the demos use sysroot_install_exec.)
extern void shell_init(void);
extern void shell_run(void);
extern void shell_run_line(const char* line);
// [M40] /bin/init is PID 1 (user/init.c). -DSECOS_NO_INIT boots straight into
// bash as before. The default /etc/inittab is installed by kernel/sysroot.c.
#ifndef SECOS_NO_INIT
extern int shell_exec_foreground(const char* path, int argc, const char* const* argv,
                                 void (*created)(process_t*), int keep_running);
static void init_mark_pid1(process_t* p) {
    process_set_init_pid(p->pid);
    debugcon_writestring("[INIT] /bin/init started, pid=");
    debugcon_print_hex(p->pid);
    debugcon_writestring(p->pid == 1 ? " (PID 1)\n" : " (WARN: not PID 1)\n");
}
// PID 1 died: SIGKILL every user process it left (console bash, services), so
// the kernel shell owns the console alone. Bounded wait for the teardown.
struct init_pids { uint32_t pid[64]; int n; };
static void init_pids_cb(process_t* p, void* u) {
    struct init_pids* l = (struct init_pids*)u;
    if (p->pid != 0 && p->state != PROC_ZOMBIE && l->n < 64) l->pid[l->n++] = p->pid;
}
static void init_kill_orphans(void) {
    extern int signal_post(process_t*, int);
    extern void sched_reap_zombies(void);
    extern int sched_count_alive_user(void);
    struct init_pids l; l.n = 0;
    process_foreach(init_pids_cb, &l);
    for (int i = 0; i < l.n; i++) {
        process_t* p = process_get_by_pid(l.pid[i]);
        if (p) { signal_post(p, 9 /*SIGKILL*/); process_put(p); }   // also resumes a stopped one
    }
    for (int spin = 0; spin < 3000 && sched_count_alive_user() > 0; spin++) {
        cpu_wait_irq();
        sched_reap_zombies();
    }
}
#endif
static uint8_t   m10_idle_stack[32768] __attribute__((aligned(16)));
static process_t m10_idle;
static trapframe_t m10_idle_tf;

__attribute__((noreturn)) static void m10_shell_idle_entry(void) {
    shell_init();
    sysroot_provision();       // /bin programs + default /etc (kernel/sysroot.c)
    sysroot_apply_settings();  // /etc/vconsole.conf KEYMAP=
#if ENABLE_FB
    // Enable the RAM back buffer + auto-flush: rendering/scroll happen in RAM and
    // only the dirty rectangle is written to VRAM — fixes slow scroll on real HW.
    { extern int fb_console_enable_dbuf(void); extern void fb_console_set_dbuf_auto(int);
      if (fb_console_enable_dbuf() == 0) fb_console_set_dbuf_auto(1); }
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
        if (sysroot_install_exec("/mnt/hello.elf", user_hello_elf, user_hello_elf_len) == 0)
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
            sysroot_install_exec("/mnt/bad.elf", bad, user_hello_elf_len);
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
        if (sysroot_install_exec("/bin/m31", user_m31_libc_elf, user_m31_libc_elf_len) == 0)
            debugcon_writestring("[M31] wrote signed libc test to /bin/m31\n");
        shell_run_line("run /bin/m31");
        // Prove the coreutils userland runs (1 spawn keeps the boot demo quick;
        // each signed spawn pays an Ed25519 verify, slow under TCG). stdout
        // mirrors to debugcon so the harness can assert on the result.
        shell_run_line("run /bin/echo CU-ECHO-OK");
    }
#endif
#if M32_PKG_DEMO
    // [M32] Install a signed package, prove a tampered one is refused, then cat
    // a file the package delivered.
    {
        extern int pkg_install(const void*, size_t);
        int r = pkg_install(user_testpkg_spkg, user_testpkg_spkg_len);
        debugcon_writestring(r > 0 ? "[M32] signed package installed\n"
                                   : "[M32] FAIL: package install\n");
        static uint8_t bad[8192];
        size_t bn = user_testpkg_spkg_len < sizeof(bad) ? user_testpkg_spkg_len : sizeof(bad);
        for (size_t i = 0; i < bn; i++) bad[i] = user_testpkg_spkg[i];
        bad[80] ^= 0x01;   // corrupt a data byte
        int br = pkg_install(bad, bn);
        debugcon_writestring(br < 0 ? "[M32] tampered package REFUSED (good)\n"
                                    : "[M32] FAIL: tampered package accepted\n");
        shell_run_line("run /bin/cat /etc/pkg-motd.txt");
    }
#endif
#ifndef SECOS_NO_INIT
    // [M40] Start /bin/init as PID 1 and wait for it (it never exits in normal
    // operation; shutdown goes through SYS_REBOOT). Missing or unsigned init:
    // fall back to the bash login shell below. PID 1 died: say so, stop what
    // it left behind and give the console to the kernel shell.
    { extern vfs_inode_t* vfs_lookup(const char*);
      if (!vfs_lookup("/bin/init")) {
          terminal_writestring("init: /bin/init not found; falling back to the console login/bash\n");
          debugcon_writestring("[INIT] /bin/init missing; fallback to bash\n");
      } else {
          terminal_writestring("Starting /bin/init (PID 1)...\n");
          static const char* const iav[] = { "init", 0 };
          int r = shell_exec_foreground("/bin/init", 1, iav, init_mark_pid1, 1);
          if (r < 0) {
              terminal_writestring("init: /bin/init could not be started (bad signature?); falling back to bash\n");
              debugcon_writestring("[INIT] FAIL: /bin/init refused (signature/load); fallback to bash\n");
          } else {
              process_set_init_pid(0);
              terminal_writestring("\n*** PID 1 (/bin/init) exited: stopping its processes, "
                                   "dropping to the SecOS kernel shell ***\n");
              debugcon_writestring("[INIT] PID 1 died; fallback to the kernel shell\n");
              init_kill_orphans();
              shell_run();
          }
      } }
#endif
#if !defined(SECOS_NO_BASH) && !defined(SECOS_CONSOLE_ROOT)
    // [M43] No working init: do not hand out an unauthenticated root shell.
    // Run the signed /bin/login on the console instead, again after every
    // logout. Only when login is missing or refused (signature) does the boot
    // fall through to the bash below (and CONSOLE=root builds keep that bash).
    { extern vfs_inode_t* vfs_lookup(const char*);
      if (vfs_lookup("/bin/login")) {
          static const char* const lav[] = { "login", 0 };
          debugcon_writestring("[INIT] console fallback: /bin/login\n");
          for (;;) {
              int r = shell_exec_foreground("/bin/login", 1, lav, 0, 0);
              if (r < 0) {
                  terminal_writestring("login: /bin/login could not be started; falling back to bash\n");
                  debugcon_writestring("[INIT] FAIL: /bin/login refused; fallback to bash\n");
                  break;
              }
          }
      } }
#endif
#ifndef SECOS_NO_BASH
    // [M39] Boot into GNU bash as the interactive login shell. When bash exits
    // (Ctrl-D / `exit`), control falls through to the SecOS kernel shell below,
    // which remains as the low-level recovery layer.
    { extern vfs_inode_t* vfs_lookup(const char*);
      if (vfs_lookup("/bin/bash")) {
          terminal_writestring("Starting GNU bash (exit drops to the SecOS recovery shell)...\n");
          shell_run_line("run /bin/bash -i");
      } }
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
#ifdef SECOS_LOCKDEP
    lockdep_selftest();   // [M50] the validator's negative controls (IDT up, one CPU)
#endif

    // [M39] Full high-half kernel W^X: text RX, rodata/note R/NX, data/bss/stack
    // RW/NX, and NX on the duplicate phys-window alias. Until now this was deferred
    // (the high-half image ran RWX); the IDT is loaded so any latent
    // write-to-rodata / exec-from-data fault surfaces as [EXC] not a triple fault.
    // NX is universal (EFER.NXE on) so this is CPU-feature-independent. Then audit.
#ifndef SECOS_NO_WX
    vmm_protect_kernel_sections();
    vmm_audit_wx();
#endif

    // [M44] Retire the boot low identity map: from here on the kernel reaches
    // physical memory only through the physmap and its high-half image. (The
    // W^X audit above still inspected it, while it was live.)
    vmm_drop_low_identity();

    heap_init();
    {   // [M44] A user address space never carries a kernel lower-half mapping.
        vmm_space_t* probe = vmm_space_create_user();
        int n = probe ? vmm_space_low_kernel_entries(probe) : -1;
        if (probe) vmm_space_destroy(probe);
        debugcon_writestring(n == 0 ? "[IDMAP] user CR3 audit: no low kernel mappings (OK)\n"
                                    : "[IDMAP] user CR3 audit: FAIL (low kernel mappings present)\n");
    }
#ifdef SMAP_VIOLATION_DEMO
    {   // [M44] Selftest hook: a deliberate direct kernel read of a user page
        // (AC=0, outside the uaccess primitives) must panic with [SMAP].
        extern int g_smap_enabled;
        vmm_space_t* sp = vmm_space_create_user();
        if (g_smap_enabled && sp &&
            vmm_alloc_page_in_space(sp, USER_CODE_BASE, VMM_FLAG_USER | VMM_FLAG_NOEXEC) == 0) {
            debugcon_writestring("[SMAP-DEMO] direct kernel read of a user page...\n");
            vmm_switch_space(sp);
            volatile uint8_t v = *(volatile uint8_t*)USER_CODE_BASE;
            (void)v;
            vmm_switch_space(vmm_get_kernel_space());
            debugcon_writestring("[SMAP-DEMO] FAIL: the access did not fault\n");
        } else {
            debugcon_writestring("[SMAP-DEMO] skipped: SMAP not enabled on this CPU\n");
        }
    }
#endif
    sched_init();
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
    { extern void pci_init(void); pci_init(); }   // [M53a] PCI registry (+ ECAM from MCFG)
    // [M53b] DMAR (VT-d) / IVRS (AMD-Vi): read-only parse + log for M54/M64.
    // After pci_init(): device scopes are resolved through the registry.
    { extern void acpi_dmar_init(void); acpi_dmar_init(); }
    timer_init(1000);
    keyboard_init();
    { extern void tty_init(void); tty_init(); }   // console termios defaults + vt reply hook

    // [M28-2] Retire the 8259 PIC + PIT IRQ0 onto the LAPIC timer + IOAPIC when
    // ACPI gave us a MADT/IOAPIC. timer_init() above already programmed the PIT
    // and timer_frequency=1000 (still used for uptime math); the switchover masks
    // the PIC and drives the same vector 0x20 (isr_timer) from the LAPIC timer at
    // 1 kHz, so timer_ticks keeps advancing. Falls back to PIC/PIT on no MADT.
#ifndef SECOS_NO_APIC   /* safe-boot: stay on the legacy 8259 PIC + PIT */
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
#endif /* SECOS_NO_APIC */

    // [M40] Input: the PS/2 keyboard becomes /dev/input/event0, then the PS/2
    // mouse (IRQ12, routed like IRQ1: IOAPIC in APIC mode, slave 8259 otherwise).
    // USB HID keyboards/mice/tablets register later from usb_init().
    {
        extern void keyboard_register_input(void);
        extern const char* keyboard_keymap_name(void);
        extern int ps2mouse_init(void);
        extern void irq_enable_aux(void);
        keyboard_register_input();
        if (ps2mouse_init() == 0) irq_enable_aux();
        debugcon_writestring("[KBD] keymap ");
        debugcon_writestring(keyboard_keymap_name());
        debugcon_writestring("\n");
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
    // Wall clock: read the CMOS RTC (UTC) once and anchor it to the TSC clock
    // (CLOCK_REALTIME, file timestamps, date).
    { extern void kclock_init(void); kclock_init(); }

    // [M29-2] Application-processor bring-up is **opt-in** (experimental): on real
    // hardware with true parallelism, heavy multicore load can deadlock/freeze —
    // likely a missing TLB shootdown / memory-barrier the QEMU/VMware paths didn't
    // expose. The default boot stays single-core + stable (with the modern LAPIC
    // timer). Bring the cores up on demand with the shell `smp` command, or build
    // with -DSECOS_SMP_AT_BOOT to start them at boot (QEMU/VMware testing).
    // [M48] SMP is on by default: TLB shootdown, reschedule IPIs and migration
    // make multicore safe (docs/devlog/M48-smp.md). Opt out at build time with
    // -DSECOS_NO_SMP or at boot with the `nosmp` command-line word (the GRUB
    // menu has a "single CPU" entry); the shell `smp` command can still bring
    // the APs up later. -DSECOS_SMP_AT_BOOT (the old opt-in) is now a no-op.
#ifndef SECOS_NO_SMP
    if (kernel_cmdline_has("nosmp")) {
        debugcon_writestring("[SMP] nosmp on the command line: APs stay parked\n");
    } else {
        extern void smp_init(void); smp_init();
    }
#else
    debugcon_writestring("[SMP] built with SECOS_NO_SMP: single CPU\n");
#endif

    // [M53c] Dynamic device vectors: self-IPIs (BSP and an AP), freed-vector
    // negative, block alignment, exhaustion. Before any driver allocates one.
    { extern void irq_vec_selftest(void); irq_vec_selftest(); }
#if defined(SECOS_LDWATCH_TEST) && defined(SECOS_LOCKDEP)
    // Forced test of the lockdep stuck-lock watchdog (kernel/ldwatch_test.c).
    { extern void ldwatch_test_run(void); ldwatch_test_run(); }
#endif

    // [M54] VT-d DMA remapping, opt-in (boot word iommu=on): every PCI function
    // goes into one passthrough domain, so the drivers probed below are
    // unchanged. Needs the PCI registry, the DMAR and a free vector (fault
    // event); before any driver enables bus mastering. No-op by default.
    { extern void iommu_init(void); iommu_init(); }
    // [M55] Kernel DMA API: with iommu=on (and no iommu.kernel=pt) the
    // drivers' functions join a strict domain that maps only .dmabss and
    // dma_alloc_coherent frames. Before any driver probes.
    { extern void dma_init(void); dma_init(); }
    // [M57] Ring-3 driver assignments (boot word udrv=): hide those functions
    // from the in-kernel probes below, bus mastering off, IOMMU-blocked until a
    // driver claims them. No-op without udrv=.
    { extern void udrv_init(void); udrv_init(); }

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

    // [M49] Big-memory checks: the sectioned PMM (frames across the whole map,
    // zones, refcounts, meminfo), a multi-MiB kmalloc (contiguous frames,
    // possibly far above 4 GiB) and the physmap leaf audit (supervisor + NX).
    {
        pmm_selftest();
        extern void* kmalloc(size_t); extern void kfree(void*);
        size_t big = 4u << 20;
        uint8_t* p = (uint8_t*)kmalloc(big);
        int ok = (p != 0);
        if (ok) {
            for (size_t i = 0; i < big; i += 4096) p[i] = (uint8_t)(i >> 12);
            p[big - 1] = 0x5A;
            for (size_t i = 0; i < big && ok; i += 4096) if (p[i] != (uint8_t)(i >> 12)) ok = 0;
            if (p[big - 1] != 0x5A) ok = 0;
            debugcon_writestring("[HEAP] kmalloc(4M) phys=");
            debugcon_print_hex(kvirt_to_phys((uint64_t)(uintptr_t)p));
            kfree(p);
        }
        debugcon_writestring(ok ? " OK\n" : "[HEAP] FAIL: kmalloc(4M)\n");
        vmm_physmap_audit();
    }

    // Heap hardening selftest: allocation patterns, aligned frees, and the
    // corruption detectors (exercised in a non-panicking expect mode).
    heap_selftest();

    // Deliberate heap corruption — must be caught at the source with a clear
    // HEAP panic. 1 = 1-byte overflow (caught at kfree), 2 = double free.
#ifndef HEAP_CORRUPT_DEMO
#define HEAP_CORRUPT_DEMO 0
#endif
#if HEAP_CORRUPT_DEMO
    {
        uint8_t* hp = (uint8_t*)kmalloc(100);
        debugcon_writestring("[HEAP-DEMO] start\n");
#if HEAP_CORRUPT_DEMO == 1
        hp[100] = 0x41;      // one byte past the end of the allocation
        kfree(hp);
#else
        kfree(hp);
        kfree(hp);
#endif
        debugcon_writestring("[HEAP-DEMO] FAIL: corruption NOT detected\n");
    }
#endif

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
            sched_make_ready(p2);
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
        // Real media (USB sticks, SATA/NVMe disks) is partitioned (MBR/GPT) — the
        // QEMU test images put a raw FS at LBA 0 and hid this. Register each
        // partition as a sub-device (usb0p1, sda1, ...) so a filesystem inside a
        // partition can be found and mounted.
        extern void block_scan_partitions(void);
        block_scan_partitions();
        if (have_virtio) {
            block_dev_t* vda = block_find("vda");
            if (vda) {
                static uint8_t s0[512];
                int r = block_read(vda, 0, s0, 1);
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
                    int wr = block_write(vda, scratch, wb, 1);
                    int rr = block_read(vda, scratch, rb, 1);
                    int ok = (wr == 1 && rr == 1);
                    for (int i = 0; ok && i < 512; i++) if (rb[i] != wb[i]) ok = 0;
                    debugcon_writestring(ok ? "[M10] virtio-blk write+readback: OK\n"
                                            : "[M10] virtio-blk write+readback: FAIL\n");
                }
#endif
            }
        }
    }
    // [M23/M41] Persistent root: a SecOS system disk (an ext2/ext4 volume with
    // the marker file /.secosroot) becomes the VFS root "/". Plain ext2/ext4
    // *data* disks lack the marker and are never grabbed as root, so the boot
    // self-tests (no system disk) keep the RAMFS root. A disk that is present
    // but unusable (damaged superblock, unsupported features, bad checksums, a
    // corrupt journal) is refused before anything is written: the boot falls
    // back to RAMFS with a loud warning, and the disk is not mounted anywhere.
    // Nothing is ever formatted here.
    int root_is_ext2 = 0;
    const char* root_dev = 0;
    const char* refused[8]; int nrefused = 0;
    {
        extern int ext2_mount_root(const char* dev_name);
        extern const char* ext2_last_error(void);
        extern block_dev_t* block_find(const char* name);
        static const char* rc[] = { "vda","sda","sdb","sdc","sdd","nvme0n1","usb0" };
        for (unsigned i=0;i<sizeof(rc)/sizeof(rc[0]) && !root_is_ext2;i++) {
            block_dev_t* bd = block_find(rc[i]);
            if (!bd) continue;
            int r = ext2_mount_root(rc[i]);
            if (r == 0) {
                root_is_ext2 = 1; root_dev = rc[i];
                debugcon_writestring("[M23] persistent ext2/ext4 root on ");
                debugcon_writestring(rc[i]); debugcon_writestring("\n");
                continue;
            }
            const char* why = 0;
            if (r == -3) why = ext2_last_error();
            else if (r == -1) {
                // No primary superblock. A backup superblock (group 1) with the
                // SECOSROOT label means a damaged system disk, not a blank one.
                static uint8_t bs[1024];
                // Group 1 starts at block 32768 (4 KiB) or 8193 (1 KiB); a backup
                // superblock sits at offset 0 of that block.
                static const uint64_t bk[] = { 262144ull /* 4 KiB blocks */, 16386ull /* 1 KiB */ };
                for (unsigned k = 0; k < 2 && !why; k++) {
                    if (bd->sector_count <= bk[k] + 2 || block_read(bd, bk[k], bs, 2) != 2) continue;
                    if (bs[56] == 0x53 && bs[57] == 0xEF && bs[120]=='S' && bs[121]=='E' && bs[122]=='C' &&
                        bs[123]=='O' && bs[124]=='S' && bs[125]=='R')
                        why = "primary superblock damaged (backup found; repair with e2fsck)";
                }
            }
            if (why) {
                if (nrefused < 8) refused[nrefused++] = rc[i];
                terminal_writestring("\n*** WARNING: the system disk "); terminal_writestring(rc[i]);
                terminal_writestring(" is unusable: "); terminal_writestring(why);
                terminal_writestring(".\n*** It was not mounted and nothing was written to it. Booting on a RAMFS\n"
                                     "*** root: changes made now are LOST at reboot.\n\n");
                debugcon_writestring("[ROOT] WARNING: system disk "); debugcon_writestring(rc[i]);
                debugcon_writestring(" refused: "); debugcon_writestring(why);
                debugcon_writestring(" -> RAMFS fallback, disk untouched\n");
            }
        }
    }
    // Root filesystem defaults to RAMFS when there is no persistent ext2 root.
    if (!root_is_ext2) {
        extern int vfs_mount_ramfs(void);
        if (vfs_mount_ramfs() == 0) terminal_writestring("[VFS] root RAMFS mounted\n");
        else terminal_writestring("[VFS] root RAMFS FAIL\n");
    }
    sysroot_set_root(root_dev ? root_dev : "ramfs", root_is_ext2);
    // Root health: which root, filesystem, journal, free space (console + debugcon).
    {
        extern int ext2_journal_state(void);
        extern int ext2_has_csum(void);
        if (root_is_ext2) {
            const char* fst = "ext2";
            vfs_inode_t* ri = vfs_lookup("/");
            if (ri && ri->ops && ri->ops->mntinfo) ri->ops->mntinfo(0, &fst);
            vfs_statfs_t st; uint64_t fmb = 0, tmb = 0;
            if (vfs_statfs("/", &st) == 0) {
                fmb = st.bavail * st.bsize / (1024 * 1024);
                tmb = st.blocks * st.bsize / (1024 * 1024);
            }
            int js = ext2_journal_state();
            const char* jt = js > 0 ? "journal on" : js < 0 ? "journal NOT written (unsupported format)" : "no journal";
            terminal_writestring("[ROOT] persistent root: "); terminal_writestring(fst);
            terminal_writestring(" on "); terminal_writestring(root_dev);
            terminal_writestring(" ("); terminal_writestring(jt);
            if (ext2_has_csum()) terminal_writestring(", metadata_csum");
            terminal_writestring("), "); print_dec(fmb); terminal_writestring(" MiB free of ");
            print_dec(tmb); terminal_writestring(" MiB\n");
            debugcon_writestring("[ROOT] persistent root fs="); debugcon_writestring(fst);
            debugcon_writestring(" dev="); debugcon_writestring(root_dev);
            debugcon_writestring(js > 0 ? " journal=on" : js < 0 ? " journal=unsupported" : " journal=none");
            debugcon_writestring(ext2_has_csum() ? " csum=on" : " csum=off");
            debugcon_writestring(" free_mib="); debugcon_print_hex(fmb);
            debugcon_writestring(" total_mib="); debugcon_print_hex(tmb); debugcon_writestring("\n");
            if (js <= 0) {
                terminal_writestring("[ROOT] WARNING: writes to this root are not crash-safe\n");
                debugcon_writestring("[ROOT] WARNING: root writes are not journalled\n");
            }
        } else {
            terminal_writestring(nrefused ? "[ROOT] root: RAMFS (FALLBACK - system disk refused, see above)\n"
                                          : "[ROOT] root: RAMFS (no system disk: changes are lost at reboot)\n");
            debugcon_writestring(nrefused ? "[ROOT] root=ramfs (fallback: system disk refused)\n"
                                          : "[ROOT] root=ramfs (no system disk)\n");
        }
    }
    // Mount a disk at /mnt: try FAT32, then ext2/ext4 (multi-mount). Skipped when
    // an ext2 disk is already the persistent root (single-root model), and never
    // a system disk refused above.
    if (!root_is_ext2) {
        extern int fat32_mount(const char* dev_name, const char* mount_point);
        extern int ext2_mount(const char* dev_name, const char* mount_point);
        extern int exfat_mount(const char* dev_name, const char* mount_point);
        extern int ntfs_mount(const char* dev_name, const char* mount_point);
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
            int skip = 0;
            for (int k = 0; k < nrefused; k++) { const char* a = refused[k]; const char* b = cand[i];
                while (*a && *a == *b) { a++; b++; } if (*a == *b) skip = 1; }
            if (skip) continue;
            if      (fat32_mount(cand[i], "/mnt") == 0) { fsname = "FAT32"; mounted_dev = cand[i]; }
            else if (ext2_mount (cand[i], "/mnt") == 0) { fsname = "extN";  mounted_dev = cand[i]; }
            else if (exfat_mount(cand[i], "/mnt") == 0) { fsname = "exFAT"; mounted_dev = cand[i]; }
            else if (ntfs_mount (cand[i], "/mnt") == 0) { fsname = "NTFS";  mounted_dev = cand[i]; }
        }
        // Partitioned media: if no whole-disk raw FS mounted, try every registered
        // block device — including the partition sub-devices (usb0p1, sda1, ...).
        if (!fsname) {
            int bn = block_count();
            for (int bi = 0; bi < bn && !fsname; bi++) {
                block_dev_t* bd = block_get(bi);
                if (!bd) continue;
                int skip = 0;
                for (int k = 0; k < nrefused; k++) { const char* a = refused[k]; const char* b = bd->name;
                    while (*a && *a == *b) { a++; b++; } if (*a == *b) skip = 1; }
                if (skip) continue;
                if      (fat32_mount(bd->name, "/mnt") == 0) { fsname = "FAT32"; mounted_dev = bd->name; }
                else if (ext2_mount (bd->name, "/mnt") == 0) { fsname = "extN";  mounted_dev = bd->name; }
                else if (exfat_mount(bd->name, "/mnt") == 0) { fsname = "exFAT"; mounted_dev = bd->name; }
                else if (ntfs_mount (bd->name, "/mnt") == 0) { fsname = "NTFS";  mounted_dev = bd->name; }
            }
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
#ifdef SECOS_PAGECACHE_TEST
    // [M50 B2] Page-cache fill races, forced through a test hook (on this CPU
    // and against an AP), once the root FS is up.
    { extern void pagecache_selftest(void); pagecache_selftest(); }
#endif
#ifdef SECOS_B3_BLKSTRESS
    // [M50 B3] Block/USB lock stress on every CPU at once (fs/blkstress.c).
    { extern void blkstress_run(void); blkstress_run(); }
#endif
#ifdef SECOS_VFS_TEST
    // [M51 C1] VFS locks: concurrent O_APPEND + mount/umount race (fs/vfs_test.c).
    { extern void vfstest_run(void); vfstest_run(); }
#endif
#ifdef SECOS_FSOPS_TEST
    // [M52 D3] Inode-keyed ftruncate/fsetattr and create/mkdir with
    // attributes on ramfs and the /mnt disk FS (fs/fsops_test.c).
    { extern void fsops_selftest(void); fsops_selftest(); }
#endif
#ifdef SECOS_FSVIRT_TEST
    // [M51 C2] ramfs / procfs locks under real concurrency (fs/fsvirt_test.c).
    { extern void fsvirt_selftest(void); fsvirt_selftest(); }
#endif
#ifdef SECOS_EXT4_LOCKTEST
    // [M51 C3] ext2/ext4 driver lock under real concurrency: every CPU drives
    // the mounted ext volume at once, outside the BKL (fs/ext4locktest.c).
    { extern void ext4locktest_run(void); ext4locktest_run(); }
#endif
#ifdef SECOS_C4_FSSTRESS
    // [M51 C4] FAT32/exFAT/NTFS driver locks under concurrency (fs/c4_fsstress.c).
    { extern void c4_fsstress_run(void); c4_fsstress_run(); }
#endif
#ifdef SECOS_TTY_TEST
    // [M51 C5] Console line discipline + pty locks driven from every CPU at
    // once (kernel/tty_test.c).
    { extern void tty_selftest(void); tty_selftest(); }
#endif
#ifdef SECOS_EXEC_TEST
    // [M51 C6] Signed-image loads on every CPU at once (kernel/exec_test.c).
    { extern void exectest_run(void); exectest_run(); }
#endif
#if M41_EXFAT_DEMO
    // [M41] exFAT self-test, once /mnt and /proc are mounted: the good image on
    // vda, or the corrupt-image suite when AHCI disks are attached.
    { extern void exfat_selftest(void); exfat_selftest(); }
#endif
#if M42_NTFS_DEMO
    // [M42] NTFS read-only self-test: the host-made image on vda (walk +
    // hashes, EROFS), or the corrupt-image suite when AHCI disks are attached.
    { extern void ntfs_selftest(void); ntfs_selftest(); }
#endif
    // [M24] Networking: probe NICs (e1000/e1000e/vmxnet3/igc), bring up the stack
    // with a static IP. Use the shell `ping` / `netinfo` commands to exercise it.
    { extern int net_init(void); net_init(); }
#if M9_USER_DEMO
    m9_run_demo(); // signed-userland demo (needs VFS) — does not return
#endif
#if M13_DEMO
    m13_run_demo(); // usability & policy demo — does not return
#endif
#if M34_LUA_DEMO
    m34_run_demo(); // lua-from-source demo — does not return
#endif
#if M35_CAP_DEMO
    m35_run_demo(); // capability confinement demo — does not return
#endif
#if M46_CONFINE_DEMO
    m46_run_demo(); // exec confinement demo — does not return
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
#if EXECVE_DEMO
    execve_run_demo(); // in-place execve demo — does not return
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
#if M30_SIGNAL_DEMO
    m30_run_demo(); // signals demo — does not return
#endif
#if M39_POSIX_DEMO
    m39_run_demo(); // POSIX shell-from-source foundation demo — does not return
#endif
#if M39_BASH_DEMO
    m39b_run_demo(); // GNU bash from source demo — does not return
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
                uint64_t base = info.virt_addr;   // [M44] physmap VA (fb_finalize_mapping)
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
char     g_kcmdline[256];          // [M48] boot command line (Multiboot2 tag 1)

// [M48] Is `word` a whitespace-separated token of the boot command line?
int kernel_cmdline_has(const char* word) {
    const char* s = g_kcmdline;
    while (*s) {
        while (*s == ' ') s++;
        const char* w = word; const char* t = s;
        while (*w && *t == *w) { t++; w++; }
        if (!*w && (*t == 0 || *t == ' ')) return 1;
        while (*s && *s != ' ') s++;
    }
    return 0;
}

// [M31] Kernel verbosity gate (see kverbose.h). Quiet by default so the shell
// shows only program output; the `verbose` command flips it.
int g_kverbose = 0;
void kvlog(const char* s) { if (g_kverbose) terminal_writestring(s); }
void kvhex(uint64_t v)    { if (g_kverbose) print_hex(v); }
void kvputc(char c)       { if (g_kverbose) terminal_putchar(c); }

// [M34] Enable SSE/SSE2 so ring-3 code (e.g. ported software using doubles, like
// lua) can run FP instructions without #UD. The kernel itself is built -mno-sse,
// so it never clobbers XMM; per-process FPU state is saved across context
// switches (fxsave/fxrstor, see context_switch). Must run before any ring-3 task.
void cpu_enable_sse(void) {
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1UL << 2);   // CR0.EM = 0 (no x87 emulation)
    cr0 |=  (1UL << 1);   // CR0.MP = 1 (monitor coprocessor)
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1UL << 9);    // CR4.OSFXSR     = 1 (SSE + fxsave/fxrstor enabled)
    cr4 |= (1UL << 10);   // CR4.OSXMMEXCPT = 1 (SIMD FP exceptions -> #XF)
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
    __asm__ volatile("fninit");
    unsigned int mxcsr = 0x1F80;          // all SIMD exceptions masked (reset default)
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
}

// [M36] Exploit mitigations rooted in the CPU. SMEP stops the kernel from ever
// executing a ring-3 page (ret2usr); SMAP stops stray kernel reads/writes of user
// memory unless explicitly allowed (stac/clac in the user_copy primitives). Both
// are gated on CPUID support so the kernel still boots on CPUs that lack them.
int g_smap_enabled = 0;   // user_copy.c emits stac/clac only when this is set
void cpu_enable_mitigations(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    int smep = (b & (1u << 7))  ? 1 : 0;
    int smap = (b & (1u << 20)) ? 1 : 0;
    if (smep) cr4 |= (1UL << 20);             // CPUID.7:EBX.SMEP -> CR4.SMEP
#ifndef SECOS_NO_SMAP
    /* [M44] SMAP is on by default whenever the CPU has it: a kernel access to a
     * user page is legal only inside the uaccess primitives (mm/user_copy.c),
     * which bracket it with stac/clac. Every kernel entry clears AC
     * (SMAP_CLAC in arch/x86/*.asm), and a stray access panics with [SMAP]
     * (kernel/panic.c). Opt out with -DSECOS_NO_SMAP. */
    if (smap) cr4 |= (1UL << 21);             // CPUID.7:EBX.SMAP -> CR4.SMAP
#endif
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
#ifndef SECOS_NO_SMAP
    if (smap) { g_smap_enabled = 1; __asm__ volatile("clac"); }
#endif
    // CR0.WP: make ring-0 writes honour read-only PTEs. Without it a kernel
    // write into a user page (copy_to_user from read()/stat()) silently ignored
    // the RO bit: it scribbled on PROT_NONE / mprotect(RO) pages and, worse, on
    // a copy-on-write frame still shared with the fork parent (no COW fault ->
    // the child's read() modified the parent's memory). With WP set the write
    // faults, and the #PF handler COW-copies or the uaccess fixup returns
    // -EFAULT. It also enforces RO kernel sections against ring-0 writes.
    {
        uint64_t cr0;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 |= (1UL << 16);                   // CR0.WP
        __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
    }
    debugcon_writestring("[M36] mitigations: SMEP=");
    debugcon_writestring(smep ? "on" : "n/a");
    debugcon_writestring(" SMAP=");
    debugcon_writestring(g_smap_enabled ? "on" : (smap ? "off(SECOS_NO_SMAP)" : "n/a"));
    debugcon_writestring(" stack-canary=on WP=on\n");
    static int smap_logged = 0;           // once (BSP); APs repeat the CR4 setup only
    if (!smap_logged) {
        smap_logged = 1;
        debugcon_writestring(g_smap_enabled ? "[SMAP] enabled: kernel user access only via uaccess (stac/clac)\n"
                             : (smap ? "[SMAP] supported but disabled (SECOS_NO_SMAP)\n"
                                     : "[SMAP] not supported by this CPU (fallback: no SMAP)\n"));
    }
}

// [M37] Measured boot. Compute a SHA-256 over the immutable kernel image
// (.text + .rodata) and record it on debugcon. This is the measurement primitive
// of a measured-boot chain: a TPM would extend a PCR with this value; here it is
// logged so a build can be attested against its expected hash. The signed user
// programs (M9) are separately verified at load, so the whole trust chain — boot
// loader (future) -> kernel measurement -> signed userland — is covered.
//
// [M45] KASLR: the relocator patched every absolute high-half reference by the
// slide, so the live bytes differ per boot. The measurement reverts each
// relocated field to its link-time value while hashing (kaslr.c), i.e. it hashes
// the image exactly as it is in the signed, reproducible kernel.bin: the value
// is slide-independent and still matches a host-side sha256 of the file.
#include "../crypto/sha256.h"
#include "../boot/kaslr.h"
extern char _text_start[], _rodata_end[];
void boot_measure(void) {
    const uint8_t* start = (const uint8_t*)_text_start;
    const uint8_t* end   = (const uint8_t*)_rodata_end;
    if (end <= start) { debugcon_writestring("[M37] measurement: bad bounds\n"); return; }
    uint8_t digest[32];
    kaslr_sha256_unrelocated(start, end, digest);
    debugcon_writestring("[M37] kernel code measurement (sha256 .text+.rodata): ");
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        char c2[2] = { hx[(digest[i] >> 4) & 0xF], hx[digest[i] & 0xF] };
        debugcon_putchar(c2[0]); debugcon_putchar(c2[1]);
    }
    debugcon_writestring("\n[M37] measured boot OK (signed userland verified at load)\n");
}

void kernel_main(uint32_t multiboot_magic, uint64_t multiboot_info) {
    // --- Phase 1 begins (old .bss stack) ---
    kaslr_init();       // [M45] read back + self-check the KASLR slide (first!)
    terminal_initialize();
    cpu_enable_sse();   // [M34] FPU/SSE for ring-3 (lua & other FP userland)
    cpu_enable_mitigations(); // [M36] SMEP/SMAP/canary
    boot_measure();           // [M37] record the kernel code measurement
    // COM1 serial console: mirrors terminal output and feeds the shell input
    // path, so the interactive shell works headless over `-serial stdio`.
    extern void serial_init(void); serial_init();
    // Crash-signature marker: visible in QEMU -debugcon log even before VGA is
    // readable.  BUILD_TS and GIT_HASH are injected by the Makefile as -D macros.
    debugcon_writestring("SECoS build " BUILD_TS " git:" GIT_HASH "\n");
    print_banner();
    kaslr_report();     // [M45] "KASLR on/off" (never the slide)

    // Save for phase2 (framebuffer)
    g_multiboot_magic = multiboot_magic;
    g_multiboot_info  = multiboot_info;
    // [M48] Keep the Multiboot2 command line (tag type 1): `nosmp` keeps the
    // APs parked. Read now, while the boot identity map still covers the info.
    if (multiboot_magic == MULTIBOOT2_BOOTLOADER_MAGIC && multiboot_info) {
        const uint8_t* mb = (const uint8_t*)multiboot_info;
        uint32_t total = *(const uint32_t*)mb;
        uint32_t off = 8;
        while (off + 8 <= total && total < (1u << 20)) {
            uint32_t ty = *(const uint32_t*)(mb + off), sz = *(const uint32_t*)(mb + off + 4);
            if (ty == 0 || sz < 8) break;
            if (ty == 1) {
                uint32_t k = 0;
                for (const char* c = (const char*)(mb + off + 8); *c && k < sizeof(g_kcmdline) - 1 && k < sz - 8; c++)
                    g_kcmdline[k++] = *c;
                g_kcmdline[k] = 0;
                break;
            }
            off += (sz + 7) & ~7u;
        }
    }

#if CONFIG_UEFI
    // [UEFI-FIX] The bootinfo struct lives in the loader's memory, which real
    // firmware may place above 512 MB. We are still on the loader's CR3 here (it
    // inherits the firmware identity map, so this deref is valid), but once we
    // install our own page tables in vmm_init() the loader region is no longer
    // mapped — phase2 (framebuffer) and acpi_init() would then fault dereferencing
    // it. Copy it into a kernel-owned struct now and use that everywhere after.
    static struct secos_boot_info g_uefi_bootinfo;
    if (multiboot_magic == 0 && multiboot_info != 0) {
        g_uefi_bootinfo  = *(struct secos_boot_info*)multiboot_info;
        g_multiboot_info = (uint64_t)&g_uefi_bootinfo;
        multiboot_info   = (uint64_t)&g_uefi_bootinfo; // keep the local in sync for PMM below
        // [M50] The loader's load options are the boot command line.
        if (g_uefi_bootinfo.flags & SECOS_BOOTINFO_F_CMDLINE) {
            uint32_t k = 0;
            for (; k < sizeof(g_uefi_bootinfo.cmdline) && k < sizeof(g_kcmdline) - 1 &&
                   g_uefi_bootinfo.cmdline[k]; k++)
                g_kcmdline[k] = g_uefi_bootinfo.cmdline[k];
            g_kcmdline[k] = 0;
        }
    }
#endif
    bkl_init_mode();   // [M50] bkl=on|off (default on)
    { extern void thread_boot_init(void); thread_boot_init(); }   // [U9] nothreads

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
    // [M49] With all RAM in the physmap, switch the PMM from its early bump
    // allocator to the sectioned bitmap (metadata lives in the physmap).
    pmm_late_init();
    // [M44] The VGA text buffer moves from the identity map to the physmap.
    { extern void terminal_relocate_vga(void); terminal_relocate_vga(); }

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
