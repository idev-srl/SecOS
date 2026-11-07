/*
 * percpu.h — Per-CPU data for SMP (M29, reworked in M48).
 *
 * Scheduler state that is inherently per-core (current task, idle task, time
 * slice) lives here instead of in file-static globals. `this_cpu()` resolves the
 * running core via its Local APIC ID (xAPIC MMIO or x2APIC MSR) through a dense
 * APIC-ID -> index map; before SMP is up it returns CPU 0, so the single-core
 * path is unchanged.
 *
 * [M48] The CPU table is sized at run time: CPU 0 is static, every AP gets a
 * cpu_t allocated when the MADT enumerates it (up to SMP_MAX_CPUS).
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef PERCPU_H
#define PERCPU_H
#include <stdint.h>

struct process;
struct vmm_space;

/* Upper bound on CPUs (x2APIC allows more than 255). Per-CPU blocks are
 * allocated on demand, so the bound only costs a pointer table. Keep in sync
 * with VMM_CPU_MASK_WORDS in mm/vmm.h (64 CPUs per mask word). */
#define SMP_MAX_CPUS 256

typedef struct cpu {
    uint32_t index;             /* dense 0-based CPU index                 */
    uint32_t lapic_id;          /* Local APIC ID (xAPIC 8-bit / x2APIC 32-bit) */
    volatile int online;        /* 1 once the core is running the kernel   */
    struct process* current;    /* task currently running on this CPU      */
    struct process* idle_task;  /* this CPU's idle task                    */
    uint32_t slice_left;        /* preemption quantum remaining            */
    uint64_t kstack_top;        /* this CPU's boot/idle kernel stack top   */
    void*    tss;               /* this CPU's TSS (per-CPU rsp0)           */
    /* [M48] SMP scheduling / TLB state. */
    uint64_t switch_stack_top;  /* private stack the final switch step runs on */
    struct vmm_space* active_space;   /* address space whose CR3 is loaded */
    volatile uint32_t tlb_pending;    /* a TLB shootdown request awaits this CPU */
    volatile uint32_t resched_pending;/* a reschedule IPI is in flight      */
    volatile uint32_t in_idle;        /* current == idle_task (kick target) */
    uint64_t nr_switches;       /* context switches performed on this CPU  */
    uint64_t nr_migrations;     /* tasks this CPU pulled from another CPU  */
    uint64_t nr_ipi_tlb;        /* TLB shootdown IPIs serviced             */
    uint64_t nr_ipi_resched;    /* reschedule IPIs serviced                */
    /* [M48] Kernel debugcon line being assembled on this CPU (kernel/klog.c):
     * emitted whole, so lines from different CPUs never interleave. */
    uint32_t dbg_n;
    uint64_t dbg_since;         /* timer tick of the line's first byte     */
    char     dbg_line[200];
} cpu_t;

extern cpu_t* g_cpu_ptr[SMP_MAX_CPUS];
extern volatile uint32_t g_cpu_count;

/* The CPU executing this call (by LAPIC ID; CPU 0 before SMP/LAPIC are up). */
cpu_t* this_cpu(void);
cpu_t* cpu_by_index(uint32_t i);
uint32_t smp_cpu_count(void);
int smp_cpu_online(uint32_t i);     /* index i online (BSP always); defined in smp.c */
uint32_t smp_online_count(void);   /* CPUs marked online (incl. BSP); defined in smp.c */

/* Register a CPU by its LAPIC ID (idempotent). Returns its dense index, -1 if
 * the table is full or the per-CPU block cannot be allocated. */
int smp_register_cpu(uint32_t lapic_id);

/* Patch CPU 0's LAPIC ID once the real BSP ID is known (after ACPI/LAPIC),
 * without disturbing the scheduler state already stored in it. */
void smp_set_bsp_lapic_id(uint32_t lapic_id);

/* Make a multi-call debugcon line atomic across cores (wrap the calls). */
uint64_t debugcon_line_lock(void);
void debugcon_line_unlock(uint64_t flags);

#endif /* PERCPU_H */
