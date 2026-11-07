/*
 * klock.h — task-owned kernel locks (M50). See docs/BKL_REMOVAL_PLAN.md §3.1
 * and docs/devlog/M50-lock-core.md.
 *
 * A klock is the BKL machinery made generic: it belongs to a TASK (the CPU's
 * current process, or the CPU itself before the scheduler runs on it), not to
 * a CPU, and a waiter spins with interrupts ON whenever its caller had them on,
 * so a CPU waiting for a long section elsewhere keeps taking its timer ticks
 * and answering TLB shootdowns. It never sleeps: ring 0 is not preemptible,
 * every disk I/O is polled and a blocked syscall rewinds and re-runs, so no
 * critical section ever has to block. klock_lock() returns with the caller's
 * RFLAGS.IF exactly as it was (the M48 bkl_acquire returned IF=0 after a
 * contended wait).
 *
 * Usage (one static lock per driver instance, level from lock_order.h):
 *
 *     static klock_t g_ext2_lock = KLOCK_INIT(LK_K30_FS, "ext2");
 *
 *     int ext2_op(...) {
 *         klock_lock(&g_ext2_lock);
 *         int r = ext2_op_locked(...);
 *         klock_unlock(&g_ext2_lock);
 *         return r;
 *     }
 *     static int ext2_op_locked(...) { KLOCK_ASSERT_HELD(&g_ext2_lock); ... }
 *
 * Rules (-DSECOS_LOCKDEP panics on a violation; a release build logs a
 * one-shot "[LOCK] BUG" for the cheap ones marked *):
 *   - never in IRQ context*                       (per-CPU irq_depth)
 *   - never while holding a spinlock               (all K levels < all S)
 *   - only in increasing level order; a non-recursive klock taken twice by the
 *     same task is a bug*
 *   - never held across switch_to, sched_block_current, cpu_wait_irq or the
 *     return to ring 3* — except the BKL, which switch_to drops (bkl.h) and
 *     which may be held across cpu_wait_irq while bkl=on
 *   - never held across copy_to/from_user (a fault on a file-backed mmap
 *     re-enters the filesystem) — except the BKL
 *   - unlock only by the owner*
 * While a task holds any klock its CPU does not preempt it (only the idle task
 * can be preempted in ring 0: the tick and the reschedule IPI check
 * klock_depth()).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KLOCK_H
#define KLOCK_H
#include <stdint.h>
#include "lock_order.h"

struct process;

typedef struct klock {
    void* volatile owner;      /* owning task / cpu_t, NULL = free           */
    uint32_t depth;            /* recursion depth (owner only)               */
    uint16_t level;            /* LK_K* (lock_order.h)                       */
    uint8_t  recursive;        /* 1 = the owner may take it again (BKL only) */
    uint8_t  reg;              /* /proc/lockstat slot + 1 (0 = not yet)      */
    const char* name;
    uint64_t t_acq;            /* TSC at the outermost acquisition           */
    /* /proc/lockstat (written by the owner / the winning waiter only) */
    uint64_t n_acq, n_contended, max_wait_cyc;
} klock_t;

#define KLOCK_INIT(lvl, nm)           { 0, 0, (lvl), 0, 0, (nm), 0, 0, 0, 0 }
#define KLOCK_INIT_RECURSIVE(lvl, nm) { 0, 0, (lvl), 1, 0, (nm), 0, 0, 0, 0 }

/* The klocks a task (or a CPU before its scheduler runs) holds. Embedded in
 * process_t (`klk`) and cpu_t (`klk`). */
#define KLOCK_STACK 8
typedef struct klock_task {
    uint32_t depth;                 /* distinct klocks held (recursion = 1);
                                       held[0..min(depth,KLOCK_STACK)) valid */
    uint32_t pad;
    klock_t* held[KLOCK_STACK];     /* in acquisition order                   */
} klock_task_t;

static inline void klock_task_reset(klock_task_t* t) {
    t->depth = 0; t->pad = 0;
    for (int i = 0; i < KLOCK_STACK; i++) t->held[i] = 0;
}

void klock_init(klock_t* l, uint16_t level, const char* name, int recursive);

/* Take / release. klock_lock spins with interrupts on iff the caller had them
 * on; klock_lock_irq_on always spins with them on (for entry paths that hold
 * nothing, e.g. a #PF from ring 3 entered with IF=0). Both return with the
 * caller's IF unchanged. */
void klock_lock(klock_t* l);
void klock_lock_irq_on(klock_t* l);
int  klock_trylock(klock_t* l);        /* 1 = taken (or re-entered, if recursive) */
void klock_unlock(klock_t* l);
/* Release `l` on the way back to ring 3: also reports any other klock the task
 * still holds (and, with lockdep, any spinlock its CPU holds). */
void klock_unlock_to_user(klock_t* l);

/* Does the calling task hold `l`? */
int  klock_held(const klock_t* l);
/* Klocks held by `p` (NULL = the calling task / CPU). */
uint32_t klock_depth(const struct process* p);

/* For the BKL wrappers (kernel/bkl.c) only. */
uint32_t klock_suspend(klock_t* l);                /* drop all levels -> depth */
void     klock_resume(klock_t* l, uint32_t depth); /* take back `depth` levels */
void     klock_drop_owner(klock_t* l, struct process* p);  /* switch_to       */
int      klock_owned_by(const klock_t* l, const struct process* p);
const char* klock_owner_desc(const klock_t* l, uint64_t* id);   /* "pid"/"cpu" */

/* Wait for the next interrupt: `sti; hlt`, returning with IF on. Replaces every
 * raw `sti; hlt` in kernel wait loops; lockdep checks that the caller holds no
 * spinlock and no klock other than the BKL. */
void cpu_wait_irq(void);

/* Hardware-interrupt nesting (called by the IRQ stubs in idt_asm.asm). */
void irq_enter(void);
void irq_exit(void);
int  in_irq(void);

/* switch_to(): `prev` leaves this CPU (after the BKL was dropped for it).
 * Checks that nothing is held and resets this CPU's IRQ nesting (a switch
 * from the tick never returns through irq_exit). */
void klock_on_switch(struct process* prev, struct process* idle);

/* Return-to-ring-3 / syscall-exit check: the calling task holds no klock and
 * its CPU no spinlock. Release builds log once; lockdep panics. */
void klock_check_user_return(const char* where);

/* Print the klocks the calling task holds (+ this CPU's spinlocks with
 * lockdep) to debugcon: the panic path calls it. */
void klock_dump_held(void);

/* One-shot "[LOCK] BUG <what> <name>" (release) / lockdep report. */
void klock_bug(const char* what, const char* name);

#ifdef SECOS_LOCKDEP
#define KLOCK_ASSERT_HELD(l) \
    do { if (!klock_held(l)) klock_bug("klock not held in " __FILE__, (l)->name); } while (0)
#else
#define KLOCK_ASSERT_HELD(l) do { (void)(l); } while (0)
#endif

#endif /* KLOCK_H */
