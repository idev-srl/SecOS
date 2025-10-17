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
#include "../mm/elf.h" // for ELF_OK

typedef struct process {
    uint32_t pid;
    vmm_space_t* space;
    uint64_t entry;
    uint64_t stack_top;
    uint64_t kstack_top; // kernel stack top (for future trap/switch)
    enum { PROC_NEW, PROC_READY, PROC_RUNNING, PROC_BLOCKED, PROC_ZOMBIE } state;
    struct regs_snapshot {
        uint64_t rip, rsp, rflags;
        uint64_t rax, rbx, rcx, rdx;
        uint64_t rsi, rdi;
        uint64_t rbp;
    } regs;
    void* manifest; // stub pointer to future manifest_t
    uint64_t* mapped_pages; // array of virtual page addresses (code+data+stack)
    uint32_t mapped_page_count; // page count
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
