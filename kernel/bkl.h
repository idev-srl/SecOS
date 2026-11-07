/*
 * bkl.h — the kernel lock (M48).
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
 *   - every syscall body (syscall_handler),
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
 * the scheduler runs), not per CPU: the idle task is preemptible in ring 0,
 * so the scheduler never preempts an idle task that holds the lock, and a
 * task that blocks, exits or is killed while holding it drops it in
 * switch_to(). A blocked task re-runs its syscall from the start, so it never
 * needs the lock back mid-body.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BKL_H
#define BKL_H
#include <stdint.h>

struct process;

/* Take the lock (recursive for the owning task). Spins with the interrupt
 * flag as the caller has it; bkl_lock_enable_irq() spins with interrupts on
 * (only for callers that hold no spinlock: syscall and user-fault entry), so a
 * CPU waiting for a long syscall elsewhere keeps taking its timer ticks. */
void bkl_lock(void);
void bkl_lock_enable_irq(void);
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

#endif /* BKL_H */
