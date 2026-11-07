/*
 * lock_order.h — the kernel lock hierarchy (M50). See docs/BKL_REMOVAL_PLAN.md
 * section 3.2 and docs/devlog/M50-lock-core.md.
 *
 * Every lock has a level. A CPU (spinlocks) or a task (klocks) may only take a
 * lock whose level is STRICTLY GREATER than the level of every lock it already
 * holds. Two different locks of the same level are never nested; taking the
 * same lock twice is recursion (allowed only for the BKL). -DSECOS_LOCKDEP
 * checks all of this at run time (kernel/lockdep.c).
 *
 * All klocks (K, task-owned, spin with IF on, kernel/klock.h) come before all
 * spinlocks (S/N, CPU-owned, short sections). A task never holds two
 * FS-instance (K30) locks at once: the VFS drops one before entering another
 * mount.
 *
 * Levels leave gaps so a later phase can slot a lock in without renumbering.
 * Where the code nests two locks that the plan put on one level, the inner one
 * got its own sub-level below (noted per entry); the order is the one the code
 * actually uses at d062882.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LOCK_ORDER_H
#define LOCK_ORDER_H

/* An unclassified lock: lockdep only checks that it is not held across
 * switch_to / cpu_wait_irq / the return to ring 3, never its order. */
#define LK_NONE            0xFFFF

/* ---- K: task-owned klocks (kernel/klock.h) -------------------------------- */
#define LK_K0_BKL          0    /* the BKL (recursive): fallback only           */
#define LK_K10_EXEC        10   /* exec_lock: g_spawn_buf / exec staging (B5)   */
#define LK_K11_SYSROOT     11   /* sysroot_lock, pkg_lock (C6)                  */
#define LK_K20_VFS_NS      20   /* vfs_ns_lock: mount/umount/remount (C1)       */
#define LK_K30_FS          30   /* one per FS driver instance (C2-C4)           */
#define LK_K40_BLKDEV      40   /* blkdev->lock on the base disk (B3)           */
#define LK_KLOCK_MAX       99   /* every klock level is <= this                 */

/* ---- S/N: CPU-owned spinlocks (arch/x86/spinlock.h) ----------------------- */
#define LK_S10_TTY         110  /* tty_con_lock (C5), g_pty_lock                */
#define LK_S15_SUBSYS      115  /* procfs g_proc_lock, g_sysfs_lock,
                                   g_input_lock, g_pipe_lock, g_ipc_lock,
                                   driver_if_lock (B1)                          */
#define LK_N0_NET_TICK     116  /* M47 net locks, net/net_lock.h                */
#define LK_N1_NET_RX       117
#define LK_N2_NET_STACK    118
#define LK_N3_NET_TX       119
#define LK_S20_MOUNT       120  /* g_mount_lock: mount table only               */
#define LK_S30_PROC        130  /* proc_lock: process table, refcounts, creds   */
#define LK_S35_MM          135  /* p->mm_lock (B1, per process)                 */
#define LK_S40_PAGECACHE   140  /* pc_lock: page-cache index                    */
#define LK_S42_BLOCK_IO    142  /* block_io_lock (fs/block.c), deleted by B3.
                                   Not in the plan table: today the page cache
                                   still calls ops->read under pc_lock (B2), so
                                   block I/O nests inside S40.                  */
#define LK_S45_XHCI        145  /* xhci_lock (B3)                               */
#define LK_S50_KBD         150  /* kbd_lock. con_lock (drivers/vt.c) and the
                                   klog/debugcon line locks (kernel/klog.c) are
                                   S50 too but are raw, bounded, recursive
                                   output locks: lockdep reports THROUGH them,
                                   so they are deliberately not tracked.        */
#define LK_S60_KSTACK      160  /* kstack_lock, g_pm_lock, kvmm_lock (future)   */
#define LK_S62_TLB         162  /* tlb_lock: shootdowns are issued from inside
                                   kstack_lock / g_pm_lock sections (plan: S60) */
#define LK_S64_LEAF        164  /* kclock, csprng: leaf locks (plan: S60)       */
#define LK_S70_HEAP        170  /* heap_lock (expand_heap calls the PMM)        */
#define LK_S75_PMM         175  /* pmm_lock: innermost                          */

#endif /* LOCK_ORDER_H */
