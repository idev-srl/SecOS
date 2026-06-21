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
    enum { PROC_NEW, PROC_READY, PROC_RUNNING, PROC_BLOCKED, PROC_ZOMBIE,
           PROC_STOPPED /* [M30] job-control stop (SIGSTOP/SIGTSTP) */ } state;
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
    int      cap_net;       // [M24] CAP_NET: 1 if the signed manifest grants sockets
    int      cap_enforce;   // [M35] 1 if confined to cap_mask (least privilege)
    uint32_t cap_mask;      // [M35] granted MANIFEST_FLAG_CAP_* bits when enforcing
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
    // [M18] Dynamic memory: brk heap (single growable ANON region) + mmap arena
    // (bump allocator). mem_limit mirrors the signed manifest max_mem (0 =
    // unlimited) and bounds runtime growth so a process can't exceed its limit.
    uint64_t  brk_start;
    uint64_t  brk_cur;
    uint64_t  mmap_next;
    uint64_t  mem_limit;
    int      exit_code;      // [M15] 0=normal (SYS_EXIT); 128+vec=killed by fault
    // [M16/M17] Blocking state. While PROC_BLOCKED, exactly one wait condition is
    // armed; the matching wake (child exit / timer tick / channel send) flips the
    // process back to PROC_READY and it re-runs the syscall (rip was rewound).
    int      wait_pid;       // [M16] child pid being waited on (-1 = none/any)
    int      wait_result;    // [M16] exit status delivered to a woken waiter
    uint8_t  wait_ready;     // [M16] 1 = wait_result is valid (child already gone)
    uint64_t sleep_until;    // [M17] wake when timer_get_ticks() >= this (0 = n/a)
    int      recv_chan;      // [M17] IPC channel the caller is blocked receiving on (-1)
    void*    wait_pipe;      // [M25] pipe object the caller is blocked on (NULL = none)
    // [M30] Signals + job control. sig_pending/sig_blocked are bitmasks (bit n =
    // signal n); sig_handler[n] is the disposition (SIG_DFL/SIG_IGN or a user
    // handler VA); sig_restorer is the libc trampoline that calls SYS_SIGRETURN.
    // pgid/ppid drive Ctrl-C delivery (to the foreground group) and SIGCHLD.
    uint64_t sig_pending;
    uint64_t sig_blocked;
    uint64_t sig_handler[32];
    uint64_t sig_restorer;
    uint32_t pgid;           // process group id (defaults to pid)
    uint32_t ppid;           // parent pid (0 = no parent process, e.g. shell-spawned)
    // [M29] SMP: the CPU this process is pinned to. Assigned round-robin at
    // creation; only this CPU schedules, preempts, and reaps it, so the context
    // switch needs no lock and a freed PCB is never touched by another core.
    uint32_t cpu_affinity;
    // Runtime metrics
    uint64_t cpu_ticks;      // accumulated CPU ticks (scheduler)
    uint64_t user_mem_bytes; // virtual memory footprint (updated at creation / future extensions)
    // Simple file descriptor table
    // [M25] When is_pipe, `inode` holds a pipe_t* and pipe_w selects the end
    // (1 = write end, 0 = read end). Otherwise `inode` is a vfs_inode_t*.
    struct proc_fd_entry { void* inode; uint64_t offset; uint32_t flags; int used; int is_pipe; int pipe_w; } fds[32];
} process_t;

int process_init_system(void); // initialize process table
process_t* process_create_from_elf(const void* elf_buf, size_t size);
process_t* process_create_from_elf_args(const void* elf_buf, size_t size,
                                        int argc, const char* const argv[]); // [M16]
process_t* process_fork(process_t* parent, trapframe_t* tf); // [M19] COW fork
void process_print(const process_t* p);
process_t* process_get_last(void);
process_t* process_find_by_pid(uint32_t pid);
void process_foreach(void (*cb)(process_t*, void*), void* user);
process_t* process_reap_one(uint32_t affinity); // [M29] detach+return one ZOMBIE
int process_destroy(process_t* p);

#endif // PROCESS_H
