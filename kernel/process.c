/*
 * SecOS Kernel - Process Management
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "kverbose.h"
#include "process.h"
#include "elf.h"
#include "heap.h"
#include "terminal.h"
#include "panic.h"
#include "mm/elf_manifest.h"
#include "mm/elf_sign.h"
#include "driver_if.h"
#include "pmm.h"
#include "debugcon.h"
#include "spinlock.h"   // [M29] SMP: serialize the process table
#include "percpu.h"     // [M29] smp_online_count for CPU affinity

#define MAX_PROCESSES 32
static process_t* proc_table[MAX_PROCESSES];
static uint32_t next_pid = 1;
static int proc_inited = 0;

// [M29] One lock guards the process table, next_pid and the affinity rotor. It
// is taken around the whole process_foreach() iteration (so the table cannot be
// mutated mid-scan); the scheduler's foreach callbacks therefore must NOT call
// any proc_lock-taking function (they don't — only read/write process fields).
// Process teardown frees a process_t only after detaching it from the table
// under this lock (process_reap_one), and only its affinity CPU reaps it, so no
// other CPU ever dereferences a freed entry.
static spinlock_t proc_lock = SPINLOCK_INIT;
static uint32_t affinity_rotor = 0;

int process_init_system(void) {
    for (int i=0;i<MAX_PROCESSES;i++) proc_table[i]=0;
    proc_inited = 1;
    terminal_writestring("[PROC] process table initialized\n");
    return 0;
}

// [M29] Allocate a unique pid (locked).
static uint32_t alloc_pid(void) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    uint32_t pid = next_pid++;
    spin_unlock_irqrestore(&proc_lock, fl);
    return pid;
}

// [M29] Round-robin a new process onto an online CPU (1 CPU -> always 0).
static uint32_t pick_affinity(void) {
    uint32_t n = smp_online_count(); if (n == 0) n = 1;
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    uint32_t a = affinity_rotor++ % n;
    spin_unlock_irqrestore(&proc_lock, fl);
    return a;
}

static int proc_add(process_t* p) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (!proc_table[i]) { proc_table[i]=p; p->kstack_slot = (uint8_t)i;
            spin_unlock_irqrestore(&proc_lock, fl); return 0; }
    }
    spin_unlock_irqrestore(&proc_lock, fl);
    return -1;
}

static void proc_remove(process_t* p) {
    if (!p) return;
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (proc_table[i] == p) { proc_table[i] = NULL; break; }
    }
    spin_unlock_irqrestore(&proc_lock, fl);
}

process_t* process_get_last(void) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    process_t* best = NULL; uint32_t best_pid = 0;
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (proc_table[i] && proc_table[i]->pid > best_pid) { best = proc_table[i]; best_pid = proc_table[i]->pid; }
    }
    spin_unlock_irqrestore(&proc_lock, fl);
    return best;
}

process_t* process_find_by_pid(uint32_t pid) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    process_t* r = NULL;
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (proc_table[i] && proc_table[i]->pid == pid) { r = proc_table[i]; break; }
    }
    spin_unlock_irqrestore(&proc_lock, fl);
    return r;
}

void process_foreach(void (*cb)(process_t*, void*), void* user) {
    if (!cb) return;
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (proc_table[i]) cb(proc_table[i], user);
    }
    spin_unlock_irqrestore(&proc_lock, fl);
}

// [M29] Atomically detach one reapable ZOMBIE pinned to `affinity` from the table
// and return it (the caller frees it via process_destroy outside the lock). Only
// the zombie's own affinity CPU reaps, and only once it has switched off the
// zombie's kernel stack (idle context), so the freed stack is never in use.
process_t* process_reap_one(uint32_t affinity) {
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    process_t* z = NULL;
    for (int i=0;i<MAX_PROCESSES;i++) {
        process_t* p = proc_table[i];
        if (p && p->state == PROC_ZOMBIE && p->cpu_affinity == affinity) {
            proc_table[i] = NULL; z = p; break;
        }
    }
    spin_unlock_irqrestore(&proc_lock, fl);
    return z;
}

// [M39] User-space ASLR: per-process random page-aligned slide for the stack,
// heap and mmap arena. Entropy from the TSC + xorshift mix (good enough for
// layout randomization, not crypto). Gate off with -DSECOS_NO_ASLR (slide 0 =
// the old fixed layout). ELF base ASLR would need PIE user binaries (deferred).
static uint64_t aslr_slide(uint64_t span_bytes) {
#ifdef SECOS_NO_ASLR
    (void)span_bytes; return 0;
#else
    if (span_bytes < 0x2000) return 0;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t x = ((uint64_t)hi << 32) | lo;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;   // xorshift64 so close TSC values diverge
    uint64_t pages = span_bytes >> 12;
    return (x % pages) << 12;                    // page-aligned offset in [0, span)
#endif
}

// [M16] Write 'n' bytes into user VA 'va' of a not-yet-running space, faulting
// in the backing (demand-paged) stack pages as needed. Returns 0 / -1.
static int uwrite(process_t* p, uint64_t va, const void* src, uint64_t n) {
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t a = va + i;
        uint64_t phys = vmm_translate_in_space(p->space, a);
        if (!phys) {
            const vma_t* vv = vma_find(&p->vmas, a);
            if (!vv) return -1;
            if (vma_fault_in(p->space, vv, a) != 0) return -1;
            phys = vmm_translate_in_space(p->space, a);
            if (!phys) return -1;
        }
        *(uint8_t*)phys_to_virt(phys) = s[i];
    }
    return 0;
}

#define PROC_MAX_ARGS    16
#define PROC_MAX_ARG_LEN 256

// [M16] Build argc/argv/envp on the user stack (top-down): copy the strings,
// then a NULL-terminated argv[] pointer array and an empty envp[]. Returns the
// new (16-byte aligned) rsp plus the user VAs of argv[] and envp[]. Fails if the
// data does not fit the reserved 8-page stack VMA.
static int setup_user_args(process_t* p, int argc, const char* const argv[],
                           uint64_t* rsp_out, uint64_t* argv_out, uint64_t* envp_out) {
    if (argc < 0) argc = 0;
    if (argc > PROC_MAX_ARGS) argc = PROC_MAX_ARGS;
    // [M39] Use the process's ASLR-randomized stack top, not the fixed constant,
    // so argv/env land inside the reserved 8-page stack VMA.
    const uint64_t st_top = p->stack_top ? p->stack_top : USER_STACK_TOP;
    const uint64_t st_lo = st_top - 8ULL * 0x1000ULL;
    uint64_t sp = st_top;
    uint64_t vptr[PROC_MAX_ARGS];
    uint64_t z = 0;

    for (int i = argc - 1; i >= 0; i--) {
        const char* a = (argv && argv[i]) ? argv[i] : "";
        uint64_t len = 0; while (a[len] && len < PROC_MAX_ARG_LEN - 1) len++;
        len++; // include NUL
        sp -= len;
        if (sp < st_lo || uwrite(p, sp, a, len) != 0) return -1;
        vptr[i] = sp;
    }
    sp &= ~7ULL;
    // envp[] = { NULL }
    sp -= 8; if (sp < st_lo || uwrite(p, sp, &z, 8) != 0) return -1;
    uint64_t envp = sp;
    // argv[] = { vptr[0..argc-1], NULL } — write the NULL terminator first.
    sp -= 8; if (sp < st_lo || uwrite(p, sp, &z, 8) != 0) return -1;
    for (int i = argc - 1; i >= 0; i--) {
        sp -= 8; if (sp < st_lo || uwrite(p, sp, &vptr[i], 8) != 0) return -1;
    }
    uint64_t argvp = sp;
    sp &= ~15ULL; // ABI: 16-byte aligned rsp
    *rsp_out = sp; *argv_out = argvp; *envp_out = envp;
    return 0;
}

process_t* process_create_from_elf(const void* elf_buf, size_t size) {
    return process_create_from_elf_args(elf_buf, size, 0, NULL);
}

process_t* process_create_from_elf_args(const void* elf_buf, size_t size,
                                        int argc, const char* const argv[]) {
    if (!proc_inited) process_init_system();

    // [M9] Code-signing gate (root of trust). Every ELF must carry a valid
    // Ed25519 signature (docs/SIGNING.md) to run. -DDEV_ALLOW_UNSIGNED downgrades
    // refusal to a warning for bootstrap (e.g. the M7/M8 synthetic demos).
    {
        int sv = elf_signature_verify(elf_buf, size);
#ifdef DEV_ALLOW_UNSIGNED
        if (sv != ELF_SIG_OK)
            debugcon_writestring("[SEC] WARN: unsigned/invalid ELF allowed (DEV_ALLOW_UNSIGNED)\n");
#else
        if (sv != ELF_SIG_OK) {
            terminal_writestring("[SEC] REFUSED: ELF signature missing/invalid\n");
            debugcon_writestring("[SEC] REFUSED unsigned/invalid ELF\n");
            return NULL;
        }
        debugcon_writestring("[SEC] ELF signature OK\n");
#endif
    }

    vmm_space_t* space = vmm_space_create_user();
    if (!space) { terminal_writestring("[PROC] space alloc failed\n"); return NULL; }

    process_t* p = (process_t*)kmalloc(sizeof(process_t));
    if (!p) {
        terminal_writestring("[PROC] pcb alloc failed\n");
        vmm_space_destroy(space);
        return NULL;
    }
    // [M14] Demand paging: init the VMA set + pinned-image fields up front so the
    // failure paths below can tear everything down uniformly.
    p->vmas.count = 0;
    p->image = NULL;
    p->image_size = 0;
    p->mapped_pages = NULL;
    p->mapped_page_count = 0;

    // [M14] Pin a private copy of the (already signature-verified) ELF image for
    // the process lifetime: FILE-backed VMAs fill pages from it on demand. Freed
    // in process_destroy.
    p->image = (uint8_t*)kmalloc(size);
    if (!p->image) {
        terminal_writestring("[PROC] image pin alloc failed\n");
        vmm_space_destroy(space);
        kfree(p);
        return NULL;
    }
    { const uint8_t* src = (const uint8_t*)elf_buf; for (size_t i = 0; i < size; i++) p->image[i] = src[i]; }
    p->image_size = size;

    uint64_t entry = 0;
    uint64_t footprint = 0;
    int r = elf_load_image_lazy(p->image, size, space, &entry, &p->vmas, &footprint);
    if (r != ELF_OK) {
        terminal_writestring("[PROC] elf lazy load fail\n");
        kfree(p->image);
        vmm_space_destroy(space);
        kfree(p);
        return NULL;
    }
    // [M14] Reserve the user stack as a demand-zero (ANON) VMA — 8 pages below
    // the (ASLR-randomized) stack top. Pages fault in on first push; the absence
    // of a VMA below the region is the guard (a stack underflow faults to the
    // unhandled path).
    const uint32_t STACK_PAGES = 8;
    // [M39] User ASLR: slide the stack top down by a random page-aligned offset
    // (<=16MB). Stays well above USER_MMAP_END so argv/env never collide.
    uint64_t st_top = USER_STACK_TOP - aslr_slide(16ULL * 1024 * 1024);
    uint64_t st_lo  = st_top - (uint64_t)STACK_PAGES * 0x1000ULL;
    if (vma_add(&p->vmas, st_lo, st_top,
                VMM_FLAG_USER | VMM_FLAG_RW | VMM_FLAG_NOEXEC,
                VMA_TYPE_ANON, NULL, 0, 0, 0) != 0) {
        terminal_writestring("[PROC] stack VMA add fail\n");
        kfree(p->image);
        vmm_space_destroy(space);
        kfree(p);
        return NULL;
    }
    footprint += (uint64_t)STACK_PAGES * 0x1000ULL;

    p->pid = alloc_pid();
    p->cpu_affinity = pick_affinity();   // [M29] pin to an online CPU
    p->space = space;
    p->entry = entry;
    p->stack_top = st_top;
    p->kstack_top = 0;
    p->kstack_slot = 0;
    p->tf = NULL;
    // [M8] Non-runnable while under construction: with preemptive scheduling
    // active, the scheduler must not pick this process (tf still NULL) until it
    // is fully built. Promoted to PROC_NEW at the end.
    p->state = PROC_BLOCKED;
    p->manifest = NULL;
    // [M11] Default privilege: USER, no device binding. Upgraded below only if
    // the (already signature-verified) manifest declares PROC_TYPE_DRIVER.
    p->proc_type = PROC_TYPE_USER;
    p->drv_dev_id = -1;
    p->drv_caps = 0;
    p->cap_net = 0;          // [M24] granted below from the signed manifest flags
    p->cap_enforce = 0;      // [M35] ambient (full trust) until a manifest confines it
    p->cap_mask = 0;
    // [M14] No eager page tracking: pages are demand-paged and freed at teardown
    // by vmm_space_destroy() (which frees every present leaf in the user range).
    // mapped_page_count/user_mem_bytes report the RESERVED footprint (sum of VMA
    // sizes), which is also what the manifest max_mem limit is checked against.
    p->cpu_ticks = 0;
    p->exit_code = 0;
    p->wait_pid = -1; p->wait_result = 0; p->wait_ready = 0;
    p->sleep_until = 0; p->recv_chan = -1;
    // [M30] Signals: default dispositions, empty masks, own process group, no
    // parent process (a shell-spawned program is reaped via the poll path; the
    // shell sets pgid for pipelines after creation). pid is assigned above.
    { extern void signal_init_proc(process_t*); signal_init_proc(p); }
    p->pgid = p->pid; p->ppid = 0;
    // [M39] User ASLR: random page-aligned slide of the heap and mmap arena bases
    // (<=32MB each, within their multi-GB regions). Transparent to programs.
    p->brk_start = USER_HEAP_BASE + aslr_slide(32ULL*1024*1024);
    p->brk_cur = p->brk_start;
    p->mmap_next = USER_MMAP_BASE + aslr_slide(32ULL*1024*1024); p->mem_limit = 0;
    p->user_mem_bytes = footprint;
    p->mapped_page_count = (uint32_t)(footprint / 4096ULL);
    // Manifest stub
    elf_manifest_t* mf = (elf_manifest_t*)kmalloc(sizeof(elf_manifest_t));
    if (mf && elf_manifest_parse(elf_buf, size, mf) == 0) {
        // Validazione entry e flags
        if (elf_manifest_validate(mf, entry) == MANIFEST_OK) {
            // [M13] Enforce the manifest memory limit: abort at load if the
            // process's mapped footprint exceeds max_mem (0 = unlimited). The
            // signature covers the manifest, so the limit is trust-rooted.
            if (mf->max_mem) {
                // [M14] Checked against the RESERVED footprint (sum of VMA sizes),
                // not lazily-mapped pages — a program that reserves more address
                // space than its signed limit is refused at load regardless of how
                // little it touches at runtime.
                uint64_t used_mem = footprint;
                if (used_mem > mf->max_mem) {
                    terminal_writestring("[MANIFEST] max_mem superato, abort processo\n");
                    debugcon_writestring("[M13] max_mem exceeded -> process REFUSED (used=");
                    debugcon_print_hex(used_mem);
                    debugcon_writestring(" limit=");
                    debugcon_print_hex(mf->max_mem);
                    debugcon_writestring(")\n");
                    kfree(mf);
                    // Leak-free teardown: free the pinned image, then
                    // vmm_space_destroy frees any faulted leaf frames, the
                    // page-table frames, the private PDPT and the PML4 (and
                    // kfrees the vmm_space_t). Mirrors process_destroy.
                    if (p->image) kfree(p->image);
                    vmm_space_destroy(space);
                    kfree(p);
                    return NULL;
                }
                // [M18] Remember the limit so brk/mmap can enforce it at runtime.
                p->mem_limit = mf->max_mem;
            }
            p->manifest = mf;
            // [M24] CAP_NET from the signed manifest flags (socket syscalls gate on it).
            p->cap_net = (mf->flags & MANIFEST_FLAG_CAP_NET) ? 1 : 0;
            // [M35] Generalized capabilities. Ambient by default (signature = full
            // trust); a manifest may opt into least-privilege confinement.
            p->cap_enforce = (mf->flags & MANIFEST_FLAG_CAP_ENFORCE) ? 1 : 0;
            p->cap_mask    = mf->flags & MANIFEST_CAP_MASK;
            if (p->cap_enforce) {
                debugcon_writestring("[M35] confined process pid loaded, caps=");
                debugcon_print_hex(p->cap_mask);
                debugcon_writestring("\n");
            }
            // [M11] Driver Space: the signed manifest is the trust root for the
            // driver claim. If it declares PROC_TYPE_DRIVER, validate the device
            // and grant only capabilities the device actually supports (subset),
            // then bind the process. A bogus claim degrades to USER (the process
            // still runs, but SYS_DRIVER will refuse it).
            if (mf->proc_type == MANIFEST_PROC_TYPE_DRIVER) {
                const device_desc_t* dev = driver_get_device((int)mf->dev_id);
                if (!dev) {
                    debugcon_writestring("[M11] driver claim refused: unknown device\n");
                } else {
                    uint32_t granted = mf->dev_caps & dev->caps_mask;
                    if (granted != mf->dev_caps)
                        debugcon_writestring("[M11] WARN: requested caps reduced to device-supported subset\n");
                    if (driver_register_binding_caps(p, (int)mf->dev_id, granted) == DRV_OK) {
                        p->proc_type = PROC_TYPE_DRIVER;
                        p->drv_dev_id = (int)mf->dev_id;
                        p->drv_caps = granted;
                        debugcon_writestring("[M11] driver bound dev=");
                        debugcon_print_hex((uint64_t)mf->dev_id);
                        debugcon_writestring(" caps=");
                        debugcon_print_hex((uint64_t)granted);
                        debugcon_writestring("\n");
                    } else {
                        debugcon_writestring("[M11] driver bind failed (no slot)\n");
                    }
                }
            }
        } else {
            terminal_writestring("[MANIFEST] validation fail, scarto manifest\n");
            kfree(mf);
        }
    }
    p->regs.rip = entry;
    p->regs.rsp = st_top;
    p->regs.rflags = 0x202; // IF abilitato default
    p->regs.rax = p->regs.rbx = p->regs.rcx = p->regs.rdx = 0;
    p->regs.rsi = p->regs.rdi = p->regs.rbp = 0;
    // Init fd table
    for(int i=0;i<32;i++){ p->fds[i].inode=NULL; p->fds[i].offset=0; p->fds[i].flags=0; p->fds[i].used=0; p->fds[i].is_pipe=0; p->fds[i].pipe_w=0; }
    // [M25] Reserve fds 0/1/2 as stdin/stdout/stderr so open()/pipe() allocate
    // from fd 3 up (otherwise a pipe end would land on fd 0/1/2 and collide with
    // the console special-cases in ksys_read/ksys_write). They carry no inode —
    // the std-stream paths are keyed on the fd number.
    p->fds[0].used=1; p->fds[1].used=1; p->fds[2].used=1;
    p->wait_pipe=NULL; // [M25]
    if (proc_add(p)!=0) {
        terminal_writestring("[PROC] table full\n");
        if (p->manifest) kfree(p->manifest);
        if (p->image) kfree(p->image);
        vmm_space_destroy(space);
        kfree(p);
        return NULL;
    }
    // [M5] Allocate per-process guarded kernel stack using bounded slot
    p->kstack_top = vmm_alloc_kernel_stack_for_slot(p->kstack_slot);
    if (!p->kstack_top) {
        terminal_writestring("[PROC] kstack alloc failed\n");
        proc_remove(p);
        if (p->manifest) kfree(p->manifest);
        if (p->image) kfree(p->image);
        vmm_space_destroy(space);
        kfree(p);
        return NULL;
    }
    // [M6] Allocate and initialize trapframe for initial iretq entry
    p->tf = (trapframe_t*)kmalloc(sizeof(trapframe_t));
    if (!p->tf) {
        terminal_writestring("[PROC] trapframe alloc failed\n");
        vmm_free_kernel_stack_for_slot(p->kstack_slot); p->kstack_top = 0;
        proc_remove(p);
        if (p->manifest) kfree(p->manifest);
        if (p->image) kfree(p->image);
        vmm_space_destroy(space);
        kfree(p);
        return NULL;
    }
    {
        // Zero all GPRs
        uint8_t* z = (uint8_t*)p->tf;
        for (int i = 0; i < (int)sizeof(trapframe_t); i++) z[i] = 0;
        // CPU iret frame
        p->tf->rip    = entry;
        p->tf->rflags = 0x202;       // IF enabled
        p->tf->rsp    = st_top;
        // [M6] User selectors for ring3 execution
        p->tf->cs     = 0x1B;  // User code: GDT entry 3, RPL=3
        p->tf->ss     = 0x23;  // User data: GDT entry 4, RPL=3
        // Markers
        p->tf->int_no   = 0x80;
        p->tf->err_code = 0;
    }
    // [M16] Set up argc/argv/envp on the user stack and pass them to the entry
    // per the System V convention (rdi=argc, rsi=argv, rdx=envp). main(void)
    // programs simply ignore the extra registers. On failure (too many/large
    // args) fall back to argc=0 rather than refusing to run.
    {
        uint64_t rsp = st_top, argvp = 0, envp = 0;
        if (argc > 0 && setup_user_args(p, argc, argv, &rsp, &argvp, &envp) == 0) {
            p->tf->rsp = rsp;
            p->tf->rdi = (uint64_t)argc;
            p->tf->rsi = argvp;
            p->tf->rdx = envp;
        } else if (argc > 0) {
            terminal_writestring("[PROC] argv setup failed; running with argc=0\n");
        }
    }
    // Hardening mapping condiviso
    vmm_harden_user_space(space);
    // [M8] Fully built — now safe for the scheduler to pick.
    p->state = PROC_NEW;
    if (g_kverbose) {
        terminal_writestring("[PROC] creato PID=");
        char hx[]="0123456789ABCDEF"; for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->pid>>i)&0xF]);
        terminal_writestring(" entry="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(entry>>i)&0xF]);
        terminal_writestring(" stack_top="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(st_top>>i)&0xF]);
        terminal_writestring(" pages="); for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->mapped_page_count>>i)&0xF]); terminal_writestring("\n");
    }
    return p;
}

// [M19] fork: create a copy-on-write child of 'parent'. 'tf' is the parent's
// live syscall trapframe; the child resumes there with rax=0 (fork()==0). The
// address space is COW-shared (vmm_fork_space); the pinned image + manifest are
// copied (the child owns them); a fresh kernel stack + trapframe are allocated.
// Driver privilege is NOT inherited (children are plain users). Returns the child
// (PROC_READY) or NULL.
process_t* process_fork(process_t* parent, trapframe_t* tf) {
    if (!parent || !parent->space || !tf) return NULL;
    process_t* c = (process_t*)kmalloc(sizeof(process_t));
    if (!c) return NULL;
    *c = *parent;                              // shallow-copy all fields (incl. VMAs, fds)
    c->pid = alloc_pid();
    c->cpu_affinity = pick_affinity();         // [M29] spread the child across CPUs
    c->space = NULL; c->image = NULL; c->manifest = NULL; c->tf = NULL;
    c->kstack_top = 0; c->kstack_slot = 0; c->mapped_pages = NULL;

    c->space = vmm_fork_space(parent->space);
    if (!c->space) { kfree(c); return NULL; }

    if (parent->image && parent->image_size) {
        c->image = (uint8_t*)kmalloc(parent->image_size);
        if (!c->image) { vmm_space_destroy(c->space); kfree(c); return NULL; }
        for (size_t i = 0; i < parent->image_size; i++) c->image[i] = parent->image[i];
        c->image_size = parent->image_size;
        // Re-point FILE VMA backing pointers into the child's own image copy.
        for (uint32_t i = 0; i < c->vmas.count; i++) {
            vma_t* v = &c->vmas.v[i];
            if (v->type == VMA_TYPE_FILE && v->file_base)
                v->file_base = c->image + ((uint64_t)v->file_base - (uint64_t)parent->image);
        }
    }
    if (parent->manifest) {
        c->manifest = kmalloc(sizeof(elf_manifest_t));
        if (c->manifest) { uint8_t* d = (uint8_t*)c->manifest; uint8_t* s = (uint8_t*)parent->manifest;
                           for (size_t i = 0; i < sizeof(elf_manifest_t); i++) d[i] = s[i]; }
    }
    c->tf = (trapframe_t*)kmalloc(sizeof(trapframe_t));
    if (!c->tf) { if (c->manifest) kfree(c->manifest); if (c->image) kfree(c->image);
                  vmm_space_destroy(c->space); kfree(c); return NULL; }
    *c->tf = *tf;                              // child resumes at the fork syscall...
    c->tf->rax = 0;                            // ...but fork() returns 0 in the child

    c->wait_pid = -1; c->wait_result = 0; c->wait_ready = 0;
    c->sleep_until = 0; c->recv_chan = -1; c->exit_code = 0; c->cpu_ticks = 0;
    c->wait_pipe = NULL;                       // [M25] not blocked on a pipe
    c->proc_type = PROC_TYPE_USER; c->drv_dev_id = -1; c->drv_caps = 0;
    // [M30] Signal dispositions/mask/restorer are inherited (shallow-copied); the
    // pending set starts empty in the child (POSIX). ppid = parent; pgid inherited.
    c->sig_pending = 0;
    c->ppid = parent->pid; c->pgid = parent->pgid;
    c->state = PROC_BLOCKED;                   // not runnable until fully built

    // [M25] The child inherits the parent's open pipe ends (fds were shallow
    // copied) — bump each inherited end's refcount so EOF/EPIPE accounting stays
    // correct across fork. The pipe object itself is shared (kernel-owned).
    { extern void pipe_ref(void*, int);
      for (int i = 0; i < 32; i++)
          if (c->fds[i].used && c->fds[i].is_pipe)
              pipe_ref(c->fds[i].inode, c->fds[i].pipe_w); }

    if (proc_add(c) != 0) {
        kfree(c->tf); if (c->manifest) kfree(c->manifest); if (c->image) kfree(c->image);
        vmm_space_destroy(c->space); kfree(c); return NULL;
    }
    c->kstack_top = vmm_alloc_kernel_stack_for_slot(c->kstack_slot);
    if (!c->kstack_top) {
        proc_remove(c); kfree(c->tf); if (c->manifest) kfree(c->manifest);
        if (c->image) kfree(c->image); vmm_space_destroy(c->space); kfree(c); return NULL;
    }
    debugcon_writestring("[FORK] parent="); debugcon_print_hex(parent->pid);
    debugcon_writestring(" -> child="); debugcon_print_hex(c->pid); debugcon_writestring("\n");
    c->state = PROC_READY;
    return c;
}

void process_print(const process_t* p) {
    if (!p) return;
    terminal_writestring("[PROC] PID=");
    char hx[]="0123456789ABCDEF"; for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->pid>>i)&0xF]);
    terminal_writestring(" entry="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(p->entry>>i)&0xF]);
    terminal_writestring(" stack_top="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(p->stack_top>>i)&0xF]);
    terminal_writestring(" state="); terminal_putchar('0'+p->state);
    terminal_writestring("\n");
}

int process_destroy(process_t* p) {
    if (!p) return -1;
    // [M11] Drop any driver bindings first: the registry holds a process_t*; a
    // stale pointer surviving kfree(p) could later alias a reused process and
    // grant it the dead driver's device — clear it unconditionally.
    driver_remove_all_bindings(p);
    // [M24] Close any sockets this process still owns (frees TCP conns + UDP binds).
    extern void socket_owner_cleanup(uint32_t pid);
    socket_owner_cleanup(p->pid);
    // [M25] Drop any still-open pipe ends so the peer observes EOF/EPIPE and the
    // pipe object is freed once both ends are gone.
    { extern void pipe_unref(void*, int);
      for (int i = 0; i < 32; i++)
          if (p->fds[i].used && p->fds[i].is_pipe) {
              pipe_unref(p->fds[i].inode, p->fds[i].pipe_w);
              p->fds[i].is_pipe = 0; p->fds[i].used = 0;
          } }
    extern int elf_unload_process(process_t* p);
    elf_unload_process(p);                       // unmap + free user page frames, zero PTEs
    if (p->manifest) { kfree(p->manifest); p->manifest = NULL; }
    if (p->mapped_pages) { kfree(p->mapped_pages); p->mapped_pages = NULL; }
    // [M14] Free the pinned ELF image backing the demand-paged FILE VMAs.
    if (p->image) { kfree(p->image); p->image = NULL; p->image_size = 0; }
    // [M6] Free saved trapframe
    if (p->tf) { kfree(p->tf); p->tf = NULL; }
    // [M5] Free per-process kernel stack
    if (p->kstack_top) { vmm_free_kernel_stack_for_slot(p->kstack_slot); p->kstack_top = 0; }
    // [M8] Free the whole address space: PT/PD page-table frames, the private
    // PML4[0] PDPT, and the PML4 itself (also kfree's the vmm_space_t).
    if (p->space) { vmm_space_destroy(p->space); p->space = NULL; }
    proc_remove(p);
    kfree(p);
    kvlog("[PROC] distrutto\n");
    return 0;
}
