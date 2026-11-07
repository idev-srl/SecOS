/*
 * bkl.h — the kernel lock (M48), a recursive level-K0 klock since M50.
 *
 * Before SMP was on by default, every syscall ran with interrupts off on the
 * only CPU running user code, so the subsystems written for that model —
 * VFS and every filesystem (ramfs, ext2/ext4, FAT32, exFAT, NTFS, devfs),
 * the TTY layer, credentials, the shared spawn/exec staging buffers, procfs
 * snapshots, driver bindings — were implicitly serialized. With tasks running
 * syscalls on several CPUs at once they are not (two writers of one ramfs
 * file corrupted the heap). Until each of them gets its own locking, one
 * recursive lock serializes kernel work done on behalf of tasks:
 *
 *   - every syscall body (syscall_handler; per syscall: syscall_bkl_mask),
 *   - user-address page faults (demand paging, COW, file-backed mmap),
 *   - process teardown (exit/kill/reap),
 *   - kernel-shell commands (the idle task on the boot CPU).
 *
 * User code still runs in parallel on every CPU; only the kernel side is
 * serialized. Subsystems that already have their own locks (PMM, heap, page
 * cache, pipes, ptys, IPC, block I/O, input, the network stack, the process
 * table) keep them and nest inside the kernel lock; interrupt handlers never
 * take it.
 *
 * Ownership is per TASK (the CPU's current process, or the CPU itself before
 * the scheduler runs), not per CPU (kernel/klock.h): the idle task is
 * preemptible in ring 0, so the scheduler never preempts an idle task that
 * holds a klock, and a task that blocks, exits or is killed while holding the
 * BKL drops it in switch_to(). A blocked task re-runs its syscall from the
 * start, so it never needs the lock back mid-body. It is the only klock that
 * may be held across cpu_wait_irq() and copy_to/from_user.
 *
 * [M50] Boot word `bkl=on|off` (MB2 command line, UEFI load options) sets
 * g_bkl_mode; without one the build default applies (Makefile BKL_DEFAULT,
 * [M52]). `bkl=on` takes the BKL for every syscall, fault, teardown and
 * kernel-shell command, exactly as M48 did: the fallback. `bkl=off` takes it
 * only for the syscalls whose bit is still set in syscall_bkl_mask[] (phase D
 * of docs/BKL_REMOVAL_PLAN.md clears bits one subsystem at a time, see
 * docs/devlog/M52-bkl-removal.md) and at the call sites phase D has not
 * removed yet.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BKL_H
#define BKL_H
#include <stdint.h>
#include "klock.h"

struct process;

/* The lock itself (for KLOCK_ASSERT_HELD(&g_bkl) and /proc/lockstat). */
extern klock_t g_bkl;

/* Take the lock (recursive for the owning task). bkl_lock spins with the
 * interrupt flag as the caller has it; bkl_lock_enable_irq spins with
 * interrupts on (only for callers that hold no spinlock: syscall and
 * user-fault entry), so a CPU waiting for a long syscall elsewhere keeps
 * taking its timer ticks. Both return with the caller's IF unchanged. */
void bkl_lock(void);
void bkl_lock_enable_irq(void);
/* Called by klock.c while a task spins for the BKL with interrupts on:
 * polls the USB HID keyboard (rate-limited, see bkl.c). */
void bkl_wait_poll(void);
void bkl_unlock(void);

/* Release completely / take back `depth` levels: for a kernel-context waiter
 * (the kernel shell waiting for its children) that must not keep the lock
 * while it waits. bkl_suspend returns the depth it released (0 if not owner). */
uint32_t bkl_suspend(void);
void bkl_resume(uint32_t depth);

/* switch_to(): `p` is leaving this CPU for good (blocked/exited/killed):
 * drop whatever it holds. */
void bkl_drop_for(struct process* p);

/* Does `p` (a task, or NULL for "this CPU's current") hold the lock? */
int bkl_held_by(struct process* p);

/* ---- [M50] mode + per-syscall mask ---- */
#define SYSCALL_BKL_NR    256          /* syscall numbers covered by the mask */
#define SYSCALL_BKL_WORDS (SYSCALL_BKL_NR / 64)
extern int g_bkl_mode;                 /* 1 = bkl=on (default), 0 = bkl=off   */
extern uint64_t syscall_bkl_mask[SYSCALL_BKL_WORDS];   /* bit set = takes the BKL */

void bkl_init_mode(void);              /* reads the boot command line         */
int  syscall_needs_bkl(uint64_t num);  /* 1 when mode is on, the bit is set or num is out of range */
uint32_t syscall_bkl_mask_count(void); /* bits set in syscall_bkl_mask        */

/* syscall_handler: take the BKL if `num` needs it (returns 1 then) / release
 * it and check that the task goes back to ring 3 holding no klock. */
int  bkl_syscall_enter(uint64_t num);
void bkl_syscall_exit(int taken);

#endif /* BKL_H */
