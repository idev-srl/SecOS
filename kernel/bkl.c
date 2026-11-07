/*
 * bkl.c — the kernel lock (M48). See bkl.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "bkl.h"
#include "percpu.h"
#include "spinlock.h"
#include "debugcon.h"

static void* volatile g_bkl_owner;   /* owning task (or cpu_t before the scheduler) */
static uint32_t g_bkl_depth;         /* recursion depth, touched by the owner only  */

static inline void* bkl_self(void) {
    cpu_t* c = this_cpu();
    return c->current ? (void*)c->current : (void*)c;
}

static void bkl_acquire(int spin_irq_on) {
    void* me = bkl_self();
    if (g_bkl_owner == me) { g_bkl_depth++; return; }
    for (;;) {
        if (!g_bkl_owner && __sync_bool_compare_and_swap(&g_bkl_owner, (void*)0, me)) {
            g_bkl_depth = 1;
            return;
        }
        if (spin_irq_on) __asm__ volatile ("sti" ::: "memory");
        while (g_bkl_owner) cpu_relax_poll();
        if (spin_irq_on) __asm__ volatile ("cli" ::: "memory");
    }
}

void bkl_lock(void) { bkl_acquire(0); }
void bkl_lock_enable_irq(void) { bkl_acquire(1); }

static void bkl_release(void) {
    g_bkl_depth = 0;
    __atomic_store_n(&g_bkl_owner, (void*)0, __ATOMIC_RELEASE);
}

void bkl_unlock(void) {
    if (g_bkl_owner != bkl_self()) {
        static int warned;
        if (!warned) { warned = 1; debugcon_writestring("[BKL] BUG: unlock by a non-owner\n"); }
        return;
    }
    if (--g_bkl_depth == 0) bkl_release();
}

uint32_t bkl_suspend(void) {
    if (g_bkl_owner != bkl_self()) return 0;
    uint32_t d = g_bkl_depth;
    bkl_release();
    return d;
}

void bkl_resume(uint32_t depth) {
    if (!depth) return;
    bkl_acquire(0);
    g_bkl_depth = depth;
}

void bkl_drop_for(struct process* p) {
    if (p && g_bkl_owner == (void*)p) bkl_release();
}

int bkl_held_by(struct process* p) {
    return g_bkl_owner == (p ? (void*)p : bkl_self());
}
