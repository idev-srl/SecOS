/*
 * kclock.h — kernel wall clock + POSIX clocks.
 *
 * The CMOS RTC is read once at boot (UTC) and anchored to the monotonic TSC
 * clock (arch/x86/tsc.c ktime_ns), so CLOCK_REALTIME advances with nanosecond
 * resolution between RTC reads and never steps on its own. settimeofday /
 * clock_settime only move the REALTIME offset; MONOTONIC/BOOTTIME are untouched.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SECOS_KCLOCK_H
#define SECOS_KCLOCK_H
#include <stdint.h>

/* POSIX clock ids (Linux values). */
#define KCLOCK_REALTIME           0
#define KCLOCK_MONOTONIC          1
#define KCLOCK_PROCESS_CPUTIME_ID 2
#define KCLOCK_THREAD_CPUTIME_ID  3
#define KCLOCK_MONOTONIC_RAW      4
#define KCLOCK_REALTIME_COARSE    5
#define KCLOCK_MONOTONIC_COARSE   6
#define KCLOCK_BOOTTIME           7

/* Read the RTC and anchor the wall clock. Idempotent; also run lazily. */
void     kclock_init(void);
/* Nanoseconds since boot (TSC when calibrated, else the 1 kHz tick). */
uint64_t kclock_mono_ns(void);
/* Nanoseconds since the Unix epoch (UTC). */
uint64_t kclock_real_ns(void);
/* Whole seconds since the Unix epoch — what filesystems stamp into inodes. */
uint64_t kclock_real_sec(void);
/* Step the wall clock to `ns` since the epoch. */
void     kclock_set_real_ns(uint64_t ns);
/* Unix seconds of a UTC civil date (no time zones). */
uint64_t kclock_civil_to_unix(int year, int mon, int day, int h, int m, int s);
/* Unix seconds -> UTC civil date. */
void     kclock_unix_to_civil(uint64_t t, int* year, int* mon, int* day, int* h, int* m, int* s);

#endif
