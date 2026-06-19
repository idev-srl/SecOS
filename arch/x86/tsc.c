/*
 * tsc.c — TSC-based monotonic timekeeping (Phase I, M28-3). See tsc.h.
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "tsc.h"
#include "debugcon.h"

#define PIT_HZ 1193182u   /* 8254 input clock */

static uint64_t g_tsc_hz;     /* calibrated cycles per second        */
static uint64_t g_tsc_base;   /* rdtsc latched at tsc_init()          */
/* ns = (cycles * g_ns_mult) >> NS_SHIFT. Fixed-point so the hot path uses only a
 * native 64x64->128 multiply + shift (no libgcc 128-bit divide). */
#define NS_SHIFT 32
static uint64_t g_ns_mult;

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t tsc_read(void) { return rdtsc(); }
uint64_t tsc_hz(void)   { return g_tsc_hz; }

void tsc_init(void) {
    if (g_tsc_hz) return;

    /* PIT channel 2: gate on (port 0x61 bit0), speaker off (bit1). Channel 2 is
     * not wired to the 8259, so it counts regardless of PIC masking / APIC mode. */
    uint8_t p61 = inb(0x61);
    outb(0x61, (uint8_t)((p61 & ~0x02) | 0x01));
    /* Channel 2, lobyte/hibyte, mode 0 (interrupt on terminal count), binary. */
    outb(0x43, 0xB0);
    /* ~50 ms window for a precise ratio: 1193182 * 0.05 = 59659 ticks. */
    uint16_t cnt = 59659;
    outb(0x42, (uint8_t)(cnt & 0xFF));
    outb(0x42, (uint8_t)(cnt >> 8));

    uint64_t t0 = rdtsc();
    /* Wait for OUT2 to go high (terminal count) — port 0x61 bit5. */
    while (!(inb(0x61) & 0x20)) { }
    uint64_t t1 = rdtsc();

    uint64_t cyc = t1 - t0;          /* TSC cycles elapsed over the 50 ms window */
    /* cycles/s = cyc / 0.05 = cyc * PIT_HZ / cnt (exact ratio, avoids fixed 50 ms). */
    g_tsc_hz = (cyc * PIT_HZ) / cnt;
    g_tsc_base = t1;
    /* Precompute the ns multiplier with a single 64-bit divide (1e9<<32 < 2^63
     * fits, so no 128-bit divide). g_tsc_hz>0 here (cyc>0 over 50 ms). */
    g_ns_mult = (1000000000ull << NS_SHIFT) / g_tsc_hz;

    debugcon_writestring("[TSC] calibrated hz="); debugcon_print_hex(g_tsc_hz);
    debugcon_writestring(" (~"); debugcon_print_hex(g_tsc_hz / 1000000u);
    debugcon_writestring(" MHz)\n");
}

uint64_t ktime_ns(void) {
    if (!g_ns_mult) return 0;
    uint64_t d = rdtsc() - g_tsc_base;
    /* Native 64x64->128 multiply (mulq) + shift — no libgcc helper. */
    return (uint64_t)(((__uint128_t)d * g_ns_mult) >> NS_SHIFT);
}

uint64_t ktime_us(void) { return ktime_ns() / 1000ull; }
uint64_t ktime_ms(void) { return ktime_ns() / 1000000ull; }
