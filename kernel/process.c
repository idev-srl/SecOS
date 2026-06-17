/*
 * SecOS Kernel - Process Management
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
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

#define MAX_PROCESSES 32
static process_t* proc_table[MAX_PROCESSES];
static uint32_t next_pid = 1;
static int proc_inited = 0;

int process_init_system(void) {
    for (int i=0;i<MAX_PROCESSES;i++) proc_table[i]=0;
    proc_inited = 1;
    terminal_writestring("[PROC] process table initialized\n");
    return 0;
}

static int proc_add(process_t* p) {
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (!proc_table[i]) { proc_table[i]=p; p->kstack_slot = (uint8_t)i; return 0; }
    }
    return -1;
}

static void proc_remove(process_t* p) {
    if (!p) return;
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (proc_table[i] == p) { proc_table[i] = NULL; return; }
    }
}

process_t* process_get_last(void) {
    process_t* best = NULL; uint32_t best_pid = 0;
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (proc_table[i] && proc_table[i]->pid > best_pid) { best = proc_table[i]; best_pid = proc_table[i]->pid; }
    }
    return best;
}

process_t* process_find_by_pid(uint32_t pid) {
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (proc_table[i] && proc_table[i]->pid == pid) return proc_table[i];
    }
    return NULL;
}

void process_foreach(void (*cb)(process_t*, void*), void* user) {
    if (!cb) return;
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (proc_table[i]) cb(proc_table[i], user);
    }
}

process_t* process_create_from_elf(const void* elf_buf, size_t size) {
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
    // USER_STACK_TOP. Pages fault in on first push; the absence of a VMA below
    // the region is the guard (a stack underflow faults to the unhandled path).
    const uint32_t STACK_PAGES = 8;
    uint64_t st_top = USER_STACK_TOP;
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

    p->pid = next_pid++;
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
    // [M14] No eager page tracking: pages are demand-paged and freed at teardown
    // by vmm_space_destroy() (which frees every present leaf in the user range).
    // mapped_page_count/user_mem_bytes report the RESERVED footprint (sum of VMA
    // sizes), which is also what the manifest max_mem limit is checked against.
    p->cpu_ticks = 0;
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
            }
            p->manifest = mf;
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
    for(int i=0;i<32;i++){ p->fds[i].inode=NULL; p->fds[i].offset=0; p->fds[i].flags=0; p->fds[i].used=0; }
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
    // Hardening mapping condiviso
    vmm_harden_user_space(space);
    // [M8] Fully built — now safe for the scheduler to pick.
    p->state = PROC_NEW;
    terminal_writestring("[PROC] creato PID=");
    char hx[]="0123456789ABCDEF"; for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->pid>>i)&0xF]);
    terminal_writestring(" entry="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(entry>>i)&0xF]);
    terminal_writestring(" stack_top="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(st_top>>i)&0xF]);
    terminal_writestring(" pages="); for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->mapped_page_count>>i)&0xF]); terminal_writestring("\n");
    return p;
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
    terminal_writestring("[PROC] distrutto\n");
    return 0;
}
