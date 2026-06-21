/*
 * smp.c — SMP application-processor bring-up (M29-2). See smp.h.
 *
 * INIT-SIPI-SIPI starts each AP in the real-mode trampoline (boot/ap_trampoline.asm,
 * copied to physical 0x8000). The trampoline switches the AP to long mode on the
 * BSP's kernel CR3 and jumps to ap_entry() on a per-AP kernel stack. APs are
 * brought up one at a time (the trampoline parameter block is shared), so the
 * bring-up is race-free.
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "smp.h"
#include "percpu.h"
#include "acpi.h"
#include "lapic.h"
#include "debugcon.h"
#include "vmm.h"   /* phys_to_virt, vmm_get_kernel_space */
#include "pmm.h"
#include "process.h"
#include "trapframe.h"
#include "tss.h"
#include <stddef.h>

/* Trampoline blob (objcopy of the flat binary) + its load/parameter addresses. */
extern uint8_t _binary_ap_trampoline_bin_start[];
extern uint8_t _binary_ap_trampoline_bin_end[];
#define AP_TRAMPOLINE_PHYS 0x8000ull
#define AP_PARAM_CR3       0x8F00ull
#define AP_PARAM_STACK     0x8F08ull
#define AP_PARAM_ENTRY     0x8F10ull
#define AP_KSTACK_SIZE     0x4000ull   /* 16 KiB per-AP kernel stack */

extern void lapic_enable_this_cpu(void);
extern void idt_ap_load(void);
extern uint64_t ktime_us(void);

static inline uint64_t read_cr3(void) {
    uint64_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v;
}

/* Busy delay via the calibrated monotonic clock (falls back to a rough spin if
 * the TSC is not yet calibrated, which shouldn't happen this late in boot). */
static void ap_udelay(uint64_t us) {
    uint64_t start = ktime_us();
    if (!start) { for (volatile uint64_t i = 0; i < us * 200; i++) __asm__ volatile ("pause"); return; }
    while (ktime_us() - start < us) __asm__ volatile ("pause");
}

/* Low memory (0x8000 / 0x8F00) is identity-mapped, so a physical address is a
 * valid pointer. */
static volatile uint8_t* lowp(uint64_t phys) { return (volatile uint8_t*)phys; }

/* Per-AP idle task (a ring-0 kernel context). When a core has no runnable user
 * task pinned to it, it runs its idle loop; the LAPIC timer preempts idle to a
 * user task and returns here when that task blocks/exits. */
static process_t ap_idle[SMP_MAX_CPUS];
static trapframe_t ap_idle_tf[SMP_MAX_CPUS];

extern void lapic_timer_start_this_cpu(uint32_t);
extern void sched_reap_zombies(void);

/* AP 64-bit entry: bring this core fully online and enter the scheduler.
 *  1. enable its Local APIC + load the shared IDT
 *  2. build its own GDT/TSS/IST (ring-3 entry lands on this core's kernel stack)
 *  3. install a per-core idle task as `current`/`idle_task`
 *  4. start its LAPIC timer and run the idle loop — the timer tick schedules any
 *     user task pinned (cpu_affinity) to this core. */
void ap_entry(void) {
    cpu_t* c = this_cpu();
    extern void cpu_enable_sse(void);
    extern void cpu_enable_mitigations(void);
    cpu_enable_sse();                    // [M34] SSE for ring-3 tasks pinned to this AP
    cpu_enable_mitigations();            // [M36] SMEP (+SMAP) on this AP too
    lapic_enable_this_cpu();
    idt_ap_load();
    tss_setup_ap(c->index, c->kstack_top);

    process_t* idle = &ap_idle[c->index];
    trapframe_t* tf = &ap_idle_tf[c->index];
    for (int i = 0; i < (int)sizeof(*idle); i++) ((uint8_t*)idle)[i] = 0;
    for (int i = 0; i < (int)sizeof(*tf); i++) ((uint8_t*)tf)[i] = 0;
    idle->pid = 0;                       /* idle tasks are not in the proc table */
    idle->space = vmm_get_kernel_space();
    idle->kstack_top = c->kstack_top;
    idle->tf = tf;
    idle->state = PROC_RUNNING;
    idle->cpu_affinity = c->index;
    c->idle_task = idle;
    c->current = idle;
    c->slice_left = 0;

    c->online = 1;
    lapic_timer_start_this_cpu(1000);
    __asm__ volatile ("sti");
    for (;;) { sched_reap_zombies(); __asm__ volatile ("hlt"); }
}

uint32_t smp_online_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < smp_cpu_count(); i++)
        if (cpu_by_index(i)->online) n++;
    return n;
}

void smp_init(void) {
    const acpi_topology_t* t = acpi_get();
    if (!t->found || t->cpu_count <= 1 || !apic_mode_active()) {
        debugcon_writestring("[SMP] single CPU (no APs to start)\n");
        if (smp_cpu_count() > 0) cpu_by_index(0)->online = 1;
        return;
    }
    cpu_by_index(0)->online = 1;           /* the BSP is online */
    uint32_t bsp = lapic_get_id();

    /* Copy the trampoline to 0x8000 and publish the shared parameters. */
    uint64_t sz = (uint64_t)(_binary_ap_trampoline_bin_end - _binary_ap_trampoline_bin_start);
    for (uint64_t i = 0; i < sz; i++) lowp(AP_TRAMPOLINE_PHYS)[i] = _binary_ap_trampoline_bin_start[i];
    *(volatile uint64_t*)lowp(AP_PARAM_CR3)   = read_cr3();
    *(volatile uint64_t*)lowp(AP_PARAM_ENTRY) = (uint64_t)&ap_entry;

    for (uint32_t i = 0; i < t->cpu_count; i++) {
        uint32_t id = t->lapic_ids[i];
        if (id == bsp) continue;
        int idx = smp_register_cpu(id);
        if (idx < 0) continue;

        /* Per-AP kernel stack (contiguous frames, addressed via the physmap). */
        void* base = pmm_alloc_contiguous(AP_KSTACK_SIZE / PMM_FRAME_SIZE);
        if (!base) { debugcon_writestring("[SMP] AP kstack alloc failed\n"); continue; }
        uint64_t stack_top = (uint64_t)phys_to_virt((uint64_t)base) + AP_KSTACK_SIZE;
        cpu_by_index(idx)->kstack_top = stack_top;
        *(volatile uint64_t*)lowp(AP_PARAM_STACK) = stack_top;

        /* INIT, then two STARTUP IPIs at vector (0x8000 >> 12). */
        lapic_send_init(id);
        ap_udelay(10000);
        lapic_send_sipi(id, (uint8_t)(AP_TRAMPOLINE_PHYS >> 12));
        ap_udelay(200);
        lapic_send_sipi(id, (uint8_t)(AP_TRAMPOLINE_PHYS >> 12));

        int online = 0;
        for (int spins = 0; spins < 100 && !online; spins++) {
            if (cpu_by_index(idx)->online) online = 1; else ap_udelay(1000);
        }
        debugcon_writestring(online ? "[SMP] cpu online lapic_id=" : "[SMP] cpu TIMEOUT lapic_id=");
        debugcon_print_hex(id);
        debugcon_writestring("\n");
    }
    debugcon_writestring("[SMP] online cpus=");
    debugcon_print_hex(smp_online_count());
    debugcon_writestring("\n");
}
