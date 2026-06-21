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

#ifdef SYSCALL_DEBUG
#include "terminal.h"
#endif

extern uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4);

uint64_t syscall_handler(trapframe_t* tf) {
    process_t* cur = sched_get_current();
    uint64_t num = tf->rax;

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
    if (num == SYS_EXIT) {
        if (cur) cur->exit_code = (int)tf->rdi; // [M16] carry the user exit status
        sched_exit_current(tf);
        return 0; // not reached when a switch occurs
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
        }
    }
    else
    // [M16] SYS_WAIT: blocking wait for a child, returning its exit status.
    // If the child is still running, block the caller (rewind rip so the syscall
    // re-runs on wake) instead of busy-polling. A woken waiter finds its result
    // pre-delivered (wait_ready) because the child may already be reaped.
    if (num == SYS_WAIT) {
        extern process_t* process_find_by_pid(uint32_t);
        if (cur && cur->wait_ready) {
            cur->wait_ready = 0;
            ret = (uint64_t)(int64_t)cur->wait_result;
        } else {
            process_t* child = process_find_by_pid((uint32_t)tf->rdi);
            if (!child) {
                ret = (uint64_t)(int64_t)-1;            // no such pid
            } else if (child->state == PROC_ZOMBIE) {
                ret = (uint64_t)(int64_t)child->exit_code; // already exited
                child->collected = 1;                      // [M39] parent reaped it -> reapable
            } else if (cur && signal_pending(cur)) {
                ret = (uint64_t)(int64_t)(-4);          // [M30] EINTR: a signal is pending
            } else if (cur) {
                cur->wait_pid = (int)tf->rdi;
                cur->wait_ready = 0;
                tf->rip -= 2;                           // re-run `int 0x80` on wake
                sched_block_current(tf);                // NORETURN (switches away)
                ret = 0;                                 // not reached
            } else {
                ret = (uint64_t)(int64_t)-1;
            }
        }
    }
    // [M39] SYS_WAITANY: waitpid(-1) — reap ANY child (bash's wait_for loop uses
    // it). Returns the child's pid (status written to *rdi), 0 if WNOHANG and no
    // child has exited yet, or -ECHILD(-10) if the caller has no children.
    else if (num == SYS_WAITANY) {
        extern process_t* process_find_zombie_child(uint32_t);
        extern int process_has_children(uint32_t);
        int* ustatus = (int*)tf->rdi;
        int wnohang  = (int)tf->rsi & 1;     // WNOHANG bit
        if (!cur) { ret = (uint64_t)(int64_t)-10; }
        else {
            process_t* z = process_find_zombie_child(cur->pid);
            if (z) {
                int code = z->exit_code;
                z->collected = 1;            // reapable now
                if (ustatus && user_range_valid(ustatus, sizeof(int)))
                    copy_to_user(ustatus, &code, sizeof(int));
                ret = (uint64_t)(int64_t)(int)z->pid;
            } else if (!process_has_children(cur->pid)) {
                ret = (uint64_t)(int64_t)-10;        // ECHILD
            } else if (wnohang) {
                ret = 0;                              // children alive, none reaped yet
            } else if (signal_pending(cur)) {
                ret = (uint64_t)(int64_t)-4;          // EINTR
            } else {
                cur->wait_pid = -2;                   // [M39] wait-any sentinel
                tf->rip -= 2;                         // re-run on wake
                sched_block_current(tf);              // NORETURN
                ret = 0;
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
            if (n == 0 && cur && signal_pending(cur)) {
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
        } else if (cur && signal_pending(cur)) {
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
    // [M25] SYS_READ / SYS_WRITE on a pipe may block. ksys_read/ksys_write return
    // -2 ("would block") when the pipe is empty (read) or full (write) with the
    // peer still open; arm wait_pipe and re-run the syscall on wake. EOF/EPIPE
    // (0 / -1) and all non-pipe reads/writes fall straight through.
    else if (num == SYS_READ || num == SYS_WRITE) {
        ret = syscall_dispatch(num, tf->rdi, tf->rsi, tf->rdx, tf->rcx, tf->r8);
        if (ret == (uint64_t)(int64_t)(-2) && cur) {
            int fd = (int)tf->rdi;
            if (signal_pending(cur)) {
                ret = (uint64_t)(int64_t)(-4);          // [M30] EINTR
            } else if (fd >= 0 && fd < 32 && cur->fds[fd].used && cur->fds[fd].is_pipe) {
                cur->wait_pipe = cur->fds[fd].inode;
                tf->rip -= 2;                           // re-run `int 0x80` on wake
                sched_block_current(tf);                // NORETURN (switches away)
                ret = 0;                                 // not reached
            }
        }
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

    return ret;
}
