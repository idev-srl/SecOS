/*
 * tlb.h — SMP TLB shootdown (M48).
 *
 * Every page-table change that removes or narrows a translation (unmap,
 * protection change, COW break, fork's write-protect, kernel VA unmap,
 * physmap caching retag) must reach every CPU that may cache the old entry:
 *
 *   - kernel space: all online CPUs (the kernel half is shared by all CR3s);
 *   - a user space: the CPUs whose CR3 currently holds it (vmm_space_t's
 *     active_cpus set, maintained by vmm_switch_space). Without PCID a CR3
 *     load flushes every non-global entry and the kernel maps nothing global,
 *     so a CPU that switched away holds nothing stale.
 *
 * Protocol (deadlock-free with locks held and interrupts off): the initiator
 * serializes on tlb_lock, publishes one request, marks each target's per-CPU
 * tlb_pending, sends a VEC_IPI_TLB IPI and spins until every target cleared its
 * flag. A target services the request from the IPI handler OR from any
 * spinlock wait loop (spin_lock polls tlb_poll() while g_tlb_inflight is set),
 * so a target spinning with IF=0 on a lock the initiator holds still
 * acknowledges. The handlers take no lock. A target that never answers (a
 * halted CPU) is given up on after a bounded wait and logged.
 *
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 */
#ifndef TLB_H
#define TLB_H
#include <stdint.h>

struct vmm_space;

/* Requests covering more pages than this flush the whole TLB (CR3 reload). */
#define TLB_FLUSH_ALL_THRESHOLD 32

/* Invalidate [va, va + npages*4K) of `space` on every CPU that may cache it,
 * including this one. npages == 0 means "everything in that space".
 * `space` NULL or the kernel space = a kernel-half mapping (all CPUs). */
void tlb_shootdown(struct vmm_space* space, uint64_t va, uint64_t npages);

/* Kernel-half convenience wrapper. */
static inline void tlb_shootdown_kernel(uint64_t va, uint64_t npages) {
    tlb_shootdown((struct vmm_space*)0, va, npages);
}

/* Service a pending request addressed to this CPU (IPI handler / lock spin). */
void tlb_poll(void);
/* C side of the VEC_IPI_TLB vector (EOIs the LAPIC). */
void tlb_ipi_handler(void);

/* Counters for /proc and the selftest. */
uint64_t tlb_stat_shootdowns(void);   /* requests that reached a remote CPU */
uint64_t tlb_stat_timeouts(void);     /* targets that never acknowledged    */

#endif /* TLB_H */
