/*
 * lapic.c — Minimal Local APIC support for MSI/MSI-X interrupt delivery.
 *
 * MSI-X interrupt support — IMPLEMENTED BUT NOT YET TESTED / not enabled by
 * default (the kernel is polled). The kernel uses the 8259 PIC + PIT; the LAPIC
 * is only needed to *receive* message-signalled interrupts. These routines are
 * called solely from the gated *_USE_IRQ driver paths and are no-ops in the
 * default build. They have NOT been exercised with a real interrupt yet.
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "lapic.h"
#include "vmm.h"        /* phys_to_virt / vmm_extend_physmap */
#include "debugcon.h"
#include <stddef.h>

/* IA32_APIC_BASE MSR: bit11 = global enable, bits 12.. = APIC base phys addr. */
#define IA32_APIC_BASE_MSR 0x1B
#define APIC_BASE_ENABLE   (1u << 11)

/* LAPIC register offsets (relative to the mapped base). */
#define LAPIC_REG_ID    0x020
#define LAPIC_REG_EOI   0x0B0
#define LAPIC_REG_SVR   0x0F0   /* Spurious Interrupt Vector Register */
#define LAPIC_SVR_ENABLE (1u << 8)

/* Spurious vector to install when we enable the LAPIC. 0xFF is conventional. */
#define LAPIC_SPURIOUS_VECTOR 0xFF

static volatile uint32_t* g_lapic;   /* mapped LAPIC base, NULL until enabled */

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static inline void cpuid_get(uint32_t leaf, uint32_t* a, uint32_t* b,
                             uint32_t* c, uint32_t* d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf));
}

int lapic_is_enabled(void) { return g_lapic != NULL; }

int lapic_enable(void) {
    if (g_lapic) return 0;   /* already up */

    /* CPUID.01h:EDX bit 9 = on-chip APIC present. */
    uint32_t a, b, c, d;
    cpuid_get(1, &a, &b, &c, &d);
    if (!(d & (1u << 9))) {
        debugcon_writestring("[MSIX] LAPIC: no APIC reported by CPUID\n");
        return -1;
    }

    /* Read the APIC base MSR, set the global enable bit, write it back. The base
     * physical address lives in bits 12.. (we keep whatever firmware set). */
    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    base |= APIC_BASE_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, base);

    uint64_t phys = base & 0xFFFFF000ull;
    vmm_extend_physmap(phys + 0x1000);
    g_lapic = (volatile uint32_t*)phys_to_virt(phys);

    /* Software-enable the LAPIC via the Spurious Interrupt Vector Register. */
    g_lapic[LAPIC_REG_SVR / 4] = LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR;

    debugcon_writestring("[MSIX] LAPIC enabled base=0x"); debugcon_print_hex(phys);
    debugcon_writestring(" id=0x"); debugcon_print_hex(g_lapic[LAPIC_REG_ID / 4] >> 24);
    debugcon_writestring(" (NOT TESTED)\n");
    return 0;
}

void lapic_eoi(void) {
    if (g_lapic) g_lapic[LAPIC_REG_EOI / 4] = 0;
}
