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
// POSIX/Linux wait-status encoding, end to end (kernel exit_code == the value
// SYS_WAIT/SYS_WAITANY hand to user space == what WIFEXITED & co. decode):
//   normal exit  : (status & 0xff) << 8           (low 7 bits 0)
//   killed by sig: sig & 0x7f  [| 0x80 core flag]  (never 0 or 0x7f)
//   stopped      : (sig << 8) | 0x7f
// A single number can no longer mean both "exited 128+N" and "killed by N".
#define W_EXITCODE(st)   ((int)(((unsigned)(st) & 0xffu) << 8))
#define W_SIGNALED(sig)  ((int)((unsigned)(sig) & 0x7fu))
#define W_STOPCODE(sig)  ((int)((((unsigned)(sig) & 0xffu) << 8) | 0x7fu))

// errno values returned (negated) by process_exec / SYS_EXECVE.
#ifndef ENOENT
#define ENOENT   2
#endif
#ifndef E2BIG
#define E2BIG    7
#endif
#ifndef ENOEXEC
#define ENOEXEC  8
#endif
#ifndef ENOMEM
#define ENOMEM  12
#endif
#ifndef EACCES
#define EACCES  13
#endif

#define PROC_TYPE_USER    0
#define PROC_TYPE_DRIVER  1

typedef struct process {
    // User x87/SSE register file (fxsave image, 16-byte aligned: process_t is
    // kmalloc'd 16-aligned and this is its first member). Ring 3 uses SSE
    // (doubles, gcc-vectorised memcpy) while the kernel is built -mno-sse, so
    // the scheduler saves it when a task is switched out and restores it when
    // the task runs again; fpu_valid = 0 means "start from the clean state".
    uint8_t  fpu[512] __attribute__((aligned(16)));
    uint8_t  fpu_valid;
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
    int      exit_code;      // POSIX wait status: W_EXITCODE(st,0) or W_SIGNALED(sig)
    uint8_t  collected;      // [M39] zombie collected by parent's wait() -> reapable
    // [M16/M17] Blocking state. While PROC_BLOCKED, exactly one wait condition is
    // armed; the matching wake (child exit / timer tick / channel send) flips the
    // process back to PROC_READY and it re-runs the syscall (rip was rewound).
    int      wait_pid;       // [M16] child pid being waited on (-1 = none/any)
    int      wait_result;    // [M16] exit status delivered to a woken waiter
    uint8_t  wait_ready;     // [M16] 1 = wait_result is valid (child already gone)
    uint64_t sleep_until;    // [M17] wake when timer_get_ticks() >= this (0 = n/a)
    int      recv_chan;      // [M17] IPC channel the caller is blocked receiving on (-1)
    void*    wait_pipe;      // [M25] pipe object the caller is blocked on (NULL = none)
    int      pipe_wdone;     // bytes of a blocked pipe write() already queued (the re-run resumes there)
    uint8_t  wait_tty;       // blocked in a console read() until input arrives (tty_read_try)
    // A caught signal woke this task out of a blocking syscall: sig_eintr_rip is
    // the user address of that syscall's `int 0x80`. If the handler runs before
    // the syscall re-runs (a timer tick at the resume boundary), the re-run must
    // still fail with EINTR instead of blocking again: sig_rerun_eintr is set on
    // the entry that matches it and makes signal_pending() report true.
    uint64_t sig_eintr_rip;
    uint8_t  sig_rerun_eintr;
    void*    wait_pty;       // pty_t* the caller is blocked on (read/write either side; NULL = none)
    uint8_t  power_phase;    // SYS_REBOOT shutdown state machine (kernel/power.c)
    uint64_t power_deadline; // tick deadline of the current shutdown phase
    // [M30] Signals + job control. sig_pending/sig_blocked are bitmasks (bit n =
    // signal n); sig_handler[n] is the disposition (SIG_DFL/SIG_IGN or a user
    // handler VA); sig_restorer is the libc trampoline that calls SYS_SIGRETURN.
    // pgid/ppid drive Ctrl-C delivery (to the foreground group) and SIGCHLD.
    uint64_t sig_pending;
    uint64_t sig_blocked;
    uint64_t sig_handler[32];
    uint64_t sig_restorer;
    // Job control: a stop/continue not yet reported to the parent's
    // waitpid(WUNTRACED / WCONTINUED). stop_sig = the signal that stopped us.
    uint8_t  stop_report;
    uint8_t  cont_report;
    int      stop_sig;
    // sigsuspend(): the mask to restore once the interrupting signal has been
    // dispatched (the handler frame saves it, so sigreturn restores it).
    uint8_t  sigsusp_active;
    uint64_t sigsusp_saved;
    uint32_t pgid;           // process group id (defaults to pid)
    uint32_t ppid;           // parent pid (0 = no parent process, e.g. shell-spawned)
    // Sessions + controlling terminal. sid = session id (a kernel-created process
    // leads its own). ctty = the struct tty* of a pty that is our controlling
    // terminal (holds a pty reference); NULL means the console, unless
    // ctty_detached is set (after setsid()/TIOCNOTTY: no controlling terminal).
    // A ctty whose tty->session no longer matches sid was hung up / given away
    // and counts as none (see pty_proc_ctty()).
    uint32_t sid;
    void*    ctty;
    uint8_t  ctty_detached;
    uint32_t spawner_pid;    // pid of the process whose SYS_SPAWN created us (0 = kernel/
                             // fork). With ppid, the only caller allowed to SYS_WAIT us.
    char     cwd[256];       // [M39] per-process current working directory (for chdir/getcwd + relative paths)
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
    // is_sock: socket index + 1 (0 = not a socket); `inode` stays NULL for a
    // socket so no file path can mistake it for a vfs_inode_t.
    // cloexec: FD_CLOEXEC — the fd is closed by a successful execve() (fcntl
    // F_SETFD, O_CLOEXEC on open, pipe2(O_CLOEXEC), F_DUPFD_CLOEXEC).
    struct proc_fd_entry { void* inode; uint64_t offset; uint32_t flags; int used; int is_pipe; int pipe_w; int cloexec; int is_sock; } fds[32];
    // Kernel-interface bookkeeping (kernel/kiface.c): procfs identity + CPU
    // accounting, umask, and the in-progress state of the blocking nanosleep /
    // poll syscalls. Set up by kiface_proc_init() at creation; fork copies it
    // and kiface_proc_fork() resets the per-instance parts.
    struct proc_ki {
        uint32_t umask;              // file-creation mask (default 022)
        uint64_t start_ns;           // monotonic creation time
        uint64_t utime_ticks;        // timer ticks spent in ring 3
        uint64_t stime_ticks;        // timer ticks spent in ring 0 (syscalls)
        uint64_t nvcsw;              // voluntary context switches (blocks)
        uint64_t nsleep_deadline;    // nanosleep: absolute monotonic ns, 0 = none
        uint64_t poll_deadline;      // poll: absolute monotonic ns, ~0 = no timeout
        uint8_t  poll_active;        // poll() in progress (re-run after a wake)
        uint8_t  poll_wait;          // blocked in poll(): pipe activity wakes it
        uint16_t cmdline_len;        // bytes in cmdline (NUL-separated argv)
        char     comm[16];           // executable name (basename of argv[0])
        char     cmdline[256];       // argv, NUL-separated (/proc/<pid>/cmdline)
    } ki;
} process_t;

int process_init_system(void); // initialize process table
process_t* process_create_from_elf(const void* elf_buf, size_t size);
process_t* process_create_from_elf_args(const void* elf_buf, size_t size,
                                        int argc, const char* const argv[]); // [M16]
process_t* process_fork(process_t* parent, trapframe_t* tf); // [M19] COW fork
// In-place execve: replace p's user image with the signed ELF 'image' (kmalloc'd,
// ownership transferred; freed on failure). 'blob' holds argc argv strings then
// envc envp strings, each NUL-terminated, 'blen' bytes total. Builds and
// validates the new address space first; on any failure returns -errno with p
// untouched. On success p (== current, 'tf' its live syscall trapframe) resumes
// at the new entry point when the syscall returns. Returns 0 / -errno.
int process_exec(process_t* p, trapframe_t* tf, uint8_t* image, size_t size,
                 const char* blob, uint64_t blen, int argc, int envc);
void process_print(const process_t* p);
process_t* process_get_last(void);
process_t* process_find_by_pid(uint32_t pid);
void process_foreach(void (*cb)(process_t*, void*), void* user);
process_t* process_reap_one(uint32_t affinity); // [M29] detach+return one ZOMBIE
int process_destroy(process_t* p);
void process_release_pipes(process_t* p); // drop pipe ends + sockets at exit (EOF to peers)

#endif // PROCESS_H
