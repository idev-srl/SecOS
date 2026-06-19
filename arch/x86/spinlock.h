/*
 * spinlock.h — SMP spinlocks (M29). Single-CPU builds still pay only an
 * uncontended xchg; the irqsave variants also give the old cli/sti mutual
 * exclusion against this CPU's own interrupt handlers.
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef SPINLOCK_H
#define SPINLOCK_H
#include <stdint.h>

typedef struct { volatile uint32_t locked; } spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_init(spinlock_t* l) { l->locked = 0; }

static inline void spin_lock(spinlock_t* l) {
    /* test-and-test-and-set with a pause-spin; acquire ordering via xchg. */
    for (;;) {
        if (__sync_lock_test_and_set(&l->locked, 1) == 0) return;
        while (l->locked) __asm__ volatile ("pause");
    }
}

static inline void spin_unlock(spinlock_t* l) {
    __sync_lock_release(&l->locked);   /* release ordering */
}

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
