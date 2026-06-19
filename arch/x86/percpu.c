/*
 * percpu.c — Per-CPU data for SMP (M29). See percpu.h.
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "percpu.h"
#include "lapic.h"

cpu_t g_cpus[SMP_MAX_CPUS];
volatile uint32_t g_cpu_count = 0;

cpu_t* cpu_by_index(uint32_t i) { return &g_cpus[i]; }
uint32_t smp_cpu_count(void) { return g_cpu_count; }

cpu_t* this_cpu(void) {
    uint32_t id = lapic_get_id();
    for (uint32_t i = 0; i < g_cpu_count; i++)
        if (g_cpus[i].lapic_id == id) return &g_cpus[i];
    return &g_cpus[0];                  /* pre-SMP / LAPIC not yet up */
}

void smp_set_bsp_lapic_id(uint32_t lapic_id) {
    if (g_cpu_count == 0) { smp_register_cpu(lapic_id); return; }
    g_cpus[0].lapic_id = lapic_id;   /* keep current/idle/slice intact */
}

int smp_register_cpu(uint32_t lapic_id) {
    for (uint32_t i = 0; i < g_cpu_count; i++)
        if (g_cpus[i].lapic_id == lapic_id) return (int)i;
    uint32_t i = g_cpu_count;
    if (i >= SMP_MAX_CPUS) return -1;
    g_cpus[i].index = i;
    g_cpus[i].lapic_id = lapic_id;
    g_cpus[i].online = 0;
    g_cpus[i].current = 0;
    g_cpus[i].idle_task = 0;
    g_cpus[i].slice_left = 0;
    g_cpus[i].kstack_top = 0;
    g_cpus[i].tss = 0;
    g_cpu_count = i + 1;
    return (int)i;
}
