/*
 * syscall_lx.c — [U1] Linux-shaped syscalls (numbers SYS_LX_BASE + Linux nr).
 * SPDX-License-Identifier: MIT
 *
 * Each call takes Linux's arguments and returns what Linux returns (-errno on
 * failure), so musl's translation layer passes it through unchanged
 * (docs/USERLAND_PLAN.md D3). Only the calls U1 needs live here:
 *   arch_prctl(ARCH_SET_FS / ARCH_GET_FS): the user thread pointer (musl sets
 *     it up in every program: TLS, errno, the stack-protector canary);
 *   set_tid_address: stores the pointer (CLONE_CHILD_CLEARTID), returns the tid;
 *   exit_group: handled with SYS_EXIT in syscall_trap.c.
 * [U9] Threads: gettid, tkill, tgkill, set_robust_list/get_robust_list here;
 * clone, exit and futex need the trapframe (syscall_trap.c).
 * None takes the BKL (kernel/bkl.c); tkill/tgkill take proc_lock through the
 * process lookups and the credential check.
 */
#include <stdint.h>
#include "syscall.h"
#include "process.h"
#include "user_copy.h"
#include "trapframe.h"
#include "signal.h"
#include "cred.h"
#include "thread.h"

extern process_t* sched_get_current(void);

#define LX_EPERM   1
#define LX_ESRCH   3
#define LX_EFAULT  14
#define LX_EINVAL  22
#define LX_ENOSYS  38

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

static long lx_arch_prctl(process_t* c, uint64_t code, uint64_t addr) {
    switch (code) {
    case ARCH_SET_FS:
        // Linux: -EPERM for an address outside the user half. Any user-half
        // value is fine: %fs-relative accesses are ordinary user accesses
        // (paging decides), and the value is canonical so wrmsr cannot #GP.
        if (addr >= USER_ADDR_MAX) return -LX_EPERM;
        c->fs_base = addr;
        arch_set_fs_base(addr);
        return 0;
    case ARCH_GET_FS:
        if (copy_to_user((void*)addr, &c->fs_base, sizeof(c->fs_base)) != 0) return -LX_EFAULT;
        return 0;
    case ARCH_SET_GS:      // no user GS base on SecOS (the kernel does not swapgs)
    case ARCH_GET_GS:
    default:
        return -LX_EINVAL;
    }
}

// [U9] tkill/tgkill: a signal to one thread. tgid = -1: tkill (any group).
static long lx_tgkill(process_t* c, int64_t tgid, int64_t tid, int64_t sig) {
    if (sig < 0 || sig >= NSIG) return -LX_EINVAL;
    if (tid <= 0 || (tgid != -1 && tgid <= 0)) return -LX_EINVAL;
    process_t* t = process_get_by_pid((uint32_t)tid);
    if (!t) return -LX_ESRCH;
    long r = 0;
    uint64_t fl = process_lock_irqsave();
    if ((tgid != -1 && t->tgid != (uint32_t)tgid) || t->state == PROC_ZOMBIE) r = -LX_ESRCH;
    else if (!cred_may_signal(c, t, (int)sig)) r = -LX_EPERM;
    process_unlock_irqrestore(fl);
    if (r == 0 && sig != 0) {
        // SIGKILL / SIGSTOP / SIGCONT act on the whole process (Linux).
        if (sig == SIGKILL || sig == SIGSTOP || sig == SIGCONT) signal_post(t, (int)sig);
        else if (signal_post_thread(t, (int)sig) != 0) r = -LX_ESRCH;
    }
    process_put(t);
    return r;
}

// set_robust_list(head, len): the head is stored (musl registers it for
// robust mutexes only). The list is not walked at thread exit (yet): a robust
// mutex held by a dying thread is not marked FUTEX_OWNER_DIED.
static long lx_set_robust_list(process_t* c, uint64_t head, uint64_t len) {
    if (len != 24) return -LX_EINVAL;               // sizeof(struct robust_list_head)
    c->robust_list = head;
    return 0;
}
static long lx_get_robust_list(process_t* c, int64_t tid, uint64_t uhead, uint64_t ulen) {
    process_t* t = (tid == 0 || (uint64_t)tid == c->pid) ? process_get(c) : process_get_by_pid((uint32_t)tid);
    if (!t) return -LX_ESRCH;
    long r = 0;
    if (t->tgid != c->tgid) r = -LX_EPERM;          // only our own threads
    else {
        uint64_t h = t->robust_list, l = 24;
        if (copy_to_user((void*)uhead, &h, 8) != 0 || copy_to_user((void*)ulen, &l, 8) != 0) r = -LX_EFAULT;
    }
    process_put(t);
    return r;
}

// [U10] Calls served by other files: the socket family (kernel/lx_net.c),
// eventfd / timerfd / epoll / interval timers (kernel/lx_event.c). Each
// returns 1 and sets *ret when it owns `num`.
extern int lx_net_syscall(trapframe_t* tf, uint64_t num, uint64_t* ret);
extern int lx_event_syscall(trapframe_t* tf, uint64_t num, uint64_t* ret);

uint64_t syscall_lx_dispatch(trapframe_t* tf, uint64_t num) {
    process_t* c = sched_get_current();
    if (!c) return (uint64_t)(int64_t)-LX_ENOSYS;
    uint64_t a0 = tf->rdi, a1 = tf->rsi;
    uint64_t r;
    // Wait conditions armed by a previous, now re-run, attempt are stale.
    c->sleep_until = 0; c->wait_pipe = NULL;
    switch (num) {
    case SYS_LX_ARCH_PRCTL:
        r = (uint64_t)lx_arch_prctl(c, a0, a1);
        break;
    case SYS_LX_SET_TID_ADDRESS:
        c->clear_child_tid = a0;          // zeroed + futex-woken at thread exit
        r = (uint64_t)c->pid;             // the tid
        break;
    case SYS_LX_GETTID:
        r = (uint64_t)c->pid;
        break;
    case SYS_LX_TKILL:
        r = (uint64_t)lx_tgkill(c, -1, (int64_t)a0, (int64_t)a1);
        break;
    case SYS_LX_TGKILL:
        r = (uint64_t)lx_tgkill(c, (int64_t)a0, (int64_t)a1, (int64_t)tf->rdx);
        break;
    case SYS_LX_SET_ROBUST_LIST:
        r = (uint64_t)lx_set_robust_list(c, a0, a1);
        break;
    case SYS_LX_GET_ROBUST_LIST:
        r = (uint64_t)lx_get_robust_list(c, (int64_t)a0, a1, tf->rdx);
        break;
    default:
        if (lx_net_syscall(tf, num, &r) || lx_event_syscall(tf, num, &r)) break;
        r = (uint64_t)(int64_t)-LX_ENOSYS;
        break;
    }
    // A call that returns has completed: its lx_wait deadline is over.
    c->lx_wait_rip = 0;
    c->sleep_until = 0;
    return r;
}
