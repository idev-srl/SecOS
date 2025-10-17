/*
 * SecOS Kernel - Simple Scheduler
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "sched.h"
#include "terminal.h"

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

int sched_add_process(process_t* p) { (void)p; return 0; }

void sched_yield(void) {
    process_t* next = pick_next(current);
    if (next && next != current) {
        if (current && current->state == PROC_RUNNING) current->state = PROC_READY;
        next->state = PROC_RUNNING;
        current = next;
    // TODO: real context switch (save regs, load CR3, etc.)
    }
}

void sched_on_timer_tick(void) {
    if (current) current->cpu_ticks++;
    // For now yield every tick (future: quantum)
    sched_yield();
}
