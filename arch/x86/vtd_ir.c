/*
 * vtd_ir.c — Intel VT-d interrupt remapping (Phase 5, M56; plan
 * docs/PHASE5_PLAN.md §3 "M56" and D4).
 *
 * Without interrupt remapping a device that can DMA can also write any
 * {0xFEEx_xxxx, data} pair, which is an MSI: it can raise any vector on any
 * CPU. With it, every interrupt request goes through an interrupt-remapping
 * table entry (IRTE) that names its vector, its destination and the source
 * allowed to use it, and the unit blocks (and records) anything else.
 *
 * Runs only with `iommu=on` (kernel/iommu.c calls vtd_ir_init() once DMA
 * translation is on) and not with the boot word `iommu.ir=off`. Requires:
 *   - every unit kernel/iommu.c uses has ECAP.IR and queued invalidation
 *     (the interrupt-entry cache is only invalidated through the queue);
 *   - in x2APIC mode, ECAP.EIM on every unit and no DMAR X2APIC_OPT_OUT for
 *     32-bit destinations; otherwise 8-bit destinations, and every CPU's
 *     APIC ID must fit;
 *   - every IOAPIC in the MADT is named by a DMAR device scope (its source
 *     id) on a unit that remaps;
 *   - no MSI / MSI-X source is live yet (the switchover runs before any
 *     driver probes; a live compatibility-format source would be blocked).
 * Anything missing: logged, interrupt remapping stays off, the M54
 * behaviour is unchanged.
 *
 * Table: one shared 4 KiB table of 256 entries, programmed into every unit
 * (IRTA, SIRTP, global IEC). **The entry index is the vector number**: the
 * M53c allocator gives every vector to exactly one source system-wide, so
 * (vector -> entry) never collides, re-composing a message is idempotent,
 * and irq_vec_free() can clear exactly the right entry. The IOAPIC lines use
 * their fixed vectors (0x21 keyboard, 0x2C mouse). The IOMMU's own fault-
 * event MSI is never remapped (it does not go through the table).
 *
 * Entry contents: present, physical destination, fixed delivery, edge (level
 * for a level-triggered IOAPIC line), the vector, the destination APIC ID
 * (bits 47:40 in xAPIC format, 63:32 with EIM), and source-id validation:
 * SVT=01b SQ=00b with the requester id (an exact match), or SVT=10b with the
 * bus range of a PCIe-to-PCI bridge for a function behind one, or the DMAR
 * scope id of an IOAPIC. Messages are composed in remappable format with
 * SHV=1 and subhandle 0 (data 0), so multi-message MSI lands on vec + i.
 *
 * Updates: a new owner (different source id) first clears the present bit
 * and invalidates, then writes the high half, then the low half with P; a
 * retarget (same owner) is one 64-bit store of the low half. Each change is
 * followed by an index-selective IEC invalidation + wait on every unit.
 *
 * Compatibility-format requests are blocked: GSTS.CFIS is cleared and never
 * set. (QEMU 6.2 and 9.2 do not implement this check and pass a compatibility
 * request through even with remapping on: see docs/devlog/M56-vtd-intremap.md.)
 *
 * Locks: g_ir_lock (LK_S63_IOMMU_IR, IRQ-safe) around the table and the
 * invalidations that follow (the unit lock S68 nests inside). irq_dispatch
 * never takes it.
 *
 * SPDX-License-Identifier: MIT
 */
#include "vtd.h"
#include "irq.h"
#include "percpu.h"
#include "lapic.h"
#include "acpi.h"
#include "pci_msi.h"
#include "vmm.h"
#include "debugcon.h"
#include <stddef.h>

#define IR_ENTRIES      256
#define IR_SIZE_FIELD   7                 /* IRTA.S: 2^(S+1) = 256 entries */
#define IR_EIME         (1ull << 11)

#define IRTE_P          (1ull << 0)
#define IRTE_TM         (1ull << 4)       /* level */
#define IRTE_VEC(v)     ((uint64_t)(v) << 16)
#define IRTE_SVT(t)     ((uint64_t)(t) << 18)

#define OWN_FREE        0
#define OWN_MSI         1
#define OWN_IOAPIC      2

static spinlock_t        g_ir_lock = SPINLOCK_INIT_CLASS(LK_S63_IOMMU_IR, "iommu_ir");
static uint64_t          g_irt_phys;
static volatile uint64_t* g_irt;
static volatile int      g_on;
static int               g_eim;
static const char*       g_state = "off (iommu=on not given)";
static uint8_t           g_unit_ir[DMAR_MAX_DRHD];
static int               g_ioapic_lines;

/* Per entry (g_ir_lock). */
static uint8_t           g_own[IR_ENTRIES];
static uint16_t          g_sid[IR_ENTRIES];
static uint8_t           g_svt[IR_ENTRIES];
static uint8_t           g_lvl[IR_ENTRIES];
static uint32_t          g_dst[IR_ENTRIES];

/* Counters. */
static volatile uint64_t g_n_set, g_n_clear, g_n_retarget, g_n_iec, g_n_iec_err;
static volatile uint64_t g_n_compat, g_n_refused;

static void logline(iommu_line_t* l) { debugcon_writestring(iommu_ln_end(l)); }
static void ln_begin(iommu_line_t* l) { l->n = 0; iommu_ln_s(l, "[IOMMU] IR: "); }

/* -------------------------------------------------------------- table */
static uint64_t irte_lo(uint8_t vec, uint32_t apic, int level) {
    uint64_t dst = g_eim ? (uint64_t)apic : ((uint64_t)(apic & 0xFF) << 8);
    return IRTE_P | (level ? IRTE_TM : 0) | IRTE_VEC(vec) | (dst << 32);
}
static uint64_t irte_hi(uint16_t sid, uint8_t svt) {
    return (uint64_t)sid | IRTE_SVT(svt);           /* SQ = 00b: all 16 bits */
}

/* Invalidate one entry on every remapping unit (g_ir_lock held). */
static int iec_all(uint32_t index, uint32_t count) {
    int rc = 0;
    for (int i = 0; i < vtd_unit_count(); i++) {
        if (!g_unit_ir[i]) continue;
        g_n_iec++;
        if (vtd_unit_iec(i, index, count)) { g_n_iec_err++; rc = -1; }
    }
    return rc;
}

static void irte_store(uint32_t idx, int half, uint64_t v) {
    volatile uint64_t* e = &g_irt[idx * 2 + (uint32_t)half];
    __atomic_store_n(e, v, __ATOMIC_RELEASE);
    vtd_flush_cache((const void*)e, 8);
}

/* Install (lo, hi) at `idx` (g_ir_lock held). */
static int irte_set_locked(uint32_t idx, uint64_t lo, uint64_t hi) {
    uint64_t olo = g_irt[idx * 2], ohi = g_irt[idx * 2 + 1];
    if (olo == lo && ohi == hi) return 0;
    if ((olo & IRTE_P) && ohi != hi) {
        /* New owner: never let the old source use the new destination or the
         * new source the old one; blocked in between. */
        irte_store(idx, 0, 0);
        if (iec_all(idx, 1)) return -1;
    }
    if (ohi != hi) irte_store(idx, 1, hi);
    irte_store(idx, 0, lo);
    return iec_all(idx, 1);
}

static int irte_clear_locked(uint32_t idx) {
    if (!(g_irt[idx * 2] & IRTE_P) && !g_irt[idx * 2 + 1]) return 0;
    irte_store(idx, 0, 0);
    int rc = iec_all(idx, 1);
    irte_store(idx, 1, 0);
    return rc;
}

/* Destination APIC ID of a CPU index, or -1 if this format cannot reach it. */
static int64_t cpu_apic(uint32_t cpu) {
    int c = irq_pick_cpu(cpu);
    if (c < 0) return -1;
    cpu_t* cp = cpu_by_index((uint32_t)c);
    if (!cp) return -1;
    if (!g_eim && cp->lapic_id > 0xFF) return -1;
    return (int64_t)cp->lapic_id;
}

/* -------------------------------------------------------------- MSI */
int vtd_ir_enabled(void) { return __atomic_load_n(&g_on, __ATOMIC_ACQUIRE); }

int vtd_ir_covers(const pci_dev_t* dev) {
    if (!vtd_ir_enabled() || !dev) return 0;
    int u = vtd_unit_for_dev(dev);
    return u >= 0 && g_unit_ir[u];
}

int vtd_ir_compose(const pci_dev_t* dev, int vec, uint32_t cpu, uint64_t* addr, uint32_t* data) {
    if (!vtd_ir_covers(dev)) {
        if (vtd_ir_enabled()) __atomic_add_fetch(&g_n_compat, 1, __ATOMIC_RELAXED);
        pci_device_t pd = { dev->bus, dev->slot, dev->func, dev->vendor_id, dev->device_id };
        return msi_compose_compat(&pd, (uint8_t)vec, cpu, addr, data);
    }
    /* Only allocator vectors: a message can never be pointed at an
     * exception, the syscall gate, an IPI or an IOAPIC line's entry. */
    if (vec < IRQ_VEC_FIRST || vec > IRQ_VEC_LAST) { g_n_refused++; return -1; }
    int64_t apic = cpu_apic(cpu);
    if (apic < 0) { g_n_refused++; return -1; }
    uint16_t sid; uint8_t svt;
    vtd_dev_requester(dev, &sid, &svt);
    uint64_t f = spin_lock_irqsave(&g_ir_lock);
    int rc = irte_set_locked((uint32_t)vec, irte_lo((uint8_t)vec, (uint32_t)apic, 0), irte_hi(sid, svt));
    if (rc == 0) {
        g_own[vec] = OWN_MSI; g_sid[vec] = sid; g_svt[vec] = svt; g_lvl[vec] = 0;
        g_dst[vec] = (uint32_t)apic;
        g_n_set++;
    }
    spin_unlock_irqrestore(&g_ir_lock, f);
    if (rc) return -1;
    /* Remappable format (VT-d 5.1.2.2): 0xFEE, handle[14:0] in 19:5, bit 4
     * interrupt format = 1, bit 3 SHV, bit 2 handle[15]. */
    uint32_t h = (uint32_t)vec;
    *addr = (uint64_t)MSI_ADDR_BASE | ((uint64_t)(h & 0x7FFF) << 5) | (1u << 4) | (1u << 3) |
            ((uint64_t)((h >> 15) & 1) << 2);
    *data = 0;                                    /* subhandle 0 */
    return 0;
}

void vtd_ir_free(int vec) {
    if (!vtd_ir_enabled() || vec < IRQ_VEC_FIRST || vec > IRQ_VEC_LAST) return;
    uint64_t f = spin_lock_irqsave(&g_ir_lock);
    if (g_own[vec] == OWN_MSI) {
        irte_clear_locked((uint32_t)vec);
        g_own[vec] = OWN_FREE; g_sid[vec] = 0; g_svt[vec] = 0; g_dst[vec] = 0;
        g_n_clear++;
    }
    spin_unlock_irqrestore(&g_ir_lock, f);
}

int vtd_ir_set_cpu(int vec, uint32_t cpu) {
    if (!vtd_ir_enabled() || vec < IRQ_VEC_FIRST || vec > IRQ_VEC_LAST) return -1;
    int64_t apic = cpu_apic(cpu);
    if (apic < 0) return -1;
    int rc = 0;
    uint64_t f = spin_lock_irqsave(&g_ir_lock);
    if (g_own[vec] == OWN_MSI) {
        /* Same owner: only the low half changes (one atomic store). */
        rc = irte_set_locked((uint32_t)vec, irte_lo((uint8_t)vec, (uint32_t)apic, 0),
                             irte_hi(g_sid[vec], g_svt[vec]));
        if (rc == 0) { g_dst[vec] = (uint32_t)apic; g_n_retarget++; }
    }
    spin_unlock_irqrestore(&g_ir_lock, f);
    return rc;
}

/* -------------------------------------------------------------- IOAPIC */
static int ioapic_entry(uint8_t ioapic_id, uint8_t vector, uint32_t dest, int level, uint16_t* handle) {
    uint16_t sid;
    if (vector < 0x20 || vector > 0x2F) return -1;      /* legacy line vectors only */
    if (acpi_dmar_ioapic_sid(ioapic_id, &sid) != 0) return -1;
    int u = vtd_unit_for_sid(0, sid);
    if (u < 0 || !g_unit_ir[u]) return -1;
    if (!g_eim && dest > 0xFF) return -1;
    uint64_t f = spin_lock_irqsave(&g_ir_lock);
    int rc = irte_set_locked(vector, irte_lo(vector, dest, level), irte_hi(sid, 1));
    if (rc == 0) {
        if (g_own[vector] != OWN_IOAPIC) g_ioapic_lines++;
        g_own[vector] = OWN_IOAPIC; g_sid[vector] = sid; g_svt[vector] = 1;
        g_lvl[vector] = (uint8_t)level; g_dst[vector] = dest;
        g_n_set++;
    }
    spin_unlock_irqrestore(&g_ir_lock, f);
    if (rc) return -1;
    *handle = vector;
    return 0;
}

/* -------------------------------------------------------------- init */
static void disable_all(void) {
    for (int i = 0; i < vtd_unit_count(); i++) {
        if (!vtd_unit_used(i)) continue;
        if (vtd_unit_gsts(i) & VTD_GSTS_IRES) vtd_unit_gcmd(i, VTD_GCMD_IRE, 0);
    }
}

static int enable_all(void* ctx) {
    (void)ctx;
    for (int i = 0; i < vtd_unit_count(); i++) {
        if (!g_unit_ir[i]) continue;
        if (vtd_unit_gcmd(i, VTD_GCMD_IRE, 1)) return -1;
    }
    /* Publish before the IOAPIC lines are rewritten: their entries exist. */
    __atomic_store_n(&g_on, 1, __ATOMIC_RELEASE);
    return 0;
}

static int refuse(const char* why) {
    iommu_line_t l; ln_begin(&l);
    iommu_ln_s(&l, "off: "); iommu_ln_s(&l, why);
    iommu_ln_s(&l, " (DMA remapping stays on, interrupts unchanged)");
    logline(&l);
    g_state = why;
    disable_all();                     /* a firmware-left IRE would block our compat MSIs */
    return -1;
}

int vtd_ir_init(void) {
    iommu_line_t l;
    if (iommu_cmdline_has("iommu.ir=off")) return refuse("boot word iommu.ir=off");
    int units = 0;
    int eim_all = 1;
    for (int i = 0; i < vtd_unit_count(); i++) {
        if (!vtd_unit_used(i)) continue;
        uint64_t e = vtd_unit_ecap(i);
        if (!(e & VTD_ECAP_IR)) return refuse("a DMA-remapping unit has no interrupt remapping (ECAP.IR)");
        if (!vtd_unit_has_qi(i)) return refuse("queued invalidation off (iommu.qi=off or not started): no IEC");
        if (!(e & VTD_ECAP_EIM)) eim_all = 0;
        units++;
    }
    if (!units) return refuse("no active unit");
    const acpi_dmar_t* dm = acpi_dmar_get();
    int x2 = lapic_x2apic_mode();
    int optout = (dm->flags & DMAR_F_X2APIC_OPT_OUT) != 0;
    g_eim = x2 && eim_all && !optout;
    if (x2 && !g_eim) {
        for (uint32_t c = 0; c < smp_cpu_count(); c++) {
            cpu_t* cp = cpu_by_index(c);
            if (cp && cp->lapic_id > 0xFF)
                return refuse(optout ? "x2APIC IDs > 255 but the DMAR opts out of x2APIC (no EIM)"
                                     : "x2APIC IDs > 255 but a unit has no EIM");
        }
    }
    const acpi_topology_t* t = acpi_get();
    for (uint32_t i = 0; i < t->ioapic_count && i < ACPI_MAX_IOAPICS; i++) {
        uint16_t sid;
        if (acpi_dmar_ioapic_sid(t->ioapics[i].id, &sid) != 0)
            return refuse("an IOAPIC is not named by any DMAR device scope");
        int u = vtd_unit_for_sid(0, sid);
        if (u < 0) return refuse("an IOAPIC's source id is on no active unit");
    }
    if (pci_msi_any_enabled()) return refuse("an MSI/MSI-X source is already live");

    g_irt_phys = vtd_table_alloc();
    if (!g_irt_phys) return refuse("no memory for the remapping table");
    g_irt = (volatile uint64_t*)phys_to_virt(g_irt_phys);

    for (int i = 0; i < vtd_unit_count(); i++) {
        if (!vtd_unit_used(i)) continue;
        if (vtd_unit_gsts(i) & VTD_GSTS_IRES) {
            ln_begin(&l); iommu_ln_s(&l, "handoff: vt-d"); iommu_ln_dec(&l, vtd_unit_index(i));
            iommu_ln_s(&l, " had remapping on from the firmware: turned off before taking over");
            logline(&l);
            vtd_unit_gcmd(i, VTD_GCMD_IRE, 0);
        }
        if (vtd_unit_set_irta(i, g_irt_phys | (g_eim ? IR_EIME : 0) | IR_SIZE_FIELD))
            return refuse("the table pointer would not latch (SIRTP)");
        if (vtd_unit_gsts(i) & VTD_GSTS_CFIS) vtd_unit_gcmd(i, VTD_GCMD_CFI, 0);
        g_unit_ir[i] = 1;
    }

    /* One switchover: IOAPIC lines masked, their entries written, IR on,
     * lines rewritten in remappable format and unmasked. */
    if (ioapic_remap_switch(ioapic_entry, enable_all, NULL) != 0) {
        __atomic_store_n(&g_on, 0, __ATOMIC_RELEASE);
        for (int i = 0; i < vtd_unit_count(); i++) g_unit_ir[i] = 0;
        return refuse("IOAPIC switchover failed");
    }
    int cfis = 0;
    for (int i = 0; i < vtd_unit_count(); i++)
        if (g_unit_ir[i] && (vtd_unit_gsts(i) & VTD_GSTS_CFIS)) cfis = 1;
    g_state = "on";
    ln_begin(&l);
    iommu_ln_s(&l, "on: "); iommu_ln_dec(&l, (uint64_t)units);
    iommu_ln_s(&l, " unit(s), "); iommu_ln_dec(&l, IR_ENTRIES);
    iommu_ln_s(&l, " entries, ");
    iommu_ln_s(&l, x2 ? (g_eim ? "x2apic eim" : "x2apic 8-bit destinations (no eim)") : "xapic");
    iommu_ln_s(&l, optout ? " (dmar x2apic opt-out)" : "");
    iommu_ln_s(&l, ", ioapic lines "); iommu_ln_dec(&l, (uint64_t)g_ioapic_lines);
    iommu_ln_s(&l, cfis ? ", compatibility format ALLOWED (CFIS stuck)" : ", compatibility format blocked");
    logline(&l);
    return 0;
}

/* -------------------------------------------------------------- reports */
const char* vtd_ir_fault_reason(uint8_t r) {
    switch (r) {
    case 0x20: return "reserved field set in interrupt request";
    case 0x21: return "interrupt index beyond the table";
    case 0x22: return "remapping entry not present";
    case 0x23: return "remapping table not readable";
    case 0x24: return "reserved field set in remapping entry";
    case 0x25: return "compatibility-format interrupt blocked";
    case 0x26: return "source id check failed";
    case 0x27: return "posted-interrupt descriptor not readable";
    default:   return "interrupt remapping fault";
    }
}

void vtd_ir_generate(void (*emit)(void* ctx, const char* line), void* ctx) {
    iommu_line_t l; l.n = 0;
    iommu_ln_s(&l, "ir "); iommu_ln_s(&l, vtd_ir_enabled() ? "on" : "off");
    if (!vtd_ir_enabled()) { iommu_ln_s(&l, " ("); iommu_ln_s(&l, g_state); iommu_ln_s(&l, ")"); }
    else {
        iommu_ln_s(&l, " entries "); iommu_ln_dec(&l, IR_ENTRIES);
        iommu_ln_s(&l, " eim "); iommu_ln_dec(&l, (uint64_t)g_eim);
        int cfis = 0;
        for (int i = 0; i < vtd_unit_count(); i++)
            if (g_unit_ir[i] && (vtd_unit_gsts(i) & VTD_GSTS_CFIS)) cfis = 1;
        iommu_ln_s(&l, " cfis "); iommu_ln_dec(&l, (uint64_t)cfis);
    }
    iommu_ln_s(&l, " set "); iommu_ln_dec(&l, g_n_set);
    iommu_ln_s(&l, " cleared "); iommu_ln_dec(&l, g_n_clear);
    iommu_ln_s(&l, " retargeted "); iommu_ln_dec(&l, g_n_retarget);
    iommu_ln_s(&l, " iec "); iommu_ln_dec(&l, g_n_iec);
    iommu_ln_s(&l, " iec-errors "); iommu_ln_dec(&l, g_n_iec_err);
    iommu_ln_s(&l, " compat-fallback "); iommu_ln_dec(&l, g_n_compat);
    iommu_ln_s(&l, " refused "); iommu_ln_dec(&l, g_n_refused);
    emit(ctx, iommu_ln_end(&l));
    if (!g_irt) return;
    for (uint32_t v = 0; v < IR_ENTRIES; v++) {
        uint64_t f = spin_lock_irqsave(&g_ir_lock);
        uint8_t own = g_own[v]; uint16_t sid = g_sid[v]; uint8_t svt = g_svt[v];
        uint32_t dst = g_dst[v]; uint8_t lvl = g_lvl[v];
        spin_unlock_irqrestore(&g_ir_lock, f);
        if (own == OWN_FREE) continue;
        iommu_ln_s(&l, "irte "); iommu_ln_hexw(&l, v, 2);
        iommu_ln_s(&l, own == OWN_IOAPIC ? " ioapic" : " msi");
        if (svt == 2) {
            iommu_ln_s(&l, " buses "); iommu_ln_hexw(&l, sid >> 8, 2);
            iommu_ln_s(&l, "-"); iommu_ln_hexw(&l, sid & 0xFF, 2);
        } else { iommu_ln_s(&l, " sid "); iommu_ln_bdf(&l, sid); }
        iommu_ln_s(&l, " dest "); iommu_ln_hex(&l, dst);
        iommu_ln_s(&l, lvl ? " level" : " edge");
        emit(ctx, iommu_ln_end(&l));
    }
}

#ifdef SECOS_IOMMU_TEST
/* Test hooks (kernel/iommu_ir_test.c). */
int vtd_ir_test_cfis(void) {
    int cfis = 0;
    for (int i = 0; i < vtd_unit_count(); i++)
        if (g_unit_ir[i] && (vtd_unit_gsts(i) & VTD_GSTS_CFIS)) cfis = 1;
    return cfis;
}
/* The entry's raw halves (0/0 when not present). */
void vtd_ir_test_entry(uint32_t idx, uint64_t* lo, uint64_t* hi) {
    *lo = (g_irt && idx < IR_ENTRIES) ? g_irt[idx * 2] : 0;
    *hi = (g_irt && idx < IR_ENTRIES) ? g_irt[idx * 2 + 1] : 0;
}
#endif
