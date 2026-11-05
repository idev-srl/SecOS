/* SecOS BusyBox overlay: <signal.h> = SecOS libc signals + POSIX sigaction /
 * sigset_t (shims in user/busybox_compat.c). SPDX-License-Identifier: MIT */
#ifndef _BB_SIGNAL_H
#define _BB_SIGNAL_H
#include <sys/types.h>
#include_next <signal.h>
#include <time.h>
#define SIGIOT    6
#define SIGSTKFLT 16
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   SIGIO
#define SIGPWR    30
#define SIGSYS    31
#define SIGUNUSED SIGSYS
#define _NSIG     NSIG
typedef unsigned long sigset_t;
union sigval { int sival_int; void* sival_ptr; };
typedef struct {
    int si_signo, si_errno, si_code;
    pid_t si_pid; uid_t si_uid; int si_status;
    void* si_addr; union sigval si_value;
} siginfo_t;
struct sigaction {
    union { void (*sa_handler)(int); void (*sa_sigaction)(int, siginfo_t*, void*); } __sa;
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};
#define sa_handler   __sa.sa_handler
#define sa_sigaction __sa.sa_sigaction
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_ONESHOT   SA_RESETHAND
#define SA_NOMASK    SA_NODEFER
typedef struct { void* ss_sp; int ss_flags; size_t ss_size; } stack_t;
int  sigaction(int sig, const struct sigaction* act, struct sigaction* old);
int  sigemptyset(sigset_t* s);
int  sigfillset(sigset_t* s);
int  sigaddset(sigset_t* s, int n);
int  sigdelset(sigset_t* s, int n);
int  sigismember(const sigset_t* s, int n);
int  sigpending(sigset_t* s);
int  sigwait(const sigset_t* s, int* sig);
int  sigtimedwait(const sigset_t* s, siginfo_t* i, const struct timespec* t);
int  siginterrupt(int sig, int flag);
int  sigaltstack(const stack_t* ss, stack_t* old);
void psignal(int sig, const char* s);
int  sigqueue(pid_t pid, int sig, const union sigval v);
sighandler_t sbb_signal(int sig, sighandler_t h);
sighandler_t bsd_signal(int sig, sighandler_t h);
sighandler_t sysv_signal(int sig, sighandler_t h);
#endif
