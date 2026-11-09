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
#include "vmm.h"        /* phys_to_virt / vmm_map_mmio */
#include "debugcon.h"
#include "spinlock.h"
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
#define VEC_MOUSE    0x2C   /* [M40] ISA IRQ12 (PS/2 AUX), same vector as the PIC mapping */

/* Spurious vector to install when we enable the LAPIC. 0xFF is conventional. */
#define LAPIC_SPURIOUS_VECTOR 0xFF

/* Interrupt Command Register (low/high) for sending IPIs (M29 SMP). */
#define LAPIC_REG_ICR_LO 0x300
#define LAPIC_REG_ICR_HI 0x310
#define ICR_DELIV_INIT   (5u << 8)
#define ICR_DELIV_STARTUP (6u << 8)
#define ICR_LEVEL_ASSERT (1u << 14)
#define ICR_DELIVERY_PENDING (1u << 12)

static volatile uint32_t* g_lapic;   /* mapped LAPIC base, NULL until enabled */
/* [M56] Routed IOAPIC lines, so the interrupt-remapping switchover can
 * rewrite every live redirection entry. g_ioapic_lock (LK_S61_IOAPIC) guards
 * the table and each IOREGSEL/IOWIN pair; the remap callback (which programs
 * the IOMMU's remapping entry, S63/S68) runs outside it. */
#define IOAPIC_MAX_LINES 8
typedef struct {
    uint8_t  used, irq, vector, ioapic, level, low;
    uint32_t idx;                    /* pin on that IOAPIC                     */
    uint32_t dest;                   /* destination APIC ID                    */
    uint8_t  remapped;               /* current RTE is in remappable format    */
} ioapic_line_t;
static ioapic_line_t g_lines[IOAPIC_MAX_LINES];
static volatile uint32_t* g_ioapic_regs[ACPI_MAX_IOAPICS];
static spinlock_t g_ioapic_lock = SPINLOCK_INIT_CLASS(LK_S61_IOAPIC, "ioapic");
static ioapic_remap_fn g_ioapic_remap;   /* NULL: compatibility format         */
static int g_apic_mode;              /* 1 once the APIC owns hardware IRQs      */
static uint32_t g_timer_init;        /* periodic LAPIC-timer count (BSP-calibrated) */
static int g_x2apic;                 /* [M48] 1 = LAPICs run in x2APIC (MSR) mode   */

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

/* [M48] Local APIC register access. In xAPIC mode the registers are MMIO at
 * the mapped base; in x2APIC mode each 16-byte-spaced register is the MSR
 * 0x800 + (offset >> 4) and the MMIO window is dead. Every LAPIC access goes
 * through these two helpers so the mode switch is transparent. */
#define IA32_APIC_BASE_EXTD (1u << 10)
#define X2APIC_MSR(reg)     (0x800u + ((reg) >> 4))
static inline uint32_t lapic_rd(uint32_t reg) {
    if (g_x2apic) return (uint32_t)rdmsr(X2APIC_MSR(reg));
    return g_lapic[reg / 4];
}
static inline void lapic_wr(uint32_t reg, uint32_t v) {
    if (g_x2apic) wrmsr(X2APIC_MSR(reg), v);
    else          g_lapic[reg / 4] = v;
}

int lapic_x2apic_mode(void) { return g_x2apic; }

/* Decide (once, on the BSP) whether to run the LAPICs in x2APIC mode: when the
 * CPU supports it (CPUID.01H:ECX[21]) — required for APIC IDs > 254 — or when
 * the firmware already switched it on (leaving x2APIC needs a full disable, so
 * honour it). -DSECOS_NO_X2APIC forces xAPIC where the firmware allows. */
static void lapic_choose_mode(void) {
    uint32_t a, b, c, d;
    cpuid_get(1, &a, &b, &c, &d);
    int supported = (c >> 21) & 1;
    int fw_on = (rdmsr(IA32_APIC_BASE_MSR) & IA32_APIC_BASE_EXTD) != 0;
#ifdef SECOS_NO_X2APIC
    g_x2apic = fw_on;
#else
    g_x2apic = supported || fw_on;
#endif
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
    lapic_choose_mode();
    base |= APIC_BASE_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, base);              /* xAPIC enable first ...   */
    if (g_x2apic) wrmsr(IA32_APIC_BASE_MSR, base | IA32_APIC_BASE_EXTD); /* ... then x2APIC */

    uint64_t phys = base & 0xFFFFF000ull;
    /* [M49] Explicit uncached MMIO mapping in the physmap (idempotent: an AP
     * re-running this finds the page already UC and changes nothing). */
    uint64_t lv = vmm_map_mmio(phys, 0x1000, VMM_CACHE_UC);
    g_lapic = (volatile uint32_t*)(lv ? lv : phys_to_virt(phys)); /* unused in x2APIC mode */

    /* Software-enable the LAPIC via the Spurious Interrupt Vector Register. */
    lapic_wr(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);

    debugcon_writestring("[MSIX] LAPIC enabled base=0x"); debugcon_print_hex(phys);
    debugcon_writestring(" id=0x"); debugcon_print_hex(lapic_get_id());
    debugcon_writestring(g_x2apic ? " mode=x2apic\n" : " mode=xapic\n");
    return 0;
}

void lapic_eoi(void) {
    if (g_lapic) lapic_wr(LAPIC_REG_EOI, 0);
}

/* ============================ M28-2: APIC switchover ====================== */

int apic_mode_active(void) { return g_apic_mode; }

/* EOI a legacy IRQ to whichever controller currently owns it. */
void irq_eoi(void) {
    if (g_apic_mode) lapic_wr(LAPIC_REG_EOI, 0);   /* LAPIC EOI */
    else             outb(0x20, 0x20);                 /* 8259 master EOI */
}

/* IOAPIC indirect register access: select via IOREGSEL (base+0x00), read/write
 * the 32-bit window IOWIN (base+0x10). g_ioapic_lock held. */
static void ioapic_write(volatile uint32_t* io, uint32_t reg, uint32_t val) {
    io[0x00 / 4] = reg;
    io[0x10 / 4] = val;
}

#define RTE_MASKED       (1u << 16)
#define RTE_ACTIVE_LOW   (1u << 13)
#define RTE_LEVEL        (1u << 15)
#define RTE_HANDLE15     (1u << 11)      /* remappable: handle[15]            */
#define RTE_HI_REMAP     (1u << 16)      /* bit 48: interrupt format = remap  */

/* Build a line's redirection entry. Compatibility format: fixed delivery,
 * physical destination (8-bit APIC ID in bits 63:56). [M56] Remappable
 * format (VT-d 5.1.5.1): bit 48 set, handle[14:0] in bits 63:49, handle[15]
 * in bit 11, delivery mode 0, vector = the remapping entry's vector (the
 * EOI broadcast matches on it); trigger mode and polarity as before. */
static void rte_build(const ioapic_line_t* ln, int remap, uint16_t handle, int masked,
                      uint32_t* low, uint32_t* high) {
    uint32_t lo = ln->vector;
    if (ln->low)   lo |= RTE_ACTIVE_LOW;
    if (ln->level) lo |= RTE_LEVEL;
    if (masked)    lo |= RTE_MASKED;
    uint32_t hi;
    if (remap) {
        if (handle & 0x8000) lo |= RTE_HANDLE15;
        hi = RTE_HI_REMAP | ((uint32_t)(handle & 0x7FFF) << 17);
    } else {
        hi = (ln->dest & 0xFF) << 24;
    }
    *low = lo; *high = hi;
}

/* Write one line's entry: destination word first, then the low word (which
 * carries the mask bit). g_ioapic_lock held. */
static void rte_write(const ioapic_line_t* ln, uint32_t low, uint32_t high) {
    volatile uint32_t* io = g_ioapic_regs[ln->ioapic];
    if (!io) return;
    ioapic_write(io, 0x10 + 2 * ln->idx, low | RTE_MASKED);   /* never half-written live */
    ioapic_write(io, 0x11 + 2 * ln->idx, high);
    ioapic_write(io, 0x10 + 2 * ln->idx, low);
}

/* The remappable handle for a line (the callback programs the remapping
 * entry; no IOAPIC lock held). 0 or -1. */
static int line_remap(const ioapic_line_t* ln, ioapic_remap_fn fn, uint16_t* handle) {
    const acpi_topology_t* t = acpi_get();
    return fn(t->ioapics[ln->ioapic].id, ln->vector, ln->dest, ln->level, handle);
}

/* Route one ISA IRQ through the IOAPIC to `vector`, delivered to `dest_apic_id`.
 * Honours the MADT interrupt-source-override polarity/trigger flags. */
static void ioapic_route_irq(uint8_t irq, uint8_t vector, uint32_t dest_apic_id) {
    const acpi_topology_t* t = acpi_get();
    uint16_t flags = 0;
    uint32_t gsi = acpi_irq_to_gsi(irq, &flags);

    /* Pick the IOAPIC whose GSI window contains this GSI (almost always #0). */
    uint32_t which = 0;
    for (uint32_t i = 0; i < t->ioapic_count && i < ACPI_MAX_IOAPICS; i++)
        if (gsi >= t->ioapics[i].gsi_base) which = i;
    uint32_t idx = gsi - t->ioapics[which].gsi_base;

    uint64_t iv = vmm_map_mmio(t->ioapics[which].base, 0x1000, VMM_CACHE_UC);   /* [M49] uncached MMIO */
    if (!iv) return;

    ioapic_line_t ln = { 1, irq, vector, (uint8_t)which, 0, 0, idx, dest_apic_id, 0 };
    /* MADT flags: bits[1:0] polarity (11=active-low), bits[3:2] trigger (11=level).
     * 0/conforms => ISA default: active-high, edge. */
    ln.low = (flags & 0x3) == 0x3;
    ln.level = (flags & 0xC) == 0xC;

    /* [M56] Remappable format once interrupt remapping owns the IOAPIC: the
     * remapping entry first (outside the IOAPIC lock); a line that cannot be
     * remapped stays masked (a compatibility-format request would be
     * blocked anyway). */
    ioapic_remap_fn fn = __atomic_load_n(&g_ioapic_remap, __ATOMIC_ACQUIRE);
    uint16_t handle = 0;
    int masked = 0;
    if (fn) {
        if (line_remap(&ln, fn, &handle) == 0) ln.remapped = 1;
        else { masked = 1; debugcon_writestring("[APIC] IOAPIC line left masked: no remapping entry\n"); }
    }
    uint32_t low, high;
    rte_build(&ln, ln.remapped, handle, masked, &low, &high);

    uint64_t f = spin_lock_irqsave(&g_ioapic_lock);
    g_ioapic_regs[which] = (volatile uint32_t*)iv;
    int slot = -1;
    for (int i = 0; i < IOAPIC_MAX_LINES; i++) {
        if (g_lines[i].used && g_lines[i].ioapic == which && g_lines[i].idx == idx) { slot = i; break; }
        if (!g_lines[i].used && slot < 0) slot = i;
    }
    if (slot >= 0) g_lines[slot] = ln;
    rte_write(&ln, low, high);
    spin_unlock_irqrestore(&g_ioapic_lock, f);

    debugcon_writestring("[APIC] IOAPIC route irq="); debugcon_print_hex(irq);
    debugcon_writestring(" gsi="); debugcon_print_hex(gsi);
    debugcon_writestring(" vec="); debugcon_print_hex(vector);
    debugcon_writestring(" flags="); debugcon_print_hex(flags);
    debugcon_writestring(ln.remapped ? " remapped\n" : "\n");
}

/* [M56] Interrupt-remapping switchover for the IOAPIC (see lapic.h). */
int ioapic_remap_switch(ioapic_remap_fn fn, int (*enable)(void* ctx), void* ctx) {
    ioapic_line_t snap[IOAPIC_MAX_LINES];
    uint16_t handle[IOAPIC_MAX_LINES];
    int ok[IOAPIC_MAX_LINES];
    uint32_t low, high;

    /* 1. Mask every live line (compatibility format, still deliverable
     *    until IR is on). */
    uint64_t f = spin_lock_irqsave(&g_ioapic_lock);
    for (int i = 0; i < IOAPIC_MAX_LINES; i++) {
        snap[i] = g_lines[i];
        if (!snap[i].used) continue;
        rte_build(&snap[i], 0, 0, 1, &low, &high);
        rte_write(&snap[i], low, high);
    }
    spin_unlock_irqrestore(&g_ioapic_lock, f);

    /* 2. Remapping entries for each line (IR still off: nothing uses them). */
    int bad = 0;
    for (int i = 0; i < IOAPIC_MAX_LINES; i++) {
        ok[i] = 0; handle[i] = 0;
        if (!snap[i].used) continue;
        ok[i] = line_remap(&snap[i], fn, &handle[i]) == 0;
        if (!ok[i]) bad++;
    }

    /* 3. Interrupt remapping on (every unit), unless a line has no entry. */
    int rc = bad ? -1 : enable(ctx);

    /* 4. Rewrite + unmask: remappable format on success, else the old
     *    compatibility entries come back as they were. */
    f = spin_lock_irqsave(&g_ioapic_lock);
    for (int i = 0; i < IOAPIC_MAX_LINES; i++) {
        if (!snap[i].used) continue;
        rte_build(&snap[i], rc == 0, handle[i], 0, &low, &high);
        rte_write(&snap[i], low, high);
        if (g_lines[i].used) g_lines[i].remapped = (rc == 0);
    }
    if (rc == 0) __atomic_store_n(&g_ioapic_remap, fn, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_ioapic_lock, f);

    /* 5. An edge that arrived while a line was masked may be lost (a real
     *    IOAPIC drops it; the 8042 then keeps its output buffer full and never
     *    raises a new edge): if the controller holds a byte, run its handler
     *    once by a self-IPI on that line's vector. Handlers read the status
     *    port first, so a spurious call is harmless. */
    uint8_t st = inb(0x64);
    if (st & 1) {
        uint8_t want = (st & 0x20) ? 12 : 1;
        for (int i = 0; i < IOAPIC_MAX_LINES; i++)
            if (snap[i].used && snap[i].irq == want) { lapic_ipi(lapic_get_id(), snap[i].vector); break; }
    }
    return rc;
}

int ioapic_line_count(void) {
    int n = 0;
    for (int i = 0; i < IOAPIC_MAX_LINES; i++) n += g_lines[i].used;
    return n;
}

/* Calibrate the LAPIC timer against PIT channel 2 (which is independent of the
 * 8259 and keeps counting even with the PIC masked), then program it periodic at
 * `hz` on VEC_TIMER. Returns the per-second LAPIC tick count, or 0 on failure. */
static uint64_t lapic_timer_start(uint32_t hz) {
    /* Accept all interrupt priorities. */
    lapic_wr(LAPIC_REG_TPR, 0);

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
    lapic_wr(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_wr(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED | VEC_TIMER);
    lapic_wr(LAPIC_REG_TIMER_INIT, 0xFFFFFFFFu);

    /* Wait for PIT ch2 to reach terminal count (OUT goes high). */
    while (!(inb(0x61) & 0x20)) { }
    uint32_t cur = lapic_rd(LAPIC_REG_TIMER_CUR);
    uint32_t elapsed = 0xFFFFFFFFu - cur;        /* LAPIC ticks in ~10 ms */
    if (elapsed == 0) return 0;

    uint64_t ticks_per_sec = (uint64_t)elapsed * 100ull;   /* 10 ms -> 1 s */
    uint32_t init = (uint32_t)(ticks_per_sec / hz);
    if (init == 0) init = 1;

    /* Program periodic at `hz`, unmasked, on VEC_TIMER. Remember the count so APs
     * can start their own LAPIC timer at the same rate without recalibrating. */
    g_timer_init = init;
    lapic_wr(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_wr(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_PERIODIC | VEC_TIMER);
    lapic_wr(LAPIC_REG_TIMER_INIT, init);

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
    ioapic_route_irq(1, VEC_KEYBOARD, t->bsp_lapic_id);

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

/* [M40] PS/2 AUX (mouse) on ISA IRQ12, vector 0x2C: routed through the IOAPIC
 * like the keyboard in APIC mode, else unmasked on the slave 8259 (+ the
 * cascade line on the master). */
void irq_enable_aux(void) {
    if (g_apic_mode) {
        ioapic_route_irq(12, VEC_MOUSE, acpi_get()->bsp_lapic_id);
    } else {
        outb(0xA1, (uint8_t)(inb(0xA1) & ~(1u << 4)));   /* IRQ12 = slave line 4 */
        outb(0x21, (uint8_t)(inb(0x21) & ~(1u << 2)));   /* cascade (IRQ2) */
    }
}
/* End-of-interrupt for IRQ12: LAPIC, or slave + master 8259. */
void irq_eoi_aux(void) {
    if (g_apic_mode) lapic_wr(LAPIC_REG_EOI, 0);
    else { outb(0xA0, 0x20); outb(0x20, 0x20); }
}

/* ============================ M29: SMP IPIs / per-CPU ===================== */

uint32_t lapic_get_id(void) {
    if (!g_lapic) return 0;
    if (g_x2apic) return (uint32_t)rdmsr(X2APIC_MSR(LAPIC_REG_ID));   /* full 32-bit ID */
    return g_lapic[LAPIC_REG_ID / 4] >> 24;
}

/* Software-enable THIS CPU's Local APIC (per-CPU MSR + SVR). Unlike lapic_enable()
 * — which also maps the shared MMIO and is idempotent for the BSP — this is run by
 * each AP, which must set its own APIC-base-enable bit and spurious vector. The
 * MMIO is already mapped (BSP did it); the LAPIC registers are per-CPU. */
void lapic_enable_this_cpu(void) {
    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    wrmsr(IA32_APIC_BASE_MSR, base | APIC_BASE_ENABLE);
    if (g_x2apic && !(base & IA32_APIC_BASE_EXTD))       /* [M48] same mode as the BSP */
        wrmsr(IA32_APIC_BASE_MSR, base | APIC_BASE_ENABLE | IA32_APIC_BASE_EXTD);
    if (g_lapic) lapic_wr(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
}

static void icr_wait_idle(void) {
    if (g_x2apic) return;                 /* x2APIC ICR writes complete synchronously */
    while (g_lapic[LAPIC_REG_ICR_LO / 4] & ICR_DELIVERY_PENDING) __asm__ volatile ("pause");
}

/* Fire one IPI. Interrupts are held off around the ICR access: in xAPIC mode
 * it is a two-register sequence (destination, then command) that an IPI sent
 * from an interrupt handler on this same CPU must not split. In x2APIC mode it
 * is one 64-bit MSR write with a full 32-bit destination. */
static void icr_send(uint32_t dest_apic_id, uint32_t low) {
    uint64_t fl;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    if (g_x2apic) {
        __asm__ volatile ("mfence" ::: "memory");   /* x2APIC ICR writes are not serializing */
        wrmsr(X2APIC_MSR(LAPIC_REG_ICR_LO), ((uint64_t)dest_apic_id << 32) | low);
    } else {
        icr_wait_idle();
        g_lapic[LAPIC_REG_ICR_HI / 4] = dest_apic_id << 24;   /* destination field */
        g_lapic[LAPIC_REG_ICR_LO / 4] = low;                  /* writing LOW fires it */
        icr_wait_idle();
    }
    __asm__ volatile ("push %0; popfq" :: "r"(fl) : "memory", "cc");
}

void lapic_send_init(uint32_t dest_apic_id) {
    if (!g_lapic) return;
    icr_send(dest_apic_id, ICR_DELIV_INIT | ICR_LEVEL_ASSERT);
}

void lapic_send_sipi(uint32_t dest_apic_id, uint8_t vector) {
    if (!g_lapic) return;
    icr_send(dest_apic_id, ICR_DELIV_STARTUP | ICR_LEVEL_ASSERT | vector);
}

void lapic_ipi(uint32_t dest_apic_id, uint8_t vector) {
    if (!g_lapic) return;
    icr_send(dest_apic_id, ICR_LEVEL_ASSERT | vector);
}

void lapic_timer_start_this_cpu(uint32_t hz) {
    (void)hz;
    if (!g_lapic || !g_timer_init) return;
    lapic_wr(LAPIC_REG_TPR, 0);
    lapic_wr(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_wr(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_PERIODIC | VEC_TIMER);
    lapic_wr(LAPIC_REG_TIMER_INIT, g_timer_init);
}
