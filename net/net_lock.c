/*
 * net_lock.c — network stack locks (M47). See net_lock.h for the hierarchy.
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 *
 * A net lock is an irqsave spinlock of class LK_N0 + level (kernel/
 * lock_order.h). [M50] The M47 net-only order checker (SECOS_LOCKDEP_LITE)
 * became the generic validator in kernel/lockdep.c, which checks these locks
 * together with every other spinlock and klock.
 */
#include "net_lock.h"

static net_spinlock_t g_stack_lock = NET_SPINLOCK_INIT(NETLK_STACK, "net-stack");

void netlk_init(net_spinlock_t* lk, uint8_t level, const char* name) {
    spin_init_class(&lk->l, (uint16_t)(LK_N0_NET_TICK + level), name);
    lk->level = level;
    lk->name = name;
}

#ifdef SECOS_LOCKDEP
int net_stack_lock_held(void) { return lockdep_spin_held(&g_stack_lock.l); }
#endif

uint64_t netlk_acquire(net_spinlock_t* lk) {
    return spin_lock_irqsave(&lk->l);
}

int netlk_try(net_spinlock_t* lk, uint64_t* flags) {
    uint64_t f;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    if (!spin_trylock(&lk->l)) {
        __asm__ volatile ("push %0; popfq" :: "r"(f) : "memory", "cc");
        return 0;
    }
    *flags = f;
    return 1;
}

void netlk_release(net_spinlock_t* lk, uint64_t flags) {
    spin_unlock_irqrestore(&lk->l, flags);
}

uint64_t net_lock(void) { return netlk_acquire(&g_stack_lock); }
void net_unlock(uint64_t flags) { netlk_release(&g_stack_lock, flags); }
