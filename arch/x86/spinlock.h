/*
 * spinlock.h — SMP spinlocks (M29). Single-CPU builds still pay only an
 * uncontended xchg; the irqsave variants also give the old cli/sti mutual
 * exclusion against this CPU's own interrupt handlers.
 *
 * [M50] Every lock has a class: SPINLOCK_INIT_CLASS(level, "name") with a level
 * from kernel/lock_order.h. In a normal build the class compiles away (the
 * lock stays one word, the operations stay inline); with -DSECOS_LOCKDEP the
 * operations go through kernel/lockdep.c, which checks the order, recursion,
 * IRQ safety and "held across" rules and keeps /proc/lockstat counters.
 * SPINLOCK_INIT is an unclassified lock (only the "held across" rules apply).
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef SPINLOCK_H
#define SPINLOCK_H
#include <stdint.h>
#include "lock_order.h"
#include "lockdep.h"

#ifdef SECOS_LOCKDEP
typedef struct spinlock {
    volatile uint32_t locked;
    uint16_t level;             /* LK_* (lock_order.h), LK_NONE = unclassified */
    uint8_t  ld_flags;          /* LD_F_* (kernel/lockdep.c)                   */
    uint8_t  reg;               /* /proc/lockstat slot + 1 (0 = not yet)       */
    const char* name;
    volatile uint32_t owner_cpu;/* CPU index + 1 of the holder                 */
    uint32_t pad;
    uint64_t t_acq;             /* TSC at acquisition                          */
    uint64_t n_acq, n_contended, max_wait_cyc;
} spinlock_t;
#define SPINLOCK_INIT_CLASS(lvl, nm) { 0, (lvl), 0, 0, (nm), 0, 0, 0, 0, 0, 0 }
#else
typedef struct spinlock { volatile uint32_t locked; } spinlock_t;
#define SPINLOCK_INIT_CLASS(lvl, nm) { 0 }
#endif

#define SPINLOCK_INIT SPINLOCK_INIT_CLASS(LK_NONE, "unclassified")

static inline void spin_init(spinlock_t* l) { l->locked = 0; }
static inline void spin_init_class(spinlock_t* l, uint16_t level, const char* name) {
    l->locked = 0;
#ifdef SECOS_LOCKDEP
    l->level = level; l->name = name; l->ld_flags = 0; l->owner_cpu = 0;
#else
    (void)level; (void)name;
#endif
}

/* [M48] A CPU waiting for a lock keeps answering TLB shootdowns (see
 * arch/x86/tlb.h): the initiator may hold this very lock with interrupts off
 * and be waiting for our acknowledgement, so waiting here must not depend on
 * the IPI being deliverable. */
extern volatile uint32_t g_tlb_inflight;
void tlb_poll(void);
static inline void cpu_relax_poll(void) {
    if (g_tlb_inflight) tlb_poll();
    __asm__ volatile ("pause");
}

/* The raw operations (lockdep itself builds on these). */
static inline void spin_lock_raw(spinlock_t* l) {
    /* test-and-test-and-set with a pause-spin; acquire ordering via xchg. */
    for (;;) {
        if (__sync_lock_test_and_set(&l->locked, 1) == 0) return;
        while (l->locked) cpu_relax_poll();
    }
}
static inline int spin_trylock_raw(spinlock_t* l) {
    return __sync_lock_test_and_set(&l->locked, 1) == 0;
}
static inline void spin_unlock_raw(spinlock_t* l) {
    __sync_lock_release(&l->locked);   /* release ordering */
}

#ifdef SECOS_LOCKDEP
static inline void spin_lock(spinlock_t* l)    { lockdep_spin_lock(l); }
static inline int  spin_trylock(spinlock_t* l) { return lockdep_spin_trylock(l); }
static inline void spin_unlock(spinlock_t* l)  { lockdep_spin_unlock(l); }
#else
static inline void spin_lock(spinlock_t* l)    { spin_lock_raw(l); }
static inline int  spin_trylock(spinlock_t* l) { return spin_trylock_raw(l); }
static inline void spin_unlock(spinlock_t* l)  { spin_unlock_raw(l); }
#endif

/* Disable local interrupts, then take the lock. Returns the prior RFLAGS so the
 * caller can restore the IF state — this makes a critical section safe both
 * against other CPUs (the lock) and against this CPU's own ISRs (IF cleared). */
static inline uint64_t spin_lock_irqsave(spinlock_t* l) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    spin_lock(l);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t* l, uint64_t flags) {
    spin_unlock(l);
    __asm__ volatile ("push %0; popfq" :: "r"(flags) : "memory", "cc");
}

#endif /* SPINLOCK_H */
