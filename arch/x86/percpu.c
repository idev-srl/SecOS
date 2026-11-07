/*
 * percpu.c — Per-CPU data for SMP (M29, M48). See percpu.h.
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "percpu.h"
#include "lapic.h"
#include "spinlock.h"

extern void* kmalloc(unsigned long);

static cpu_t g_cpu0;                      /* the BSP: usable before the heap */
cpu_t* g_cpu_ptr[SMP_MAX_CPUS];
volatile uint32_t g_cpu_count = 0;

/* APIC ID -> dense index (+1; 0 = unknown). xAPIC IDs are 8-bit; x2APIC IDs
 * are 32-bit but firmware numbers them densely, so a 4096-entry map covers any
 * realistic topology. Larger IDs fall back to a linear scan. */
#define APIC_MAP_SIZE 4096
static uint16_t g_apic_map[APIC_MAP_SIZE];

/* debugcon_line_lock/unlock live in kernel/klog.c since M48 (one lock for the
 * kernel line buffers and the user-console mirror). */

cpu_t* cpu_by_index(uint32_t i) {
    if (i >= g_cpu_count || !g_cpu_ptr[i]) return &g_cpu0;
    return g_cpu_ptr[i];
}
uint32_t smp_cpu_count(void) { return g_cpu_count; }

cpu_t* this_cpu(void) {
    if (g_cpu_count <= 1) return &g_cpu0;          /* single core / pre-SMP */
    uint32_t id = lapic_get_id();
    if (id < APIC_MAP_SIZE) {
        uint32_t i = g_apic_map[id];
        if (i) return g_cpu_ptr[i - 1];
    } else {
        for (uint32_t i = 0; i < g_cpu_count; i++)
            if (g_cpu_ptr[i] && g_cpu_ptr[i]->lapic_id == id) return g_cpu_ptr[i];
    }
    return &g_cpu0;                                /* not registered: the BSP */
}

void smp_set_bsp_lapic_id(uint32_t lapic_id) {
    if (g_cpu_count == 0) { smp_register_cpu(lapic_id); return; }
    if (g_cpu0.lapic_id < APIC_MAP_SIZE && g_apic_map[g_cpu0.lapic_id] == 1)
        g_apic_map[g_cpu0.lapic_id] = 0;
    g_cpu0.lapic_id = lapic_id;   /* keep current/idle/slice intact */
    if (lapic_id < APIC_MAP_SIZE) g_apic_map[lapic_id] = 1;
}

int smp_register_cpu(uint32_t lapic_id) {
    for (uint32_t i = 0; i < g_cpu_count; i++)
        if (g_cpu_ptr[i]->lapic_id == lapic_id) return (int)i;
    uint32_t i = g_cpu_count;
    if (i >= SMP_MAX_CPUS) return -1;
    cpu_t* c = (i == 0) ? &g_cpu0 : (cpu_t*)kmalloc(sizeof(cpu_t));
    if (!c) return -1;
    for (unsigned k = 0; k < sizeof(cpu_t); k++) ((uint8_t*)c)[k] = 0;
    c->index = i;
    c->lapic_id = lapic_id;
    g_cpu_ptr[i] = c;
    if (lapic_id < APIC_MAP_SIZE) g_apic_map[lapic_id] = (uint16_t)(i + 1);
    __sync_synchronize();          /* the block is complete before it is counted */
    g_cpu_count = i + 1;
    return (int)i;
}
