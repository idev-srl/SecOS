/*
 * SecOS Kernel - POSIX-style signals
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M30] See signal.h. Posting (signal_post) only sets a pending bit and, if the
 * target is blocked in a syscall, wakes it (so the syscall returns EINTR and the
 * signal can then be delivered). Delivery (signal_dispatch) happens at the
 * return-to-ring3 chokepoints in the syscall / timer / exception asm stubs.
 */
#include "signal.h"
#include "process.h"
#include "sched.h"
#include "debugcon.h"
#include "../mm/user_copy.h"
#include "cred.h"
#include "klock.h"
#define INT32_MIN_ (-2147483647 - 1)

extern process_t* sched_get_current(void);
extern void process_foreach(void (*cb)(process_t*, void*), void* user);
extern void sched_kill_current(int sig) __attribute__((noreturn));
extern void sched_stop_current(trapframe_t* tf) __attribute__((noreturn));

// ---- foreground process group (console job control) ----
// [M51] Written under the console line-discipline lock (kernel/tty.c,
// tty_con_lock S10), so it never changes in the middle of a console ldisc
// operation; never set it from under that lock or from IRQ context. Read
// lock-free: the keyboard IRQ and the USB HID poll (which hold kbd_lock S50 /
// hid_lock S12, inside the console lock's level) and procfs (S15) only need a
// consistent 32-bit value, and the console ldisc reads it under its lock.
extern uint64_t tty_con_lock_irqsave(void);
extern void     tty_con_unlock_irqrestore(uint64_t fl);
static volatile uint32_t g_foreground_pgid = 0;
void signal_set_foreground_pgid(uint32_t pgid) {
    uint64_t fl = tty_con_lock_irqsave();
    __atomic_store_n(&g_foreground_pgid, pgid, __ATOMIC_RELEASE);
    tty_con_unlock_irqrestore(fl);
}
uint32_t signal_get_foreground_pgid(void) { return __atomic_load_n(&g_foreground_pgid, __ATOMIC_ACQUIRE); }

// ---- disposition helpers ----
static int sig_default_ignore(int s) { return s == SIGCHLD || s == SIGCONT || s == SIGURG || s == SIGWINCH; }
static int sig_default_stop(int s) {
    return s == SIGSTOP || s == SIGTSTP || s == SIGTTIN || s == SIGTTOU;
}

void signal_init_proc(process_t* p) {
    if (!p) return;
    p->sig_pending = 0;
    p->sig_blocked = 0;
    for (int i = 0; i < NSIG; i++) p->sig_handler[i] = SIG_DFL;
    p->sig_restorer = 0;
}

// Pending signals whose delivery would do nothing: explicitly ignored, or
// default-ignore (SIGCHLD/SIGCONT with SIG_DFL). They must not interrupt a
// blocking syscall (POSIX), e.g. the SIGCONT that resumes a stopped job.
static uint64_t sig_noop_mask(process_t* p) {
    uint64_t m = 0;
    for (int s = 1; s < NSIG; s++) {
        uint64_t d = p->sig_handler[s];
        if (s == SIGKILL || s == SIGSTOP) continue;
        if (d == SIG_IGN || (d == SIG_DFL && sig_default_ignore(s))) m |= SIGBIT(s);
    }
    return m;
}

// Is a non-blocked (or unmaskable) signal pending whose delivery has an effect?
// Used by blocking syscalls to return EINTR instead of sleeping.
int signal_pending(process_t* p) {
    if (!p) return 0;
    uint64_t deliverable = (p->sig_pending & ~p->sig_blocked) |
                           (p->sig_pending & SIG_UNMASKABLE);
    deliverable &= ~sig_noop_mask(p);
    return deliverable != 0 || p->sig_rerun_eintr;
}

// Job control: if a deliverable signal whose action is "stop" is pending
// (SIGSTOP, or SIGTSTP/SIGTTIN/SIGTTOU at SIG_DFL), consume it and return its
// number, else 0. A blocking syscall uses this to stop the caller with the
// syscall rewound, so it transparently restarts after SIGCONT instead of
// failing with EINTR (Linux restarts syscalls interrupted by a stop).
int signal_take_stop(process_t* p) {
    if (!p) return 0;
    uint64_t deliverable = (p->sig_pending & ~p->sig_blocked) |
                           (p->sig_pending & SIG_UNMASKABLE);
    if (deliverable & SIGBIT(SIGKILL)) return 0;    // dying: never stop again
    for (int s = 1; s < NSIG; s++) {
        if (!((deliverable >> s) & 1)) continue;
        if (s == SIGSTOP || (sig_default_stop(s) && p->sig_handler[s] == SIG_DFL)) {
            __atomic_and_fetch(&p->sig_pending, ~SIGBIT(s), __ATOMIC_SEQ_CST);   // [M48] other CPUs post concurrently
            return s;
        }
    }
    return 0;
}

// Would SIGTTIN be acted on (neither blocked nor ignored)? A background reader
// of the TTY whose SIGTTIN is blocked/ignored gets EIO instead (POSIX).
int signal_ttin_acts(process_t* p) {
    if (!p) return 0;
    if ((p->sig_blocked >> SIGTTIN) & 1) return 0;
    return p->sig_handler[SIGTTIN] != SIG_IGN;
}

// Post one signal to a process. If it is blocked in a syscall and the signal is
// deliverable, wake it (clearing the wait condition) so the syscall re-runs,
// observes the pending signal and returns EINTR. SIGCONT resumes a stopped task.
int signal_post(process_t* p, int sig) {
    if (!p || sig <= 0 || sig >= NSIG) return -1;
    if (p->state == PROC_ZOMBIE) return -1;

    // SIGCONT cancels pending stops and resumes a stopped process.
    if (sig == SIGCONT) {
        __atomic_and_fetch(&p->sig_pending, ~(SIGBIT(SIGSTOP) | SIGBIT(SIGTSTP) |
                            SIGBIT(SIGTTIN) | SIGBIT(SIGTTOU)), __ATOMIC_SEQ_CST);
        if (p->state == PROC_STOPPED && sched_wake_task(p, PROC_STOPPED)) {
            // waitpid(WCONTINUED) report + SIGCHLD to the parent (POSIX).
            p->stop_report = 0; p->cont_report = 1;
            extern void sched_notify_parent_cont(process_t*);
            sched_notify_parent_cont(p);
        }
    }
    // SIGKILL also ends a stopped process (POSIX): make it runnable so the
    // pending SIGKILL is acted on at its next kernel exit.
    if (sig == SIGKILL && p->state == PROC_STOPPED && sched_wake_task(p, PROC_STOPPED)) p->stop_report = 0;
    // A stop signal cancels a pending SIGCONT.
    if (sig_default_stop(sig)) __atomic_and_fetch(&p->sig_pending, ~SIGBIT(SIGCONT), __ATOMIC_SEQ_CST);

    __atomic_or_fetch(&p->sig_pending, SIGBIT(sig), __ATOMIC_SEQ_CST);   // [M48] atomic: ISRs and other CPUs post too
    sched_wake_note();      // [M48] a task deciding to block right now re-checks

    int deliverable = (sig == SIGKILL || sig == SIGSTOP) ||
                      !((p->sig_blocked >> sig) & 1);
    // A signal whose delivery is a no-op (ignored / default-ignore) must not
    // knock a blocked task out of its syscall.
    if (deliverable && (sig_noop_mask(p) & SIGBIT(sig))) deliverable = 0;
    if (deliverable && p->state == PROC_BLOCKED) {
        // Tear down the wait condition so the woken task does not re-block before
        // it gets a chance to see the signal. SIGKILL needs no syscall re-run, but
        // making it READY lets the scheduler dispatch (and then kill) it.
        // sleep_until is kept: a stop restarts SYS_SLEEP with its original
        // deadline (the EINTR path clears it itself).
        p->wait_pid = -1; p->recv_chan = -1; p->wait_pipe = NULL; p->wait_tty = 0;
        p->wait_pty = NULL;
        if (sig != SIGKILL && p->sig_handler[sig] > 1 /* caught */ && p->tf)
            p->sig_eintr_rip = p->tf->rip;       // the rewound `int 0x80`
        sched_wake_task(p, PROC_BLOCKED);
    }
    return 0;
}

// Collect the group under proc_lock, post outside it: signal_post may look up
// the parent (SIGCONT -> SIGCHLD), which takes proc_lock again. [M50-B1] Each
// collected target is held by a reference (taken under proc_lock, inside the
// callback) until it has been posted, so the reaper cannot free it meanwhile.
#define PG_MAX 64
// sender != NULL: kill(2) from a process -- only targets it may signal are
// collected (kernel/cred.c: cred_may_signal); NULL = kernel-originated (Ctrl-C).
struct pg_ctx { uint32_t pgid; int n; process_t* t[PG_MAX]; uint32_t skip; int all; process_t* sender; int sig; int denied; };
static void pg_cb(process_t* p, void* u) {
    struct pg_ctx* c = (struct pg_ctx*)u;
    if (p->state == PROC_ZOMBIE || c->n >= PG_MAX) return;
    if (!(c->all ? (p->pid != 0 && p->pid != c->skip) : (p->pgid == c->pgid))) return;
    if (c->sender && !cred_may_signal(c->sender, p, c->sig)) {   // [M43]
        c->denied++;
        return;
    }
    if (c->all && c->sender && p->pid == 1 && !cred_is_root(c->sender)) return;
    c->t[c->n++] = process_get(p);
}
static void put_collected(struct pg_ctx* c) {
    for (int i = 0; i < c->n; i++) process_put(c->t[i]);
    c->n = 0;
}
static int post_collected(struct pg_ctx* c, int sig) {
    int n = 0;
    for (int i = 0; i < c->n; i++) if (signal_post(c->t[i], sig) == 0) n++;
    put_collected(c);
    return n;
}
int signal_post_pgid(uint32_t pgid, int sig) {
    if (pgid == 0) return 0;
    struct pg_ctx c; c.pgid = pgid; c.n = 0; c.skip = 0; c.all = 0; c.sender = NULL; c.sig = sig; c.denied = 0;
    process_foreach(pg_cb, &c);
    return post_collected(&c, sig);
}
// kill(-1, sig): every user process except the caller (POSIX "all processes
// the caller may signal"; single-user system).
static int signal_post_all(uint32_t except, int sig) {
    struct pg_ctx c; c.pgid = 0; c.n = 0; c.skip = except; c.all = 1; c.sender = NULL; c.sig = sig; c.denied = 0;
    process_foreach(pg_cb, &c);
    return post_collected(&c, sig);
}
// [M43] kill(-1 / -pgid) from `sender`: post to the targets it may signal.
// Returns the number signalled; *denied counts the ones refused (EPERM when
// nothing could be signalled but something matched).
static int signal_post_checked(process_t* sender, uint32_t pgid, int all, int sig, int* denied) {
    struct pg_ctx c; c.pgid = pgid; c.n = 0; c.skip = sender ? sender->pid : 0; c.all = all;
    c.sender = sender; c.sig = sig; c.denied = 0;
    if (!all) c.skip = 0;
    process_foreach(pg_cb, &c);
    *denied = c.denied;
    if (sig == 0) { int n = c.n; put_collected(&c); return n; }   // existence/permission probe only
    return post_collected(&c, sig);
}

// ---- delivery: called from the asm return-to-ring3 chokepoints ----
// Sets up a custom handler frame on the user stack (returns, asm iretq's into the
// handler) or performs the default action (terminate/stop/ignore). The terminate
// and stop paths NEVER return (they switch to another task), abandoning the asm
// stub mid-restore — exactly like a timer preemption.
void signal_dispatch(trapframe_t* tf) {
    if (!tf) return;
    if ((tf->cs & 3) != 3) return;              // only deliver returning to ring-3
#ifdef SECOS_LOCKDEP
    klock_check_user_return("iret to ring 3");  // [M50] nothing held going back to user
#endif
    process_t* cur = sched_get_current();
    if (!cur) return;

    for (;;) {
        uint64_t deliverable = (cur->sig_pending & ~cur->sig_blocked) |
                               (cur->sig_pending & SIG_UNMASKABLE);
        int sig = 1; while (sig < NSIG && !((deliverable >> sig) & 1)) sig++;
        if (!deliverable || sig >= NSIG) {
            // sigsuspend() returned without a handler frame taking over its
            // saved mask (only ignored signals): restore the mask now.
            if (cur->sigsusp_active) { cur->sig_blocked = cur->sigsusp_saved; cur->sigsusp_active = 0; }
            return;
        }
        __atomic_and_fetch(&cur->sig_pending, ~SIGBIT(sig), __ATOMIC_SEQ_CST);

        uint64_t disp = cur->sig_handler[sig];

        // SIGKILL is never catchable.
        if (sig == SIGKILL) sched_kill_current(sig);          // NORETURN
        // SIGSTOP is never catchable.
        if (sig == SIGSTOP) { cur->stop_sig = sig; sched_stop_current(tf); }  // NORETURN

        if (disp == SIG_IGN) continue;
        if (disp == SIG_DFL) {
            if (sig_default_ignore(sig)) continue;
            if (sig_default_stop(sig)) { cur->stop_sig = sig; sched_stop_current(tf); } // NORETURN
            sched_kill_current(sig);                                // NORETURN (term)
        }

        // ---- custom handler: build a sigframe on the user stack ----
        sigframe_t sf;
        sf.magic = SIGFRAME_MAGIC;
        sf.saved_mask = cur->sig_blocked;
        if (cur->sigsusp_active) {               // handler interrupts sigsuspend():
            sf.saved_mask = cur->sigsusp_saved;  // sigreturn restores the pre-suspend mask
            cur->sigsusp_active = 0;
        }
        sf.saved = *tf;                          // full interrupted context
        __asm__ volatile("fxsave64 %0" : "=m"(*(uint8_t(*)[512])sf.fpu));   // [M48] live user FPU

        uint64_t usp = tf->rsp;
        usp -= 128;                              // skip the SysV red zone
        uint64_t frame = (usp - sizeof(sigframe_t)) & ~15ULL;
        if (copy_to_user((void*)frame, &sf, sizeof(sf)) != 0) {
            // Cannot deliver safely (bad stack) -> fall back to terminate.
            sched_kill_current(sig);             // NORETURN
        }
        uint64_t ret_slot = frame - 8;           // handler returns here -> restorer
        if (copy_to_user((void*)ret_slot, &cur->sig_restorer, 8) != 0)
            sched_kill_current(sig);             // NORETURN

        // Mask this signal while its handler runs (POSIX), then enter the handler.
        cur->sig_blocked |= SIGBIT(sig);
        tf->rip = disp;                          // handler entry
        tf->rsp = ret_slot;                      // rsp%16==8 at entry (ABI)
        tf->rdi = (uint64_t)sig;                 // first arg: signum
        tf->rax = 0;
        tf->rflags &= ~0x400ULL;                 // DF=0 at handler entry (SysV ABI;
                                                 // sigreturn restores the saved DF)
        debugcon_writestring("[SIG] deliver sig=");
        debugcon_print_hex((uint64_t)sig);
        debugcon_writestring(" pid=");
        debugcon_print_hex(cur->pid);
        debugcon_writestring(" handler\n");
        return;                                  // asm iretq's into the handler
    }
}

// ---- syscall backends ----
long ksys_sigaction(int sig, uint64_t handler, uint64_t restorer) {
    process_t* cur = sched_get_current();
    if (!cur || sig <= 0 || sig >= NSIG) return -1;
    if (sig == SIGKILL || sig == SIGSTOP) return -1;   // cannot be caught/ignored
    cur->sig_handler[sig] = handler;                    // SIG_DFL/SIG_IGN or VA
    if (restorer) cur->sig_restorer = restorer;
    return 0;
}

long ksys_sigreturn(trapframe_t* tf) {
    process_t* cur = sched_get_current();
    if (!cur) return -1;
    sigframe_t sf;
    if (copy_from_user(&sf, (void*)tf->rsp, sizeof(sf)) != 0) return -1;
    if (sf.magic != SIGFRAME_MAGIC) { sched_kill_current(SIGSEGV); } // tampered -> kill
    // Only restore the user-controlled parts; force ring-3 selectors and IF so a
    // forged frame cannot escalate privilege or run with interrupts disabled.
    trapframe_t* s = &sf.saved;
    s->cs = 0x1B; s->ss = 0x23;
    s->rflags = (s->rflags & ~0x200ULL) | 0x202ULL;     // keep IF set, sane bits
    if ((s->rsp >= 0x0000800000000000ULL)) return -1;   // user range only
    cur->sig_blocked = sf.saved_mask;
    {   // [M48] Restore the interrupted FPU/SSE state. Reserved MXCSR bits would
        // make fxrstor #GP in the kernel: mask them with the CPU's MXCSR_MASK.
        static uint32_t mxcsr_mask;
        if (!mxcsr_mask) {
            static uint8_t probe[512] __attribute__((aligned(16)));
            __asm__ volatile("fxsave64 %0" : "=m"(probe));
            uint32_t m = *(uint32_t*)(probe + 28);
            mxcsr_mask = m ? m : 0xFFBFu;
        }
        *(uint32_t*)(sf.fpu + 24) &= mxcsr_mask;
        __asm__ volatile("fxrstor64 %0" : : "m"(*(const uint8_t(*)[512])sf.fpu));
    }
    *tf = *s;                                            // restore in place
    return (long)s->rax;                                // preserve interrupted rax
}

// POSIX kill(): pid > 0 one process, 0 the caller's group, -1 everyone else,
// < -1 the process group -pid (killpg; bash's `kill %job`). Errors: -EINVAL(22)
// for a bad signal, -ESRCH(3) when nothing matched.
struct pg_exists { uint32_t g; int n; };
static void pg_exists_cb(process_t* p, void* u) {
    struct pg_exists* e = (struct pg_exists*)u;
    if (p->pgid == e->g && p->state != PROC_ZOMBIE) e->n++;
}
// Does a live process group `pgid` exist? (TIOCSPGRP validation)
int signal_pgid_exists(uint32_t pgid) {
    struct pg_exists e = { pgid, 0 };
    process_foreach(pg_exists_cb, &e);
    return e.n > 0;
}
long ksys_kill(int pid, int sig) {
    if (sig < 0 || sig >= NSIG) return -22;
    process_t* cur = sched_get_current();
    // [M43] Credential rules (kernel/cred.c): root, or the sender's real or
    // effective uid matches the target's real or saved uid (SIGCONT also within
    // the session). -EPERM(1) when every match was refused.
    if (pid <= 0) {
        if (!cur) return -3;
        if (pid == INT32_MIN_) return -3;
        uint32_t g = (pid == 0) ? cur->pgid : (uint32_t)(-pid);
        int denied = 0;
        int n = signal_post_checked(cur, pid == -1 ? 0 : g, pid == -1, sig, &denied);
        if (n > 0) return 0;
        return denied ? -1 : -3;
    }
    process_t* t = process_get_by_pid((uint32_t)pid);   // [M50-B1] held until posted
    if (!t) return -3;
    // The target's credentials are read under proc_lock (process_set_cred).
    uint64_t fl = process_lock_irqsave();
    int zombie = (t->state == PROC_ZOMBIE);
    int may = zombie || cred_may_signal(cur, t, sig);
    process_unlock_irqrestore(fl);
    long r;
    if (!may) r = -1;                                    // -EPERM
    else if (sig == 0 || zombie) r = 0;                  // exists (a zombie until reaped)
    else r = signal_post(t, sig) == 0 ? 0 : -3;
    process_put(t);
    return r;
}

// [M30] setpgid(pid, pgid): pid 0 = caller; pgid 0 = the target's own pid (make
// it a group leader). Minimal model — no session/leader checks (the signature is
// the trust boundary in SecOS), just enough for the shell to group a pipeline.
long ksys_setpgid(int pid, int pgid) {
    process_t* cur = sched_get_current();
    if (!cur) return -1;
    process_t* t = (pid == 0) ? process_get(cur) : process_get_by_pid((uint32_t)pid);
    if (!t) return -3;                                   // -ESRCH
    long r = 0;
    // [M43] POSIX: only the caller itself or one of its children (fork or
    // spawn), in the caller's session; a root caller may move anything (the
    // kernel shell builds pipelines that way).
    if (t != cur && !cred_is_root(cur) &&
        !((t->ppid == cur->pid || t->spawner_pid == cur->pid) && t->sid == cur->sid)) r = -3;
    else if (pgid < 0) r = -22;                          // -EINVAL
    else t->pgid = (pgid == 0) ? t->pid : (uint32_t)pgid;
    process_put(t);
    return r;
}

long ksys_sigprocmask(int how, uint64_t set, uint64_t* oldset_user) {
    process_t* cur = sched_get_current();
    if (!cur) return -1;
    uint64_t old = cur->sig_blocked;
    switch (how) {
        case SIG_BLOCK:   cur->sig_blocked |= set; break;
        case SIG_UNBLOCK: cur->sig_blocked &= ~set; break;
        case SIG_SETMASK: cur->sig_blocked = set; break;
        default: return -1;
    }
    cur->sig_blocked &= ~SIG_UNMASKABLE;                 // KILL/STOP never blockable
    if (oldset_user) { if (copy_to_user(oldset_user, &old, sizeof(old)) != 0) return -1; }
    return 0;
}

// getpgid(pid): pid 0 = caller. -ESRCH(3) for an unknown pid.
long ksys_getpgid(int pid) {
    process_t* cur = sched_get_current();
    if (pid == 0) return cur ? (long)cur->pgid : -3;
    process_t* t = process_get_by_pid((uint32_t)pid);
    if (!t) return -3;
    long g = (long)t->pgid;
    process_put(t);
    return g;
}
