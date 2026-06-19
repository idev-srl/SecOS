/*
 * percpu.h — Per-CPU data for SMP (M29).
 *
 * Scheduler state that is inherently per-core (current task, idle task, time
 * slice) lives here instead of in file-static globals. `this_cpu()` resolves the
 * running core via its Local APIC ID; before SMP is up it returns CPU 0, so the
 * single-core path is unchanged.
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef PERCPU_H
#define PERCPU_H
#include <stdint.h>

struct process;

#define SMP_MAX_CPUS 8

typedef struct cpu {
    uint32_t index;             /* dense 0-based CPU index                 */
    uint32_t lapic_id;          /* Local APIC ID (from ACPI/MADT)          */
    volatile int online;        /* 1 once the core is running the kernel   */
    struct process* current;    /* task currently running on this CPU      */
    struct process* idle_task;  /* this CPU's idle task                    */
    uint32_t slice_left;        /* preemption quantum remaining            */
    uint64_t kstack_top;        /* this CPU's boot/idle kernel stack top   */
    void*    tss;               /* this CPU's TSS (per-CPU rsp0)           */
} cpu_t;

extern cpu_t g_cpus[SMP_MAX_CPUS];
extern volatile uint32_t g_cpu_count;

/* The CPU executing this call (by LAPIC ID; CPU 0 before SMP/LAPIC are up). */
cpu_t* this_cpu(void);
cpu_t* cpu_by_index(uint32_t i);
uint32_t smp_cpu_count(void);
uint32_t smp_online_count(void);   /* CPUs marked online (incl. BSP); defined in smp.c */

/* Register a CPU by its LAPIC ID (idempotent). Returns its dense index, -1 if
 * the table is full. */
int smp_register_cpu(uint32_t lapic_id);

/* Patch CPU 0's LAPIC ID once the real BSP ID is known (after ACPI/LAPIC),
 * without disturbing the scheduler state already stored in it. */
void smp_set_bsp_lapic_id(uint32_t lapic_id);

#endif /* PERCPU_H */
