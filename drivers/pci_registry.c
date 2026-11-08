/*
 * SecOS Kernel - PCI device registry (M53a)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * One enumeration at boot (BSP only, before the APs start) into g_devs[]:
 * ids, class, header type, BARs with their sizes, the capability offsets that
 * Phase 5 needs (PM, MSI, MSI-X, PCIe, AF; extended AER, ACS, ATS, SR-IOV),
 * FLR support, bridge bus ranges and the upstream bridge of every function.
 * After pci_init() the table is read-only, so readers take no lock.
 *
 * The scan is the same brute force over buses 0..255 that pci_find used to do
 * on every call, in the same order, so pci_find / pci_find_class return the
 * same first match as before.
 *
 * ECAM: the segment-0 MCFG window is validated (alignment, bus range, not over
 * RAM), mapped UC, and switched on only if a read of every function's header
 * through it equals the CF8/CFC read ("[PCI] ecam self-check OK"). Any
 * mismatch keeps CF8/CFC. Boot word `pci=noecam` skips ECAM entirely.
 *
 * Isolation (used by M57 to decide which functions may go to ring 3): a
 * function is assignable alone when it has its own requester ID and cannot
 * reach a peer without passing the IOMMU: it is not a bridge, no bridge on its
 * path is conventional PCI or PCIe-to-PCI (those alias the requester ID), every
 * root/downstream port on the path has ACS SV+RR+CR+UF, and a multifunction
 * sibling set either has ACS or is a single function. Functions that fail share
 * an isolation group with the bridge (or function 0) where isolation stops.
 */
#include "pci.h"
#include "pci_internal.h"
#include "vmm.h"
#include "pmm.h"
#include "acpi.h"
#include "debugcon.h"

static pci_dev_t g_devs[PCI_MAX_DEVS];
static int g_ndev;
static int g_overflow;          /* more functions than PCI_MAX_DEVS: fall back */
static volatile int g_inited;
static int g_ecam_state;        /* 0 none, 1 on, 2 refused, 3 self-check failed, 4 disabled */

/* --------------------------------------------------------------- formatting */
struct lb { char b[256]; int n; };
static void lb_c(struct lb* l, char c) { if (l->n < (int)sizeof(l->b) - 1) l->b[l->n++] = c; }
static void lb_s(struct lb* l, const char* s) { while (*s) lb_c(l, *s++); }
static void lb_x(struct lb* l, uint64_t v, int digits) {
    static const char h[] = "0123456789abcdef";
    char t[16];
    if (digits > 16) digits = 16;
    for (int i = digits - 1; i >= 0; i--) { t[i] = h[v & 0xF]; v >>= 4; }
    for (int i = 0; i < digits; i++) lb_c(l, t[i]);
}
static void lb_xv(struct lb* l, uint64_t v) {      /* minimal-width hex */
    int d = 1; for (uint64_t t = v >> 4; t; t >>= 4) d++;
    lb_x(l, v, d);
}
static void lb_d(struct lb* l, uint64_t v) {
    char t[24]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n--) lb_c(l, t[n]);
}
static void lb_size(struct lb* l, uint64_t sz) {
    if (sz >= (1ULL << 30) && !(sz & ((1ULL << 30) - 1))) { lb_d(l, sz >> 30); lb_c(l, 'G'); }
    else if (sz >= (1ULL << 20) && !(sz & ((1ULL << 20) - 1))) { lb_d(l, sz >> 20); lb_c(l, 'M'); }
    else if (sz >= 1024 && !(sz & 1023)) { lb_d(l, sz >> 10); lb_c(l, 'K'); }
    else lb_d(l, sz);
}
static void lb_end(struct lb* l) { l->b[l->n] = 0; }

/* --------------------------------------------------------------- scanning */
static inline uint64_t irq_save_cli(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

static uint64_t lowest_bit(uint64_t v) { return v & (~v + 1); }

/* Size and record the BARs. Decode is off while a BAR holds all-ones, and
 * interrupts are off so no handler on this CPU touches the device meanwhile
 * (the APs are not running yet). Host bridges are left alone, as Linux does. */
static void scan_bars(pci_dev_t* d) {
    uint8_t b = d->bus, s = d->slot, f = d->func;
    int host_bridge = (d->class_code == 0x06 && d->subclass == 0x00);
    uint64_t fl = irq_save_cli();
    uint16_t cmd = pci_config_read16(b, s, f, PCI_CFG_COMMAND);
    if (!host_bridge)
        pci_config_write16(b, s, f, PCI_CFG_COMMAND,
                           (uint16_t)(cmd & ~(PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE)));
    for (int i = 0; i < d->nbars; i++) {
        uint8_t off = (uint8_t)(PCI_CFG_BAR0 + i * 4);
        uint32_t lo = pci_config_read32(b, s, f, off);
        if (lo & 0x1) {                                        /* I/O BAR */
            d->bar_flags[i] = PCI_BAR_IO;
            d->bar_base[i] = lo & 0xFFFFFFFCu;
            if (host_bridge) continue;
            pci_config_write32(b, s, f, off, 0xFFFFFFFFu);
            uint32_t m = pci_config_read32(b, s, f, off);
            pci_config_write32(b, s, f, off, lo);
            uint32_t mask = m & 0xFFFFFFFCu;
            if (!(m & 0xFFFF0000u)) mask &= 0xFFFFu;          /* 16-bit decoder */
            uint64_t sz = lowest_bit(mask);
            if (sz) { d->bar_size[i] = sz; d->bar_flags[i] |= PCI_BAR_PRESENT; }
            continue;
        }
        int is64 = (((lo >> 1) & 0x3) == 0x2) && i + 1 < d->nbars;
        uint32_t hi = is64 ? pci_config_read32(b, s, f, (uint8_t)(off + 4)) : 0;
        d->bar_flags[i] = (uint8_t)((is64 ? PCI_BAR_MEM64 : 0) | ((lo & 0x8) ? PCI_BAR_PREFETCH : 0));
        d->bar_base[i] = ((uint64_t)hi << 32) | (lo & 0xFFFFFFF0u);
        if (!host_bridge) {
            pci_config_write32(b, s, f, off, 0xFFFFFFFFu);
            uint32_t mlo = pci_config_read32(b, s, f, off);
            uint32_t mhi = 0xFFFFFFFFu;
            if (is64) {
                pci_config_write32(b, s, f, (uint8_t)(off + 4), 0xFFFFFFFFu);
                mhi = pci_config_read32(b, s, f, (uint8_t)(off + 4));
                pci_config_write32(b, s, f, (uint8_t)(off + 4), hi);
            }
            pci_config_write32(b, s, f, off, lo);
            /* Same rule as pci_bar_size: no low size bits => unimplemented. */
            if (mlo & 0xFFFFFFF0u) {
                uint64_t mask = ((uint64_t)mhi << 32) | (mlo & 0xFFFFFFF0u);
                d->bar_size[i] = ~mask + 1;
                d->bar_flags[i] |= PCI_BAR_PRESENT;
            }
        }
        if (is64) { i++; d->bar_flags[i] = PCI_BAR_UPPER; }
    }
    if (!host_bridge) pci_config_write16(b, s, f, PCI_CFG_COMMAND, cmd);
    irq_restore(fl);
}

/* Standard capability list: bounded (48 hops), pointers >= 0x40 only. */
static void scan_caps(pci_dev_t* d) {
    uint8_t b = d->bus, s = d->slot, f = d->func;
    d->pcie_type = PCI_EXP_NONE;
    if (!(pci_config_read16(b, s, f, PCI_CFG_STATUS) & PCI_STATUS_CAP_LIST)) return;
    uint8_t ptr = (uint8_t)(pci_config_read8(b, s, f, (uint8_t)(d->header_type == 2 ? 0x14 : PCI_CFG_CAP_PTR)) & 0xFC);
    for (int guard = 0; ptr >= 0x40 && guard < 48; guard++) {
        uint16_t w = pci_config_read16(b, s, f, ptr);
        uint8_t id = (uint8_t)(w & 0xFF), next = (uint8_t)((w >> 8) & 0xFC);
        switch (id) {
        case PCI_CAP_ID_PM:   if (!d->cap_pm)   d->cap_pm = ptr; break;
        case PCI_CAP_ID_MSI:  if (!d->cap_msi)  d->cap_msi = ptr; break;
        case PCI_CAP_ID_MSIX: if (!d->cap_msix) d->cap_msix = ptr; break;
        case PCI_CAP_ID_AF:   if (!d->cap_af)   d->cap_af = ptr; break;
        case PCI_CAP_ID_PCIE:
            if (!d->cap_pcie) {
                d->cap_pcie = ptr;
                d->pcie_type = (uint8_t)((pci_config_read16(b, s, f, (uint8_t)(ptr + 2)) >> 4) & 0xF);
                if (pci_config_read32(b, s, f, (uint8_t)(ptr + 4)) & (1u << 28)) d->flr |= PCI_FLR_PCIE;
            }
            break;
        default: break;
        }
        ptr = next;
    }
    if (d->cap_af) {
        uint8_t afcap = pci_config_read8(b, s, f, (uint8_t)(d->cap_af + 3));
        if ((afcap & 0x3) == 0x3) d->flr |= PCI_FLR_AF;          /* TP + FLR */
    }
}

/* Extended capabilities (ECAM only): bounded walk, every next pointer must be
 * a dword in 0x100..0xFFC; a cycle ends at the hop limit. */
static void scan_ecaps(pci_dev_t* d) {
    if (!pci_ecam_active() || !d->cap_pcie) return;
    uint16_t off = 0x100;
    for (int guard = 0; guard < 128; guard++) {
        uint32_t h = pci_config_read32_ext(d->bus, d->slot, d->func, off);
        if (h == 0 || h == 0xFFFFFFFFu) break;
        uint16_t id = (uint16_t)(h & 0xFFFF);
        uint16_t next = (uint16_t)((h >> 20) & 0xFFC);
        switch (id) {
        case PCI_ECAP_ID_AER:   if (!d->ecap_aer) d->ecap_aer = off; break;
        case PCI_ECAP_ID_ATS:   if (!d->ecap_ats) d->ecap_ats = off; break;
        case PCI_ECAP_ID_SRIOV: if (!d->ecap_sriov) d->ecap_sriov = off; break;
        case PCI_ECAP_ID_ACS:
            if (!d->ecap_acs && off <= 0xFF8) {
                d->ecap_acs = off;
                uint32_t r = pci_config_read32_ext(d->bus, d->slot, d->func, (uint16_t)(off + 4));
                d->acs_cap = (uint16_t)(r & 0xFFFF);
                d->acs_ctrl = (uint16_t)(r >> 16);
            }
            break;
        default: break;
        }
        if (next < 0x100) break;
        off = next;
    }
}

static void add_function(uint8_t bus, uint8_t slot, uint8_t func, uint8_t mf) {
    if (g_ndev >= PCI_MAX_DEVS) { g_overflow++; return; }
    pci_dev_t* d = &g_devs[g_ndev];
    for (unsigned i = 0; i < sizeof(*d); i++) ((uint8_t*)d)[i] = 0;
    uint32_t id = pci_config_read32(bus, slot, func, PCI_CFG_VENDOR_ID);
    uint32_t cl = pci_config_read32(bus, slot, func, 0x08);
    d->bus = bus; d->slot = slot; d->func = func; d->multifunc = mf;
    d->vendor_id = (uint16_t)(id & 0xFFFF);
    d->device_id = (uint16_t)(id >> 16);
    d->revision = (uint8_t)cl; d->prog_if = (uint8_t)(cl >> 8);
    d->subclass = (uint8_t)(cl >> 16); d->class_code = (uint8_t)(cl >> 24);
    d->header_type = (uint8_t)(pci_config_read8(bus, slot, func, 0x0E) & 0x7F);
    d->parent = -1; d->group = (int16_t)g_ndev;
    uint32_t irq = pci_config_read32(bus, slot, func, PCI_CFG_INT_LINE);
    d->irq_line = (uint8_t)irq; d->irq_pin = (uint8_t)(irq >> 8);
    if (d->header_type == 0) {
        d->nbars = 6;
        uint32_t ss = pci_config_read32(bus, slot, func, 0x2C);
        d->subsys_vendor = (uint16_t)ss; d->subsys_id = (uint16_t)(ss >> 16);
    } else if (d->header_type == 1 || d->header_type == 2) {
        d->nbars = (uint8_t)(d->header_type == 1 ? 2 : 1);
        d->sec_bus = pci_config_read8(bus, slot, func, 0x19);
        d->sub_bus = pci_config_read8(bus, slot, func, 0x1A);
    }
    scan_bars(d);
    scan_caps(d);
    g_ndev++;
}

/* The old brute force, in the old order. */
static void enumerate(void) {
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            if (pci_config_read16((uint8_t)bus, slot, 0, PCI_CFG_VENDOR_ID) == 0xFFFF) continue;
            uint8_t mf = (uint8_t)((pci_config_read8((uint8_t)bus, slot, 0, 0x0E) & 0x80) ? 1 : 0);
            uint8_t nfunc = mf ? 8 : 1;
            for (uint8_t func = 0; func < nfunc; func++) {
                if (pci_config_read16((uint8_t)bus, slot, func, PCI_CFG_VENDOR_ID) == 0xFFFF) continue;
                add_function((uint8_t)bus, slot, func, mf);
            }
        }
    }
}

/* --------------------------------------------------------------- ECAM */
extern int kernel_cmdline_has(const char* word);

/* Validate + map the segment-0 MCFG window. Returns the virtual address of
 * bus 0 (0 = none) and the bus range. */
static uint64_t ecam_candidate(uint8_t* sb, uint8_t* eb) {
    int n = acpi_mcfg_count();
    if (n <= 0) { debugcon_writestring("[PCI] ecam: no MCFG, using CF8/CFC\n"); return 0; }
    const acpi_mcfg_entry_t* e = 0;
    for (int i = 0; i < n; i++) {
        const acpi_mcfg_entry_t* c = acpi_mcfg_get(i);
        if (!c) continue;
        if (c->segment != 0) {
            struct lb l = {.n = 0};
            lb_s(&l, "[PCI] ecam: segment "); lb_xv(&l, c->segment);
            lb_s(&l, " ignored (only segment 0 is supported)\n"); lb_end(&l);
            debugcon_writestring(l.b);
            continue;
        }
        if (!e) e = c;
    }
    struct lb l = {.n = 0};
    if (!e) { debugcon_writestring("[PCI] ecam: no segment-0 MCFG entry, using CF8/CFC\n"); return 0; }
    uint64_t base = e->base;
    uint8_t s = e->start_bus, t = e->end_bus;
#if defined(SECOS_PCI_ECAM_TEST) && SECOS_PCI_ECAM_TEST == 2
    base |= 0x1000;                       /* negative test: misaligned window */
#endif
    const char* bad = 0;
    if (s > t) bad = "start bus > end bus";
    else if (base & 0xFFFFFULL) bad = "base not 1 MiB aligned";
    else if (base >= VMM_PHYSMAP_SIZE || VMM_PHYSMAP_SIZE - base < ((uint64_t)t + 1) << 20)
        bad = "window beyond the physmap";
    uint64_t lo = base + ((uint64_t)s << 20), hi = base + (((uint64_t)t + 1) << 20);
    if (!bad) {
        for (int i = 0; i < pmm_ram_range_count(); i++) {
            uint64_t rs, re;
            if (pmm_ram_range(i, &rs, &re) == 0 && rs < hi && lo < re) { bad = "window overlaps RAM"; break; }
        }
    }
    lb_s(&l, "[PCI] ecam: MCFG base "); lb_xv(&l, base); lb_s(&l, " buses ");
    lb_x(&l, s, 2); lb_c(&l, '-'); lb_x(&l, t, 2);
    if (bad) { lb_s(&l, " refused: "); lb_s(&l, bad); lb_s(&l, ", using CF8/CFC\n"); lb_end(&l); debugcon_writestring(l.b); g_ecam_state = 2; return 0; }
    uint64_t v = vmm_map_mmio(lo, hi - lo, VMM_CACHE_UC);
    if (!v) { lb_s(&l, " map failed, using CF8/CFC\n"); lb_end(&l); debugcon_writestring(l.b); g_ecam_state = 2; return 0; }
    lb_c(&l, '\n'); lb_end(&l); debugcon_writestring(l.b);
    *sb = s; *eb = t;
    return v - ((uint64_t)s << 20);
}

/* Compare the 64-byte header of every enumerated function, and the id dword of
 * every slot/function in range, through ECAM and through CF8/CFC. Volatile
 * fields (status, BIST, a bridge's secondary status) are masked. */
static int ecam_self_check(uint64_t v0, uint8_t sb, uint8_t eb, int* nfuncs) {
    struct lb l = {.n = 0};
    for (uint32_t bus = sb; bus <= eb; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t a = pci_ecam_probe_read32(v0, (uint8_t)bus, slot, fn, 0);
                uint32_t c = pci_cf8_read32((uint8_t)bus, slot, fn, 0);
                if (a != c) {
                    lb_s(&l, "[PCI] ecam self-check FAIL at "); lb_x(&l, bus, 2); lb_c(&l, ':');
                    lb_x(&l, slot, 2); lb_c(&l, '.'); lb_d(&l, fn); lb_s(&l, " id ecam=");
                    lb_xv(&l, a); lb_s(&l, " cf8="); lb_xv(&l, c); lb_s(&l, ", using CF8/CFC\n");
                    lb_end(&l); debugcon_writestring(l.b);
                    return -1;
                }
                if (fn == 0 && (c & 0xFFFF) == 0xFFFF) break;   /* empty slot */
            }
        }
    }
    int n = 0;
    for (int i = 0; i < g_ndev; i++) {
        const pci_dev_t* d = &g_devs[i];
        if (d->bus < sb || d->bus > eb) continue;
        for (uint8_t off = 0; off < 0x40; off += 4) {
            uint32_t mask = 0xFFFFFFFFu;
            if (off == 0x04) mask = 0x0000FFFFu;                  /* status */
            if (off == 0x0C) mask = 0x00FFFFFFu;                  /* BIST   */
            if (d->header_type == 1 && off == 0x1C) mask = 0x0000FFFFu;  /* sec status */
            if (d->header_type == 2 && off >= 0x10 && off < 0x3C) continue;
            uint32_t a = pci_ecam_probe_read32(v0, d->bus, d->slot, d->func, off) & mask;
            uint32_t c = pci_cf8_read32(d->bus, d->slot, d->func, off) & mask;
            if (a != c) {
                lb_s(&l, "[PCI] ecam self-check FAIL at "); lb_x(&l, d->bus, 2); lb_c(&l, ':');
                lb_x(&l, d->slot, 2); lb_c(&l, '.'); lb_d(&l, d->func); lb_s(&l, " off ");
                lb_x(&l, off, 2); lb_s(&l, " ecam="); lb_xv(&l, a); lb_s(&l, " cf8=");
                lb_xv(&l, c); lb_s(&l, ", using CF8/CFC\n"); lb_end(&l);
                debugcon_writestring(l.b);
                return -1;
            }
        }
        n++;
    }
    *nfuncs = n;
    return 0;
}

static void ecam_setup(void) {
    if (kernel_cmdline_has("pci=noecam")) {
        debugcon_writestring("[PCI] ecam: disabled (pci=noecam), using CF8/CFC\n");
        g_ecam_state = 4;
        return;
    }
    uint8_t sb = 0, eb = 0;
    uint64_t v0 = ecam_candidate(&sb, &eb);
    if (!v0) return;
#if defined(SECOS_PCI_ECAM_TEST) && SECOS_PCI_ECAM_TEST == 1
    v0 += 1ULL << 20;                     /* negative test: off by one bus */
    eb = (uint8_t)(eb > sb ? eb - 1 : eb);
#endif
    int n = 0;
    if (ecam_self_check(v0, sb, eb, &n) != 0) { g_ecam_state = 3; return; }
    pci_ecam_enable(v0, sb, eb);
    g_ecam_state = 1;
    struct lb l = {.n = 0};
    lb_s(&l, "[PCI] ecam self-check OK ("); lb_d(&l, (uint64_t)n);
    lb_s(&l, " functions)\n"); lb_end(&l);
    debugcon_writestring(l.b);
}

/* --------------------------------------------------------------- topology */
static int valid_bridge(const pci_dev_t* b) {
    return (b->header_type == 1 || b->header_type == 2) &&
           b->sec_bus > b->bus && b->sub_bus >= b->sec_bus;
}

/* Parents: the valid bridge whose secondary bus is the function's bus. A valid
 * bridge's secondary bus is above its own bus, so the parent chain strictly
 * descends in bus number and cannot loop. */
static void link_parents(void) {
    for (int i = 0; i < g_ndev; i++) {
        pci_dev_t* d = &g_devs[i];
        d->parent = -1;
        for (int j = 0; j < g_ndev; j++) {
            const pci_dev_t* b = &g_devs[j];
            if (j != i && valid_bridge(b) && b->sec_bus == d->bus) { d->parent = (int16_t)j; break; }
        }
    }
}

/* A function is on a "root bus" if no valid bridge's range covers its bus. */
static int bus_is_covered(uint8_t bus) {
    for (int j = 0; j < g_ndev; j++) {
        const pci_dev_t* b = &g_devs[j];
        if (valid_bridge(b) && bus >= b->sec_bus && bus <= b->sub_bus) return 1;
    }
    return 0;
}

static int acs_isolates(const pci_dev_t* d) {
    return d->ecap_acs && (d->acs_cap & PCI_ACS_ISOLATE) == PCI_ACS_ISOLATE;
}

static void compute_groups(void) {
    for (int i = 0; i < g_ndev; i++) {
        pci_dev_t* d = &g_devs[i];
        int rep = i;
        uint8_t why = PCI_ASSIGN_OK;
        if (d->header_type != 0 || d->class_code == 0x06) why = PCI_ASSIGN_BRIDGE;
        else if (d->class_code == 0x08 && d->subclass == 0x06) why = PCI_ASSIGN_IOMMU;
        /* Path to the root: remember the highest bridge where isolation stops. */
        if (d->parent < 0 && bus_is_covered(d->bus)) { if (why == PCI_ASSIGN_OK) why = PCI_ASSIGN_ORPHAN; }
        int cur = d->parent;
        for (int depth = 0; cur >= 0 && depth < 256; depth++) {
            const pci_dev_t* b = &g_devs[cur];
            int stop = 0;
            if (b->pcie_type == PCI_EXP_NONE || b->pcie_type == PCI_EXP_PCIE_TO_PCI) {
                if (why == PCI_ASSIGN_OK || why == PCI_ASSIGN_NO_ACS) why = PCI_ASSIGN_CONV_BRIDGE;
                stop = 1;
            } else if ((b->pcie_type == PCI_EXP_ROOT_PORT || b->pcie_type == PCI_EXP_DOWNSTREAM) &&
                       !acs_isolates(b)) {
                if (why == PCI_ASSIGN_OK) why = PCI_ASSIGN_NO_ACS;
                stop = 1;
            }
            /* The group is rooted at the highest bridge where isolation stops
             * (a bridge itself keeps its own group). */
            if (stop && why != PCI_ASSIGN_BRIDGE && why != PCI_ASSIGN_ORPHAN && why != PCI_ASSIGN_IOMMU) rep = cur;
            cur = b->parent;
        }
        /* Multifunction siblings without ACS can talk to each other. */
        if (why == PCI_ASSIGN_OK && d->multifunc && !acs_isolates(d)) {
            int first = -1, count = 0;
            for (int j = 0; j < g_ndev; j++) {
                const pci_dev_t* o = &g_devs[j];
                if (o->seg == d->seg && o->bus == d->bus && o->slot == d->slot) {
                    if (first < 0) first = j;
                    count++;
                }
            }
            if (count > 1) { why = PCI_ASSIGN_MF_NO_ACS; rep = first; }
        }
        d->why = why;
        d->assignable = (uint8_t)(why == PCI_ASSIGN_OK);
        d->group = (int16_t)rep;
    }
}

/* --------------------------------------------------------------- logging */
static const char* pcie_type_name(uint8_t t) {
    switch (t) {
    case PCI_EXP_ENDPOINT:     return "ep";
    case PCI_EXP_LEG_ENDPOINT: return "legacy-ep";
    case PCI_EXP_ROOT_PORT:    return "root-port";
    case PCI_EXP_UPSTREAM:     return "upstream";
    case PCI_EXP_DOWNSTREAM:   return "downstream";
    case PCI_EXP_PCIE_TO_PCI:  return "pcie-to-pci";
    case PCI_EXP_PCI_TO_PCIE:  return "pci-to-pcie";
    case PCI_EXP_RC_END:       return "rciep";
    case PCI_EXP_RC_EC:        return "rc-ec";
    case PCI_EXP_NONE:         return "pci";
    default:                   return "pcie?";
    }
}

const char* pci_assign_reason(uint8_t why) {
    switch (why) {
    case PCI_ASSIGN_OK:          return "yes";
    case PCI_ASSIGN_BRIDGE:      return "no(bridge)";
    case PCI_ASSIGN_CONV_BRIDGE: return "no(behind-pci-bridge)";
    case PCI_ASSIGN_NO_ACS:      return "no(port-without-acs)";
    case PCI_ASSIGN_MF_NO_ACS:   return "no(multifunction-without-acs)";
    case PCI_ASSIGN_ORPHAN:      return "no(orphan-bus)";
    case PCI_ASSIGN_IOMMU:       return "no(iommu)";
    default:                     return "no(?)";
    }
}

/* One line per function: identity, PCIe type, topology, isolation, caps, BARs.
 * tools/pci_check.py compares these against QMP query-pci. */
static void log_dev(const pci_dev_t* d) {
    struct lb l = {.n = 0};
    lb_s(&l, "[PCI] "); lb_x(&l, d->bus, 2); lb_c(&l, ':'); lb_x(&l, d->slot, 2);
    lb_c(&l, '.'); lb_d(&l, d->func); lb_c(&l, ' ');
    lb_x(&l, d->vendor_id, 4); lb_c(&l, ':'); lb_x(&l, d->device_id, 4);
    lb_s(&l, " class "); lb_x(&l, d->class_code, 2); lb_x(&l, d->subclass, 2);
    lb_x(&l, d->prog_if, 2); lb_s(&l, " hdr "); lb_d(&l, d->header_type);
    lb_c(&l, ' '); lb_s(&l, pcie_type_name(d->pcie_type));
    if (d->header_type == 1 || d->header_type == 2) {
        lb_s(&l, " bus "); lb_x(&l, d->sec_bus, 2); lb_c(&l, '-'); lb_x(&l, d->sub_bus, 2);
    }
    lb_s(&l, " up ");
    if (d->parent >= 0) {
        const pci_dev_t* p = &g_devs[d->parent];
        lb_x(&l, p->bus, 2); lb_c(&l, ':'); lb_x(&l, p->slot, 2); lb_c(&l, '.'); lb_d(&l, p->func);
    } else lb_c(&l, '-');
    lb_s(&l, " grp "); lb_d(&l, (uint64_t)d->group);
    lb_s(&l, " assign "); lb_s(&l, pci_assign_reason(d->why));
    if (d->cap_msi) lb_s(&l, " msi");
    if (d->cap_msix) lb_s(&l, " msix");
    if (d->cap_pm) lb_s(&l, " pm");
    if (d->flr) lb_s(&l, " flr");
    if (d->ecap_acs) { lb_s(&l, " acs="); lb_xv(&l, d->acs_cap); }
    if (d->ecap_ats) lb_s(&l, " ats");
    if (d->ecap_aer) lb_s(&l, " aer");
    if (d->ecap_sriov) lb_s(&l, " sriov");
    for (int i = 0; i < d->nbars; i++) {
        if (!(d->bar_flags[i] & PCI_BAR_PRESENT)) continue;
        lb_s(&l, " bar"); lb_d(&l, (uint64_t)i); lb_c(&l, '=');
        lb_s(&l, (d->bar_flags[i] & PCI_BAR_IO) ? "io:" : (d->bar_flags[i] & PCI_BAR_MEM64) ? "m64:" : "m32:");
        lb_xv(&l, d->bar_base[i]); lb_c(&l, '+'); lb_size(&l, d->bar_size[i]);
    }
    lb_c(&l, '\n'); lb_end(&l);
    debugcon_writestring(l.b);
}

/* --------------------------------------------------------------- public API */
void pci_init(void) {
    if (g_inited) return;
    g_inited = 1;
    g_ndev = 0; g_overflow = 0;
    enumerate();                 /* CF8/CFC (ECAM is not on yet) */
    ecam_setup();
    for (int i = 0; i < g_ndev; i++) scan_ecaps(&g_devs[i]);
    link_parents();
    compute_groups();
    for (int i = 0; i < g_ndev; i++) log_dev(&g_devs[i]);
    struct lb l = {.n = 0};
    lb_s(&l, "[PCI] registry: "); lb_d(&l, (uint64_t)g_ndev); lb_s(&l, " functions, config ");
    lb_s(&l, pci_ecam_active() ? "ecam" : "cf8");
    int na = 0; for (int i = 0; i < g_ndev; i++) na += g_devs[i].assignable;
    lb_s(&l, ", "); lb_d(&l, (uint64_t)na); lb_s(&l, " assignable");
    if (g_overflow) { lb_s(&l, ", "); lb_d(&l, (uint64_t)g_overflow); lb_s(&l, " NOT recorded (table full)"); }
    lb_c(&l, '\n'); lb_end(&l);
    debugcon_writestring(l.b);
}

int pci_dev_count(void) { pci_init(); return g_ndev; }
const pci_dev_t* pci_dev_at(int idx) {
    pci_init();
    return (idx >= 0 && idx < g_ndev) ? &g_devs[idx] : 0;
}
int pci_dev_index(const pci_dev_t* d) {
    if (d < &g_devs[0] || d >= &g_devs[g_ndev]) return -1;
    return (int)(d - &g_devs[0]);
}
const pci_dev_t* pci_dev_lookup(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func) {
    pci_init();
    for (int i = 0; i < g_ndev; i++) {
        const pci_dev_t* d = &g_devs[i];
        if (d->seg == seg && d->bus == bus && d->slot == slot && d->func == func) return d;
    }
    return 0;
}
const pci_dev_t* pci_dev_of(const pci_device_t* d) {
    return d ? pci_dev_lookup(0, d->bus, d->slot, d->func) : 0;
}
const pci_dev_t* pci_bridge_for_bus(uint16_t seg, uint8_t bus) {
    pci_init();
    for (int j = 0; j < g_ndev; j++) {
        const pci_dev_t* b = &g_devs[j];
        if (b->seg == seg && valid_bridge(b) && b->sec_bus == bus) return b;
    }
    return 0;
}

static void fill_out(const pci_dev_t* d, pci_device_t* out) {
    if (!out) return;
    out->bus = d->bus; out->slot = d->slot; out->func = d->func;
    out->vendor_id = d->vendor_id; out->device_id = d->device_id;
}

/* The pre-M53a live scan, used only when the table overflowed (it may then
 * miss functions past PCI_MAX_DEVS). */
static int live_find(int by_class, uint16_t v_or_c, uint16_t d_or_s, uint8_t progif, pci_device_t* out) {
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            if (pci_config_read16((uint8_t)bus, slot, 0, PCI_CFG_VENDOR_ID) == 0xFFFF) continue;
            uint8_t nfunc = (pci_config_read8((uint8_t)bus, slot, 0, 0x0E) & 0x80) ? 8 : 1;
            for (uint8_t func = 0; func < nfunc; func++) {
                uint32_t id = pci_config_read32((uint8_t)bus, slot, func, PCI_CFG_VENDOR_ID);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                int hit;
                if (by_class) {
                    uint32_t cl = pci_config_read32((uint8_t)bus, slot, func, 0x08);
                    hit = ((cl >> 24) & 0xFF) == v_or_c && ((cl >> 16) & 0xFF) == d_or_s &&
                          (progif == 0xFF || ((cl >> 8) & 0xFF) == progif);
                } else {
                    hit = (id & 0xFFFF) == v_or_c && (d_or_s == 0 || (id >> 16) == d_or_s);
                }
                if (hit) {
                    if (out) {
                        out->bus = (uint8_t)bus; out->slot = slot; out->func = func;
                        out->vendor_id = (uint16_t)id; out->device_id = (uint16_t)(id >> 16);
                    }
                    return 0;
                }
            }
        }
    }
    return -1;
}

int pci_find(uint16_t want_vendor, uint16_t want_device, pci_device_t* out) {
    pci_init();
    if (g_overflow) return live_find(0, want_vendor, want_device, 0, out);
    for (int i = 0; i < g_ndev; i++) {
        const pci_dev_t* d = &g_devs[i];
        if (d->vendor_id == want_vendor && (want_device == 0 || d->device_id == want_device)) {
            fill_out(d, out);
            return 0;
        }
    }
    return -1;
}

int pci_find_class(uint8_t cls, uint8_t subcls, uint8_t progif, pci_device_t* out) {
    pci_init();
    if (g_overflow) return live_find(1, cls, subcls, progif, out);
    for (int i = 0; i < g_ndev; i++) {
        const pci_dev_t* d = &g_devs[i];
        if (d->class_code == cls && d->subclass == subcls && (progif == 0xFF || d->prog_if == progif)) {
            fill_out(d, out);
            return 0;
        }
    }
    return -1;
}
