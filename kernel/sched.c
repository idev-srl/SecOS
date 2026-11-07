/*
 * SecOS Kernel - Scheduler
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M7] Cooperative ring-3 scheduling via SYS_YIELD.
 * [M8] Preemptive (timer-driven) scheduling + process lifecycle (exit/reap)
 *      with a kernel idle task as the always-available fallback.
 * [M48] SMP for real: tasks migrate between CPUs. Each task has a home CPU
 *      (a soft preference) and an optional hard pin; a CPU picks its own
 *      tasks first and pulls any other runnable task when it has none (or its
 *      quantum expired), new tasks are pushed to the least-loaded CPU, and an
 *      idle CPU is kicked with a reschedule IPI when work appears. A task is
 *      claimed under the process-table lock and carries on_cpu until its CPU
 *      has fully left it (arch_switch_finish), so no two CPUs ever run, or
 *      reap, the same task. Wake-ups that race a decision to block are caught
 *      by the global wake sequence (g_wake_seq).
 */
#include "sched.h"
#include "terminal.h"
#include "debugcon.h"
#include "vmm.h"
#include "percpu.h"
#include "lapic.h"
#include "bkl.h"
#include "klock.h"

extern uint64_t timer_get_ticks(void);

// [M29] Scheduler state is per-CPU: `current`, `idle_task` and the preemption
// quantum live in this CPU's cpu_t. `this_cpu()` returns CPU 0 before SMP is
// up, so the pre-SMP path is identical.
static inline process_t* cur_task(void) { return this_cpu()->current; }
static inline process_t* idle_of_cpu(void) { return this_cpu()->idle_task; }

// [M8] Preemption quantum: switch the running user task every N timer ticks.
#define SCHED_QUANTUM_TICKS 3

extern void process_foreach(void (*cb)(process_t*, void*), void* user);
extern void tss_set_kernel_stack(uint64_t stack);
extern void arch_iret_to_tf(trapframe_t* tf) __attribute__((noreturn));
extern void arch_switch_finish(trapframe_t* tf, volatile uint32_t* prev_on_cpu,
                               uint64_t stack_top) __attribute__((noreturn));
extern int  process_destroy(process_t* p);
extern void irq_eoi(void);

// [M48] Bumped (after the wake condition became true) by every wake-up that
// can race a task deciding to block: a pipe/pty/IPC/child-exit/signal event.
// syscall_handler snapshots it into current->wake_snap at entry; if it moved
// by the time sched_block_current() would block, the syscall re-runs at once
// instead of sleeping on a condition that is already satisfied.
volatile uint64_t g_wake_seq = 1;
static inline void wake_seq_bump(void) { __atomic_add_fetch(&g_wake_seq, 1, __ATOMIC_SEQ_CST); }
uint64_t sched_wake_seq(void) { return __atomic_load_n(&g_wake_seq, __ATOMIC_SEQ_CST); }

// The final step of every switch runs on a private per-CPU stack (CPU 0's is
// here; APs get theirs in smp_init).
static uint8_t g_cpu0_switch_stack[4096] __attribute__((aligned(16)));

// ---- CPU selection ----
static process_t* claim_next(process_t* after) {
    return process_claim_next(this_cpu()->index, after);
}

// [M48] Kick one idle CPU into the scheduler because a task became runnable.
// `home` (or -1) is the woken task's home CPU, preferred when it is idle;
// `pin` (or -1) restricts the choice to that CPU.
static uint32_t g_kick_rotor;
static void kick_idle_cpu(int32_t home, int32_t pin) {
    uint32_t n = g_cpu_count;
    if (n <= 1) return;
    cpu_t* me = this_cpu();
    cpu_t* target = NULL;
    if (pin >= 0) {
        cpu_t* c = cpu_by_index((uint32_t)pin);
        if (c != me && c->online && c->idle_task && c->in_idle) target = c;
    } else {
        if (home >= 0 && (uint32_t)home < n) {
            cpu_t* c = cpu_by_index((uint32_t)home);
            if (c != me && c->online && c->idle_task && c->in_idle && !c->resched_pending) target = c;
        }
        uint32_t r = __atomic_fetch_add(&g_kick_rotor, 1, __ATOMIC_RELAXED);
        for (uint32_t k = 0; k < n && !target; k++) {
            cpu_t* c = cpu_by_index((r + k) % n);
            if (c != me && c->online && c->idle_task && c->in_idle && !c->resched_pending) target = c;
        }
    }
    if (!target) return;
    if (__sync_lock_test_and_set(&target->resched_pending, 1) == 0)
        lapic_ipi(target->lapic_id, VEC_IPI_RESCHED);
}
void sched_kick_for(process_t* p) {
    if (!p) { kick_idle_cpu(-1, -1); return; }
    kick_idle_cpu((int32_t)p->cpu_affinity, p->cpu_pin);
}

void sched_init(void) {
    // [M29] Ensure the BSP (CPU 0) exists before any per-CPU access. Its real
    // LAPIC ID is fixed up after ACPI/LAPIC bring-up via smp_set_bsp_lapic_id().
    if (smp_cpu_count() == 0) smp_register_cpu(0);
    cpu_t* c0 = cpu_by_index(0);
    c0->online = 1;     // the boot CPU is always online
    c0->switch_stack_top = (uint64_t)(g_cpu0_switch_stack + sizeof(g_cpu0_switch_stack));
    c0->current = NULL; c0->idle_task = NULL;
}
process_t* sched_get_current(void) { return cur_task(); }
void sched_set_current(process_t* p) {
    cpu_t* me = this_cpu();
    me->current = p;
    me->in_idle = (p && p == me->idle_task);
    // A user task installed by hand (the M7 demo) is on this CPU like a claimed one.
    if (p && p != me->idle_task && p->pid != 0) p->on_cpu = 1;
}
void sched_set_idle(process_t* idle) {
    cpu_t* me = this_cpu();
    me->idle_task = idle;
    me->in_idle = (me->current && me->current == idle);
}
int sched_add_process(process_t* p) { (void)p; return 0; }

// [M48] Make a freshly created task (PROC_NEW, never picked) runnable. A
// process under construction is NEW and invisible to every CPU, so its creator
// can finish wiring it (fds, pgid, credentials) without racing a CPU that would
// otherwise run it half-built; only this NEW -> READY step publishes it.
// Returns 1 if it was published, 0 if it was not NEW.
int sched_make_ready(process_t* p) {
    if (!p) return 0;
    p->ready_tick = timer_get_ticks();
    if (!__sync_bool_compare_and_swap(&p->state, PROC_NEW, PROC_READY)) return 0;
    sched_kick_for(p);
    return 1;
}

// [M48] BLOCKED/STOPPED -> READY from outside proc_lock (signals, job control).
int sched_wake_task(process_t* p, int from_state) {
    if (!p) return 0;
    if (p->state != from_state) return 0;
    p->ready_tick = timer_get_ticks();
    if (!__sync_bool_compare_and_swap(&p->state, from_state, PROC_READY)) return 0;
    wake_seq_bump();
    sched_kick_for(p);
    return 1;
}

// ---- low-level switch: load target space + kernel stack, iretq into it ----
static void switch_to(process_t* next) __attribute__((noreturn));
// x87/SSE state of user tasks: saved at switch-out, restored at switch-in (the
// kernel never touches these registers: it is built -mno-sse). Eager, so a task
// that migrates carries its exact register file to the next CPU: the image is
// written here, before on_cpu is released, and loaded by whichever CPU claims it.
static const uint8_t g_fpu_init[512] __attribute__((aligned(16))) = {
    [0] = 0x7F, [1] = 0x03,                        // FCW = 0x037F
    [24] = 0x80, [25] = 0x1F,                      // MXCSR = 0x1F80
};
static void fpu_switch(process_t* prev, process_t* next, process_t* idle) {
    if (prev == next) return;
    // A dying task (ZOMBIE) keeps nothing worth saving.
    if (prev && prev != idle && prev->pid != 0 && prev->state != PROC_ZOMBIE) {
        __asm__ volatile("fxsave64 %0" : "=m"(*(uint8_t(*)[512])prev->fpu));
        prev->fpu_valid = 1;
    }
    if (next && next->pid != 0) {
        const uint8_t* img = next->fpu_valid ? next->fpu : g_fpu_init;
        __asm__ volatile("fxrstor64 %0" : : "m"(*(const uint8_t(*)[512])img));
    }
}

static void switch_to(process_t* next) {
    __asm__ volatile ("cli" ::: "memory");
    cpu_t* me = this_cpu();
    process_t* prev = me->current;
    // [M48] A task leaving the CPU mid-syscall (block/exit/kill/stop/yield)
    // gives up the kernel lock; it re-runs its syscall from the start. (An
    // idle task holding it is never switched away: see the tick.)
    if (prev && prev != me->idle_task) bkl_drop_for(prev);
    klock_on_switch(prev, me->idle_task);   // [M50] nothing else held; IRQ nesting reset
    { extern void kiface_count_switch(void); kiface_count_switch(); }   // /proc/stat ctxt
    fpu_switch(prev, next, me->idle_task);
    me->current = next;
    me->in_idle = (next == me->idle_task);
    next->state = PROC_RUNNING;
    me->nr_switches++;
    // [M29] Observability: a user task running on an application processor proves
    // multicore scheduling (logged once per task per AP it lands on).
    if (me->index != 0 && next->pid != 0 && me->index < 64 &&
        !((next->smp_logged >> me->index) & 1)) {
        next->smp_logged |= 1ULL << me->index;
        debugcon_writestring("[SMP] cpu="); debugcon_print_hex(me->index);
        debugcon_writestring(" run pid="); debugcon_print_hex(next->pid);
        debugcon_writestring("\n");
    }
    vmm_switch_space(next->space);
    tss_set_kernel_stack(next->kstack_top);
    me->slice_left = SCHED_QUANTUM_TICKS;
    // Release the previous task only once we are off its kernel stack: the
    // asm switches to this CPU's switch stack first (idle tasks are per-CPU and
    // not in the table, so they need no release).
    volatile uint32_t* rel = (prev && prev != next && prev != me->idle_task && prev->pid != 0)
                             ? &prev->on_cpu : 0;
    arch_switch_finish(next->tf, rel, me->switch_stack_top);
}

static inline void save_tf(process_t* p, trapframe_t* tf) {
    const uint8_t* s = (const uint8_t*)tf;
    uint8_t* d = (uint8_t*)p->tf;
    for (int i = 0; i < (int)sizeof(trapframe_t); i++) d[i] = s[i];
}

// The task leaving the CPU becomes READY. It is still on_cpu (nobody may run it
// before this CPU has left it), so no kick: an idle CPU's next tick pulls it.
static void requeue(process_t* p) {
    p->ready_tick = timer_get_ticks();
    p->state = PROC_READY;
}

// ---- [M8] preemptive tick ----
void sched_on_timer_tick(trapframe_t* tf) {
    cpu_t* me = this_cpu();
    process_t* cur = me->current;
    if (cur) cur->cpu_ticks++;
    { extern void kiface_account_tick(process_t*, int, int);         // /proc CPU times
      kiface_account_tick(cur, (tf->cs & 3) == 3, cur == me->idle_task); }
    { extern uint64_t timer_get_ticks(void); uint64_t now = timer_get_ticks();
      // [M48] Time-driven wake-ups run once per tick on the BSP (they scan the
      // whole table; with N CPUs they ran N times per millisecond).
      if (me->index == 0) {
          sched_wake_sleepers(now);                                   // [M17]
          extern void tty_wake_readers(uint64_t); tty_wake_readers(now);   // blocked console readers
          extern void debugcon_user_tick(uint64_t); debugcon_user_tick(now);  // [M46] prompt tail
      }
      extern void debugcon_ktick(uint64_t); debugcon_ktick(now); }    // [M48] stale partial line
    if (!me->idle_task) return;        // preemption only active once a scheduler is armed
    if (!cur) return;                  // not yet running the scheduler

    int from_user = ((tf->cs & 3) == 3);

    if (cur == me->idle_task) {
        // [M48] ... unless it holds the kernel lock (a kernel-shell command or
        // a reap in progress): the lock belongs to the idle task, which must
        // finish its critical section first.
        if (bkl_held_by(cur)) return;
        // Idle is always safe to preempt: hand the CPU to a runnable user task.
        // Save idle's interrupted context so that returning to it (after the
        // user task exits) resumes where it was — this lets the interactive
        // shell run as the idle task and get control back after `run`.
        process_t* next = claim_next(NULL);
        if (next) { if (cur->tf) save_tf(cur, tf); irq_eoi(); switch_to(next); }
        return;
    }

    // A user task is current. Only preempt when it was interrupted in ring-3
    // (never mid-syscall in ring-0), and only when its quantum has expired.
    if (!from_user) return;
    if (me->slice_left > 1) { me->slice_left--; return; }

    process_t* next = claim_next(cur);
    if (!next) { me->slice_left = SCHED_QUANTUM_TICKS; return; }   // nothing else: keep running

    save_tf(cur, tf);
    if (cur->state == PROC_RUNNING) requeue(cur);
    debugcon_writestring("[SCHED] preempt ");
    debugcon_print_hex(cur->pid);
    debugcon_writestring(" -> ");
    debugcon_print_hex(next->pid);
    debugcon_writestring("\n");
    irq_eoi();
    switch_to(next);
}

// ---- [M48] reschedule IPI: an idle CPU was kicked because work appeared ----
void sched_on_resched_ipi(trapframe_t* tf) {
    cpu_t* me = this_cpu();
    me->resched_pending = 0;
    me->nr_ipi_resched++;
    extern void lapic_eoi(void);
    lapic_eoi();                       // before a switch that never returns here
    process_t* cur = me->current;
    if (!me->idle_task || !cur || cur != me->idle_task) return;
    if (bkl_held_by(cur)) return;      // idle in a kernel-lock section: next tick
    process_t* next = claim_next(NULL);
    if (next) { if (cur->tf) save_tf(cur, tf); switch_to(next); }
}

// ---- [M7] cooperative yield from SYS_YIELD ----
void sched_yield_from_syscall(trapframe_t* tf) {
    if (!cur_task()) return;
    process_t* cur = cur_task();
    if (cur->tf) { save_tf(cur, tf); cur->tf->rax = 0; } // yield returns 0

    process_t* next = claim_next(cur);
    if (!next) return;                                // nothing else: keep running

    debugcon_writestring("[SCHED] switch ");
    debugcon_print_hex(cur->pid);
    debugcon_writestring(" -> ");
    debugcon_print_hex(next->pid);
    debugcon_writestring("\n");
    if (cur->state == PROC_RUNNING && cur != idle_of_cpu()) requeue(cur);
    switch_to(next);
}

// ---- legacy cooperative entry (kept for compatibility) ----
void sched_yield(void) {
    if (!cur_task()) return;
    process_t* cur = cur_task();
    process_t* next = claim_next(cur);
    if (next) {
        if (cur->state == PROC_RUNNING && cur != idle_of_cpu()) requeue(cur);
        switch_to(next);
    }
}

// Next task for a CPU whose current task stops running (exit/kill/stop/block).
static process_t* next_or_idle(process_t* cur) {
    process_t* next = claim_next(cur);
    return next ? next : idle_of_cpu();
}

// ---- [M8] exit + reaping ----
struct alive_ctx { int n; process_t* idle; };
static void alive_cb(process_t* p, void* u) {
    struct alive_ctx* c = (struct alive_ctx*)u;
    if (p == c->idle) return;
    if (p->state != PROC_ZOMBIE) c->n++;
}
int sched_count_alive_user(void) {
    struct alive_ctx c = { 0, idle_of_cpu() };
    process_foreach(alive_cb, &c);
    return c.n;
}

void sched_reap_zombies(void) {
    // [M48] Reap every zombie that no CPU is on any more (process_reap_one
    // checks on_cpu and detaches it under the proc lock), from any CPU.
    process_t* z;
    while ((z = process_reap_one()) != NULL) {
        bkl_lock();                     // [M48] teardown touches VFS/pty/console state
        process_destroy(z);             // frees space/tables/kstack/tf (zombie != current)
        bkl_unlock();
        process_reap_done();            // only now is its memory back in the PMM
    }
}

// [M30] Notify the parent of a child's exit/stop via SIGCHLD.
static void notify_parent_sigchld(process_t* child) {
    extern process_t* process_find_by_pid(uint32_t);
    extern int signal_post(process_t*, int);
    if (!child || child->ppid == 0) return;
    process_t* parent = process_find_by_pid(child->ppid);
    if (parent) signal_post(parent, 17 /*SIGCHLD*/);
}

// An exiting process's children go to init (PID 1). When init itself exits,
// say so on the console: the kernel shell takes over (kernel/kernel.c).
static void exit_reparent(process_t* p) {
    uint32_t init = process_init_pid();
    if (!init) return;
    if (p->pid == init) {
        process_set_init_pid(0);
        debugcon_writestring("[INIT] PID 1 (/bin/init) exited, status=");
        debugcon_print_hex((uint64_t)(uint32_t)p->exit_code);
        debugcon_writestring("\n");
        return;
    }
    if (process_reparent_children(p->pid)) sched_wake_parent_waitany(init);
}

void sched_exit_current(trapframe_t* tf) {
    (void)tf;
    process_t* cur = cur_task();
    if (!cur || cur == idle_of_cpu()) return;
    bkl_lock();                        // [M48] teardown below; switch_to drops it
    debugcon_writestring("[SCHED] exit ");
    debugcon_print_hex(cur->pid);
    debugcon_writestring("\n");
    // [M16] exit_code was set by the SYS_EXIT handler from the user's status.
    process_release_pipes(cur);        // close pipe ends now: readers see EOF before wait()
    cur->state = PROC_ZOMBIE;
    exit_reparent(cur);                                   // children -> PID 1
    notify_parent_sigchld(cur);                           // [M30] SIGCHLD to parent
    // [M16/M39] Deliver the status to a blocked waiter; if one collected it, this
    // zombie is reapable. Otherwise it persists until the parent calls wait().
    if (sched_wake_waitpid(cur->pid, cur->exit_code) > 0) cur->collected = 1;
    sched_wake_parent_waitany(cur->ppid);                 // [M39] wait-any waiters

    // We are running on the exiting task's kernel stack, so it cannot reap
    // itself; any CPU reaps it once switch_to has released it (on_cpu = 0).
    switch_to(next_or_idle(cur));
}

// [M15] Terminate the current process after an unrecoverable ring-3 fault and
// switch to the next runnable task — the same mechanism as a SYS_EXIT, but
// driven by the exception handler. NEVER returns. If there is no killable
// current (idle/none), the fault is unrecoverable and we halt (a kernel bug).
// 'sig' is the terminating signal number (a ring-3 CPU fault is mapped to its
// signal by the exception handler: #PF/#GP -> SIGSEGV, #DE -> SIGFPE, ...).
void sched_kill_current(int sig) {
    process_t* cur = cur_task();
    if (!cur || cur == idle_of_cpu()) {
        debugcon_writestring("[KILL] no killable process -> halt\n");
        debugcon_kflush();
        for (;;) __asm__ volatile("cli; hlt");
    }
    bkl_lock();                        // [M48] teardown below; switch_to drops it
    debugcon_writestring("[KILL] pid=");
    debugcon_print_hex(cur->pid);
    debugcon_writestring(" sig=");
    debugcon_print_hex((uint64_t)sig);
    debugcon_writestring("\n");
    // POSIX "killed by signal" wait status (no core dumps -> core flag clear).
    cur->exit_code = W_SIGNALED(sig);
    process_release_pipes(cur);        // close pipe ends now: readers see EOF before wait()
    cur->state = PROC_ZOMBIE;
    exit_reparent(cur);                                   // children -> PID 1
    notify_parent_sigchld(cur);                           // [M30] SIGCHLD to parent
    if (sched_wake_waitpid(cur->pid, cur->exit_code) > 0) cur->collected = 1;
    sched_wake_parent_waitany(cur->ppid);                 // [M39] wait-any waiters

    switch_to(next_or_idle(cur));      // NORETURN
}

// [M30] Stop the current process (SIGSTOP/SIGTSTP): save its ring-3 context,
// mark it PROC_STOPPED (the scheduler skips it until SIGCONT flips it READY),
// notify the parent, and switch away. Mirrors sched_block_current. NORETURN.
void sched_stop_current(trapframe_t* tf) {
    process_t* cur = cur_task();
    if (!cur || cur == idle_of_cpu()) {
        for (;;) __asm__ volatile("cli; hlt");
    }
    bkl_lock();                        // [M48] notify paths below; switch_to drops it
    debugcon_writestring("[STOP] pid=");
    debugcon_print_hex(cur->pid);
    debugcon_writestring("\n");
    save_tf(cur, tf);
    cur->stop_report = 1;              // waitpid(WUNTRACED) reports it once
    cur->cont_report = 0;
    if (cur->stop_sig <= 0) cur->stop_sig = 19 /*SIGSTOP*/;
    cur->state = PROC_STOPPED;         // on_cpu still held until switch_to leaves it
    notify_parent_sigchld(cur);
    sched_wake_parent_waitany(cur->ppid);   // a blocked waitpid(-1, WUNTRACED)
    switch_to(next_or_idle(cur));      // NORETURN
}

// [M16/M17] Block the current process: save its mid-syscall trapframe and switch
// away. The caller must have armed a wait condition (wait_pid / sleep_until /
// recv_chan) and rewound rip so the syscall re-runs on wake. NORETURN for a
// user task. [M48] If a wake-up happened since the syscall started (another
// CPU may have satisfied the condition between the caller's check and here,
// and its wake found nobody BLOCKED yet), do not sleep: re-run the syscall at
// once. The check and the RUNNING -> BLOCKED step are one critical section
// under the proc lock that every table-scanning waker also takes.
void sched_block_current(trapframe_t* tf) {
    process_t* cur = cur_task();
    if (!cur || cur == idle_of_cpu()) return;
    save_tf(cur, tf);
    uint64_t fl = process_lock_irqsave();
    if (sched_wake_seq() != cur->wake_snap) {
        process_unlock_irqrestore(fl);
        cur->wake_snap = sched_wake_seq();
        bkl_drop_for(cur);             // back to ring 3 without passing syscall_handler's unlock
        arch_iret_to_tf(cur->tf);      // re-execute the rewound `int 0x80`
    }
    cur->state = PROC_BLOCKED;
    process_unlock_irqrestore(fl);
    cur->ki.nvcsw++;
    switch_to(next_or_idle(cur));      // NORETURN
}

// ---- wake-ups ----
// Every table-scanning wake runs its callback under the proc lock and flips
// BLOCKED -> READY with a compare-and-swap (signal_post may race it from
// outside the lock). Woken tasks' home/pin are collected so idle CPUs can be
// kicked after the scan, outside the lock.
#define WAKE_KICK_MAX 8
struct wake_kick { int n; int32_t home[WAKE_KICK_MAX]; int32_t pin[WAKE_KICK_MAX]; };
static int wake_locked(process_t* p, struct wake_kick* k) {
    if (p->state != PROC_BLOCKED) return 0;
    p->ready_tick = timer_get_ticks();
    if (!__sync_bool_compare_and_swap(&p->state, PROC_BLOCKED, PROC_READY)) return 0;
    if (k->n < WAKE_KICK_MAX) { k->home[k->n] = (int32_t)p->cpu_affinity; k->pin[k->n] = p->cpu_pin; }
    k->n++;
    return 1;
}
static void wake_kick_all(const struct wake_kick* k) {
    for (int i = 0; i < k->n; i++) {
        if (i < WAKE_KICK_MAX) kick_idle_cpu(k->home[i], k->pin[i]);
        else kick_idle_cpu(-1, -1);
    }
}

// [M16] Wake any process blocked in SYS_WAIT on 'pid', delivering 'code'.
struct wake_wp_ctx { uint32_t pid; int code; int woke; struct wake_kick k; };
static void wake_wp_cb(process_t* p, void* u) {
    struct wake_wp_ctx* w = (struct wake_wp_ctx*)u;
    if (p->state == PROC_BLOCKED && p->wait_pid >= 0 && (uint32_t)p->wait_pid == w->pid) {
        p->wait_result = w->code;
        p->wait_ready  = 1;
        p->wait_pid    = -1;
        if (wake_locked(p, &w->k)) w->woke++;
    }
}
// [M39] Returns the number of waiters woken (0 = no parent was blocked on this
// pid yet, so the exiting child must remain an uncollected zombie until wait()).
int sched_wake_waitpid(uint32_t pid, int code) {
    struct wake_wp_ctx w = { pid, code, 0, { 0, {0}, {0} } };
    wake_seq_bump();
    process_foreach(wake_wp_cb, &w);
    wake_kick_all(&w.k);
    return w.woke;
}

// [M39] Wake a parent blocked in SYS_WAITANY (waitpid(-1)) when one of its
// children exits. The parent re-runs SYS_WAITANY and reaps the now-zombie child
// (which stays uncollected until then), so we just flip it READY.
struct wake_wa_ctx { uint32_t ppid; struct wake_kick k; };
static void wake_waitany_cb(process_t* p, void* u) {
    struct wake_wa_ctx* c = (struct wake_wa_ctx*)u;
    if (p->pid == c->ppid && p->state == PROC_BLOCKED && p->wait_pid == -2) {
        p->wait_pid = -1;
        wake_locked(p, &c->k);
    }
}
void sched_wake_parent_waitany(uint32_t ppid) {
    if (ppid == 0) return;
    struct wake_wa_ctx c = { ppid, { 0, {0}, {0} } };
    wake_seq_bump();
    process_foreach(wake_waitany_cb, &c);
    wake_kick_all(&c.k);
}

// Job control: a stopped child was resumed by SIGCONT. SIGCHLD + wake a parent
// blocked in waitpid(-1, WCONTINUED). Called from signal_post (outside proc_lock).
void sched_notify_parent_cont(process_t* child) {
    if (!child) return;
    notify_parent_sigchld(child);
    sched_wake_parent_waitany(child->ppid);
}

// [M17] Wake processes whose SYS_SLEEP deadline has elapsed. Time only moves
// forward, so a sleeper that blocks just after this scan is woken by the next
// tick: no wake-sequence bump needed.
struct wake_sl_ctx { uint64_t now; struct wake_kick k; };
static void wake_sleep_cb(process_t* p, void* u) {
    struct wake_sl_ctx* c = (struct wake_sl_ctx*)u;
    if (p->state == PROC_BLOCKED && p->sleep_until != 0 && c->now >= p->sleep_until) {
        wake_locked(p, &c->k); // sleep_until cleared when the syscall re-runs
    }
}
void sched_wake_sleepers(uint64_t now) {
    struct wake_sl_ctx c = { now, { 0, {0}, {0} } };
    process_foreach(wake_sleep_cb, &c);
    wake_kick_all(&c.k);
}

// [M17] Wake processes blocked receiving on IPC channel 'chan'.
struct wake_ch_ctx { int chan; struct wake_kick k; };
static void wake_chan_cb(process_t* p, void* u) {
    struct wake_ch_ctx* c = (struct wake_ch_ctx*)u;
    if (p->state == PROC_BLOCKED && p->recv_chan == c->chan) {
        p->recv_chan = -1;
        wake_locked(p, &c->k);
    }
}
void sched_wake_chan(int chan) {
    struct wake_ch_ctx c = { chan, { 0, {0}, {0} } };
    wake_seq_bump();
    process_foreach(wake_chan_cb, &c);
    wake_kick_all(&c.k);
}

// [M25] Wake processes blocked on pipe 'pp' (either a reader waiting for data or
// a writer waiting for space). The woken task re-runs its read/write syscall and
// re-checks the condition, so a spurious wake just re-blocks.
struct wake_pp_ctx { void* pp; struct wake_kick k; };
static void wake_pipe_cb(process_t* p, void* u) {
    struct wake_pp_ctx* c = (struct wake_pp_ctx*)u;
    if (p->state == PROC_BLOCKED && p->wait_pipe == c->pp) {
        p->wait_pipe = 0;
        wake_locked(p, &c->k);
    }
    // A poll() sleeper re-checks every fd it watches on any pipe activity.
    if (p->state == PROC_BLOCKED && p->ki.poll_wait) {
        p->ki.poll_wait = 0;
        wake_locked(p, &c->k);
    }
}
void sched_wake_pipe(void* pp) {
    struct wake_pp_ctx c = { pp, { 0, {0}, {0} } };
    wake_seq_bump();
    process_foreach(wake_pipe_cb, &c);
    wake_kick_all(&c.k);
}

// Wake processes blocked on pty 'pt' (a slave reader waiting for input, a
// master reader waiting for output, a writer on either side waiting for room)
// or blocked polling any pty (wait_pty == PTY_WAIT_ANY, see pty.h). Like the
// pipe wake, the woken task re-runs its syscall and re-checks, so a spurious
// wake just re-blocks.
struct wake_pt_ctx { void* pt; struct wake_kick k; };
static void wake_pty_cb(process_t* p, void* u) {
    struct wake_pt_ctx* c = (struct wake_pt_ctx*)u;
    if (p->state == PROC_BLOCKED && p->wait_pty &&
        (p->wait_pty == c->pt || p->wait_pty == (void*)1 /* PTY_WAIT_ANY */)) {
        p->wait_pty = 0;
        wake_locked(p, &c->k);
    }
}
void sched_wake_pty(void* pt) {
    struct wake_pt_ctx c = { pt, { 0, {0}, {0} } };
    wake_seq_bump();
    process_foreach(wake_pty_cb, &c);
    wake_kick_all(&c.k);
}

// [M48] Generic "something a blocked task may wait for changed" (tty input,
// sockets): bump the wake sequence so a racing block re-runs its syscall.
void sched_wake_note(void) { wake_seq_bump(); }

// [M48] Per-CPU scheduler statistics (shell `cpus`, /proc).
void sched_cpu_stats(uint32_t cpu, uint64_t* switches, uint64_t* migrations,
                     uint64_t* ipi_tlb, uint64_t* ipi_resched) {
    cpu_t* c = cpu_by_index(cpu);
    *switches = c->nr_switches; *migrations = c->nr_migrations;
    *ipi_tlb = c->nr_ipi_tlb; *ipi_resched = c->nr_ipi_resched;
}
