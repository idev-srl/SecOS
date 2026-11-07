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
#include "tlb.h"

void smp_tlb_selftest_ap(cpu_t* c);
#include <stddef.h>
#include "klock.h"   /* [M50] cpu_wait_irq */

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

/* Busy delay via the calibrated monotonic clock (falls back to a rough spin if
 * the TSC is not yet calibrated, which shouldn't happen this late in boot). */
static void ap_udelay(uint64_t us) {
    uint64_t start = ktime_us();
    if (!start) { for (volatile uint64_t i = 0; i < us * 200; i++) __asm__ volatile ("pause"); return; }
    while (ktime_us() - start < us) __asm__ volatile ("pause");
}

/* Low memory (0x8000 / 0x8F00) is written through the physmap. [M44] The low
 * identity map is gone from the kernel page tables; only the AP itself runs
 * on it, briefly, via the dedicated trampoline CR3 (vmm_ap_trampoline_cr3). */
static volatile uint8_t* lowp(uint64_t phys) { return (volatile uint8_t*)phys_to_virt(phys); }

/* Per-AP idle task (a ring-0 kernel context). When a core has no runnable user
 * task, it runs its idle loop; the LAPIC timer (or a reschedule IPI) preempts
 * idle to a user task and returns here when that task blocks/exits. [M48] The
 * PCB and its trapframe are allocated per AP in smp_init (sized by the MADT). */
extern void* kmalloc(unsigned long);
extern void lapic_timer_start_this_cpu(uint32_t);
extern void sched_reap_zombies(void);
#define AP_SWITCH_STACK_SIZE 0x2000ull   /* private stack for the final switch step */

/* AP 64-bit entry: bring this core fully online and enter the scheduler.
 *  1. enable its Local APIC (x2APIC first if the BSP chose it: this_cpu()
 *     reads the APIC ID in that mode) + load the shared IDT
 *  2. build its own GDT/TSS/IST (ring-3 entry lands on this core's kernel stack)
 *  3. install the per-core idle task as `current`/`idle_task`
 *  4. start its LAPIC timer and run the idle loop — the timer tick and the
 *     reschedule IPI pick runnable user tasks (any home; see kernel/sched.c). */
void ap_entry(void) {
    /* [M44] Leave the trampoline CR3 (kernel high half + the boot identity map
     * the real-mode stub needed) for the kernel CR3 at once: from here on the
     * AP runs high only, and the identity map exists nowhere it can reach. */
    { uint64_t k = vmm_get_kernel_space()->pml4_phys;
      __asm__ volatile ("mov %0, %%cr3" :: "r"(k) : "memory"); }
    lapic_enable_this_cpu();
    cpu_t* c = this_cpu();
    vmm_switch_space(vmm_get_kernel_space());   /* records the active space */
    extern void cpu_enable_sse(void);
    extern void cpu_enable_mitigations(void);
    cpu_enable_sse();                    // [M34] SSE for ring-3 tasks on this AP
    cpu_enable_mitigations();            // [M36] SMEP (+SMAP) on this AP too
    vmm_pat_init();                      // [M48] same PAT (WC slot) as the BSP: tasks migrate
    idt_ap_load();
    tss_setup_ap(c->index, c->kstack_top);

    process_t* idle = (process_t*)c->idle_task;   /* allocated by smp_init */
    c->current = idle;
    c->in_idle = 1;
    c->slice_left = 0;

    __sync_synchronize();
    c->online = 1;
    lapic_timer_start_this_cpu(1000);
    __asm__ volatile ("sti");
    for (;;) {
        smp_tlb_selftest_ap(c);
#ifdef SECOS_PAGECACHE_TEST
        { extern void pagecache_selftest_ap(cpu_t*); pagecache_selftest_ap(c); }   // [M50 B2]
#endif
#ifdef SECOS_B3_BLKSTRESS
        { extern void blkstress_ap(cpu_t*); blkstress_ap(c); }                    // [M50 B3]
#endif
#ifdef SECOS_VFS_TEST
        { extern void vfstest_ap(cpu_t*); vfstest_ap(c); }                        // [M51 C1]
#endif
        sched_reap_zombies();
        cpu_wait_irq();
    }
}

/* ---- [M48] TLB shootdown selftest (boot, >= 2 CPUs online) ----
 * The BSP maps a scratch kernel page (the last kernel-stack slot, unused at
 * boot) to frame A and has every AP read it, so each AP caches the
 * translation. It then remaps the page to frame B WITH a shootdown: every AP
 * must now read B. As a negative control it remaps to frame C WITHOUT the
 * shootdown: an AP still holding the old translation reads B, which shows the
 * test can see a missing invalidation (QEMU's per-vCPU soft TLB keeps stale
 * entries exactly like hardware). APs participate from their idle loop. */
static volatile struct {
    uint32_t phase;            /* 0 idle, 1..3 = read round n                */
    uint64_t va;
    volatile uint64_t seen[3]; /* per round: APs that answered               */
    volatile uint64_t good[3]; /* per round: APs that read the expected value */
    uint64_t expect[3];
} g_tlbt;
void smp_tlb_selftest_ap(cpu_t* c) {
    uint32_t ph = g_tlbt.phase;
    if (!ph || ph > 3) return;
    static volatile uint8_t done[3][SMP_MAX_CPUS];
    if (done[ph - 1][c->index]) return;
    uint64_t v = *(volatile uint64_t*)g_tlbt.va;
    if (v == g_tlbt.expect[ph - 1]) __atomic_add_fetch(&g_tlbt.good[ph - 1], 1, __ATOMIC_SEQ_CST);
    done[ph - 1][c->index] = 1;
    __atomic_add_fetch(&g_tlbt.seen[ph - 1], 1, __ATOMIC_SEQ_CST);
}
static int tlbt_round(uint32_t ph, uint64_t want, uint32_t naps) {
    g_tlbt.expect[ph - 1] = want;
    __sync_synchronize();
    g_tlbt.phase = ph;
    uint64_t t0 = ktime_us();
    while (g_tlbt.seen[ph - 1] < naps && ktime_us() - t0 < 2000000ULL)
        cpu_wait_irq();               /* APs answer on their next tick */
    return (int)g_tlbt.seen[ph - 1];
}
void smp_tlb_selftest(void) {
    extern uint64_t vmm_alloc_kernel_stack_for_slot(uint32_t);
    extern int vmm_free_kernel_stack_for_slot(uint32_t);
    extern int vmm_selftest_remap(uint64_t, uint64_t, int);
    uint32_t naps = smp_online_count() - 1;
    static int done_once;
    // Boot only: the scratch slot is free while no process exists (a later
    // shell `smp` may run with the table full).
    if (naps == 0 || done_once || process_count_all() != 0) return;
    done_once = 1;
    uint64_t top = vmm_alloc_kernel_stack_for_slot(M5_MAX_KSTACK_SLOTS - 1);
    void* fb = pmm_alloc_frame(); void* fc = pmm_alloc_frame();
    if (!top || !fb || !fc) { debugcon_writestring("[M48] TLB selftest: no scratch page\n"); return; }
    uint64_t va = top - 0x1000ULL;                   /* top page of the slot */
    *(volatile uint64_t*)va = 0xAAAAAAAAAAAAAAAAULL;
    *(volatile uint64_t*)phys_to_virt((uint64_t)fb) = 0xBBBBBBBBBBBBBBBBULL;
    *(volatile uint64_t*)phys_to_virt((uint64_t)fc) = 0xCCCCCCCCCCCCCCCCULL;
    uint64_t fa = vmm_translate(va) & ~0xFFFULL;
    g_tlbt.va = va;
    tlbt_round(1, 0xAAAAAAAAAAAAAAAAULL, naps);      /* APs cache va -> A */
    vmm_selftest_remap(va, (uint64_t)fb, 1);         /* va -> B, shot down */
    int seen2 = tlbt_round(2, 0xBBBBBBBBBBBBBBBBULL, naps);
    vmm_selftest_remap(va, (uint64_t)fc, 0);         /* va -> C, NOT shot down */
    tlbt_round(3, 0xCCCCCCCCCCCCCCCCULL, naps);
    g_tlbt.phase = 0;
    debugcon_writestring("[M48] TLB selftest: aps="); debugcon_print_hex(naps);
    debugcon_writestring(" with-shootdown fresh="); debugcon_print_hex(g_tlbt.good[1]);
    debugcon_writestring("/"); debugcon_print_hex((uint64_t)seen2);
    debugcon_writestring(" without stale="); debugcon_print_hex(g_tlbt.seen[2] - g_tlbt.good[2]);
    debugcon_writestring("\n");
    debugcon_writestring(g_tlbt.good[1] == naps && (uint64_t)seen2 == naps
                         ? "[M48] TLB shootdown selftest PASS\n" : "[M48] TLB shootdown selftest FAIL\n");
    /* Put frame A back so the slot frees what it allocated, then retire it
     * (the broken remap above left stale entries: flush them everywhere). */
    vmm_selftest_remap(va, fa, 1);
    vmm_free_kernel_stack_for_slot(M5_MAX_KSTACK_SLOTS - 1);
    tlb_shootdown_kernel(top - M5_KSTACK_SLOT_SIZE, M5_KSTACK_SLOT_PAGES);
    pmm_free_frame(fb); pmm_free_frame(fc);
}

/* Allocate an AP's idle PCB + trapframe + switch stack. */
static int ap_alloc_idle(cpu_t* c) {
    process_t* idle = (process_t*)kmalloc(sizeof(process_t));
    trapframe_t* tf = (trapframe_t*)kmalloc(sizeof(trapframe_t));
    void* sw = pmm_alloc_contiguous(AP_SWITCH_STACK_SIZE / PMM_FRAME_SIZE);
    if (!idle || !tf || !sw) return -1;
    for (unsigned i = 0; i < sizeof(*idle); i++) ((uint8_t*)idle)[i] = 0;
    for (unsigned i = 0; i < sizeof(*tf); i++) ((uint8_t*)tf)[i] = 0;
    idle->pid = 0;                       /* idle tasks are not in the proc table */
    idle->space = vmm_get_kernel_space();
    idle->kstack_top = c->kstack_top;
    idle->tf = tf;
    idle->state = PROC_RUNNING;
    idle->cpu_affinity = c->index;
    idle->cpu_pin = (int32_t)c->index;
    c->idle_task = idle;
    c->switch_stack_top = (uint64_t)phys_to_virt((uint64_t)sw) + AP_SWITCH_STACK_SIZE;
    return 0;
}

uint32_t smp_cpu_lapic_id(uint32_t i) { return cpu_by_index(i)->lapic_id; }

/* [M48] Panic: park every other online CPU (VEC_IPI_STOP: cli; hlt forever) so
 * the panic report is the last thing the machine does. A CPU running with
 * interrupts off parks once it enables them again. */
void smp_stop_others(void) {
    if (g_cpu_count <= 1) return;
    cpu_t* me = this_cpu();
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        cpu_t* c = cpu_by_index(i);
        if (c != me && c->online) lapic_ipi(c->lapic_id, VEC_IPI_STOP);
    }
}

/* CPU index i is running the kernel (the BSP always is). */
int smp_cpu_online(uint32_t i) {
    return i == 0 || (i < smp_cpu_count() && cpu_by_index(i)->online);
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
    uint64_t tramp_cr3 = vmm_ap_trampoline_cr3();
    if (!tramp_cr3) {
        debugcon_writestring("[SMP] no AP-trampoline CR3 (identity tables missing) — single CPU\n");
        return;
    }
    *(volatile uint64_t*)lowp(AP_PARAM_CR3)   = tramp_cr3;
    *(volatile uint64_t*)lowp(AP_PARAM_ENTRY) = (uint64_t)&ap_entry;

    for (uint32_t i = 0; i < t->cpu_count; i++) {
        uint32_t id = t->lapic_ids[i];
        if (id == bsp) continue;
        int idx = smp_register_cpu(id);
        if (idx < 0) continue;

        if (cpu_by_index(idx)->online) continue;      /* already up (shell `smp` twice) */
        /* Per-AP kernel stack (contiguous frames, addressed via the physmap). */
        void* base = pmm_alloc_contiguous(AP_KSTACK_SIZE / PMM_FRAME_SIZE);
        if (!base) { debugcon_writestring("[SMP] AP kstack alloc failed\n"); continue; }
        uint64_t stack_top = (uint64_t)phys_to_virt((uint64_t)base) + AP_KSTACK_SIZE;
        cpu_by_index(idx)->kstack_top = stack_top;
        if (ap_alloc_idle(cpu_by_index(idx)) != 0) {
            debugcon_writestring("[SMP] AP idle/switch-stack alloc failed\n"); continue;
        }
        *(volatile uint64_t*)lowp(AP_PARAM_STACK) = stack_top;

        /* INIT, then two STARTUP IPIs at vector (0x8000 >> 12). */
        lapic_send_init(id);
        ap_udelay(10000);
        lapic_send_sipi(id, (uint8_t)(AP_TRAMPOLINE_PHYS >> 12));
        ap_udelay(200);
        lapic_send_sipi(id, (uint8_t)(AP_TRAMPOLINE_PHYS >> 12));

        int online = 0;
        for (int spins = 0; spins < 200 && !online; spins++) {
            if (cpu_by_index(idx)->online) online = 1; else ap_udelay(1000);
        }
        debugcon_writestring(online ? "[SMP] cpu online lapic_id=" : "[SMP] cpu TIMEOUT lapic_id=");
        debugcon_print_hex(id);
        debugcon_writestring("\n");
    }
    debugcon_writestring("[SMP] online cpus=");
    debugcon_print_hex(smp_online_count());
    debugcon_writestring("\n");
    smp_tlb_selftest();
}
