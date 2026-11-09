/*
 * udrv.h — Driver Space v2: PCI functions driven from ring 3 (Phase 5, M57).
 *
 * The ABI (SYS_UDRV, the structs, the claim fd) is user/include/secos/udrv.h.
 * Design: docs/PHASE5_PLAN.md D5-D10, devlog docs/devlog/M57-udrv-core.md.
 *
 * Policy, from the boot command line (read once by udrv_init):
 *   udrv=<bb:dd.f|vvvv:dddd>[,...]  the PCI functions given to ring 3. They are
 *                    hidden from the in-kernel probes (pci_find*), left with
 *                    bus mastering off and detached from the IOMMU (blocked)
 *                    until a driver claims them. No runtime unbind.
 *   udrv.unsafe      development only: DMA and bus mastering for a ring-3
 *                    driver without an IOMMU (IOVA = phys) and without
 *                    interrupt remapping. Loud warning; /proc/udrv TAINTED.
 *   udrv.noir        allow ring-3 DMA with DMA remapping but no interrupt
 *                    remapping (tests until M56; logged).
 *   udrv.watchdog=kill  [M58] kill a network driver whose TX makes no
 *                    progress for 5 s (default: log "[UDRV] ethN stalled").
 * Without both an active IOMMU covering the device and interrupt remapping (or
 * an override) a claim gets MMIO only: DMA_ALLOC is -ENODEV, BUSMASTER and
 * IRQ_SETUP are -EPERM (on QEMU an MSI is a bus-master write).
 *
 * A DMA fault from a claimed device kills its driver (SIGKILL, logged); after
 * UDRV_QUAR_N such kills within UDRV_QUAR_WINDOW_MS the device is quarantined
 * (bus mastering off, every later claim -EIO).
 *
 * Locking (kernel/lock_order.h): g_udrv_lock (K45 klock) serializes claims,
 * ops and teardown; each device's event lock (S25) guards the IRQ state and
 * the owner pointer (taken in IRQ context); the claim-fd pool lock is S24.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef UDRV_H
#define UDRV_H
#include <stdint.h>

struct process;

/* Boot: parse udrv=, reserve and quiesce the named functions. Call after
 * iommu_init() and before any in-kernel driver probes. Logs [UDRV] lines;
 * silent when udrv= is absent. */
void udrv_init(void);

/* SYS_UDRV body: op/handle/arg/size from the trapframe. -errno on failure. */
long udrv_syscall(uint64_t op, uint64_t handle, uint64_t arg, uint64_t size);

/* Every claim of p ends (exit, kill, exec, destroy). Idempotent; a lock-free
 * check first, so it is cheap for a process that owns nothing. Must be called
 * with no spinlock held. */
void udrv_release_all(struct process* p);

/* Claim fds (an inode-tagged fd kind, like evdev clients). */
int  udrv_is_fd(const void* ino);
void udrv_fd_ref(void* ino);
void udrv_fd_unref(void* ino);
int  udrv_fd_read(struct process* c, void* ino, void* ubuf, int len, int nonblock);
int  udrv_fd_write(struct process* c, void* ino, const void* ubuf, int len);
int  udrv_fd_poll(void* ino);

/* [M58] 1 when udrv= gave a network-class function to ring 3 (the default
 * /etc/inittab then starts its driver, kernel/sysroot.c). */
int  udrv_has_net(void);

/* /proc/udrv: one line per emit call. */
void udrv_generate(void (*emit)(void* ctx, const char* line), void* ctx);

#endif /* UDRV_H */
