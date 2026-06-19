/*
 * tsc.h — TSC-based monotonic timekeeping (Phase I, M28-3).
 *
 * The Time Stamp Counter (rdtsc) is calibrated once at boot against PIT channel
 * 2 (the gate/poll trick — independent of the 8259, the PIT IRQ0 and the APIC
 * switchover, so it works in either interrupt mode). It then backs a monotonic
 * nanosecond clock for sub-millisecond timekeeping; the 1 kHz scheduler tick
 * (timer_ticks) is unchanged and still drives preemption/sleep.
 *
 * Assumes a constant/invariant TSC (true on every CPU SecOS targets — QEMU,
 * modern Intel/AMD, VMware). We do not yet handle TSC that varies with P-states.
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef TSC_H
#define TSC_H
#include <stdint.h>

/* Calibrate the TSC against PIT channel 2 and latch the boot baseline. Idempotent.
 * Safe to call in either PIC or APIC interrupt mode. */
void tsc_init(void);

/* Raw 64-bit time-stamp counter. */
uint64_t tsc_read(void);

/* Calibrated TSC frequency in Hz (cycles per second), 0 if not yet calibrated. */
uint64_t tsc_hz(void);

/* Monotonic time since tsc_init(), in nanoseconds / microseconds / milliseconds.
 * 0 if the TSC was never calibrated. */
uint64_t ktime_ns(void);
uint64_t ktime_us(void);
uint64_t ktime_ms(void);

#endif /* TSC_H */
