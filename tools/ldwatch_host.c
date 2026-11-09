/*
 * ldwatch_host.c — host test of the lockdep stuck-lock watchdog decision
 * (kernel/lockdep_wait.h, the exact code the kernel runs) on synthetic time.
 *
 *   cc -O2 -Wall -Wextra -I kernel -o /tmp/ldwatch_host tools/ldwatch_host.c && /tmp/ldwatch_host
 *
 * The kernel samples every 1024 spins; here a sample is taken every `step`
 * microseconds of the waiter's own running time, and gaps (the waiter's vCPU
 * descheduled) and hand-overs are injected. Exit 0 = every case passed.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdint.h>
#include "lockdep_wait.h"

#define HZ 1000000000ULL                 /* 1 GHz TSC: 1 tick = 1 ns */
#define US(x) ((uint64_t)(x) * 1000ULL)
#define MS(x) ((uint64_t)(x) * 1000000ULL)

static int g_fail;

/* Run a wait of `total` ns of wall time, sampling every `step` ns. gap_at/gap
 * inject one stall (no samples) of `gap` ns starting at `gap_at`; handover
 * advances the progress counter every `hand` ns while t < hand_until.
 * Returns the time of the first STUCK report (or 0) and of the first STARVED. */
struct run { uint64_t stuck_at, starved_at; int nstuck, nstarved; };
static struct run simulate(uint64_t total, uint64_t step, uint64_t gap_at, uint64_t gap,
                           uint64_t hand, uint64_t hand_until) {
    ld_wait_t w;
    struct run r = { 0, 0, 0, 0 };
    uint64_t base = 12345, prog = 7, next_hand = hand;
    ld_wait_start(&w, prog, base);
    for (uint64_t t = step; t <= total; t += step) {
        if (gap && t > gap_at && t < gap_at + gap) continue;     /* not running */
        if (hand && t < hand_until && t >= next_hand) { prog++; next_hand += hand; }
        int x = ld_wait_sample(&w, prog, base + t, HZ);
        if (x == LD_WAIT_STUCK) { r.nstuck++; if (!r.stuck_at) r.stuck_at = t; }
        if (x == LD_WAIT_STARVED) { r.nstarved++; if (!r.starved_at) r.starved_at = t; }
    }
    return r;
}

static void check(int ok, const char* what) {
    printf("[LDWATCH-HOST] %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

int main(void) {
    struct run r;

    r = simulate(MS(10000), US(20), 0, 0, 0, 0);
    check(r.nstuck == 1 && r.stuck_at > MS(2000) && r.stuck_at < MS(2001),
          "a lock that never changes hands is reported once, 2 s into the wait");

    r = simulate(MS(10000), US(20), 0, 0, MS(50), MS(10000));
    check(r.nstuck == 0 && r.nstarved == 0, "a lock changing hands every 50 ms is never stuck (10 s)");

    r = simulate(MS(10000), US(20), 0, 0, MS(50), MS(3000));
    check(r.nstuck == 1 && r.stuck_at > MS(4900) && r.stuck_at < MS(5001),
          "hand-overs stop at 3 s: reported 2 s after the last one");

    r = simulate(MS(40000), US(20), 0, 0, MS(100), MS(40000));
    check(r.nstuck == 0 && r.nstarved == 1 && r.starved_at > MS(30000) && r.starved_at < MS(30001),
          "a waiter losing every race for 30 s is reported once as starved");

    r = simulate(MS(3000), US(20), MS(500), MS(2500), 0, 0);
    check(r.nstuck == 0, "the waiter's vCPU stalled 2.5 s mid-wait: no report on resume");

    r = simulate(MS(10000), US(20), MS(500), MS(2500), 0, 0);
    check(r.nstuck == 1 && r.stuck_at > MS(5000) && r.stuck_at < MS(5001),
          "after a 2.5 s stall a lock still held is reported 2 s after the resume");

    r = simulate(MS(10000), US(20), MS(1500), MS(200), 0, 0);
    check(r.nstuck == 1 && r.stuck_at > MS(2000) && r.stuck_at < MS(2001),
          "a 200 ms pause (under the gap limit) does not delay detection");

    r = simulate(MS(10000), MS(5), 0, 0, 0, 0);
    check(r.nstuck == 1 && r.stuck_at <= MS(2005), "slow sampling (5 ms per 1024 spins) still detects");

    printf("[LDWATCH-HOST] %s\n", g_fail ? "FAILED" : "all PASS");
    return g_fail ? 1 : 0;
}
