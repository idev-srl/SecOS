/*
 * lapic.c — Local APIC + IOAPIC interrupt support.
 *
 * Two roles:
 *  - MSI/MSI-X delivery for the gated *_USE_IRQ driver paths (lapic_enable /
 *    lapic_eoi). Off by default unless a driver opts in.
 *  - M28-2 APIC switchover (apic_switchover): retire the legacy 8259 PIC + PIT
 *    IRQ0 in favour of the Local APIC timer (scheduler tick) and the IOAPIC
 *    (device IRQs), driven by the ACPI/MADT topology (arch/x86/acpi.c). Falls
 *    back to PIC/PIT when no MADT/IOAPIC is present. irq_eoi() routes the
 *    end-of-interrupt to whichever controller currently owns the line.
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "lapic.h"
#include "acpi.h"       /* acpi_get / acpi_irq_to_gsi — discovered topology */
#include "vmm.h"        /* phys_to_virt / vmm_extend_physmap */
#include "debugcon.h"
#include <stddef.h>

/* IA32_APIC_BASE MSR: bit11 = global enable, bits 12.. = APIC base phys addr. */
#define IA32_APIC_BASE_MSR 0x1B
#define APIC_BASE_ENABLE   (1u << 11)

/* LAPIC register offsets (relative to the mapped base). */
#define LAPIC_REG_ID    0x020
#define LAPIC_REG_TPR   0x080   /* Task Priority Register                     */
#define LAPIC_REG_EOI   0x0B0
#define LAPIC_REG_SVR   0x0F0   /* Spurious Interrupt Vector Register         */
#define LAPIC_SVR_ENABLE (1u << 8)
#define LAPIC_REG_LVT_TIMER 0x320
#define LAPIC_REG_TIMER_INIT 0x380   /* initial count    */
#define LAPIC_REG_TIMER_CUR  0x390   /* current count    */
#define LAPIC_REG_TIMER_DIV  0x3E0   /* divide config    */
#define LAPIC_TIMER_PERIODIC (1u << 17)
#define LAPIC_LVT_MASKED     (1u << 16)
#define LAPIC_TIMER_DIV_16   0x3      /* divide config encoding for /16 */

/* Scheduler-tick vector: the LAPIC timer reuses the PIT IRQ0 vector (0x20) so
 * isr_timer + the IDT gate are unchanged — only the EOI source moves. Keyboard
 * (ISA IRQ1) keeps vector 0x21 routed through the IOAPIC instead of the PIC. */
#define VEC_TIMER    0x20
#define VEC_KEYBOARD 0x21

/* Spurious vector to install when we enable the LAPIC. 0xFF is conventional. */
#define LAPIC_SPURIOUS_VECTOR 0xFF

static volatile uint32_t* g_lapic;   /* mapped LAPIC base, NULL until enabled */
static volatile uint32_t* g_ioapic;  /* mapped IOAPIC base, NULL until routed  */
static int g_apic_mode;              /* 1 once the APIC owns hardware IRQs      */

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}

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

/* ============================ M28-2: APIC switchover ====================== */

int apic_mode_active(void) { return g_apic_mode; }

/* EOI a legacy IRQ to whichever controller currently owns it. */
void irq_eoi(void) {
    if (g_apic_mode) g_lapic[LAPIC_REG_EOI / 4] = 0;   /* LAPIC EOI */
    else             outb(0x20, 0x20);                 /* 8259 master EOI */
}

/* IOAPIC indirect register access: select via IOREGSEL (base+0x00), read/write
 * the 32-bit window IOWIN (base+0x10). */
static void ioapic_write(uint32_t reg, uint32_t val) {
    g_ioapic[0x00 / 4] = reg;
    g_ioapic[0x10 / 4] = val;
}

/* Route one ISA IRQ through the IOAPIC to `vector`, delivered to `dest_apic_id`.
 * Honours the MADT interrupt-source-override polarity/trigger flags. */
static void ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t dest_apic_id) {
    const acpi_topology_t* t = acpi_get();
    uint16_t flags = 0;
    uint32_t gsi = acpi_irq_to_gsi(irq, &flags);

    /* Pick the IOAPIC whose GSI window contains this GSI (almost always #0). */
    uint64_t base = t->ioapics[0].base;
    uint32_t gsi_base = t->ioapics[0].gsi_base;
    for (uint32_t i = 0; i < t->ioapic_count; i++)
        if (gsi >= t->ioapics[i].gsi_base) { base = t->ioapics[i].base; gsi_base = t->ioapics[i].gsi_base; }
    uint32_t idx = gsi - gsi_base;

    vmm_extend_physmap(base + 0x1000);
    g_ioapic = (volatile uint32_t*)phys_to_virt(base);

    /* MADT flags: bits[1:0] polarity (11=active-low), bits[3:2] trigger (11=level).
     * 0/conforms => ISA default: active-high, edge. */
    uint32_t low = vector;                 /* fixed delivery, physical dest, unmasked */
    if ((flags & 0x3) == 0x3)  low |= (1u << 13);   /* active low  */
    if ((flags & 0xC) == 0xC)  low |= (1u << 15);   /* level trig  */
    uint32_t high = (uint32_t)dest_apic_id << 24;

    /* Redirection entry: low = 0x10 + 2*idx, high = 0x11 + 2*idx. Program high
     * (destination) before unmasking via the low word. */
    ioapic_write(0x11 + 2 * idx, high);
    ioapic_write(0x10 + 2 * idx, low);

    debugcon_writestring("[APIC] IOAPIC route irq="); debugcon_print_hex(irq);
    debugcon_writestring(" gsi="); debugcon_print_hex(gsi);
    debugcon_writestring(" vec="); debugcon_print_hex(vector);
    debugcon_writestring(" flags="); debugcon_print_hex(flags);
    debugcon_writestring("\n");
}

/* Calibrate the LAPIC timer against PIT channel 2 (which is independent of the
 * 8259 and keeps counting even with the PIC masked), then program it periodic at
 * `hz` on VEC_TIMER. Returns the per-second LAPIC tick count, or 0 on failure. */
static uint64_t lapic_timer_start(uint32_t hz) {
    /* Accept all interrupt priorities. */
    g_lapic[LAPIC_REG_TPR / 4] = 0;

    /* PIT channel 2: gate on (bit0), speaker off (bit1). OUT2 status = port 0x61 bit5. */
    uint8_t p61 = inb(0x61);
    outb(0x61, (uint8_t)((p61 & ~0x02) | 0x01));
    /* Channel 2, lobyte/hibyte, mode 0 (interrupt on terminal count), binary. */
    outb(0x43, 0xB0);
    /* ~10 ms: 1193182 / 100 = 11931.8 -> 11932 ticks. */
    uint16_t cnt = 11932;
    outb(0x42, (uint8_t)(cnt & 0xFF));
    outb(0x42, (uint8_t)(cnt >> 8));

    /* LAPIC one-shot, /16, masked, counting from the max so we can measure decay. */
    g_lapic[LAPIC_REG_TIMER_DIV / 4]  = LAPIC_TIMER_DIV_16;
    g_lapic[LAPIC_REG_LVT_TIMER / 4]  = LAPIC_LVT_MASKED | VEC_TIMER;
    g_lapic[LAPIC_REG_TIMER_INIT / 4] = 0xFFFFFFFFu;

    /* Wait for PIT ch2 to reach terminal count (OUT goes high). */
    while (!(inb(0x61) & 0x20)) { }
    uint32_t cur = g_lapic[LAPIC_REG_TIMER_CUR / 4];
    uint32_t elapsed = 0xFFFFFFFFu - cur;        /* LAPIC ticks in ~10 ms */
    if (elapsed == 0) return 0;

    uint64_t ticks_per_sec = (uint64_t)elapsed * 100ull;   /* 10 ms -> 1 s */
    uint32_t init = (uint32_t)(ticks_per_sec / hz);
    if (init == 0) init = 1;

    /* Program periodic at `hz`, unmasked, on VEC_TIMER. */
    g_lapic[LAPIC_REG_TIMER_DIV / 4]  = LAPIC_TIMER_DIV_16;
    g_lapic[LAPIC_REG_LVT_TIMER / 4]  = LAPIC_TIMER_PERIODIC | VEC_TIMER;
    g_lapic[LAPIC_REG_TIMER_INIT / 4] = init;

    debugcon_writestring("[APIC] LAPIC timer hz="); debugcon_print_hex(hz);
    debugcon_writestring(" ticks/s="); debugcon_print_hex(ticks_per_sec);
    debugcon_writestring(" init="); debugcon_print_hex(init);
    debugcon_writestring("\n");
    return ticks_per_sec;
}

int apic_switchover(uint32_t hz) {
    const acpi_topology_t* t = acpi_get();
    if (!t->found || t->ioapic_count == 0) {
        debugcon_writestring("[APIC] no ACPI/IOAPIC — staying on PIC/PIT\n");
        return -1;
    }
    if (lapic_enable() != 0) {
        debugcon_writestring("[APIC] LAPIC enable failed — staying on PIC/PIT\n");
        return -1;
    }

    /* Critical section: stop the PIC, route through the IOAPIC, start the LAPIC
     * timer. Interrupts are disabled so no half-switched IRQ can be delivered. */
    __asm__ volatile ("cli");

    /* If the firmware came up in PIC mode (MADT PCAT_COMPAT), flip the IMCR so the
     * IOAPIC, not the 8259, drives INTR. Harmless where no IMCR exists (QEMU). */
    outb(0x22, 0x70);
    outb(0x23, 0x01);

    /* Mask every 8259 line — the IOAPIC owns device IRQs now. */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    /* Route the keyboard (ISA IRQ1) to VEC_KEYBOARD on the boot CPU. */
    ioapic_route_irq(1, VEC_KEYBOARD, (uint8_t)t->bsp_lapic_id);

    uint64_t tps = lapic_timer_start(hz);
    if (tps == 0) {
        /* Calibration failed: fall back to the PIC/PIT we just masked. */
        outb(0x21, 0xFC);   /* re-enable IRQ0+IRQ1 on the master */
        outb(0xA1, 0xFF);
        __asm__ volatile ("sti");
        debugcon_writestring("[APIC] calibration failed — reverted to PIC/PIT\n");
        return -1;
    }

    g_apic_mode = 1;
    __asm__ volatile ("sti");
    debugcon_writestring("[APIC] mode active (PIC masked, LAPIC timer + IOAPIC)\n");
    return 0;
}
