/*
 * tlb.c — SMP TLB shootdown (M48). See tlb.h for the protocol.
 *
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 */
#include "tlb.h"
#include "percpu.h"
#include "lapic.h"
#include "spinlock.h"
#include "vmm.h"
#include "debugcon.h"

#define TLB_ADDR_MASK   0x000FFFFFFFFFF000ULL
#define TLB_TIMEOUT_US  200000ULL          /* 200 ms: a live CPU answers in µs */

extern uint64_t ktime_us(void);

/* Non-zero while a request is in flight: spin loops poll tlb_poll() then. */
volatile uint32_t g_tlb_inflight;

static spinlock_t tlb_lock = SPINLOCK_INIT;
static struct {
    uint64_t pml4;      /* user space: its PML4 physical address           */
    uint64_t va;        /* first page                                      */
    uint64_t npages;    /* 0 = flush everything                            */
    int      kernel;    /* 1 = kernel-half mapping (every CPU flushes)     */
} g_req;
static volatile uint64_t g_shootdowns, g_timeouts;

typedef char tlb_mask_covers_cpus[(VMM_CPU_MASK_WORDS * 64 >= SMP_MAX_CPUS) ? 1 : -1];

static inline uint64_t tlb_read_cr3(void) {
    uint64_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v;
}

/* Apply one request to this CPU's TLB. A user-space request only matters when
 * that space is the CR3 loaded here (otherwise the last CR3 load already
 * dropped every entry of it: the kernel maps nothing global and uses no PCID). */
static void tlb_flush_local(int kernel, uint64_t pml4, uint64_t va, uint64_t npages) {
    if (!kernel && (tlb_read_cr3() & TLB_ADDR_MASK) != pml4) return;
    if (npages == 0 || npages > TLB_FLUSH_ALL_THRESHOLD) {
        uint64_t cr3 = tlb_read_cr3();
        __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
        return;
    }
    for (uint64_t i = 0; i < npages; i++)
        __asm__ volatile ("invlpg (%0)" :: "r"(va + i * 0x1000ULL) : "memory");
}

void tlb_poll(void) {
    cpu_t* c = this_cpu();
    if (!c->tlb_pending) return;
    __asm__ volatile ("" ::: "memory");          /* read the request after the flag */
    tlb_flush_local(g_req.kernel, g_req.pml4, g_req.va, g_req.npages);
    c->nr_ipi_tlb++;
    __atomic_store_n(&c->tlb_pending, 0, __ATOMIC_RELEASE);
}

void tlb_ipi_handler(void) {
    tlb_poll();
    lapic_eoi();
}

static inline int space_on_cpu(const vmm_space_t* s, uint32_t i) {
    return (int)((s->active_cpus[i >> 6] >> (i & 63)) & 1);
}

void tlb_shootdown(vmm_space_t* space, uint64_t va, uint64_t npages) {
    vmm_space_t* ks = vmm_get_kernel_space();
    int kernel = (!space || space == ks);
    uint64_t pml4 = kernel ? 0 : (space->pml4_phys & TLB_ADDR_MASK);
    va &= ~0xFFFULL;
    tlb_flush_local(kernel, pml4, va, npages);
    if (g_cpu_count <= 1) return;

    /* The page-table store that made the old entry stale must be visible
     * before we sample who has the space loaded (vmm_switch_space sets its
     * bit with a locked RMW before loading CR3, so either it is in the set or
     * its CR3 load sees the new entry). */
    __sync_synchronize();
    cpu_t* me = this_cpu();
    uint64_t fl = spin_lock_irqsave(&tlb_lock);
    g_req.kernel = kernel; g_req.pml4 = pml4; g_req.va = va; g_req.npages = npages;
    uint32_t ncpu = g_cpu_count, targets = 0;
    for (uint32_t i = 0; i < ncpu; i++) {
        cpu_t* c = cpu_by_index(i);
        if (c == me || !c->online) continue;
        if (!kernel && !space_on_cpu(space, i)) continue;
        __atomic_store_n(&c->tlb_pending, 1, __ATOMIC_RELEASE);
        targets++;
    }
    if (targets) {
        __atomic_store_n(&g_tlb_inflight, 1, __ATOMIC_SEQ_CST);
        for (uint32_t i = 0; i < ncpu; i++) {
            cpu_t* c = cpu_by_index(i);
            if (c != me && c->tlb_pending) lapic_ipi(c->lapic_id, VEC_IPI_TLB);
        }
        uint64_t t0 = ktime_us();
        uint64_t spins = 0;
        for (;;) {
            int busy = 0;
            for (uint32_t i = 0; i < ncpu && !busy; i++) {
                cpu_t* c = cpu_by_index(i);
                if (c != me && c->tlb_pending) busy = 1;
            }
            if (!busy) break;
            __asm__ volatile ("pause");
            uint64_t now = t0 ? ktime_us() : 0;
            if ((t0 && now - t0 > TLB_TIMEOUT_US) || (!t0 && ++spins > 400000000ULL)) {
                for (uint32_t i = 0; i < ncpu; i++) {
                    cpu_t* c = cpu_by_index(i);
                    if (c == me || !c->tlb_pending) continue;
                    debugcon_writestring("[TLB] shootdown timeout cpu=");
                    debugcon_print_hex(c->index);
                    debugcon_writestring("\n");
                    c->tlb_pending = 0;
                    g_timeouts++;
                }
                break;
            }
        }
        __atomic_store_n(&g_tlb_inflight, 0, __ATOMIC_SEQ_CST);
        g_shootdowns++;
    }
    spin_unlock_irqrestore(&tlb_lock, fl);
}

uint64_t tlb_stat_shootdowns(void) { return g_shootdowns; }
uint64_t tlb_stat_timeouts(void)   { return g_timeouts; }
