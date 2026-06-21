/* <signal.h> — SecOS libc. SPDX-License-Identifier: MIT
 * [M30] POSIX-style signals over the custom syscall ABI. The function prototypes
 * (signal/kill/raise/sigprocmask/setpgid + sighandler_t) live in libsecos.h. */
#ifndef _SIGNAL_H
#define _SIGNAL_H
#include "libsecos.h"

typedef int sig_atomic_t;

/* Signal numbers (Linux-compatible where it matters for source ports). */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22

/* Handler dispositions (must match the kernel's signal.h). */
#define SIG_DFL  ((sighandler_t)0)
#define SIG_IGN  ((sighandler_t)1)
#define SIG_ERR  ((sighandler_t)-1)

/* sigprocmask 'how'. */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#endif /* _SIGNAL_H */
