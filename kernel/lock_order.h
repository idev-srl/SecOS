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
#define LK_K10_EXEC        10   /* reserved, unused: C6 made exec staging per
                                   call (no g_spawn_buf), so no exec_lock      */
#define LK_K11_SYSROOT     11   /* sysroot_lock, pkg_lock (C6; never nested)    */
#define LK_K20_VFS_NS      20   /* vfs_ns_lock: mount/umount/remount (C1)       */
#define LK_K25_VFS_APPEND  25   /* vfs_append_lock: O_APPEND/truncate fallback
                                   for an FS without ->append; the FS (K30)
                                   is called inside it (C1)                     */
#define LK_K30_FS          30   /* one per FS driver instance (C2-C4). ramfs:
                                   ramfs_fs_lock() (C2); inside it only the
                                   page-cache index (S40) and the heap         */
#define LK_K35_USB_ENUM    35   /* xhci_enum_lock: USB enumeration incl. hubs;
                                   registers usbN disks and reads them (B3)     */
#define LK_K40_BLKDEV      40   /* blkdev->lock on the base disk; a partition
                                   uses its parent's (fs/block.h, B3)           */
#define LK_KLOCK_MAX       99   /* every klock level is <= this                 */

/* ---- S/N: CPU-owned spinlocks (arch/x86/spinlock.h) ----------------------- */
#define LK_S10_TTY         110  /* tty_con_lock (console ldisc, M51 C5) and
                                   g_pty_lock (every pty): never nested. Inside:
                                   kbd S50, con_lock (echo), proc S30 (console
                                   ISIG posts). Never across cpu_wait_irq, a
                                   user copy or the USB HID poll; IRQ/HID
                                   context never takes it (lock-free ISIG
                                   snapshot + reply ring, kernel/tty.c)        */
#define LK_S12_HID         112  /* hid_lock (drivers/usb_hid.c): HID slots; the
                                   poll only trylocks it and delivers reports
                                   under it (input S15, proc S30, kbd S50,
                                   xhci S45 nest inside) (B3)                   */
#define LK_S15_SUBSYS      115  /* procfs g_proc_lock, g_sysfs_lock,
                                   g_input_lock, g_pipe_lock, g_ipc_lock,
                                   driver_if_lock (B1), g_devfs_lock (C2: only
                                   the block-node append and node chmod/chown;
                                   the block registry S64 nests inside)        */
#define LK_N0_NET_TICK     116  /* M47 net locks, net/net_lock.h                */
#define LK_N1_NET_RX       117
#define LK_N2_NET_STACK    118
#define LK_N3_NET_TX       119
#define LK_S20_MOUNT       120  /* g_mount_lock: mount table only               */
#define LK_S30_PROC        130  /* proc_lock: process table, refcounts, creds   */
#define LK_S35_MM          135  /* p->mm_lock (B1, per process). Since C2 it
                                   also publishes p->cwd and what each p->fds[]
                                   entry is: the owner stores under it, procfs
                                   and sysfs read under proc_lock -> mm_lock   */
#define LK_S40_PAGECACHE   140  /* pc_lock: page-cache index only. Since B2 no
                                   FS or block call runs under it (misses are
                                   filled unlocked), so it nests INSIDE the FS
                                   (K30) and block (K40/S45) locks, never above
                                   them; only the PMM (S75) is taken inside.
                                   (B3 deleted block_io_lock, the old S42.)    */
#define LK_S45_XHCI        145  /* xhci_lock: one xHCI transfer/command (B3);
                                   HID only trylocks it                         */
#define LK_S50_KBD         150  /* kbd_lock. con_lock (drivers/vt.c) and the
                                   klog/debugcon line locks (kernel/klog.c) are
                                   S50 too but are raw, bounded, recursive
                                   output locks: lockdep reports THROUGH them,
                                   so they are deliberately not tracked.
                                   con_lock also guards the KD text/graphics
                                   mode (drivers/fbdev.c, C6).                  */
#define LK_S60_KSTACK      160  /* kstack_lock, g_pm_lock, kvmm_lock (future)   */
#define LK_S62_TLB         162  /* tlb_lock: shootdowns are issued from inside
                                   kstack_lock / g_pm_lock sections (plan: S60) */
#define LK_S64_LEAF        164  /* kclock, csprng, elf-revoke (C6, writers
                                   only): leaf locks (plan: S60)                */
#define LK_S70_HEAP        170  /* heap_lock (expand_heap calls the PMM)        */
#define LK_S75_PMM         175  /* pmm_lock: innermost                          */

#endif /* LOCK_ORDER_H */
