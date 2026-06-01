/*
 * SecOS Kernel - Scheduler
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M7] Cooperative ring-3 scheduling via SYS_YIELD.
 * [M8] Preemptive (timer-driven) scheduling + process lifecycle (exit/reap)
 *      with a kernel idle task as the always-available fallback.
 */
#include "sched.h"
#include "terminal.h"
#include "debugcon.h"
#include "vmm.h"

static process_t* current = NULL;
static process_t* idle_task = NULL;

// [M8] Preemption quantum: switch the running user task every N timer ticks.
#define SCHED_QUANTUM_TICKS 3
static uint32_t slice_left = SCHED_QUANTUM_TICKS;

extern void process_foreach(void (*cb)(process_t*, void*), void* user);
extern void tss_set_kernel_stack(uint64_t stack);
extern void arch_iret_to_tf(trapframe_t* tf) __attribute__((noreturn));
extern int  process_destroy(process_t* p);

// ---- process selection (round-robin over runnable user processes) ----
struct pick_ctx { process_t* after; int passed; process_t* cand; };
static void pick_scan_cb(process_t* p, void* user) {
    struct pick_ctx* c = (struct pick_ctx*)user;
    if (p == idle_task) return;                       // idle is never picked here
    if (p == c->after) { c->passed = 1; return; }
    if (!c->passed && c->after != NULL) return;       // not yet past 'after'
    if (p->state == PROC_NEW || p->state == PROC_READY) { if (!c->cand) c->cand = p; }
}
static process_t* pick_user(process_t* after) {
    struct pick_ctx c = { after, after == NULL ? 1 : 0, NULL };
    process_foreach(pick_scan_cb, &c);
    if (!c.cand) { // wrap around from the beginning
        struct pick_ctx c2 = { NULL, 1, NULL };
        process_foreach(pick_scan_cb, &c2);
        return c2.cand;
    }
    return c.cand;
}

void sched_init(void) { current = NULL; idle_task = NULL; }
process_t* sched_get_current(void) { return current; }
void sched_set_current(process_t* p) { current = p; }
void sched_set_idle(process_t* idle) { idle_task = idle; }
int sched_add_process(process_t* p) { (void)p; return 0; }

// ---- low-level switch: load target space + kernel stack, iretq into it ----
static void switch_to(process_t* next) __attribute__((noreturn));
static void switch_to(process_t* next) {
    current = next;
    next->state = PROC_RUNNING;
    vmm_switch_space(next->space);
    tss_set_kernel_stack(next->kstack_top);
    slice_left = SCHED_QUANTUM_TICKS;
    arch_iret_to_tf(next->tf);
    // not reached
}

static inline void save_tf(process_t* p, trapframe_t* tf) {
    const uint8_t* s = (const uint8_t*)tf;
    uint8_t* d = (uint8_t*)p->tf;
    for (int i = 0; i < (int)sizeof(trapframe_t); i++) d[i] = s[i];
}

static inline void pic_eoi(void) { __asm__ volatile("outb %0,$0x20"::"a"((uint8_t)0x20)); }

// ---- [M8] preemptive tick ----
void sched_on_timer_tick(trapframe_t* tf) {
    if (current) current->cpu_ticks++;
    if (!idle_task) return;            // preemption only active once a scheduler is armed
    if (!current) return;              // not yet running the scheduler

    int from_user = ((tf->cs & 3) == 3);

    if (current == idle_task) {
        // Idle is always safe to preempt: hand the CPU to a runnable user task.
        // Save idle's interrupted context so that returning to it (after the
        // user task exits) resumes where it was — this lets the interactive
        // shell run as the idle task and get control back after `run`.
        process_t* next = pick_user(NULL);
        if (next) { if (idle_task->tf) save_tf(idle_task, tf); pic_eoi(); switch_to(next); }
        return;
    }

    // A user task is current. Only preempt when it was interrupted in ring-3
    // (never mid-syscall in ring-0), and only when its quantum has expired.
    if (!from_user) return;
    if (slice_left > 1) { slice_left--; return; }

    process_t* next = pick_user(current);
    if (!next) next = idle_task;       // nobody else runnable → go idle
    if (next == current) { slice_left = SCHED_QUANTUM_TICKS; return; }

    if (current->state == PROC_RUNNING) current->state = PROC_READY;
    save_tf(current, tf);
    debugcon_writestring("[SCHED] preempt ");
    debugcon_print_hex(current->pid);
    debugcon_writestring(" -> ");
    debugcon_print_hex(next->pid);
    debugcon_writestring("\n");
    pic_eoi();
    switch_to(next);
}

// ---- [M7] cooperative yield from SYS_YIELD ----
void sched_yield_from_syscall(trapframe_t* tf) {
    if (!current) return;
    if (current->tf) { save_tf(current, tf); current->tf->rax = 0; } // yield returns 0

    process_t* next = pick_user(current);
    if (!next && idle_task) next = idle_task;
    if (!next || next == current) return;             // nothing else: keep running

    debugcon_writestring("[SCHED] switch ");
    debugcon_print_hex(current->pid);
    debugcon_writestring(" -> ");
    debugcon_print_hex(next->pid);
    debugcon_writestring("\n");
    if (current->state == PROC_RUNNING) current->state = PROC_READY;
    switch_to(next);
}

// ---- legacy cooperative entry (kept for compatibility) ----
void sched_yield(void) {
    if (!current) return;
    process_t* next = pick_user(current);
    if (!next && idle_task) next = idle_task;
    if (next && next != current) {
        if (current->state == PROC_RUNNING) current->state = PROC_READY;
        switch_to(next);
    }
}

// ---- [M8] exit + reaping ----
struct alive_ctx { int n; };
static void alive_cb(process_t* p, void* u) {
    struct alive_ctx* c = (struct alive_ctx*)u;
    if (p == idle_task) return;
    if (p->state != PROC_ZOMBIE) c->n++;
}
int sched_count_alive_user(void) {
    struct alive_ctx c = { 0 };
    process_foreach(alive_cb, &c);
    return c.n;
}

struct reap_ctx { process_t* skip; };
static void reap_cb(process_t* p, void* u) {
    struct reap_ctx* c = (struct reap_ctx*)u;
    if (p == c->skip || p == idle_task) return;
    if (p->state == PROC_ZOMBIE) process_destroy(p); // frees space/tables/kstack/tf
}
void sched_reap_zombies(void) {
    // Destroy zombies that are not the running task (their kernel stack is idle).
    struct reap_ctx c = { current };
    process_foreach(reap_cb, &c);
}

void sched_exit_current(trapframe_t* tf) {
    (void)tf;
    if (!current || current == idle_task) return;
    debugcon_writestring("[SCHED] exit ");
    debugcon_print_hex(current->pid);
    debugcon_writestring("\n");
    current->state = PROC_ZOMBIE;

    process_t* next = pick_user(current);
    if (!next) next = idle_task;       // last one out → idle reaps and reports
    // We are running on the exiting task's kernel stack, so it cannot reap
    // itself; the next scheduling context (often idle) calls sched_reap_zombies.
    switch_to(next);
}
