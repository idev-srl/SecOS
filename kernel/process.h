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
#include "cred.h"      // [M43] uid/gid credentials
#include "klock.h"     // [M50] klock_task_t
#include "spinlock.h"  // [M50-B1] mm_lock

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

// [U9] One fd-table entry (was declared inside process_t).
// When is_pipe, `inode` holds a pipe_t* and pipe_w selects the end
// (1 = write end, 0 = read end). Otherwise `inode` is a vfs_inode_t*.
// is_sock: socket index + 1 (0 = not a socket); `inode` stays NULL for a
// socket so no file path can mistake it for a vfs_inode_t.
// cloexec: FD_CLOEXEC — the fd is closed by a successful execve() (fcntl
// F_SETFD, O_CLOEXEC on open, pipe2(O_CLOEXEC), F_DUPFD_CLOEXEC).
// gen: [U10] a number unique to this fd opening (set by fd_alloc), so epoll
// notices a registered fd that was closed and its number reused.
struct proc_fd_entry { void* inode; uint64_t offset; uint32_t flags; int used; int is_pipe; int pipe_w; int cloexec; int is_sock; uint32_t gen; };

#define PROC_NFDS   32          // fd table size
#define PROC_NSIG   64          // [U9] signals 1..63 (bit n of a mask = signal n)

// [U9] Threads (docs/USERLAND_PLAN.md D5, option T2). process_t stays the
// schedulable task (one per thread: kernel stack, trapframe, FPU, state, wait
// fields, signal mask, FS base); what the threads of a process share lives in
// two refcounted objects (kernel/thread.c):
//   proc_mm_t       the address space: page tables, VMAs, pinned ELF image,
//                   brk/mmap state and the lock that guards them;
//   thread_group_t  everything else a POSIX process owns: tgid, the fd table,
//                   cwd, umask, signal dispositions, the process-directed
//                   pending set, group exit / exec / stop state.
// fork and every image creation make new ones; clone(CLONE_VM|CLONE_FILES|
// CLONE_SIGHAND|CLONE_THREAD...) shares both. Each process_t holds one
// reference on each, dropped by process_destroy (the last one frees).
typedef struct proc_mm {
    volatile uint32_t refs;
    // [M50-B1] Address-space lock (LK_S35_MM, was process_t.mm_lock): the
    // VMA set, the user page tables of 'space' and the 'space' / 'image'
    // pointers. Every thread takes it for its edits (mmap/munmap/mprotect/
    // brk, exec swap, fork copy, the PTE install of a fault); other tasks take
    // it to read (procfs, umount). It also publishes fd-table edits (see
    // kernel/syscall.c fdt_lock). Rules: never held across a page-cache/FS
    // call, a user copy, or anything that takes proc_lock (S30 < S35).
    // [M52] irqsave: see mm_lock() below.
    spinlock_t lock;
    // [U9] Serializes the VMA editors (brk/mmap/munmap/mprotect) of the
    // threads: their page-table loops drop 'lock' now and then (mm_relax).
    klock_t   map_lock;
    vmm_space_t* space;     // owned: destroyed with the last reference
    vma_set_t vmas;
    // [M14] The pinned ELF copy backing FILE VMAs (freed with the mm).
    uint8_t*  image;
    size_t    image_size;
    // [M18] brk heap + mmap arena (bump allocator).
    uint64_t  brk_start;
    uint64_t  brk_cur;
    uint64_t  mmap_next;
} proc_mm_t;

typedef struct thread_group {
    volatile uint32_t refs;     // process_t pointing here (memory lifetime)
    uint32_t tgid;              // the leader's pid = getpid() of every thread
    // Under proc_lock: threads that have not exited yet. The last one to
    // exit ends the process (fds closed, parent notified, leader collectable).
    uint32_t nr_live;
    // Under proc_lock: set by the last thread once the process has fully
    // ended (fds closed, status final). Only then may the parent collect the
    // leader (wait) and the reaper free it.
    uint8_t  dead;
    // Group exit (exit_group, a fatal signal): set once, under proc_lock;
    // every other thread is SIGKILLed and exits with group_code.
    uint8_t  group_exit;
    int      group_code;
    // execve from a thread: the tid that kills the others and waits for them.
    uint32_t exec_tid;
    // Group stop (SIGSTOP & co. acted on by one thread stops them all):
    // stop_active from the first thread that acts on it until SIGCONT;
    // nr_stopped counts the threads that reached PROC_STOPPED, and stop_done
    // is set (and the parent told, once) when all live threads have.
    uint8_t  stop_active;
    uint8_t  stop_done;
    int      stop_sig;
    uint32_t nr_stopped;
    // Process-directed pending signals (bit n = signal n), atomic.
    uint64_t shared_pending;
    // Shared by CLONE_FILES / CLONE_FS: process_t.fds and .cwd point here.
    // fd_lock (LK_K12_FILES) makes an fd read-modify-write and the object
    // reference it takes or drops one step (another thread may close the fd
    // meanwhile); the stores themselves are still published under mm->lock.
    klock_t  fd_lock;
    struct proc_fd_entry fd[PROC_NFDS];
    char     cwd[256];
    uint32_t umask;             // file-creation mask (default 022)
    // Shared by CLONE_SIGHAND: process_t.sig_handler points here.
    uint64_t handler[PROC_NSIG];
    uint64_t restorer;          // the libc trampoline that calls SYS_SIGRETURN
    // [U10] ITIMER_REAL (setitimer/alarm), per process: next expiry (monotonic
    // ns, 0 = disarmed) and reload interval; the BSP tick posts SIGALRM to the
    // process (signal_post). Zeroed for a new process and on fork.
    volatile uint64_t itimer_next_ns;
    uint64_t itimer_int_ns;
} thread_group_t;

typedef struct process {
    // User x87/SSE register file (fxsave image, 16-byte aligned: process_t is
    // kmalloc'd 16-aligned and this is its first member). Ring 3 uses SSE
    // (doubles, gcc-vectorised memcpy) while the kernel is built -mno-sse, so
    // the scheduler saves it when a task is switched out and restores it when
    // the task runs again; fpu_valid = 0 means "start from the clean state".
    uint8_t  fpu[512] __attribute__((aligned(16)));
    uint8_t  fpu_valid;
    uint32_t pid;         // [U9] the thread id (unique); getpid() is tgid
    uint32_t tgid;        // [U9] thread-group id = the leader's pid
    proc_mm_t* mm;        // [U9] shared address space (kernel/thread.c)
    thread_group_t* tg;   // [U9] shared process state (kernel/thread.c)
    vmm_space_t* space;   // == mm->space (a copy: the scheduler loads it)
    uint64_t entry;
    uint64_t stack_top;
    uint64_t kstack_top; // kernel stack top (for future trap/switch)
    uint16_t kstack_slot; // bounded slot index for kernel stack region
    // [M52] PROC_EMBRYO: in the table but still being built (no kstack / tf
    // yet) or held by its creator while it is wired up. Nothing ever moves it
    // out of that state but its creator: no wake-up (signal_post, waits)
    // touches it -- they only know BLOCKED/STOPPED -- and no CPU picks it.
    // It used to be BLOCKED, and a signal posted to its group (Ctrl-C, or
    // kill() from another CPU) could make a half-built process READY.
    enum { PROC_NEW, PROC_READY, PROC_RUNNING, PROC_BLOCKED, PROC_ZOMBIE,
           PROC_STOPPED /* [M30] job-control stop (SIGSTOP/SIGTSTP) */,
           PROC_EMBRYO } state;
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
    int      no_new_privs;  // [M46] sticky: exec/spawn can only shrink caps, no set-id, no driver
    uint64_t* mapped_pages; // array of virtual page addresses (code+data+stack)
    uint32_t mapped_page_count; // page count
    // [M14] Demand paging: pages are not eagerly mapped. 'vmas' describes the
    // reserved virtual ranges; the #PF handler materializes pages on first
    // touch. 'image'/'image_size' is the pinned ELF copy backing FILE VMAs
    // (freed at process_destroy). Teardown of faulted frames is handled by
    // vmm_space_destroy(), which frees every present leaf in the user range.
    // [U9] The VMA set, the pinned image and the brk/mmap state live in mm.
    // mem_limit mirrors the signed manifest max_mem (0 = unlimited) and bounds
    // runtime growth (a per-thread copy: only an exec changes it).
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
    void*    wait_udrv;      // [M57] ring-3 driver claim whose IRQ fd read() we sleep in (NULL = none)
    uint8_t  power_phase;    // SYS_REBOOT shutdown state machine (kernel/power.c)
    uint64_t power_deadline; // tick deadline of the current shutdown phase
    // [M30] Signals + job control. sig_pending/sig_blocked are bitmasks (bit n =
    // signal n); sig_handler[n] is the disposition (SIG_DFL/SIG_IGN or a user
    // handler VA); sig_restorer is the libc trampoline that calls SYS_SIGRETURN.
    // pgid/ppid drive Ctrl-C delivery (to the foreground group) and SIGCHLD.
    // [U9] sig_pending is thread-directed (tgkill, faults); process-directed
    // signals pend in tg->shared_pending. sig_blocked is per thread.
    // sig_handler points at tg->handler (shared by CLONE_SIGHAND).
    uint64_t sig_pending;
    uint64_t sig_blocked;
    uint64_t* sig_handler;
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
    // [M43] Credentials: real/effective/saved uid + gid and the supplementary
    // groups. Inherited by fork, spawn and execve; changed by the set*id
    // syscalls and a set-id exec (kernel/cred.c). Kernel-made processes: root.
    struct proc_cred cred;
    char*    cwd;            // [M39] current working directory: [U9] points at tg->cwd (256 bytes)
    // [M48] SMP scheduling. cpu_affinity is the task's HOME CPU (a soft
    // preference: that CPU's scheduler picks it first, any idle CPU may pull
    // it, and the CPU that runs it becomes its new home). cpu_pin >= 0 is an
    // explicit hard affinity (only that CPU may run it), -1 = none. on_cpu is
    // 1 from the moment a CPU claims the task (process_claim_next) until that
    // CPU has completely switched off its kernel stack, CR3 and FPU state
    // (arch_switch_finish); no other CPU may run, claim or reap it meanwhile.
    // wake_snap is g_wake_seq at syscall entry: a wake-up that raced the
    // decision to block turns the block into an immediate re-run.
    uint32_t cpu_affinity;
    int32_t  cpu_pin;
    volatile uint32_t on_cpu;
    uint64_t wake_snap;
    uint64_t ready_tick;     // timer tick at which it last became READY (0 = unknown)
    uint64_t smp_logged;     // APs (index < 64) an [SMP] run marker was logged for
    // Runtime metrics
    uint64_t cpu_ticks;      // accumulated CPU ticks (scheduler)
    uint64_t user_mem_bytes; // virtual memory footprint (updated at creation / future extensions)
    // File descriptor table (struct proc_fd_entry above): [U9] points at
    // tg->fd[PROC_NFDS], shared by the threads of a process (CLONE_FILES).
    struct proc_fd_entry* fds;
    // Kernel-interface bookkeeping (kernel/kiface.c): procfs identity + CPU
    // accounting, umask, and the in-progress state of the blocking nanosleep /
    // poll syscalls. Set up by kiface_proc_init() at creation; fork copies it
    // and kiface_proc_fork() resets the per-instance parts.
    struct proc_ki {
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
    // [M50] Klocks this task holds (kernel/klock.h). Never inherited: fork and
    // every creation path reset it (klock_task_reset).
    klock_task_t klk;
    // [M50-B1] Lifetime: references taken by process_get_by_pid()/process_get()
    // (under proc_lock) and dropped by process_put(). The reaper never frees a
    // zombie whose refs > 0; the next sched_reap_zombies() after the last put
    // does. A process_t* obtained any other way is only valid while the caller
    // is that process, holds proc_lock (process_foreach callbacks), or created
    // it and has not published it yet.
    volatile uint32_t refs;
    // [U1] Linux-shaped thread state (syscalls 512 + Linux nr): the FS base
    // (thread pointer, set by arch_prctl(ARCH_SET_FS); loaded on every switch
    // to the task, inherited by fork, cleared by exec), the set_tid_address
    // pointer (CLONE_CHILD_CLEARTID: zeroed + futex-woken at thread exit) and
    // the user address of the
    // auxv of the current image (process_auxv_set_creds).
    uint64_t fs_base;
    uint64_t clear_child_tid;
    uint64_t auxv_va;
    // [U10] Linux-shaped blocking calls (kernel/syscall_trap.c lx_wait): the
    // call site (user rip) whose timeout lx_deadline (monotonic ns, 0 = none)
    // belongs to; a re-run of the same blocked call keeps it, any completed
    // call clears it. Per thread. (ITIMER_REAL is per process: thread_group_t.)
    uint64_t lx_wait_rip;
    uint64_t lx_deadline;
    // [U9] Threads: set_robust_list head (stored, see docs/devlog/U9-threads.md);
    // a thread killed by its group (exit_group, SIGKILL, execve of another
    // thread) exits with the group's status, not its own.
    uint64_t robust_list;
    uint8_t  thread_killed;
    // [U9] futex wait (kernel/futex.c): on a hash chain while futex_queued;
    // futex_woken is set by the waker, so the re-run returns 0 even when the
    // value did not change. futex_key/futex_bits identify what we wait for.
    struct process* futex_next;
    uint64_t futex_key0, futex_key1;
    uint32_t futex_bits;
    uint8_t  futex_active;      // a FUTEX_WAIT is in progress (re-runs resume it)
    uint8_t  futex_queued;
    volatile uint8_t futex_woken;
    uint64_t futex_deadline;    // absolute monotonic ns, 0 = no timeout
    // [U9] The fd object this syscall works on, referenced for the call when
    // the process has several threads (thread_fd_pin / thread_fd_unpin).
    struct proc_fd_entry fd_pin;
    uint8_t  fd_pin_on;
} process_t;

// [M50-B1] mm_lock helpers (proc_mm_t.lock, shared by the threads of a process).
// [M52] irqsave. No interrupt handler takes it, but procfs, sysfs, the
// umount busy check and ramfs take it under proc_lock (S30 -> S35), and
// the timer tick and the keyboard IRQ take proc_lock: a holder with IF
// on that took the tick while another CPU waited for this mm_lock under
// proc_lock deadlocked both CPUs (seen at -smp 8 once syscalls ran off
// the BKL: CPU A holds mm, its tick spins on proc; CPU B holds proc,
// spins on mm). The cost: the BSP tick waits behind a long fork walk.
static inline uint64_t mm_lock(process_t* p) { return spin_lock_irqsave(&p->mm->lock); }
static inline void mm_unlock(process_t* p, uint64_t fl) { spin_unlock_irqrestore(&p->mm->lock, fl); }
// Let a waiter (procfs, umount) in during a long page-table loop: drop and
// retake the lock. Only for loops whose state does not depend on the lock
// staying held (the owner is the only writer of its own VMAs).
static inline void mm_relax(process_t* p, uint64_t* fl) {
    spin_unlock_irqrestore(&p->mm->lock, *fl);    // interrupts get in here
    *fl = spin_lock_irqsave(&p->mm->lock);
}
#define MM_ASSERT_HELD(p) SPIN_ASSERT_HELD(&(p)->mm->lock)

int process_init_system(void); // initialize process table
process_t* process_create_from_elf(const void* elf_buf, size_t size);
process_t* process_create_from_elf_args(const void* elf_buf, size_t size,
                                        int argc, const char* const argv[]); // [M16]
/* [M51-C6] Takes ownership of the kmalloc'd 'image' (pinned, or freed on failure). */
process_t* process_create_from_elf_owned(uint8_t* image, size_t size,
                                         int argc, const char* const argv[]);
process_t* process_fork(process_t* parent, trapframe_t* tf); // [M19] COW fork
// In-place execve: replace p's user image with the signed ELF 'image' (kmalloc'd,
// ownership transferred; freed on failure). 'blob' holds argc argv strings then
// envc envp strings, each NUL-terminated, 'blen' bytes total. Builds and
// validates the new address space first; on any failure returns -errno with p
// untouched. On success p (== current, 'tf' its live syscall trapframe) resumes
// at the new entry point when the syscall returns. [U1] 'execfn' is the path
// handed to the new image as AT_EXECFN (NULL: argv[0]). Returns 0 / -errno.
int process_exec(process_t* p, trapframe_t* tf, uint8_t* image, size_t size,
                 const char* blob, uint64_t blen, int argc, int envc, const char* execfn);
// [U1] Patch the auxv credential entries (AT_UID..AT_SECURE) of p's freshly
// built initial stack from p->cred (after a spawn's or set-id exec's creds).
void process_auxv_set_creds(process_t* p);
// [U1] The FS base (user thread pointer): MSR_FS_BASE on this CPU.
static inline void arch_set_fs_base(uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(0xC0000100u), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}
void process_print(const process_t* p);
int  process_manifest_setid(const process_t* p);   // [M43] MANIFEST_FLAG_SETID in the signed manifest
// [M50-B1] Process references. process_get_by_pid() returns the process with a
// reference held (NULL if no such pid; zombies are returned too); every
// successful get is paired with process_put(). process_get(p) adds a reference
// to a process the caller already knows is alive (itself, or inside a
// process_foreach callback, which runs under proc_lock). process_get_last()
// returns the highest pid with a reference held.
process_t* process_get_by_pid(uint32_t pid);
process_t* process_get(process_t* p);
void       process_put(process_t* p);
process_t* process_get_last(void);
int        process_pid_state(uint32_t pid);    // state of pid, -1 when gone (no pointer escapes)
// Remove a process that never ran (PROC_NEW, off every CPU, no reference)
// from the table and return it for process_destroy(); NULL otherwise.
process_t* process_detach_unstarted(uint32_t pid);
void process_foreach(void (*cb)(process_t*, void*), void* user);
void process_foreach_locked(void (*cb)(process_t*, void*), void* user);   // proc_lock held
int  process_inode_in_use(const void* ino);   // [M52] an fd or file mapping holds it
process_t* process_reap_one(void); // [M48] detach+return one reapable, off-CPU, unreferenced ZOMBIE
// [M39/M50-B1] wait-any: collect an uncollected zombie child of ppid (marks it
// collected under proc_lock); 1 + *pid/*code when one was found, else 0.
int        process_collect_zombie_child(uint32_t ppid, uint32_t* pid, int* code);
// Job control: consume a stop/continue report of a child of ppid; its pid, or 0.
uint32_t   process_take_jobctl_child(uint32_t ppid, int want_stop, int want_cont, int* status);
int        process_has_children(uint32_t ppid);
// [M50-B1] Credentials: cross-process readers (kill permission, procfs owner)
// read p->cred under proc_lock; the owner writes it under proc_lock.
void       process_set_cred(process_t* p, const struct proc_cred* cr);      // [U9] + its threads
void       process_set_cred_task(process_t* p, const struct proc_cred* cr); // [U9] this task only
// [M48] SMP scheduler support (kernel/process.c).
process_t* process_claim_next(uint32_t cpu, process_t* after); // pick+claim (RUNNING, on_cpu)
uint32_t process_pick_home(void);          // least-loaded online CPU for a new task
int      process_count_runnable(void);     // READY tasks not on any CPU
int      process_count_all(void);          // every task in the table (incl. zombies) + teardowns in flight
void     process_reap_done(void);          // a process_reap_one() zombie has been destroyed
uint64_t process_lock_irqsave(void);       // the process-table lock (for the
void     process_unlock_irqrestore(uint64_t); //  scheduler's block/claim steps)
int      process_set_pin(uint32_t pid, int32_t cpu); // hard affinity; -1 clears
int      process_get_affinity(uint32_t pid, uint64_t* mask, uint32_t nwords); // sched_getaffinity
int process_destroy(process_t* p);
void process_release_pipes(process_t* p); // drop pipe ends + sockets at exit (EOF to peers)
// PID 1: the kernel records /bin/init's pid; exiting processes' children are
// re-parented to it (returns 1 if one of them is already a zombie).
void     process_set_init_pid(uint32_t pid);
uint32_t process_init_pid(void);
int      process_reparent_children(uint32_t dead);

#endif // PROCESS_H
