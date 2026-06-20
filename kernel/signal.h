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

#define NSIG 32                 /* signals 1..31 ; bit n in the masks = signal n */

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
typedef struct sigframe {
    uint64_t   magic;
    uint64_t   saved_mask;      /* sig_blocked to restore */
    trapframe_t saved;          /* full interrupted ring-3 context */
} sigframe_t;

void signal_init_proc(struct process* p);       /* zero handlers/masks at create */
void signal_dispatch(trapframe_t* tf);          /* deliver at return-to-ring3 */
int  signal_post(struct process* p, int sig);   /* post one signal to a process */
int  signal_post_pgid(uint32_t pgid, int sig);  /* post to a whole process group */
int  signal_pending(struct process* p);         /* a deliverable signal is pending? */

/* Syscall backends (args already validated/copied by the trap layer). */
long ksys_sigaction(int sig, uint64_t handler, uint64_t restorer);
long ksys_sigreturn(trapframe_t* tf);           /* restores tf in place; returns saved rax */
long ksys_kill(int pid, int sig);
long ksys_sigprocmask(int how, uint64_t set, uint64_t* oldset_user);

/* Foreground process group for the console (job control). 0 = none (shell). */
void     signal_set_foreground_pgid(uint32_t pgid);
uint32_t signal_get_foreground_pgid(void);

#endif /* SIGNAL_H */
