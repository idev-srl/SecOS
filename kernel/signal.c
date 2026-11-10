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
#include "thread.h"   // [U9]
#include "heap.h"     // [U9] the signal frame is too big for the stack
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
    p->tg->restorer = 0;
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

// [U9] Thread-directed pending | the group's process-directed pending.
static inline uint64_t sig_shared(process_t* p) {
    return p->tg ? __atomic_load_n(&p->tg->shared_pending, __ATOMIC_SEQ_CST) : 0;
}
static inline uint64_t sig_all_pending(process_t* p) {
    return __atomic_load_n(&p->sig_pending, __ATOMIC_SEQ_CST) | sig_shared(p);
}
static inline int multi(process_t* p) { return thread_group_live(p) > 1; }
// Take signal `sig` off p's own pending set, else off its group's. 1 if this
// call removed it (another thread may race us for a process-directed one).
static int sig_dequeue(process_t* p, int sig) {
    uint64_t bit = SIGBIT(sig);
    if (__atomic_fetch_and(&p->sig_pending, ~bit, __ATOMIC_SEQ_CST) & bit) return 1;
    if (p->tg && (__atomic_fetch_and(&p->tg->shared_pending, ~bit, __ATOMIC_SEQ_CST) & bit)) return 1;
    return 0;
}

// Is a non-blocked (or unmaskable) signal pending whose delivery has an effect?
// Used by blocking syscalls to return EINTR instead of sleeping. [U9] A group
// stop or a group kill in progress counts too (the thread must get to it).
int signal_pending(process_t* p) {
    if (!p) return 0;
    uint64_t pend = sig_all_pending(p);
    uint64_t deliverable = (pend & ~p->sig_blocked) | (pend & SIG_UNMASKABLE);
    deliverable &= ~sig_noop_mask(p);
    if (p->tg && (p->tg->stop_active || p->thread_killed)) return 1;
    return deliverable != 0 || p->sig_rerun_eintr;
}

static void group_stop_start(process_t* cur, int sig);

// Job control: if a deliverable signal whose action is "stop" is pending
// (SIGSTOP, or SIGTSTP/SIGTTIN/SIGTTOU at SIG_DFL), consume it and return its
// number, else 0. A blocking syscall uses this to stop the caller with the
// syscall rewound, so it transparently restarts after SIGCONT instead of
// failing with EINTR (Linux restarts syscalls interrupted by a stop).
// [U9] In a multi-threaded process acting on it starts a group stop, and a
// thread of a group being stopped joins it (nothing to consume).
int signal_take_stop(process_t* p) {
    if (!p) return 0;
    if (p->thread_killed) return 0;                  // dying: never stop again
    if (p->tg && p->tg->stop_active && multi(p)) return p->tg->stop_sig ? p->tg->stop_sig : SIGSTOP;
    for (;;) {
        uint64_t pend = sig_all_pending(p);
        uint64_t deliverable = (pend & ~p->sig_blocked) | (pend & SIG_UNMASKABLE);
        if (deliverable & SIGBIT(SIGKILL)) return 0;    // dying: never stop again
        int s = 1;
        for (; s < NSIG; s++) {
            if (!((deliverable >> s) & 1)) continue;
            if (s == SIGSTOP || (sig_default_stop(s) && p->sig_handler[s] == SIG_DFL)) break;
        }
        if (s >= NSIG) return 0;
        if (!sig_dequeue(p, s)) continue;            // another thread took it: look again
        if (multi(p)) group_stop_start(p, s);
        return s;
    }
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
// Tear down a BLOCKED task's wait condition and make it runnable so its
// syscall re-runs, sees the signal and returns EINTR (or stops / dies).
static void sig_kick(process_t* p, int sig) {
    if (p->state != PROC_BLOCKED) return;
    // Tear down the wait condition so the woken task does not re-block before
    // it gets a chance to see the signal. SIGKILL needs no syscall re-run, but
    // making it READY lets the scheduler dispatch (and then kill) it.
    // sleep_until is kept: a stop restarts SYS_SLEEP with its original
    // deadline (the EINTR path clears it itself).
    p->wait_pid = -1; p->recv_chan = -1; p->wait_pipe = NULL; p->wait_tty = 0;
    p->wait_pty = NULL;
    if (sig > 0 && sig != SIGKILL && p->sig_handler[sig] > 1 /* caught */ && p->tf)
        p->sig_eintr_rip = p->tf->rip;       // the rewound `int 0x80`
    sched_wake_task(p, PROC_BLOCKED);
}

int signal_post_thread(process_t* p, int sig) {
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
    // [M52] A stop that raced the checks above: the target dequeued its stop
    // signal while RUNNING, we saw RUNNING, then it went STOPPED. It looks at
    // its pending set after storing STOPPED (sched_stop_current) and we look
    // at its state after setting the bit, so one of the two sees the other
    // and the CAS lets only one of them resume it.
    if ((sig == SIGCONT || sig == SIGKILL) && p->state == PROC_STOPPED &&
        sched_wake_task(p, PROC_STOPPED)) {
        p->stop_report = 0;
        if (sig == SIGCONT) {
            p->cont_report = 1;
            extern void sched_notify_parent_cont(process_t*);
            sched_notify_parent_cont(p);
        }
    }

    int deliverable = (sig == SIGKILL || sig == SIGSTOP) ||
                      !((p->sig_blocked >> sig) & 1);
    // A signal whose delivery is a no-op (ignored / default-ignore) must not
    // knock a blocked task out of its syscall.
    if (deliverable && (sig_noop_mask(p) & SIGBIT(sig))) deliverable = 0;
    if (deliverable && p->state == PROC_BLOCKED) sig_kick(p, sig);
    return 0;
}

// ---- [U9] process-directed signals in a multi-threaded process ----
#define TG_MAX 64
struct tg_ctx { thread_group_t* tg; int n; process_t* t[TG_MAX]; int more; };
static void tg_cb(process_t* p, void* u) {
    struct tg_ctx* c = (struct tg_ctx*)u;
    if (p->tg != c->tg || p->state == PROC_ZOMBIE) return;
    if (c->n >= TG_MAX) { c->more = 1; return; }
    c->t[c->n++] = process_get(p);
}
static void tg_put_all(struct tg_ctx* c) { for (int i = 0; i < c->n; i++) process_put(c->t[i]); c->n = 0; }
static process_t* tg_leader_locked(thread_group_t* tg, uint32_t tgid);

// SIGCONT for the whole group: end a group stop (every stopped thread runs
// again) and, if the group had stopped completely, report "continued".
static void group_cont(process_t* p) {
    thread_group_t* tg = p->tg;
    const uint64_t stops = SIGBIT(SIGSTOP) | SIGBIT(SIGTSTP) | SIGBIT(SIGTTIN) | SIGBIT(SIGTTOU);
    __atomic_and_fetch(&tg->shared_pending, ~stops, __ATOMIC_SEQ_CST);
    struct tg_ctx c; c.tg = tg; c.n = 0; c.more = 0;
    process_t* lead = NULL;
    int was_done;
    uint64_t fl = process_lock_irqsave();
    was_done = tg->stop_done;
    tg->stop_active = 0; tg->stop_done = 0; tg->nr_stopped = 0;
    process_foreach_locked(tg_cb, &c);
    if (was_done) {
        lead = tg_leader_locked(tg, p->tgid);
        if (lead) { lead->stop_report = 0; lead->cont_report = 1; process_get(lead); }
    }
    process_unlock_irqrestore(fl);
    sched_wake_note();                       // a thread about to stop re-checks
    for (int i = 0; i < c.n; i++) {
        __atomic_and_fetch(&c.t[i]->sig_pending, ~stops, __ATOMIC_SEQ_CST);
        sched_wake_task(c.t[i], PROC_STOPPED);
    }
    tg_put_all(&c);
    if (lead) {
        extern void sched_notify_parent_cont(process_t*);
        sched_notify_parent_cont(lead);
        process_put(lead);
    }
}

// A process-directed signal with several threads: it pends on the group and
// one thread that does not block it is woken to take it (the target p if it
// can, else one that is not asleep, else a sleeping one).
static int signal_post_group(process_t* p, int sig) {
    thread_group_t* tg = p->tg;
    if (sig == SIGKILL) {
        thread_kill_others(p, 1, W_SIGNALED(SIGKILL));   // marks the group exit
        if (p->state != PROC_ZOMBIE) signal_post_thread(p, SIGKILL);
        return 0;
    }
    if (sig == SIGCONT) group_cont(p);
    if (sig_default_stop(sig)) {
        __atomic_and_fetch(&tg->shared_pending, ~SIGBIT(SIGCONT), __ATOMIC_SEQ_CST);
    }
    __atomic_or_fetch(&tg->shared_pending, SIGBIT(sig), __ATOMIC_SEQ_CST);
    sched_wake_note();                       // a task deciding to block right now re-checks
    if (sig != SIGSTOP && (sig_noop_mask(p) & SIGBIT(sig))) return 0;   // ignored: nobody to wake
    struct tg_ctx c; c.tg = tg; c.n = 0; c.more = 0;
    process_foreach(tg_cb, &c);
    process_t* pick = NULL;
    for (int pass = 0; pass < 3 && !pick; pass++) {
        for (int i = 0; i < c.n && !pick; i++) {
            process_t* t = c.t[i];
            if (sig != SIGSTOP && ((t->sig_blocked >> sig) & 1)) continue;
            if (pass == 0 && t != p) continue;
            if (pass == 1 && t->state == PROC_BLOCKED) continue;
            pick = t;
        }
    }
    if (pick && pick->state == PROC_BLOCKED) sig_kick(pick, sig);
    tg_put_all(&c);
    return 0;
}

// A process-directed signal. A process with one thread: that thread (as
// before U9). With several threads: signal_post_group.
int signal_post(process_t* p, int sig) {
    if (!p || sig <= 0 || sig >= NSIG) return -1;
    int live = thread_group_live(p);
    if (p->state == PROC_ZOMBIE && live <= 0) return -1;
    if (live > 1 || p->state == PROC_ZOMBIE) return signal_post_group(p, sig);
    return signal_post_thread(p, sig);
}

struct lead_find { thread_group_t* tg; uint32_t tgid; process_t* r; };
static void lead_find_cb(process_t* q, void* u) {
    struct lead_find* x = (struct lead_find*)u;
    if (!x->r && q->tg == x->tg && q->pid == x->tgid) x->r = q;
}
// The leader (pid == tgid) of group tg; proc_lock held. NULL once reaped.
static process_t* tg_leader_locked(thread_group_t* tg, uint32_t tgid) {
    struct lead_find x = { tg, tgid, NULL };
    process_foreach_locked(lead_find_cb, &x);
    return x.r;
}

// [U9] Start a group stop: every other thread stops at its next return to
// ring 3 (a sleeping one is woken for it). The parent is told once all have
// stopped (signal_group_stopped).
static void group_stop_start(process_t* cur, int sig) {
    thread_group_t* tg = cur->tg;
    struct tg_ctx c; c.tg = tg; c.n = 0; c.more = 0;
    uint64_t fl = process_lock_irqsave();
    if (!tg->stop_active) {
        tg->stop_active = 1; tg->stop_done = 0; tg->nr_stopped = 0;
        tg->stop_sig = sig;
        process_foreach_locked(tg_cb, &c);
    }
    process_unlock_irqrestore(fl);
    sched_wake_note();
    for (int i = 0; i < c.n; i++) if (c.t[i] != cur) sig_kick(c.t[i], 0);
    tg_put_all(&c);
}

// Called by sched_stop_current once a thread of a multi-threaded group is
// PROC_STOPPED: when the last live one stops, the leader reports the stop.
void signal_group_stopped(process_t* p) {
    thread_group_t* tg = p->tg;
    process_t* lead = NULL;
    uint64_t fl = process_lock_irqsave();
    tg->nr_stopped++;
    if (tg->stop_active && !tg->stop_done && tg->nr_stopped >= tg->nr_live) {
        tg->stop_done = 1;
        lead = tg_leader_locked(tg, p->tgid);
        if (lead) {
            lead->stop_report = 1; lead->cont_report = 0;
            lead->stop_sig = tg->stop_sig;
            process_get(lead);
        }
    }
    process_unlock_irqrestore(fl);
    if (lead) {
        extern void sched_notify_parent_stop(process_t*);
        sched_notify_parent_stop(lead);
        process_put(lead);
    }
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
    if (!(c->all ? (p->pid != 0 && p->tgid != c->skip) : (p->pgid == c->pgid))) return;
    if (c->sender && !cred_may_signal(c->sender, p, c->sig)) {   // [M43]
        c->denied++;
        return;
    }
    if (c->all && c->sender && p->tgid == 1 && !cred_is_root(c->sender)) return;
    // [U9] One entry per process: a process-directed signal is posted once
    // to its group (and 64 threads of one process must not fill the list).
    for (int i = 0; i < c->n; i++) if (c->t[i]->tg == p->tg) return;
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
    struct pg_ctx c; c.pgid = pgid; c.n = 0; c.skip = sender ? sender->tgid : 0; c.all = all;
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
// [U9] Linux sigset (bit n-1 = signal n) <-> SecOS mask (bit n = signal n).
static inline uint64_t mask_to_lx(uint64_t m) { return m >> 1; }
static inline uint64_t mask_from_lx(uint64_t l) { return (l << 1) & ~1ULL; }

static void uc_fill(lx_ucontext_t* uc, const trapframe_t* t, uint64_t mask, uint64_t frame_va,
                    const uint8_t fpu[512]) {
    uint8_t* z = (uint8_t*)uc; for (size_t i = 0; i < sizeof(*uc); i++) z[i] = 0;
    uint64_t* g = uc->gregs;
    g[LX_REG_R8] = t->r8;   g[LX_REG_R9] = t->r9;   g[LX_REG_R10] = t->r10; g[LX_REG_R11] = t->r11;
    g[LX_REG_R12] = t->r12; g[LX_REG_R13] = t->r13; g[LX_REG_R14] = t->r14; g[LX_REG_R15] = t->r15;
    g[LX_REG_RDI] = t->rdi; g[LX_REG_RSI] = t->rsi; g[LX_REG_RBP] = t->rbp; g[LX_REG_RBX] = t->rbx;
    g[LX_REG_RDX] = t->rdx; g[LX_REG_RAX] = t->rax; g[LX_REG_RCX] = t->rcx; g[LX_REG_RSP] = t->rsp;
    g[LX_REG_RIP] = t->rip; g[LX_REG_EFL] = t->rflags;
    g[LX_REG_CSGSFS] = t->cs & 0xffff;
    g[LX_REG_ERR] = t->err_code; g[LX_REG_TRAPNO] = t->int_no;
    uc->uc_sigmask[0] = mask_to_lx(mask);
    uc->fpregs = frame_va + __builtin_offsetof(sigframe_t, uc) + __builtin_offsetof(lx_ucontext_t, fpregs_mem);
    for (int i = 0; i < 512; i++) uc->fpregs_mem[i] = fpu[i];
}
static void uc_restore(const lx_ucontext_t* uc, trapframe_t* t) {
    const uint64_t* g = uc->gregs;
    t->r8 = g[LX_REG_R8];   t->r9 = g[LX_REG_R9];   t->r10 = g[LX_REG_R10]; t->r11 = g[LX_REG_R11];
    t->r12 = g[LX_REG_R12]; t->r13 = g[LX_REG_R13]; t->r14 = g[LX_REG_R14]; t->r15 = g[LX_REG_R15];
    t->rdi = g[LX_REG_RDI]; t->rsi = g[LX_REG_RSI]; t->rbp = g[LX_REG_RBP]; t->rbx = g[LX_REG_RBX];
    t->rdx = g[LX_REG_RDX]; t->rax = g[LX_REG_RAX]; t->rcx = g[LX_REG_RCX]; t->rsp = g[LX_REG_RSP];
    t->rip = g[LX_REG_RIP]; t->rflags = g[LX_REG_EFL];
}

// ---- delivery: called from the asm return-to-ring3 chokepoints ----
// Sets up a custom handler frame on the user stack (returns, asm iretq's into the
// handler) or performs the default action (terminate/stop/ignore). The terminate
// and stop paths NEVER return (they switch to another task), abandoning the asm
// stub mid-restore — exactly like a timer preemption.
// [U9] A thread takes its own pending signals first, then its group's (a
// process-directed signal goes to the first thread that does not block it);
// a thread of a group being stopped stops here.
void signal_dispatch(trapframe_t* tf) {
    if (!tf) return;
    if ((tf->cs & 3) != 3) return;              // only deliver returning to ring-3
#ifdef SECOS_LOCKDEP
    klock_check_user_return("iret to ring 3");  // [M50] nothing held going back to user
#endif
    process_t* cur = sched_get_current();
    if (!cur) return;

    for (;;) {
        if (cur->tg && cur->tg->stop_active && !cur->thread_killed && multi(cur)) {
            uint64_t kp = sig_all_pending(cur) & SIGBIT(SIGKILL);
            if (!kp) {                           // join the group stop
                cur->stop_sig = cur->tg->stop_sig ? cur->tg->stop_sig : SIGSTOP;
                sched_stop_current(tf);          // NORETURN
            }
        }
        uint64_t pend = sig_all_pending(cur);
        uint64_t deliverable = (pend & ~cur->sig_blocked) | (pend & SIG_UNMASKABLE);
        int sig = 1; while (sig < NSIG && !((deliverable >> sig) & 1)) sig++;
        if (!deliverable || sig >= NSIG) {
            // sigsuspend() returned without a handler frame taking over its
            // saved mask (only ignored signals): restore the mask now.
            if (cur->sigsusp_active) { cur->sig_blocked = cur->sigsusp_saved; cur->sigsusp_active = 0; }
            return;
        }
        if (!sig_dequeue(cur, sig)) continue;    // another thread took it

        uint64_t disp = cur->sig_handler[sig];

        // SIGKILL is never catchable.
        if (sig == SIGKILL) sched_kill_current(sig);          // NORETURN
        // SIGSTOP is never catchable.
        if (sig == SIGSTOP) {
            if (multi(cur)) group_stop_start(cur, sig);
            cur->stop_sig = sig; sched_stop_current(tf);       // NORETURN
        }

        if (disp == SIG_IGN) continue;
        if (disp == SIG_DFL) {
            if (sig_default_ignore(sig)) continue;
            if (sig_default_stop(sig)) {
                if (multi(cur)) group_stop_start(cur, sig);
                cur->stop_sig = sig; sched_stop_current(tf);   // NORETURN
            }
            sched_kill_current(sig);                           // NORETURN (term: the group)
        }

        // ---- custom handler: build a sigframe on the user stack ----
        sigframe_t* sf = (sigframe_t*)kmalloc(sizeof(sigframe_t));
        if (!sf) sched_kill_current(sig);        // cannot deliver: terminate (NORETURN)
        sf->magic = SIGFRAME_MAGIC;
        sf->saved_mask = cur->sig_blocked;
        if (cur->sigsusp_active) {               // handler interrupts sigsuspend():
            sf->saved_mask = cur->sigsusp_saved; // sigreturn restores the pre-suspend mask
            cur->sigsusp_active = 0;
        }
        sf->saved = *tf;                         // full interrupted context
        __asm__ volatile("fxsave64 %0" : "=m"(*(uint8_t(*)[512])sf->fpu));   // [M48] live user FPU

        uint64_t usp = tf->rsp;
        usp -= 128;                              // skip the SysV red zone
        uint64_t frame = (usp - sizeof(sigframe_t)) & ~15ULL;
        // [U9] siginfo_t + ucontext_t (SA_SIGINFO handlers: rsi, rdx).
        for (size_t i = 0; i < sizeof(sf->si); i++) ((uint8_t*)&sf->si)[i] = 0;
        sf->si.si_signo = sig;
        sf->si.si_code = 0;                      // SI_USER
        uc_fill(&sf->uc, tf, sf->saved_mask, frame, sf->fpu);
        int bad = copy_to_user((void*)frame, sf, sizeof(*sf)) != 0;
        kfree(sf);
        if (bad) {
            // Cannot deliver safely (bad stack) -> fall back to terminate.
            sched_kill_current(sig);             // NORETURN
        }
        uint64_t ret_slot = frame - 8;           // handler returns here -> restorer
        uint64_t rest = cur->tg->restorer;
        if (copy_to_user((void*)ret_slot, &rest, 8) != 0)
            sched_kill_current(sig);             // NORETURN

        // Mask this signal while its handler runs (POSIX), then enter the handler.
        cur->sig_blocked |= SIGBIT(sig);
        tf->rip = disp;                          // handler entry
        tf->rsp = ret_slot;                      // rsp%16==8 at entry (ABI)
        tf->rdi = (uint64_t)sig;                 // first arg: signum
        tf->rsi = frame + __builtin_offsetof(sigframe_t, si);   // [U9] siginfo_t*
        tf->rdx = frame + __builtin_offsetof(sigframe_t, uc);   // [U9] ucontext_t*
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
    cur->sig_handler[sig] = handler;                    // SIG_DFL/SIG_IGN or VA ([U9] shared)
    if (restorer) cur->tg->restorer = restorer;
    return 0;
}

long ksys_sigreturn(trapframe_t* tf) {
    process_t* cur = sched_get_current();
    if (!cur) return -1;
    sigframe_t* sf = (sigframe_t*)kmalloc(sizeof(sigframe_t));
    if (!sf) return -1;
    if (copy_from_user(sf, (void*)tf->rsp, sizeof(*sf)) != 0) { kfree(sf); return -1; }
    if (sf->magic != SIGFRAME_MAGIC) { kfree(sf); sched_kill_current(SIGSEGV); } // tampered -> kill
    // [U9] The context comes back from the ucontext (a handler may have
    // changed it: musl's cancellation moves the pc), the selectors and
    // int_no/err_code from the saved trapframe.
    trapframe_t s = sf->saved;
    uc_restore(&sf->uc, &s);
    // Only restore the user-controlled parts; force ring-3 selectors and IF so a
    // forged frame cannot escalate privilege or run with interrupts disabled.
    s.cs = 0x1B; s.ss = 0x23;
    s.rflags = (s.rflags & ~0x200ULL) | 0x202ULL;       // keep IF set, sane bits
    s.rflags &= ~(0x3000ULL | 0x4000ULL | 0x20000ULL);  // no IOPL, NT, VM
    if ((s.rsp >= 0x0000800000000000ULL) || s.rip >= 0x0000800000000000ULL) { kfree(sf); return -1; }   // user range only
    cur->sig_blocked = mask_from_lx(sf->uc.uc_sigmask[0]) & ~SIG_UNMASKABLE;
    {   // [M48] Restore the interrupted FPU/SSE state. Reserved MXCSR bits would
        // make fxrstor #GP in the kernel: mask them with the CPU's MXCSR_MASK.
        // [M52] The probe buffer is per call: two CPUs may run this at once
        // (the mask itself is the same value on every CPU).
        static volatile uint32_t mxcsr_mask;
        if (!mxcsr_mask) {
            uint8_t probe[512] __attribute__((aligned(16)));
            __asm__ volatile("fxsave64 %0" : "=m"(probe));
            uint32_t m = *(uint32_t*)(probe + 28);
            mxcsr_mask = m ? m : 0xFFBFu;
        }
        *(uint32_t*)(sf->uc.fpregs_mem + 24) &= mxcsr_mask;
        __asm__ volatile("fxrstor64 %0" : : "m"(*(const uint8_t(*)[512])sf->uc.fpregs_mem));
    }
    kfree(sf);
    *tf = s;                                             // restore in place
    return (long)s.rax;                                  // preserve interrupted rax
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
    // [U9] A leader that exited while its other threads run is still the process.
    int zombie = (t->state == PROC_ZOMBIE && thread_group_live(t) <= 0);
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
    // [M52] Check and store under proc_lock: without the BKL the target can
    // run setsid() on another CPU between the two (a session leader whose
    // pgid != pid). The readers of pgid/sid (signal_post_pgid, the tty
    // checks) take proc_lock too.
    uint64_t fl = process_lock_irqsave();
    if (t->tg != cur->tg && !cred_is_root(cur) &&
        !((t->ppid == cur->tgid || t->spawner_pid == cur->tgid) && t->sid == cur->sid)) r = -3;
    else if (pgid < 0) r = -22;                          // -EINVAL
    else {
        t->pgid = (pgid == 0) ? t->tgid : (uint32_t)pgid;
        thread_group_sync_locked(t, TG_SYNC_PGID);       // [U9] one pgid per process
    }
    process_unlock_irqrestore(fl);
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
