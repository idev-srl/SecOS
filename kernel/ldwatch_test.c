/*
 * ldwatch_test.c — forced test of the lockdep stuck-lock watchdog
 * (-DSECOS_LDWATCH_TEST, needs -DSECOS_LOCKDEP and two CPUs).
 *
 * The BSP holds a test spinlock with interrupts off; an IPI makes an AP spin
 * on it from IRQ context, where the watchdog samples it. Watchdog reports are
 * counted (lockdep_stuck_expect) and printed as "[LDTEST]   caught: ..."
 * instead of [LOCKDEP] lines.
 *   1. For 3 s the BSP keeps advancing the lock's hand-over counter (what
 *      other holders taking and releasing it would do): no report.
 *   2. For 3 s more it holds the lock without any hand-over: exactly one
 *      "stuck" report, naming the BSP (cpu 0) as the owner.
 *   3. After the release the AP gets the lock.
 * The time-gap rule (a waiter that did not run) is covered on the host by
 * tools/ldwatch_host.c, which runs the same lockdep_wait.h.
 * Lines: "[LDWATCH] PASS|FAIL <what>", then "[LDWATCH] DONE ok=N fail=M".
 *
 * SPDX-License-Identifier: MIT
 */
#if defined(SECOS_LDWATCH_TEST) && defined(SECOS_LOCKDEP)
#include <stdint.h>
#include "spinlock.h"
#include "percpu.h"
#include "lockdep.h"
#include "debugcon.h"
#include "../arch/x86/irq.h"
#include "../arch/x86/lapic.h"

extern uint64_t tsc_read(void);
extern uint64_t tsc_hz(void);

static spinlock_t g_lw = SPINLOCK_INIT_CLASS(LK_NONE, "ldwatch-test");
static volatile uint32_t g_lw_in, g_lw_done;
static int g_ok, g_fail;

static void lw_check(int ok, const char* what) {
    debugcon_writestring(ok ? "[LDWATCH] PASS " : "[LDWATCH] FAIL ");
    debugcon_writestring(what);
    debugcon_writestring("\n");
    if (ok) g_ok++; else g_fail++;
}

/* On the AP, in IRQ context (interrupts off): wait for the lock. */
static void lw_handler(uint8_t vec, void* ctx) {
    (void)vec; (void)ctx;
    __atomic_store_n(&g_lw_in, 1, __ATOMIC_SEQ_CST);
    spin_lock(&g_lw);
    spin_unlock(&g_lw);
    __atomic_store_n(&g_lw_done, 1, __ATOMIC_SEQ_CST);
}

/* Busy-wait `ms` with the lock held; `handover` advances its counter every
 * 100 ms. Keeps answering TLB shootdowns like any lock waiter. */
static void lw_hold(uint64_t ms, int handover) {
    uint64_t hz = tsc_hz(), t0 = tsc_read(), next = t0 + hz / 10;
    while (tsc_read() - t0 < hz / 1000 * ms) {
        cpu_relax_poll();
        if (handover && tsc_read() >= next) {
            __atomic_store_n(&g_lw.n_acq, g_lw.n_acq + 1, __ATOMIC_RELAXED);
            next += hz / 10;
        }
    }
}

static int lw_wait_flag(volatile uint32_t* f, uint64_t ms) {
    uint64_t hz = tsc_hz(), t0 = tsc_read();
    while (!__atomic_load_n(f, __ATOMIC_SEQ_CST) && tsc_read() - t0 < hz / 1000 * ms)
        cpu_relax_poll();
    return *f != 0;
}

void ldwatch_test_run(void) {
    uint32_t ap = 0;
    for (uint32_t i = 1; i < smp_cpu_count(); i++) if (smp_cpu_online(i)) { ap = i; break; }
    if (!ap || !tsc_hz()) {
        debugcon_writestring("[LDWATCH] SKIP (needs two CPUs and a calibrated TSC)\n");
        return;
    }
    int v = irq_vec_alloc(ap, lw_handler, 0, "ldwatch");
    if (v < 0) { lw_check(0, "vector allocation"); goto done; }

    lockdep_stuck_expect(1);
    uint32_t c0 = lockdep_stuck_caught();
    uint64_t f = spin_lock_irqsave(&g_lw);
    lapic_ipi(cpu_by_index(ap)->lapic_id, (uint8_t)v);
    lw_check(lw_wait_flag(&g_lw_in, 2000), "the AP is waiting for the lock (IRQ context)");

    lw_hold(3000, 1);
    lw_check(lockdep_stuck_caught() == c0, "a lock that keeps changing hands is not reported as stuck");

    lw_hold(3000, 0);
    lw_check(lockdep_stuck_caught() == c0 + 1, "a lock held 3 s without hand-over is reported once");

    spin_unlock_irqrestore(&g_lw, f);
    lw_check(lw_wait_flag(&g_lw_done, 2000), "the waiter takes the lock after the release");
    lockdep_stuck_expect(0);
    irq_vec_free(v);
done:
    debugcon_writestring(g_fail ? "[LDWATCH] DONE fail ok=" : "[LDWATCH] DONE ok=");
    debugcon_print_hex((uint64_t)g_ok);
    debugcon_writestring(" fail=");
    debugcon_print_hex((uint64_t)g_fail);
    debugcon_writestring("\n");
}
#endif
