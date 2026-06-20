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

extern process_t* sched_get_current(void);
extern process_t* process_find_by_pid(uint32_t pid);
extern void process_foreach(void (*cb)(process_t*, void*), void* user);
extern void sched_kill_current(int reason) __attribute__((noreturn));
extern void sched_stop_current(trapframe_t* tf) __attribute__((noreturn));

// ---- foreground process group (console job control) ----
static uint32_t g_foreground_pgid = 0;
void     signal_set_foreground_pgid(uint32_t pgid) { g_foreground_pgid = pgid; }
uint32_t signal_get_foreground_pgid(void) { return g_foreground_pgid; }

// ---- disposition helpers ----
static int sig_default_ignore(int s) { return s == SIGCHLD || s == SIGCONT; }
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

// Is a non-blocked (or unmaskable) signal pending? Used by blocking syscalls to
// return EINTR instead of sleeping.
int signal_pending(process_t* p) {
    if (!p) return 0;
    uint64_t deliverable = (p->sig_pending & ~p->sig_blocked) |
                           (p->sig_pending & SIG_UNMASKABLE);
    return deliverable != 0;
}

// Post one signal to a process. If it is blocked in a syscall and the signal is
// deliverable, wake it (clearing the wait condition) so the syscall re-runs,
// observes the pending signal and returns EINTR. SIGCONT resumes a stopped task.
int signal_post(process_t* p, int sig) {
    if (!p || sig <= 0 || sig >= NSIG) return -1;
    if (p->state == PROC_ZOMBIE) return -1;

    // SIGCONT cancels pending stops and resumes a stopped process.
    if (sig == SIGCONT) {
        p->sig_pending &= ~(SIGBIT(SIGSTOP) | SIGBIT(SIGTSTP) |
                            SIGBIT(SIGTTIN) | SIGBIT(SIGTTOU));
        if (p->state == PROC_STOPPED) p->state = PROC_READY;
    }
    // A stop signal cancels a pending SIGCONT.
    if (sig_default_stop(sig)) p->sig_pending &= ~SIGBIT(SIGCONT);

    p->sig_pending |= SIGBIT(sig);

    int deliverable = (sig == SIGKILL || sig == SIGSTOP) ||
                      !((p->sig_blocked >> sig) & 1);
    if (deliverable && p->state == PROC_BLOCKED) {
        // Tear down the wait condition so the woken task does not re-block before
        // it gets a chance to see the signal. SIGKILL needs no syscall re-run, but
        // making it READY lets the scheduler dispatch (and then kill) it.
        p->wait_pid = -1; p->sleep_until = 0; p->recv_chan = -1; p->wait_pipe = NULL;
        p->state = PROC_READY;
    }
    return 0;
}

struct pg_ctx { uint32_t pgid; int sig; int n; };
static void pg_cb(process_t* p, void* u) {
    struct pg_ctx* c = (struct pg_ctx*)u;
    if (p->pgid == c->pgid && p->state != PROC_ZOMBIE) { signal_post(p, c->sig); c->n++; }
}
int signal_post_pgid(uint32_t pgid, int sig) {
    if (pgid == 0) return 0;
    struct pg_ctx c = { pgid, sig, 0 };
    process_foreach(pg_cb, &c);
    return c.n;
}

// ---- delivery: called from the asm return-to-ring3 chokepoints ----
// Sets up a custom handler frame on the user stack (returns, asm iretq's into the
// handler) or performs the default action (terminate/stop/ignore). The terminate
// and stop paths NEVER return (they switch to another task), abandoning the asm
// stub mid-restore — exactly like a timer preemption.
void signal_dispatch(trapframe_t* tf) {
    if (!tf) return;
    if ((tf->cs & 3) != 3) return;              // only deliver returning to ring-3
    process_t* cur = sched_get_current();
    if (!cur) return;

    for (;;) {
        uint64_t deliverable = (cur->sig_pending & ~cur->sig_blocked) |
                               (cur->sig_pending & SIG_UNMASKABLE);
        if (!deliverable) return;

        int sig = 1; while (sig < NSIG && !((deliverable >> sig) & 1)) sig++;
        if (sig >= NSIG) return;
        cur->sig_pending &= ~SIGBIT(sig);

        uint64_t disp = cur->sig_handler[sig];

        // SIGKILL is never catchable.
        if (sig == SIGKILL) sched_kill_current(sig);          // NORETURN
        // SIGSTOP is never catchable.
        if (sig == SIGSTOP) sched_stop_current(tf);           // NORETURN

        if (disp == SIG_IGN) continue;
        if (disp == SIG_DFL) {
            if (sig_default_ignore(sig)) continue;
            if (sig_default_stop(sig))   sched_stop_current(tf);   // NORETURN
            sched_kill_current(sig);                                // NORETURN (term)
        }

        // ---- custom handler: build a sigframe on the user stack ----
        sigframe_t sf;
        sf.magic = SIGFRAME_MAGIC;
        sf.saved_mask = cur->sig_blocked;
        sf.saved = *tf;                          // full interrupted context

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
    *tf = *s;                                            // restore in place
    return (long)s->rax;                                // preserve interrupted rax
}

long ksys_kill(int pid, int sig) {
    if (sig < 0 || sig >= NSIG) return -1;
    if (pid <= 0) {
        // pid 0 / -1: address the caller's process group (job-control friendly).
        process_t* cur = sched_get_current();
        if (!cur) return -1;
        if (sig == 0) return 0;
        return signal_post_pgid(cur->pgid, sig) > 0 ? 0 : -1;
    }
    process_t* t = process_find_by_pid((uint32_t)pid);
    if (!t) return -1;
    if (sig == 0) return 0;                              // existence check only
    return signal_post(t, sig);
}

// [M30] setpgid(pid, pgid): pid 0 = caller; pgid 0 = the target's own pid (make
// it a group leader). Minimal model — no session/leader checks (the signature is
// the trust boundary in SecOS), just enough for the shell to group a pipeline.
long ksys_setpgid(int pid, int pgid) {
    process_t* cur = sched_get_current();
    if (!cur) return -1;
    process_t* t = (pid == 0) ? cur : process_find_by_pid((uint32_t)pid);
    if (!t) return -1;
    t->pgid = (pgid == 0) ? t->pid : (uint32_t)pgid;
    return 0;
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
