/*
 * syscall_lx.c — [U1] Linux-shaped syscalls (numbers SYS_LX_BASE + Linux nr).
 * SPDX-License-Identifier: MIT
 *
 * Each call takes Linux's arguments and returns what Linux returns (-errno on
 * failure), so musl's translation layer passes it through unchanged
 * (docs/USERLAND_PLAN.md D3). Only the calls U1 needs live here:
 *   arch_prctl(ARCH_SET_FS / ARCH_GET_FS): the user thread pointer (musl sets
 *     it up in every program: TLS, errno, the stack-protector canary);
 *   set_tid_address: stores the pointer, returns the tid (= pid until U9);
 *   exit_group: handled with SYS_EXIT in syscall_trap.c (one thread per process).
 * Every call touches only the caller's own process_t, so none takes a lock
 * (and none takes the BKL, kernel/bkl.c).
 */
#include <stdint.h>
#include "syscall.h"
#include "process.h"
#include "user_copy.h"
#include "trapframe.h"

extern process_t* sched_get_current(void);

#define LX_EPERM   1
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
        c->clear_child_tid = a0;          // meaningful with CLONE_CHILD_CLEARTID (U9)
        r = (uint64_t)c->pid;             // tid == pid: one thread per process
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
