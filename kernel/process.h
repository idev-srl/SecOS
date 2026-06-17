/*
 * SecOS Kernel - Process Management (Header)
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef PROCESS_H
#define PROCESS_H
#include <stdint.h>
#include <stddef.h>
#include "vmm.h"
#include "trapframe.h"
#include "../mm/elf.h" // for ELF_OK
#include "../mm/vma.h" // [M14] per-process VMAs for demand paging

// [M11] Process privilege type. A "driver" is a Ring-3 process granted
// capability-mediated hardware access (SYS_DRIVER). The claim is rooted in the
// signed `.note.secos` manifest — see mm/elf_manifest.h / docs/DRIVER_SPACE.md.
#define PROC_TYPE_USER    0
#define PROC_TYPE_DRIVER  1

typedef struct process {
    uint32_t pid;
    vmm_space_t* space;
    uint64_t entry;
    uint64_t stack_top;
    uint64_t kstack_top; // kernel stack top (for future trap/switch)
    uint8_t  kstack_slot; // bounded slot index for kernel stack region
    enum { PROC_NEW, PROC_READY, PROC_RUNNING, PROC_BLOCKED, PROC_ZOMBIE } state;
    trapframe_t* tf;      // [M6] saved trapframe for context switch
    struct regs_snapshot {
        uint64_t rip, rsp, rflags;
        uint64_t rax, rbx, rcx, rdx;
        uint64_t rsi, rdi;
        uint64_t rbp;
    } regs;
    void* manifest; // stub pointer to future manifest_t
    // [M11] Driver Space: privilege type + granted device binding (from the
    // signed manifest). proc_type defaults to PROC_TYPE_USER.
    int      proc_type;     // PROC_TYPE_USER / PROC_TYPE_DRIVER
    int      drv_dev_id;    // device id bound to this driver, -1 if none
    uint32_t drv_caps;      // granted capability mask (DEV_CAP_*), 0 if none
    uint64_t* mapped_pages; // array of virtual page addresses (code+data+stack)
    uint32_t mapped_page_count; // page count
    // [M14] Demand paging: pages are not eagerly mapped. 'vmas' describes the
    // reserved virtual ranges; the #PF handler materializes pages on first
    // touch. 'image'/'image_size' is the pinned ELF copy backing FILE VMAs
    // (freed at process_destroy). Teardown of faulted frames is handled by
    // vmm_space_destroy(), which frees every present leaf in the user range.
    vma_set_t vmas;
    uint8_t*  image;
    size_t    image_size;
    int      exit_code;      // [M15] 0=normal (SYS_EXIT); 128+vec=killed by fault
    // Runtime metrics
    uint64_t cpu_ticks;      // accumulated CPU ticks (scheduler)
    uint64_t user_mem_bytes; // virtual memory footprint (updated at creation / future extensions)
    // Simple file descriptor table
    struct proc_fd_entry { void* inode; uint64_t offset; uint32_t flags; int used; } fds[32];
} process_t;

int process_init_system(void); // initialize process table
process_t* process_create_from_elf(const void* elf_buf, size_t size);
void process_print(const process_t* p);
process_t* process_get_last(void);
process_t* process_find_by_pid(uint32_t pid);
void process_foreach(void (*cb)(process_t*, void*), void* user);
int process_destroy(process_t* p);

#endif // PROCESS_H
