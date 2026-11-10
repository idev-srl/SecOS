/*
 * SecOS Kernel - POSIX-style signals (Header)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M30] Asynchronous signal delivery for ring-3 processes. Signals are posted
 * into a per-process pending bitmask (signal_post) and delivered when the
 * process is about to return to ring-3 (signal_dispatch, called from the
 * syscall / timer / exception return paths). A custom handler runs on the user
 * stack via a libc-provided trampoline (sa_restorer) that calls SYS_SIGRETURN
 * to restore the interrupted context — no executable stack (W^X-clean).
 */
#ifndef SIGNAL_H
#define SIGNAL_H
#include <stdint.h>
#include "trapframe.h"

#define NSIG 64                 /* [U9] signals 1..63 ; bit n in the masks = signal n
                                   (Linux: 1..64; SIGRTMAX = 63 here) */

/* POSIX signal numbers (Linux-compatible where it matters for ports). */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9              /* uncatchable: always terminates */
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17             /* default: ignore */
#define SIGCONT  18             /* default: resume a stopped process */
#define SIGSTOP  19             /* uncatchable: always stops */
#define SIGTSTP  20             /* default: stop (Ctrl-Z) */
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23             /* default: ignore */
#define SIGWINCH 28             /* default: ignore (terminal resized) */
#define SIGRTMIN_ 32            /* real-time signals 32..63: default terminate
                                   (musl uses 32 timers, 33 SIGCANCEL, 34 SIGSYNCCALL) */

/* Handler sentinels (must match user/include/signal.h). */
#define SIG_DFL  0UL
#define SIG_IGN  1UL

/* sigprocmask 'how'. */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SIGBIT(n)   (1ULL << (n))
/* Signals that can never be blocked or caught. */
#define SIG_UNMASKABLE (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))

struct process;

/* Saved on the user stack when entering a custom handler; SYS_SIGRETURN reads it
 * back to restore the interrupted context exactly. */
#define SIGFRAME_MAGIC 0x5347465253454330ULL /* "SGFRSEC0" */
/* [U9] Linux x86-64 siginfo_t (128 bytes) and ucontext_t, as musl declares
 * them: an SA_SIGINFO handler gets pointers to these (rsi, rdx), and what it
 * writes into uc_mcontext.gregs / uc_sigmask is what sigreturn restores
 * (musl's thread cancellation moves the interrupted pc). */
typedef struct lx_siginfo {
    int32_t  si_signo, si_errno, si_code, pad0;
    int32_t  si_pid;  uint32_t si_uid;      /* kill() / tkill() sender */
    uint8_t  rest[128 - 24];
} lx_siginfo_t;
enum { LX_REG_R8 = 0, LX_REG_R9, LX_REG_R10, LX_REG_R11, LX_REG_R12, LX_REG_R13, LX_REG_R14,
       LX_REG_R15, LX_REG_RDI, LX_REG_RSI, LX_REG_RBP, LX_REG_RBX, LX_REG_RDX, LX_REG_RAX,
       LX_REG_RCX, LX_REG_RSP, LX_REG_RIP, LX_REG_EFL, LX_REG_CSGSFS, LX_REG_ERR,
       LX_REG_TRAPNO, LX_REG_OLDMASK, LX_REG_CR2, LX_NGREG };
typedef struct lx_ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    uint64_t ss_sp; int32_t ss_flags; int32_t pad1; uint64_t ss_size;   /* uc_stack */
    uint64_t gregs[LX_NGREG];               /* uc_mcontext */
    uint64_t fpregs;                        /*  -> fpregs_mem */
    uint64_t mc_reserved[8];
    uint64_t uc_sigmask[16];                /* Linux sigset_t: bit n-1 = signal n */
    uint8_t  fpregs_mem[512] __attribute__((aligned(16)));
} lx_ucontext_t;

typedef struct sigframe {
    uint64_t   magic;
    uint64_t   saved_mask;      /* sig_blocked to restore */
    trapframe_t saved;          /* full interrupted ring-3 context */
    /* [M48] The interrupted x87/SSE register file (fxsave image): a handler
     * that uses SSE (printf of a double, vectorised memcpy) must not clobber
     * the interrupted code's registers. sigreturn restores it (MXCSR masked). */
    uint8_t    fpu[512] __attribute__((aligned(16)));
    /* [U9] The Linux view of the same context (SA_SIGINFO). sigreturn takes
     * the registers, the mask and the FPU image from uc: a handler that does
     * not touch it gets exactly the values above back. */
    lx_siginfo_t si;
    lx_ucontext_t uc;
} sigframe_t;

void signal_init_proc(struct process* p);       /* zero handlers/masks at create */
void signal_dispatch(trapframe_t* tf);          /* deliver at return-to-ring3 */
int  signal_post(struct process* p, int sig);   /* post one signal to a process */
int  signal_post_thread(struct process* p, int sig);  /* [U9] to one thread (tgkill) */
int  signal_post_pgid(uint32_t pgid, int sig);  /* post to a whole process group */
int  signal_pending(struct process* p);         /* a deliverable signal is pending? */
int  signal_take_stop(struct process* p);       /* consume a pending default-stop signal -> signo / 0 */
int  signal_ttin_acts(struct process* p);       /* SIGTTIN neither blocked nor ignored? */
/* [U9] The thread was stopped by its group's stop (sched_stop_current). */
void signal_group_stopped(struct process* p);

/* Syscall backends (args already validated/copied by the trap layer). */
long ksys_sigaction(int sig, uint64_t handler, uint64_t restorer);
long ksys_sigreturn(trapframe_t* tf);           /* restores tf in place; returns saved rax */
long ksys_kill(int pid, int sig);
long ksys_sigprocmask(int how, uint64_t set, uint64_t* oldset_user);
long ksys_getpgid(int pid);                     /* pid 0 = caller; -ESRCH */

/* Foreground process group for the console (job control). 0 = none (shell). */
void     signal_set_foreground_pgid(uint32_t pgid);
uint32_t signal_get_foreground_pgid(void);

#endif /* SIGNAL_H */
