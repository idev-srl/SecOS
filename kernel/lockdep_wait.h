/*
 * lockdep_wait.h — the stuck-lock watchdog decision (-DSECOS_LOCKDEP).
 *
 * Pure arithmetic on time samples, so the host test (tools/ldwatch_host.c)
 * runs the exact code the kernel runs. kernel/lockdep.c samples every 1024
 * spins of a contended spinlock or klock wait and calls ld_wait_sample().
 *
 * A waiter reports a lock as stuck only when it has not changed hands for
 * LD_STUCK_S of time this waiter itself observed:
 *   - `progress` is a counter the holders advance on every hand-over (a
 *     spinlock's n_acq, a klock's serving ticket). A busy lock that keeps
 *     changing hands while one waiter loses every race is starvation, not a
 *     deadlock; it is reported (as "starved") only after LD_STARVE_S.
 *   - A gap longer than LD_GAP_MS between two samples means this vCPU did not
 *     run (host preemption, an emulator stall): nothing can be said about the
 *     lock meanwhile, so the window without hand-over restarts. 1024 spins
 *     take microseconds to a few milliseconds even under emulation.
 * A real deadlock is still reported LD_STUCK_S after the waiter last ran
 * without a gap, and the owner/held time are printed as before.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LOCKDEP_WAIT_H
#define LOCKDEP_WAIT_H
#include <stdint.h>

#define LD_STUCK_S  2          /* no hand-over for longer than this: stuck  */
#define LD_STARVE_S 30         /* a waiter that lost for this long: starved */
#define LD_GAP_MS   250        /* samples further apart: we were not running */

typedef struct ld_wait {
    uint64_t t0;        /* wait start                                    */
    uint64_t tw;        /* start of the current window without hand-over */
    uint64_t last;      /* previous sample                               */
    uint64_t seen;      /* hand-over counter at its last change          */
    uint32_t spins;
    uint8_t  stuck, starved;
} ld_wait_t;

enum { LD_WAIT_OK = 0, LD_WAIT_STUCK = 1, LD_WAIT_STARVED = 2 };

static inline void ld_wait_start(ld_wait_t* w, uint64_t progress, uint64_t now) {
    w->t0 = now; w->tw = now; w->last = now;
    w->seen = progress;
    w->spins = 0; w->stuck = 0; w->starved = 0;
}

/* One sample at `now` (TSC, `hz` ticks per second). Returns LD_WAIT_STUCK or
 * LD_WAIT_STARVED once per wait when it is time to report, else LD_WAIT_OK. */
static inline int ld_wait_sample(ld_wait_t* w, uint64_t progress, uint64_t now, uint64_t hz) {
    if (!hz) return LD_WAIT_OK;
    if (now - w->last > hz / 1000 * LD_GAP_MS) w->tw = now;     /* we were not running */
    w->last = now;
    if (progress != w->seen) { w->seen = progress; w->tw = now; }
    if (!w->stuck && now - w->tw > hz * LD_STUCK_S) { w->stuck = 1; return LD_WAIT_STUCK; }
    if (!w->stuck && !w->starved && now - w->t0 > hz * LD_STARVE_S) {
        w->starved = 1;
        return LD_WAIT_STARVED;
    }
    return LD_WAIT_OK;
}

#endif /* LOCKDEP_WAIT_H */
