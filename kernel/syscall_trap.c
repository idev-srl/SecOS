/*
 * SecOS Kernel - Trapframe-based syscall handler
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M5.1] C entry point called from syscall_entry asm stub.
 * Extracts syscall ABI registers from the trapframe and forwards
 * to the existing syscall_dispatch().
 *
 * [M6] Saves trapframe snapshot into current process for context switch.
 */
#include "trapframe.h"
#include "syscall.h"
#include "sched.h"
#include "process.h"
#include "signal.h"
#include "debugcon.h"
#include "kverbose.h"
#include "cap.h"
#include "user_copy.h"
#include "power.h"
#include "bkl.h"
#include "thread.h"   // [U9] clone, thread exit, fd pins
#include "futex.h"

#ifdef SYSCALL_DEBUG
#include "terminal.h"
#endif

extern uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4);

// Job control: a blocking syscall about to sleep or fail with EINTR first
// checks for a pending default-action stop (SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU).
// If there is one, the caller stops right here with the syscall rewound, so on
// SIGCONT it re-runs transparently instead of returning EINTR (Linux restarts
// syscalls across a stop). NORETURN when it stops; otherwise returns.
static void stop_and_restart_if_pending(process_t* cur, trapframe_t* tf) {
    if (!cur) return;
    int ss = signal_take_stop(cur);
    if (!ss) return;
    cur->stop_sig = ss;
    tf->rip -= 2;                                   // re-run `int 0x80` after SIGCONT
    sched_stop_current(tf);                         // NORETURN
}
// "Would a blocking syscall be interrupted?" Stops+restarts on a pending stop
// signal (NORETURN then), else reports whether an effective signal is pending.
static int sig_interrupted(process_t* cur, trapframe_t* tf) {
    stop_and_restart_if_pending(cur, tf);
    return signal_pending(cur);
}

// Socket I/O that may block: read/write/recv/send/recvfrom/sendto/accept/connect on a
// socket fd. Returns the fd's entry when `num` is one of them and the fd is a
// socket, else NULL.
static struct proc_fd_entry* sock_wait_fd(process_t* cur, uint64_t num, uint64_t a0) {
    if (!cur) return NULL;
    if (num != SYS_READ && num != SYS_WRITE && num != SYS_RECV && num != SYS_SEND &&
        num != SYS_RECVFROM && num != SYS_SENDTO && num != SYS_ACCEPT &&
        num != SYS_CONNECT) return NULL;
    if (a0 >= 32 || !cur->fds[a0].used || !cur->fds[a0].is_sock) return NULL;
    return &cur->fds[a0];
}
#define SOCK_O_NONBLOCK 0x800
#define SOCK_WAIT_TICKS 2   // re-check readiness every ~2 ms (no RX wakeup hook yet)

static uint64_t syscall_handler_body(trapframe_t* tf);

// [M48] Every syscall body runs under the kernel lock (kernel/bkl.h). Paths
// that never come back here (exit, block, stop, kill, a yield that switched)
// drop it in switch_to(). [M50] Per syscall: syscall_bkl_mask (all set today,
// so bkl=off still serializes every syscall). The exit also checks that the
// task goes back to ring 3 holding no klock.
uint64_t syscall_handler(trapframe_t* tf) {
    int bkl = bkl_syscall_enter(tf->rax);
    // [U9] In a multi-threaded process another thread may close the fd this
    // call works on: hold a reference on its object for the call (dropped
    // here, or before the task blocks/stops/exits: kernel/sched.c).
    process_t* cur = sched_get_current();
    if (cur) thread_fd_pin(cur, tf->rax, tf->rdi);
    uint64_t r = syscall_handler_body(tf);
    if (cur) thread_fd_unpin(cur);
    bkl_syscall_exit(bkl);
    return r;
}

static uint64_t syscall_handler_body(trapframe_t* tf) {
    process_t* cur = sched_get_current();
    uint64_t num = tf->rax;
    // Re-run of a blocking syscall a caught signal woke (see process.h)?
    if (cur) {
        cur->wake_snap = sched_wake_seq();   // [M48] lost-wakeup guard (sched_block_current)
        cur->sig_rerun_eintr = 0;
        if (cur->sig_eintr_rip && tf->rip - 2 == cur->sig_eintr_rip) {
            cur->sig_rerun_eintr = 1;
            cur->sig_eintr_rip = 0;
        }
    }

    // [M7] Debugcon: log pid + syscall number (gated on verbosity since M34 so it
    // doesn't interleave with ring-3 program output, e.g. the lua interpreter).
    if (g_kverbose) {
        debugcon_writestring("[SYSCALL] pid=");
        debugcon_print_hex(cur ? cur->pid : 0);
        debugcon_writestring(" num=");
        debugcon_print_hex(num);
        debugcon_writestring("\n");
    }

#ifdef SYSCALL_DEBUG
    {
        terminal_writestring("[SYSCALL] pid=");
        char hx[] = "0123456789ABCDEF";
        uint32_t pid = cur ? cur->pid : 0;
        for (int i = 28; i >= 0; i -= 4)
            terminal_putchar(hx[(pid >> i) & 0xF]);
        terminal_writestring(" num=");
        for (int i = 60; i >= 0; i -= 4)
            terminal_putchar(hx[(num >> i) & 0xF]);
        terminal_writestring("\n");
    }
#endif

    // [M7] SYS_YIELD: cooperative context switch.
    // sched_yield_from_syscall does NOT return if it switches to another process.
    // If no other process is ready, it returns and we fall through (rax=0).
    if (num == SYS_YIELD) {
        sched_yield_from_syscall(tf);
        // Reached only if no switch occurred — return 0 to caller
        return 0;
    }

    // [M8] SYS_EXIT: terminate the caller. Marks it ZOMBIE and switches away;
    // does NOT return (the task is reaped later from the idle/scheduler context).
    // [U1] exit_group (Linux-shaped, 512+231) is SYS_EXIT while a process has
    // one thread.
    if (num == SYS_EXIT || num == SYS_LX_EXIT_GROUP) {
        if (cur) cur->exit_code = W_EXITCODE(tf->rdi); // [M16] POSIX status: (st&0xff)<<8
        sched_exit_current(tf);
        return 0; // not reached when a switch occurs
    }
    // [U9] Linux exit(2): the calling thread only (pthread_exit). The last
    // thread of a process ends it, as exit_group would.
    if (num == SYS_LX_EXIT) {
        sched_thread_exit_current((int)tf->rdi);
        return 0; // not reached for a user task
    }

    // Reap this CPU's reapable zombies before creating a process. Zombies were
    // only reaped from the idle context, which never runs while user tasks stay
    // runnable (e.g. a bash loop of `$(...)` substitutions), so dead processes
    // piled up until the process/kernel-stack slots ran out. This is safe here:
    // every zombie has already switched off its own kernel stack and address
    // space, and the caller (current) is not a zombie.
    if (num == SYS_FORK || num == SYS_SPAWN || num == SYS_LX_CLONE) {
        extern void sched_reap_zombies(void);
        sched_reap_zombies();
    }

    uint64_t ret;

    // [M30] SYS_SIGRETURN: restore the context saved when a signal handler was
    // entered (reads the sigframe from the user stack and rewrites tf in place).
    // The returned value is the interrupted code's original rax, preserved below.
    if (num == SYS_SIGRETURN) {
        extern long ksys_sigreturn(trapframe_t*);
        ret = (uint64_t)ksys_sigreturn(tf);
    }
    else
    // [M19] SYS_FORK: copy-on-write child from the parent's live trapframe. The
    // child resumes at the same point with rax=0; the parent gets the child pid.
    if (num == SYS_FORK) {
        if (!cap_check(cur, CAP_PROC, num, "fork")) {   // [M35]
            ret = (uint64_t)(int64_t)-1;
        } else {
            extern process_t* process_fork(process_t*, trapframe_t*);
            process_t* ch = cur ? process_fork(cur, tf) : 0;
            ret = ch ? (uint64_t)(int64_t)ch->pid : (uint64_t)(int64_t)-1;
            // [M48] Publish the child only after reading its pid: once READY it
            // may run, exit and be reaped on another CPU before we look again.
            if (ch) sched_make_ready(ch);
        }
    }
    else
    // [U9] clone(flags, newsp, ptid, ctid, tls): a thread of the caller's
    // process (musl's pthread_create flag set), or a plain fork (SIGCHLD).
    if (num == SYS_LX_CLONE) {
        uint64_t fl = tf->rdi;
        if (cur && (fl & ~0xFFULL) == 0 && (fl & 0xFF) == 17 /*SIGCHLD*/ && tf->rsi == 0) {
            if (!cap_check(cur, CAP_PROC, num, "clone-fork")) {
                ret = (uint64_t)(int64_t)-1;
            } else {
                process_t* ch = process_fork(cur, tf);
                ret = ch ? (uint64_t)(int64_t)ch->pid : (uint64_t)(int64_t)-11;
                if (ch) sched_make_ready(ch);
            }
        } else {
            process_t* th = NULL;
            long r = cur ? thread_clone(cur, tf, fl, tf->rsi, tf->rdx, tf->rcx, tf->r8, &th)
                         : -22;
            ret = (uint64_t)(int64_t)r;
            if (th) sched_make_ready(th);              // after the tid was read
        }
    }
    else
    // [U9] futex(uaddr, op, val, timeout|val2, uaddr2, val3); WAIT may block
    // (rip rewound, re-run resumes it).
    if (num == SYS_LX_FUTEX) {
        ret = (uint64_t)futex_syscall(tf, tf->rdi, tf->rsi, tf->rdx, tf->rcx, tf->r8, tf->r9);
    }
    else
    // SYS_EXECVE(path, argv, envp): replace the caller's image in place. On
    // success the trapframe now enters the new image (rax=0); on failure the
    // old image continues with -errno. Needs CAP_PROC when confined (like spawn).
    // [U9] From a multi-threaded process the other threads are killed first
    // and the caller waits (one tick at a time, re-running the call) until
    // it is alone; then the exec proceeds as for a single thread.
    if (num == SYS_EXECVE) {
        if (!cap_check(cur, CAP_PROC, num, "execve")) {
            ret = (uint64_t)(int64_t)-1;                // EPERM
        } else {
            int dt = cur ? thread_exec_dethread(cur) : 0;
            if (dt == 1) {
                extern uint64_t timer_get_ticks(void);
                cur->sleep_until = timer_get_ticks() + 1;
                tf->rip -= 2;                           // re-run once the others are gone
                sched_block_current(tf);                // NORETURN
                ret = 0;
            } else if (dt < 0) {
                ret = (uint64_t)(int64_t)-4;            // being killed: EINTR, the kill follows
            } else {
                if (cur) cur->sleep_until = 0;
                ret = (uint64_t)ksys_execve(tf, tf->rdi, tf->rsi, tf->rdx);
            }
        }
    }
    else
    // [M16] SYS_WAIT: blocking wait for a child, returning its exit status.
    // If the child is still running, block the caller (rewind rip so the syscall
    // re-runs on wake) instead of busy-polling. A woken waiter finds its result
    // pre-delivered (wait_ready) because the child may already be reaped.
    if (num == SYS_WAIT) {
        if (cur && cur->wait_ready) {
            cur->wait_ready = 0;
            ret = (uint64_t)(int64_t)cur->wait_result;
        } else {
            // [M50-B1] The reference keeps the child alive while we look at
            // it; it is dropped on every path (sched_block_current does not
            // return, so drop it before blocking).
            process_t* child = process_get_by_pid((uint32_t)tf->rdi);
            if (!child) {
                ret = (uint64_t)(int64_t)-1;            // no such pid
            } else if (cur && ((child->ppid != cur->tgid && child->spawner_pid != cur->tgid) ||
                               child->pid != child->tgid)) {   // [U9] a process, not a thread
                // Only the parent (fork) or the spawner may wait on / collect a
                // process; anyone else could otherwise steal its exit status and
                // mark it collected (reapable) behind the real parent's back.
                ret = (uint64_t)(int64_t)-10;           // ECHILD: not our child
            } else if (child->state == PROC_ZOMBIE && child->tg->dead) {
                ret = (uint64_t)(int64_t)child->exit_code; // already exited
                child->collected = 1;                      // [M39] parent reaped it -> reapable
            } else if (cur && sig_interrupted(cur, tf)) {
                ret = (uint64_t)(int64_t)(-4);          // [M30] EINTR: a signal is pending
            } else if (cur) {
                process_put(child); child = NULL;
                cur->wait_pid = (int)tf->rdi;
                cur->wait_ready = 0;
                tf->rip -= 2;                           // re-run `int 0x80` on wake
                sched_block_current(tf);                // NORETURN (switches away)
                ret = 0;                                 // not reached
            } else {
                ret = (uint64_t)(int64_t)-1;
            }
            process_put(child);
        }
    }
    // [M39] SYS_WAITANY: waitpid(-1) — reap ANY child (bash's wait_for loop uses
    // it). Returns the child's pid (status written to *rdi), 0 if WNOHANG and no
    // child has exited yet, or -ECHILD(-10) if the caller has no children.
    else if (num == SYS_WAITANY) {
        int* ustatus = (int*)tf->rdi;
        int opts     = (int)tf->rsi;
        int wnohang  = opts & 1;             // WNOHANG
        int wuntrace = opts & 2;             // WUNTRACED: report stopped children
        int wcont    = opts & 8;             // WCONTINUED: report resumed children
        if (!cur) { ret = (uint64_t)(int64_t)-10; }
        else {
            // [M50-B1] Collected under proc_lock: only pid + status come out
            // (once collected, the reaper may free the zombie on another CPU).
            uint32_t zpid = 0; int code = 0;
            int z = process_collect_zombie_child(cur->tgid, &zpid, &code);   // [U9] tgid
            uint32_t j = 0; int jst = 0;
            if (!z && (wuntrace || wcont))
                j = process_take_jobctl_child(cur->tgid, wuntrace, wcont, &jst);
            if (z) {
                if (ustatus && user_range_valid(ustatus, sizeof(int)))
                    copy_to_user(ustatus, &code, sizeof(int));
                ret = (uint64_t)(int64_t)(int)zpid;
            } else if (j) {                  // stopped (sig<<8|0x7f) / continued (0xffff)
                if (ustatus && user_range_valid(ustatus, sizeof(int)))
                    copy_to_user(ustatus, &jst, sizeof(int));
                ret = (uint64_t)(int64_t)(int)j;
            } else if (!process_has_children(cur->tgid)) {
                ret = (uint64_t)(int64_t)-10;        // ECHILD
            } else if (wnohang) {
                ret = 0;                              // children alive, none reaped yet
            } else if (sig_interrupted(cur, tf)) {
                ret = (uint64_t)(int64_t)-4;          // EINTR
            } else {
                cur->wait_pid = -2;                   // [M39] wait-any sentinel
                tf->rip -= 2;                         // re-run on wake
                sched_block_current(tf);              // NORETURN
                ret = 0;
            }
        }
    }
    // SYS_SIGSUSPEND(mask): install `mask` and sleep until a signal that does
    // something is delivered; the old mask comes back after the handler ran
    // (signal_dispatch stores it in the handler frame). Always -EINTR.
    else if (num == SYS_SIGSUSPEND) {
        if (!cur) { ret = (uint64_t)(int64_t)-4; }
        else {
            if (!cur->sigsusp_active) {
                cur->sigsusp_saved  = cur->sig_blocked;
                cur->sig_blocked    = tf->rdi & ~SIG_UNMASKABLE;
                cur->sigsusp_active = 1;
            }
            if (sig_interrupted(cur, tf)) {
                ret = (uint64_t)(int64_t)-4;          // EINTR; dispatch restores the mask
            } else {
                tf->rip -= 2;                         // re-run on wake (any signal_post)
                sched_block_current(tf);              // NORETURN
                ret = 0;
            }
        }
    }
    // SYS_REBOOT(cmd): orderly poweroff/restart/halt (kernel/power.c). The
    // shutdown waits for the other processes to exit, so the caller sleeps and
    // re-runs the syscall (state kept in its PCB) until the final phase, which
    // never returns. A confined manifest needs CAP_POWER.
    else if (num == SYS_REBOOT) {
        if (!cap_check(cur, CAP_POWER, num, "reboot") || !cred_is_root(cur)) {   // [M43] root only
            ret = (uint64_t)(int64_t)-1;                // EPERM
        } else {
            long r = power_syscall_step(cur, (int)tf->rdi);
            if (r == 1 && cur) {
                tf->rip -= 2;                           // re-run after the poll interval
                sched_block_current(tf);                // NORETURN
                ret = 0;
            } else {
                ret = (uint64_t)(int64_t)r;
            }
        }
    }
    // [M17] SYS_MSG_RECV: block the caller on an empty IPC channel until a send
    // wakes it (vs the old non-blocking poll). A pre-queued message returns
    // immediately, so existing pollers keep working.
    else if (num == SYS_MSG_RECV) {
        if (!cap_check(cur, CAP_IPC, num, "msg_recv")) { // [M35]
            ret = (uint64_t)(int64_t)-1;
        } else {
            extern int ksys_msg_recv_try(int chan, void* ubuf, int len);
            int n = ksys_msg_recv_try((int)tf->rdi, (void*)tf->rsi, (int)tf->rdx);
            if (n == 0 && cur && sig_interrupted(cur, tf)) {
                ret = (uint64_t)(int64_t)(-4);              // [M30] EINTR
            } else if (n == 0 && cur) {
                cur->recv_chan = (int)tf->rdi;
                tf->rip -= 2;                               // re-run on wake
                sched_block_current(tf);                    // NORETURN
                ret = 0;                                     // not reached
            } else {
                ret = (uint64_t)(int64_t)n;
            }
        }
    }
    // [M17] SYS_SLEEP: block the caller until timer tick deadline.
    else if (num == SYS_SLEEP) {
        extern uint64_t timer_get_ticks(void);
        uint64_t now = timer_get_ticks();
        if (cur && cur->sleep_until != 0 && now >= cur->sleep_until) {
            cur->sleep_until = 0;                       // deadline reached
            ret = 0;
        } else if (cur && sig_interrupted(cur, tf)) {
            cur->sleep_until = 0;                       // [M30] EINTR: abandon the sleep
            ret = (uint64_t)(int64_t)(-4);
        } else if (cur) {
            if (cur->sleep_until == 0) {                // first entry: arm deadline
                uint64_t ticks = tf->rdi;
                cur->sleep_until = now + (ticks ? ticks : 1);
            }
            tf->rip -= 2;                               // re-run on wake
            sched_block_current(tf);                    // NORETURN
            ret = 0;                                     // not reached
        } else {
            ret = 0;
        }
    }
    // Blocking socket I/O: the socket layer is non-blocking and returns -EAGAIN;
    // for a blocking fd sleep a couple of ticks (the timer tick polls the NIC)
    // and re-run the syscall, so a process waiting on the network yields the
    // CPU instead of spinning in ring 0. A pending signal ends the wait with
    // -EINTR. sleep_until is ours between re-runs and is cleared on entry.
    else if (sock_wait_fd(cur, num, tf->rdi)) {
        struct proc_fd_entry* e = sock_wait_fd(cur, num, tf->rdi);
        cur->sleep_until = 0;
        ret = syscall_dispatch(num, tf->rdi, tf->rsi, tf->rdx, tf->rcx, tf->r8);
        // [M47] connect() is non-blocking in the socket layer too: -EINPROGRESS
        // (handshake started) / -EALREADY (still running) mean "would block".
        int wouldblock = ret == (uint64_t)(int64_t)(-11) ||
            (num == SYS_CONNECT && (ret == (uint64_t)(int64_t)(-115) ||
                                    ret == (uint64_t)(int64_t)(-114)));
        if (wouldblock && !(e->flags & SOCK_O_NONBLOCK)) {
            if (sig_interrupted(cur, tf)) {
                ret = (uint64_t)(int64_t)(-4);          // EINTR
            } else {
                extern uint64_t timer_get_ticks(void);
                cur->sleep_until = timer_get_ticks() + SOCK_WAIT_TICKS;
                tf->rip -= 2;                           // re-run on wake
                sched_block_current(tf);                // NORETURN
                ret = 0;                                 // not reached
            }
        }
    }
    // [M25] SYS_READ / SYS_WRITE on a pipe may block. ksys_read/ksys_write return
    // -2 ("would block") when the pipe is empty (read) or full (write) with the
    // peer still open; arm wait_pipe and re-run the syscall on wake. EOF/EPIPE
    // (0 / -1) and all non-pipe reads/writes fall straight through.
    else if (num == SYS_READ || num == SYS_WRITE) {
        if (cur) { cur->sleep_until = 0; cur->wait_pipe = NULL; }   // our pipe wait (below)
        ret = syscall_dispatch(num, tf->rdi, tf->rsi, tf->rdx, tf->rcx, tf->r8);
        // A TTY read interrupted by a stop (Ctrl-Z, SIGTTIN for a background
        // reader, kill -STOP) restarts after SIGCONT instead of failing EINTR.
        // Same for a pty write stopped by SIGTTOU (TOSTOP).
        if (ret == (uint64_t)(int64_t)(-4) && cur)
            stop_and_restart_if_pending(cur, tf);
        if (ret == (uint64_t)(int64_t)(-2) && cur) {
            int fd = (int)tf->rdi;
            int pipe_fd = fd >= 0 && fd < 32 && cur->fds[fd].used && cur->fds[fd].is_pipe;
            if (pipe_fd && (cur->fds[fd].flags & 0x800 /*O_NONBLOCK*/)) {
                // Never block: report the bytes already queued, else -EAGAIN.
                ret = (num == SYS_WRITE && cur->pipe_wdone > 0) ? (uint64_t)cur->pipe_wdone
                                                                : (uint64_t)(int64_t)(-11);
                cur->pipe_wdone = 0;
            } else if (sig_interrupted(cur, tf)) {
                cur->wait_tty = 0;
                cur->wait_pty = NULL;
                ret = (uint64_t)(int64_t)(-4);          // [M30] EINTR
                // A pipe write interrupted after queueing some bytes reports
                // them (POSIX short count) instead of failing with EINTR.
                if (num == SYS_WRITE && cur->pipe_wdone > 0) ret = (uint64_t)cur->pipe_wdone;
                cur->pipe_wdone = 0;
            } else if (cur->wait_pty) {
                // Pty read/write that cannot progress yet: sleep until the
                // other side (sched_wake_pty) or the tty tick re-runs it.
                tf->rip -= 2;
                sched_block_current(tf);                // NORETURN
                ret = 0;
            } else if (num == SYS_READ && cur->wait_tty) {
                // Console read with no input yet: sleep until tty_wake_readers()
                // (timer tick) sees input, then re-run the read.
                tf->rip -= 2;
                sched_block_current(tf);                // NORETURN
                ret = 0;
            } else if (num == SYS_READ && cur->sleep_until) {
                // [M40] evdev read with no event yet: the device asked for a
                // timed re-poll (USB HID has no interrupt). Sleep, then re-run.
                tf->rip -= 2;
                sched_block_current(tf);                // NORETURN
                ret = 0;
            } else if (pipe_fd) {
                cur->wait_pipe = cur->fds[fd].inode;
                // SMP: the peer on another CPU can queue data / free room and
                // call sched_wake_pipe() between our -2 and the block below.
                // [M48] That wake bumps the wake sequence, so the block below
                // re-runs the read/write at once instead of sleeping.
                tf->rip -= 2;                           // re-run `int 0x80` on wake
                sched_block_current(tf);                // NORETURN (switches away)
                ret = 0;                                 // not reached
            }
        }
    }
    // Syscalls 65..79 (clocks, nanosleep, syslog, statfs, sync, rename, link,
    // truncate, rmdir, access, umask, uname, poll, sysinfo): kernel/kiface.c.
    // nanosleep and poll may block (rip rewound like SYS_SLEEP).
    else if (num >= SYS_CLOCK_GETTIME && num <= SYS_SYSINFO) {
        extern int kiface_syscall(trapframe_t*, uint64_t, uint64_t*);
        kiface_syscall(tf, num, &ret);
    }
    else if (num >= SYS_LX_BASE && num < SYS_LX_MAX) {
        // [U1] Linux-shaped calls (kernel/syscall_lx.c): 6 arguments, -errno.
        extern uint64_t syscall_lx_dispatch(uint64_t, uint64_t, uint64_t, uint64_t,
                                            uint64_t, uint64_t, uint64_t);
        ret = syscall_lx_dispatch(num, tf->rdi, tf->rsi, tf->rdx, tf->rcx, tf->r8, tf->r9);
    }
    else if (num == SYS_MMAP) {
        // [M40] mmap takes a 6th argument (file offset, r9) for device maps.
        ret = ksys_mmap6(tf->rdi, tf->rsi, (int)tf->rdx, (int)tf->rcx, (int)tf->r8, tf->r9);
    }
    else {
        ret = syscall_dispatch(num, tf->rdi, tf->rsi, tf->rdx, tf->rcx, tf->r8);
    }

    // Save trapframe snapshot into current process (persistent copy)
    if (cur && cur->tf) {
        const uint8_t* s = (const uint8_t*)tf;
        uint8_t* d = (uint8_t*)cur->tf;
        for (int i = 0; i < (int)sizeof(trapframe_t); i++) d[i] = s[i];
        cur->tf->rax = ret; // patch return value
    }
    if (cur) cur->sig_rerun_eintr = 0;

    return ret;
}
