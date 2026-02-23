/*
 * SecOS Kernel - Simple Scheduler
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "sched.h"
#include "terminal.h"
#include "debugcon.h"
#include "vmm.h"

static process_t* current = NULL;
// Simple strategy: iterate process table and pick next NEW/READY.
extern void process_foreach(void (*cb)(process_t*, void*), void* user);

struct pick_ctx { process_t* after; int passed; process_t* cand; };
static void pick_scan_cb(process_t* p, void* user) {
    struct pick_ctx* c = (struct pick_ctx*)user;
    if (p == c->after) { c->passed = 1; return; }
    if (!c->passed && c->after!=NULL) return; // not yet past 'after'
    if (p->state == PROC_NEW || p->state == PROC_READY) { if (!c->cand) c->cand = p; }
}
static process_t* pick_next(process_t* after) {
    struct pick_ctx c = { after, after==NULL?1:0, NULL };
    process_foreach(pick_scan_cb, &c);
    if (!c.cand) {
    // retry from beginning
        struct pick_ctx c2 = { NULL, 1, NULL };
        process_foreach(pick_scan_cb, &c2);
        return c2.cand;
    }
    return c.cand;
}

void sched_init(void) {
    current = NULL;
}

process_t* sched_get_current(void) { return current; }
void sched_set_current(process_t* p) { current = p; }

int sched_add_process(process_t* p) { (void)p; return 0; }

// [M5] Update TSS.rsp0 on context switch
extern void tss_set_kernel_stack(uint64_t stack);
// [M6] Full context switch via trapframe restore + iretq
extern void arch_switch_to_process(process_t* next);

void sched_yield(void) {
    process_t* next = pick_next(current);
    if (next && next != current) {
        if (current && current->state == PROC_RUNNING) current->state = PROC_READY;
        next->state = PROC_RUNNING;
        current = next;
        // [M5] Set per-process kernel stack in TSS for ring-3 → ring-0 entry
        if (next->kstack_top)
            tss_set_kernel_stack(next->kstack_top);
        // [M6] Perform real context switch if trapframe is ready.
        // arch_switch_to_process does NOT return (ends with iretq).
        // NOTE: for timer-tick preemption, caller must send EOI before this point.
        if (next->tf)
            arch_switch_to_process(next);
    }
}

// [M7] Cooperative yield from SYS_YIELD syscall.
// Saves caller trapframe, picks next READY process, switches to it.
// Does NOT return if a switch occurs (ends with iretq into next process).
extern void arch_iret_to_tf(trapframe_t* tf) __attribute__((noreturn));

void sched_yield_from_syscall(trapframe_t* tf) {
    if (!current) return;

    // Save caller's trapframe into persistent heap copy
    if (current->tf) {
        const uint8_t* s = (const uint8_t*)tf;
        uint8_t* d = (uint8_t*)current->tf;
        for (int i = 0; i < (int)sizeof(trapframe_t); i++) d[i] = s[i];
        current->tf->rax = 0; // yield returns 0
    }

    process_t* next = pick_next(current);
    if (!next || next == current) return; // no switch

    // Log context switch
    debugcon_writestring("[SCHED] switch ");
    debugcon_print_hex(current->pid);
    debugcon_writestring(" -> ");
    debugcon_print_hex(next->pid);
    debugcon_writestring("\n");

    current->state = PROC_READY;
    next->state = PROC_RUNNING;
    current = next;

    vmm_switch_space(next->space);
    tss_set_kernel_stack(next->kstack_top);
    arch_iret_to_tf(next->tf);
    // NOT REACHED
}

void sched_on_timer_tick(void) {
    if (current) current->cpu_ticks++;
    // For now yield every tick (future: quantum)
    sched_yield();
}
