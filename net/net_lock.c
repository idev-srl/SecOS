/*
 * net_lock.c — network stack locks + the optional lock-order checker (M47).
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 *
 * See net_lock.h for the hierarchy. Without SECOS_LOCKDEP_LITE a net lock is a
 * plain irqsave spinlock; with it, each CPU keeps a bitmask + stack of the net
 * lock levels it holds and any violation of the order panics with the lock's
 * name, so an inversion shows up on the first run instead of as a rare SMP
 * deadlock.
 */
#include "net_lock.h"

static net_spinlock_t g_stack_lock = { SPINLOCK_INIT, NETLK_STACK, "net-stack" };

void netlk_init(net_spinlock_t* lk, uint8_t level, const char* name) {
    spin_init(&lk->l);
    lk->level = level;
    lk->name = name;
}

#ifdef SECOS_LOCKDEP_LITE
#include "percpu.h"
#include "panic.h"
#include "debugcon.h"

#define LD_DEPTH 8
static struct {
    uint32_t mask;                       /* bit n = level n held */
    uint8_t  depth;
    uint8_t  stack[LD_DEPTH];            /* levels in acquisition order */
} g_ld[SMP_MAX_CPUS];

void netlk_bug(const char* what, const char* where) {
    debugcon_writestring("[LOCKDEP] net: ");
    debugcon_writestring(what);
    debugcon_writestring(" (");
    debugcon_writestring(where);
    debugcon_writestring(")\n");
    kernel_panic("net lockdep violation", __FILE__, __LINE__);
    for (;;) __asm__ volatile ("cli; hlt");
}

static uint32_t ld_cpu(void) {
    uint32_t i = this_cpu()->index;
    return i < SMP_MAX_CPUS ? i : 0;
}

static void ld_before(net_spinlock_t* lk) {
    uint32_t c = ld_cpu();
    uint32_t bit = 1u << lk->level;
    if (g_ld[c].mask & bit) netlk_bug("recursive acquisition", lk->name);
    if (g_ld[c].mask & ~(bit - 1u)) netlk_bug("lock order inversion", lk->name);
}

static void ld_acquired(net_spinlock_t* lk) {
    uint32_t c = ld_cpu();
    if (g_ld[c].depth >= LD_DEPTH) netlk_bug("lock stack overflow", lk->name);
    g_ld[c].stack[g_ld[c].depth++] = lk->level;
    g_ld[c].mask |= 1u << lk->level;
}

static void ld_release(net_spinlock_t* lk) {
    uint64_t rf;
    __asm__ volatile ("pushfq; pop %0" : "=r"(rf));
    if (rf & (1u << 9)) netlk_bug("held with interrupts enabled", lk->name);
    uint32_t c = ld_cpu();
    if (!g_ld[c].depth || g_ld[c].stack[g_ld[c].depth - 1] != lk->level)
        netlk_bug("out-of-order release", lk->name);
    g_ld[c].depth--;
    g_ld[c].mask &= ~(1u << lk->level);
}

int net_stack_lock_held(void) {
    uint64_t rf;
    __asm__ volatile ("pushfq; pop %0" : "=r"(rf));
    if (rf & (1u << 9)) return 0;        /* a holder always runs with IF=0 */
    return (g_ld[ld_cpu()].mask & (1u << NETLK_STACK)) != 0;
}
#endif

uint64_t netlk_acquire(net_spinlock_t* lk) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
#ifdef SECOS_LOCKDEP_LITE
    ld_before(lk);
#endif
    spin_lock(&lk->l);
#ifdef SECOS_LOCKDEP_LITE
    ld_acquired(lk);
#endif
    return flags;
}

int netlk_try(net_spinlock_t* lk, uint64_t* flags) {
    uint64_t f;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory");
#ifdef SECOS_LOCKDEP_LITE
    ld_before(lk);
#endif
    if (__sync_lock_test_and_set(&lk->l.locked, 1) != 0) {
        __asm__ volatile ("push %0; popfq" :: "r"(f) : "memory", "cc");
        return 0;
    }
#ifdef SECOS_LOCKDEP_LITE
    ld_acquired(lk);
#endif
    *flags = f;
    return 1;
}

void netlk_release(net_spinlock_t* lk, uint64_t flags) {
#ifdef SECOS_LOCKDEP_LITE
    ld_release(lk);
#endif
    spin_unlock_irqrestore(&lk->l, flags);
}

uint64_t net_lock(void) { return netlk_acquire(&g_stack_lock); }
void net_unlock(uint64_t flags) { netlk_release(&g_stack_lock, flags); }
