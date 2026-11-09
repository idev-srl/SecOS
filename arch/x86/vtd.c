/*
 * vtd.c — Intel VT-d DMA remapping: units, invalidation, faults, contexts
 * (Phase 5, M54; plan docs/PHASE5_PLAN.md §3 "M54", D1/D2).
 *
 * Runs only with the `iommu=on` boot word (kernel/iommu.c). For every DRHD
 * the ACPI DMAR describes (arch/x86/acpi_dmar.c, M53b):
 *   - map its registers (UC), read VER/CAP/ECAP; refuse a unit the firmware
 *     described badly (scopes not all parsed, iGPU-only) or whose
 *     capabilities do not fit (no 39/48-bit AGAW, no domain ids);
 *   - firmware handoff: if the firmware left queued invalidation on, drain
 *     and disable it; if it left translation on, our root table replaces
 *     its own while TE stays on (SRTP + global invalidations), then the
 *     protected-memory regions are disabled. Every step is logged;
 *   - a root table, context tables on demand (one 4 KiB table per bus);
 *   - queued invalidation (one 4 KiB queue, wait descriptors with a status
 *     write) or, without ECAP.QI or with `iommu.qi=off`, the register-based
 *     CCMD / IOTLB interface;
 *   - a fault-event MSI on a vector from the M53c allocator.
 *
 * Caching mode (CAP.CM, QEMU `caching-mode=on`): the hardware may cache
 * not-present and invalid entries, tagged with domain id 0. So domain id 0
 * is never handed out, every context-entry change is followed by a
 * device-selective context-cache invalidation (domain id 0 for a not-present
 * -> present change, as the spec asks) and a domain-selective IOTLB
 * invalidation, and every new mapping by a page-selective IOTLB
 * invalidation. Without CM a new mapping needs only a write-buffer flush
 * (CAP.RWBF). An unmap always waits for its IOTLB invalidation.
 *
 * Locks: g_unit[i].lock (LK_S68_IOMMU_UNIT, IRQ-safe) around a unit's
 * registers, queue, context tables and fault records. The page-table code
 * runs under the domain lock (S67) taken by kernel/iommu.c; flushes nest the
 * unit lock inside it. The fault interrupt takes only the unit lock and
 * reports with no lock held.
 *
 * SPDX-License-Identifier: MIT
 */
#include "vtd.h"
#include "irq.h"
#include "percpu.h"
#include "pmm.h"
#include "vmm.h"
#include "tsc.h"
#include "debugcon.h"
#include "pci_msi.h"
#include <stddef.h>

#define VTD_MAX_UNITS   DMAR_MAX_DRHD
#define VTD_DID_MAX     1024          /* domain ids we hand out (1..1023)    */
#define VTD_IQ_ENTRIES  256           /* one 4 KiB page of 128-bit descriptors */
#define VTD_ALIAS_MAX   32
#define VTD_TIMEOUT_NS  200000000ull  /* 200 ms for any register handshake   */

typedef struct vtd_unit {
    int              used;            /* probed and kept                      */
    int              active;          /* translation enabled by us            */
    uint32_t         drhd_idx;
    const dmar_drhd_t* drhd;
    volatile uint8_t* regs;
    uint64_t         base, reg_size;
    uint32_t         ver;
    uint64_t         cap, ecap;
    uint32_t         iro, fro, nfr, mamv;
    uint8_t          cm, rwbf, coherent, pt, psi, drd, dwd, use_qi;
    uint8_t          fw_te, fw_qi, fw_ir;   /* firmware left them on          */
    uint32_t         nd_domains;
    uint64_t         root;            /* root table (phys)                    */
    uint64_t         ctx[256];        /* context table per bus (phys)         */
    uint64_t         iq;              /* invalidation queue (phys)            */
    uint32_t         iq_tail;         /* next free descriptor index           */
    uint64_t         stat_phys;
    volatile uint32_t* stat;
    uint32_t         seq;
    int              vec;             /* fault-event vector (-1 none)         */
    spinlock_t       lock;
    /* counters */
    uint64_t         n_cc, n_iotlb, n_psi, n_dsi, n_wait, n_wbf;
    uint64_t         n_timeout, n_iqe, n_ctx_set, n_ctx_clear;
} vtd_unit_t;

static vtd_unit_t g_unit[VTD_MAX_UNITS];
static int        g_nunits;
static int        g_levels = 3;       /* common page-table depth              */
static uint32_t   g_huge;             /* common SLLPS bits (2M=1, 1G=2)       */
static int        g_all_pt;           /* every unit does TT=10b passthrough   */
static uint64_t   g_max_iova;
static uint32_t   g_did_limit = VTD_DID_MAX;
static volatile uint32_t g_did_map[VTD_DID_MAX / 32];
static iommu_domain_t* volatile g_did_dom[VTD_DID_MAX];
static int        g_force_no_qi, g_force_no_pt;

uint64_t g_vtd_table_limit = ~0ull;
int      g_vtd_clflush;

/* Requester-id aliases (devices behind a PCIe-to-PCI or PCI bridge reach the
 * IOMMU with the bridge's id): one shared context per alias, refcounted.
 * Guarded by the device lock (S65) in kernel/iommu.c (set_context runs
 * under it). */
typedef struct { uint16_t sid; uint8_t unit; uint32_t refs; iommu_domain_t* dom; } vtd_alias_t;
static vtd_alias_t g_alias[VTD_ALIAS_MAX];

/* ------------------------------------------------------------ helpers */
static inline uint32_t r32(vtd_unit_t* u, uint32_t off) { return *(volatile uint32_t*)(u->regs + off); }
static inline uint64_t r64(vtd_unit_t* u, uint32_t off) { return *(volatile uint64_t*)(u->regs + off); }
static inline void w32(vtd_unit_t* u, uint32_t off, uint32_t v) { *(volatile uint32_t*)(u->regs + off) = v; }
static inline void w64(vtd_unit_t* u, uint32_t off, uint64_t v) { *(volatile uint64_t*)(u->regs + off) = v; }

static void logline(iommu_line_t* l) { debugcon_writestring(iommu_ln_end(l)); }
static void ln_begin(iommu_line_t* l, vtd_unit_t* u) {
    l->n = 0;
    iommu_ln_s(l, "[IOMMU] vt-d");
    if (u) { iommu_ln_dec(l, u->drhd_idx); }
    iommu_ln_s(l, ": ");
}

void vtd_flush_cache(const void* p, uint64_t len) {
    if (!g_vtd_clflush || !len) return;
    uint64_t a = (uint64_t)p & ~63ull, e = (uint64_t)p + len;
    __asm__ volatile ("mfence" ::: "memory");
    for (; a < e; a += 64) __asm__ volatile ("clflush (%0)" :: "r"(a) : "memory");
    __asm__ volatile ("mfence" ::: "memory");
}

uint64_t vtd_table_alloc(void) {
    void* f = pmm_alloc_below(g_vtd_table_limit);
    if (!f) return 0;
    uint64_t pa = (uint64_t)f;
    uint64_t* v = (uint64_t*)phys_to_virt(pa);
    for (int i = 0; i < 512; i++) v[i] = 0;
    vtd_flush_cache(v, 4096);
    return pa;
}
void vtd_table_free(uint64_t phys) { if (phys) pmm_free_frame((void*)phys); }

/* Spin until (GSTS & mask) == want; 0 or -1 on timeout. */
static int wait_gsts(vtd_unit_t* u, uint32_t mask, uint32_t want) {
    uint64_t dl = ktime_ns() + VTD_TIMEOUT_NS;
    while ((r32(u, VTD_REG_GSTS) & mask) != want) {
        if (ktime_ns() > dl) { u->n_timeout++; return -1; }
        cpu_relax_poll();
    }
    return 0;
}
/* Set or clear a persistent GCMD bit and wait for its status. */
static int gcmd(vtd_unit_t* u, uint32_t bit, int on) {
    uint32_t v = r32(u, VTD_REG_GSTS) & VTD_GSTS_PERSIST;
    v = on ? (v | bit) : (v & ~bit);
    w32(u, VTD_REG_GCMD, v);
    return wait_gsts(u, bit, on ? bit : 0);   /* GCMD bit n reports in GSTS bit n */
}

/* ------------------------------------------------------ invalidation */
/* All below run with u->lock held. */

static int qi_push(vtd_unit_t* u, uint64_t lo, uint64_t hi) {
    uint32_t next = (u->iq_tail + 1) % VTD_IQ_ENTRIES;
    uint64_t dl = ktime_ns() + VTD_TIMEOUT_NS;
    while (((uint32_t)(r64(u, VTD_REG_IQH) >> 4) & (VTD_IQ_ENTRIES - 1)) == next) {
        if (ktime_ns() > dl) { u->n_timeout++; return -1; }
        cpu_relax_poll();
    }
    volatile uint64_t* q = (volatile uint64_t*)phys_to_virt(u->iq);
    q[u->iq_tail * 2] = lo;
    q[u->iq_tail * 2 + 1] = hi;
    vtd_flush_cache((const void*)&q[u->iq_tail * 2], 16);
    u->iq_tail = next;
    return 0;
}

/* Wait for everything queued so far (wait descriptor + status write). */
static int qi_sync(vtd_unit_t* u) {
    uint32_t seq = ++u->seq;
    if (seq == 0) seq = u->seq = 1;
    uint64_t lo = VTD_QI_WAIT | VTD_QI_WAIT_SW | VTD_QI_WAIT_FN | ((uint64_t)seq << 32);
    uint64_t hi = u->stat_phys;
    if (qi_push(u, lo, hi)) return -1;
    __asm__ volatile ("mfence" ::: "memory");
    w64(u, VTD_REG_IQT, (uint64_t)u->iq_tail << 4);
    u->n_wait++;
    uint64_t dl = ktime_ns() + VTD_TIMEOUT_NS;
    int rc = 0;
    while (__atomic_load_n(u->stat, __ATOMIC_ACQUIRE) != seq) {
        uint32_t fs = r32(u, VTD_REG_FSTS);
        if (fs & VTD_FSTS_IQE) {
            /* A descriptor the unit refused: replace it with our wait
             * descriptor so the queue moves on, then clear the error. */
            uint32_t h = (uint32_t)(r64(u, VTD_REG_IQH) >> 4) & (VTD_IQ_ENTRIES - 1);
            volatile uint64_t* q = (volatile uint64_t*)phys_to_virt(u->iq);
            q[h * 2] = lo; q[h * 2 + 1] = hi;
            vtd_flush_cache((const void*)&q[h * 2], 16);
            w32(u, VTD_REG_FSTS, VTD_FSTS_IQE);
            u->n_iqe++;
            rc = -1;
        }
        if (ktime_ns() > dl) {
            u->n_timeout++;
            debugcon_writestring("[IOMMU] vt-d: invalidation wait TIMEOUT\n");
            return -1;
        }
        cpu_relax_poll();
    }
    return rc;
}

/* Register-based invalidation (no QI). */
static int reg_ccmd(vtd_unit_t* u, uint64_t cmd) {
    w64(u, VTD_REG_CCMD, cmd | VTD_CCMD_ICC);
    uint64_t dl = ktime_ns() + VTD_TIMEOUT_NS;
    while (r64(u, VTD_REG_CCMD) & VTD_CCMD_ICC) {
        if (ktime_ns() > dl) { u->n_timeout++; return -1; }
        cpu_relax_poll();
    }
    return 0;
}
static int reg_iotlb(vtd_unit_t* u, uint64_t cmd, uint64_t iva) {
    if (iva) w64(u, u->iro, iva);
    if (u->drd) cmd |= VTD_IOTLB_DR;
    if (u->dwd) cmd |= VTD_IOTLB_DW;
    w64(u, u->iro + 8, cmd | VTD_IOTLB_IVT);
    uint64_t dl = ktime_ns() + VTD_TIMEOUT_NS;
    while (r64(u, u->iro + 8) & VTD_IOTLB_IVT) {
        if (ktime_ns() > dl) { u->n_timeout++; return -1; }
        cpu_relax_poll();
    }
    return 0;
}

#define INV_CC_GLOBAL    1
#define INV_CC_DEVICE    2
#define INV_IOTLB_GLOBAL 3
#define INV_IOTLB_DSI    4
#define INV_IOTLB_PSI    5

/* Queue (QI) or perform (registers) one invalidation; inv_sync() waits. */
static int inv(vtd_unit_t* u, int kind, uint16_t did, uint16_t sid, uint64_t addr, uint32_t am) {
    if (kind == INV_IOTLB_PSI && (!u->psi || am > u->mamv)) kind = INV_IOTLB_DSI;
    if (kind <= INV_CC_DEVICE) u->n_cc++; else u->n_iotlb++;
    if (kind == INV_IOTLB_PSI) u->n_psi++;
    if (kind == INV_IOTLB_DSI) u->n_dsi++;
    if (u->use_qi) {
        uint64_t lo = 0, hi = 0;
        switch (kind) {
        case INV_CC_GLOBAL: lo = VTD_QI_CC | VTD_QI_GRAN_GLOBAL; break;
        case INV_CC_DEVICE: lo = VTD_QI_CC | VTD_QI_GRAN_DEVPAGE | ((uint64_t)did << 16) | ((uint64_t)sid << 32); break;
        case INV_IOTLB_GLOBAL: lo = VTD_QI_IOTLB | VTD_QI_GRAN_GLOBAL; break;
        case INV_IOTLB_DSI: lo = VTD_QI_IOTLB | VTD_QI_GRAN_DOMAIN | ((uint64_t)did << 16); break;
        default: lo = VTD_QI_IOTLB | VTD_QI_GRAN_DEVPAGE | ((uint64_t)did << 16);
                 hi = (addr & ~0xFFFull) | am; break;
        }
        if (kind >= INV_IOTLB_GLOBAL) {
            if (u->drd) lo |= VTD_QI_IOTLB_DR;
            if (u->dwd) lo |= VTD_QI_IOTLB_DW;
        }
        return qi_push(u, lo, hi);
    }
    switch (kind) {
    case INV_CC_GLOBAL: return reg_ccmd(u, VTD_CCMD_GLOBAL);
    case INV_CC_DEVICE: return reg_ccmd(u, VTD_CCMD_DEVICE | did | ((uint64_t)sid << 16));
    case INV_IOTLB_GLOBAL: return reg_iotlb(u, VTD_IOTLB_GLOBAL, 0);
    case INV_IOTLB_DSI: return reg_iotlb(u, VTD_IOTLB_DSI | ((uint64_t)did << 32), 0);
    default: return reg_iotlb(u, VTD_IOTLB_PSI | ((uint64_t)did << 32), (addr & ~0xFFFull) | am);
    }
}
static int inv_sync(vtd_unit_t* u) { return u->use_qi ? qi_sync(u) : 0; }

static int write_buffer_flush(vtd_unit_t* u) {
    if (!u->rwbf) return 0;
    u->n_wbf++;
    w32(u, VTD_REG_GCMD, (r32(u, VTD_REG_GSTS) & VTD_GSTS_PERSIST) | VTD_GCMD_WBF);
    return wait_gsts(u, VTD_GSTS_WBFS, 0);
}

/* IOTLB flush of [iova, iova+len) for `did`: page-selective with the
 * smallest naturally aligned block that holds the range, else the domain. */
static int flush_range(vtd_unit_t* u, uint16_t did, uint64_t iova, uint64_t len) {
    uint64_t first = iova >> 12, last = (iova + len - 1) >> 12;
    uint32_t am = 0;
    while (am < 63 && ((first >> am) != (last >> am))) am++;
    int rc;
    if (am > u->mamv || !u->psi || len > (1ull << 30)) rc = inv(u, INV_IOTLB_DSI, did, 0, 0, 0);
    else rc = inv(u, INV_IOTLB_PSI, did, 0, (first >> am) << (am + 12), am);
    if (rc == 0) rc = inv_sync(u);
    return rc;
}

/* ----------------------------------------------------- domain ids */
static int did_alloc(iommu_domain_t* d) {
    for (uint32_t i = 1; i < g_did_limit; i++) {
        uint32_t w = i / 32, b = 1u << (i % 32);
        uint32_t old = __atomic_fetch_or(&g_did_map[w], b, __ATOMIC_ACQ_REL);
        if (!(old & b)) { g_did_dom[i] = d; return (int)i; }
    }
    return -1;
}
static void did_free(uint32_t did) {
    if (did == 0 || did >= g_did_limit) return;
    g_did_dom[did] = 0;
    __atomic_fetch_and(&g_did_map[did / 32], ~(1u << (did % 32)), __ATOMIC_ACQ_REL);
}

/* ------------------------------------------------------ ops: domains */
static uint64_t vtd_max_iova(void) { return g_max_iova; }

/* Identity-map every RAM range and every RMRR into `h`. */
static int build_identity(vtd_dom_t* h) {
    int n = pmm_ram_range_count();
    for (int i = 0; i < n; i++) {
        uint64_t s, e;
        if (pmm_ram_range(i, &s, &e) < 0) break;
        s &= ~0xFFFull; e = (e + 0xFFF) & ~0xFFFull;
        if (e > g_max_iova + 1) e = g_max_iova + 1;
        if (s >= e) continue;
        int rc = vtd_pt_map(h, s, s, e - s, IOMMU_READ | IOMMU_WRITE, g_huge, 1);
        if (rc) return rc;
    }
    const acpi_dmar_t* dm = acpi_dmar_get();
    for (uint32_t i = 0; i < dm->rmrr_count && i < DMAR_MAX_RMRR; i++) {
        const dmar_rmrr_t* r = &dm->rmrr[i];
        if (!r->valid || r->limit > g_max_iova) continue;
        int rc = vtd_pt_map(h, r->base, r->base, r->limit + 1 - r->base, IOMMU_READ | IOMMU_WRITE, 0, 1);
        if (rc) return rc;
    }
    return 0;
}

static int vtd_dom_init(iommu_domain_t* d) {
    vtd_dom_t* h = (vtd_dom_t*)d->hw;
    int did = did_alloc(d);
    if (did < 0) return IOMMU_ENOMEM;
    h->did = (uint16_t)did;
    h->levels = (uint8_t)g_levels;
    if (d->kind == IDOM_BLOCKED) return 0;
    if (d->kind == IDOM_PT && g_all_pt) { h->pt = 1; return 0; }
    int rc = vtd_pt_init(h, g_levels);
    if (rc == 0 && (d->kind == IDOM_IDENTITY || d->kind == IDOM_PT)) rc = build_identity(h);
    if (rc) { vtd_pt_free(h); did_free(h->did); }
    return rc;
}

static void vtd_dom_fini(iommu_domain_t* d) {
    vtd_dom_t* h = (vtd_dom_t*)d->hw;
    vtd_pt_free(h);
    did_free(h->did);
}

static int vtd_map(iommu_domain_t* d, uint64_t iova, uint64_t phys, uint64_t len, int prot) {
    vtd_dom_t* h = (vtd_dom_t*)d->hw;
    if (h->pt || !h->root) return IOMMU_EINVAL;
    if (phys + len - 1 >= g_vtd_table_limit) return IOMMU_ERANGE;   /* beyond the haw */
    int rc = vtd_pt_map(h, iova, phys, len, prot, 0, 0);
    if (rc) return rc;
    /* Make the new entries visible: caching mode needs an IOTLB flush
     * (not-present entries may be cached), otherwise a write-buffer flush. */
    for (int i = 0; i < g_nunits; i++) {
        vtd_unit_t* u = &g_unit[i];
        if (!u->used || !__atomic_load_n(&h->unit_refs[i], __ATOMIC_ACQUIRE)) continue;
        uint64_t f = spin_lock_irqsave(&u->lock);
        if (u->cm) flush_range(u, h->did, iova, len);
        else write_buffer_flush(u);
        spin_unlock_irqrestore(&u->lock, f);
    }
    return 0;
}

static int vtd_unmap(iommu_domain_t* d, uint64_t iova, uint64_t len) {
    vtd_dom_t* h = (vtd_dom_t*)d->hw;
    if (h->pt || !h->root) return IOMMU_EINVAL;
    uint64_t n = 0;
    int rc = vtd_pt_unmap(h, iova, len, &n);
    if (rc) return rc;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#ifdef SECOS_IOMMU_TEST_NOFLUSH
    /* Negative control for the edu flush test: no IOTLB invalidation. */
    (void)n;
    return 0;
#endif
    int bad = 0;
    for (int i = 0; i < g_nunits; i++) {
        vtd_unit_t* u = &g_unit[i];
        if (!u->used || !__atomic_load_n(&h->unit_refs[i], __ATOMIC_ACQUIRE)) continue;
        uint64_t f = spin_lock_irqsave(&u->lock);
        if (flush_range(u, h->did, iova, len)) bad = 1;
        spin_unlock_irqrestore(&u->lock, f);
    }
    return bad ? IOMMU_EIO : (int)0;
}

static uint64_t vtd_translate(iommu_domain_t* d, uint64_t iova, int* prot) {
    vtd_dom_t* h = (vtd_dom_t*)d->hw;
    if (h->pt) { if (prot) *prot = IOMMU_READ | IOMMU_WRITE; return iova; }
    return vtd_pt_translate(h, iova, prot);
}

/* ------------------------------------------------------ ops: contexts */
static vtd_unit_t* unit_for(uint16_t seg, uint16_t sid) {
    const dmar_drhd_t* r = acpi_dmar_drhd_for(seg, sid);
    if (!r) return NULL;
    for (int i = 0; i < g_nunits; i++)
        if (g_unit[i].used && g_unit[i].drhd == r) return &g_unit[i];
    return NULL;
}

static uint16_t dev_sid(const pci_dev_t* p) { return DMAR_BDF(p->bus, p->slot, p->func); }

/* The requester id a function's DMA carries when a bridge on its path
 * aliases it (VT-d 3.2 / PCIe-to-PCI bridge rules): the PCIe-to-PCI bridge
 * closest to the root takes (secondary bus, 00.0) ownership; a conventional
 * PCI-PCI bridge under PCIe forwards with its own id. Returns 1 + *alias. */
static int dev_alias(const pci_dev_t* p, uint16_t* alias) {
    int found = 0, hops = 0;
    int16_t up = p->parent;
    while (up >= 0 && hops++ < 32) {
        const pci_dev_t* b = pci_dev_at(up);
        if (!b) break;
        if (b->pcie_type == PCI_EXP_PCIE_TO_PCI) { *alias = DMAR_BDF(b->sec_bus, 0, 0); found = 1; }
        else if (b->pcie_type == PCI_EXP_NONE) {
            /* A conventional bridge: aliases only if its parent bus is PCIe
             * (a conventional bus on the root complex is not aliased). */
            const pci_dev_t* bp = b->parent >= 0 ? pci_dev_at(b->parent) : NULL;
            if (bp && bp->pcie_type != PCI_EXP_NONE) { *alias = dev_sid(b); found = 1; }
        }
        up = b->parent;
    }
    return found;
}

/* Install (h != NULL) or clear the context entry of `sid` on `u`, with the
 * invalidations the change needs. u->lock is held. Returns 0 or IOMMU_E*. */
static int ctx_write(vtd_unit_t* u, uint16_t sid, iommu_domain_t* nd) {
    uint8_t bus = (uint8_t)(sid >> 8), devfn = (uint8_t)sid;
    vtd_dom_t* h = nd ? (vtd_dom_t*)nd->hw : NULL;
    int ui = (int)(u - g_unit);
    if (!u->ctx[bus]) {
        if (!h) return 0;
        uint64_t c = vtd_table_alloc();
        if (!c) return IOMMU_ENOMEM;
        u->ctx[bus] = c;
        volatile uint64_t* re = (volatile uint64_t*)phys_to_virt(u->root) + bus * 2;
        re[1] = 0;
        __atomic_store_n(&re[0], c | 1, __ATOMIC_RELEASE);
        vtd_flush_cache((const void*)re, 16);
    }
    volatile uint64_t* ce = (volatile uint64_t*)phys_to_virt(u->ctx[bus]) + devfn * 2;
    uint64_t olo = ce[0], ohi = ce[1];
    if (olo & 1) {
        uint16_t odid = (uint16_t)((ohi >> 8) & 0xFFFF);
        int opt = ((olo >> 2) & 3) == 2;
        if (h && opt == h->pt && (h->pt || (olo & VTD_SL_ADDR) == h->root) && odid == h->did)
            return 0;                                  /* already there */
        __atomic_store_n(&ce[0], 0, __ATOMIC_RELEASE);
        ce[1] = 0;
        vtd_flush_cache((const void*)ce, 16);
        inv(u, INV_CC_DEVICE, odid, sid, 0, 0);
        inv(u, INV_IOTLB_DSI, odid, 0, 0, 0);
        int rc = inv_sync(u);
        u->n_ctx_clear++;
        if (odid < g_did_limit && g_did_dom[odid]) {
            vtd_dom_t* oh = (vtd_dom_t*)g_did_dom[odid]->hw;
            if (oh->unit_refs[ui]) __atomic_fetch_sub(&oh->unit_refs[ui], 1, __ATOMIC_ACQ_REL);
        }
        if (rc) return IOMMU_EIO;
    }
    if (!h) return 0;
    __atomic_fetch_add(&h->unit_refs[ui], 1, __ATOMIC_ACQ_REL);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t aw = (uint64_t)(g_levels - 2);
    uint64_t hi = aw | ((uint64_t)h->did << 8);
    uint64_t lo = h->pt ? (2ull << 2) : (h->root & VTD_SL_ADDR);
    ce[1] = hi;
    __atomic_store_n(&ce[0], lo | 1, __ATOMIC_RELEASE);
    vtd_flush_cache((const void*)ce, 16);
    /* Not present -> present: in caching mode the not-present entry may be
     * cached under domain id 0. Invalidate in both modes (cheap, and the
     * hardware may have cached the old entry we just cleared). */
    inv(u, INV_CC_DEVICE, u->cm ? 0 : h->did, sid, 0, 0);
    inv(u, INV_IOTLB_DSI, h->did, 0, 0, 0);
    int rc = inv_sync(u);
    if (!u->cm) write_buffer_flush(u);
    u->n_ctx_set++;
    return rc ? IOMMU_EIO : 0;
}

static int ctx_set(uint16_t seg, uint16_t sid, iommu_domain_t* d) {
    vtd_unit_t* u = unit_for(seg, sid);
    if (!u) return IOMMU_ENODEV;
    uint64_t f = spin_lock_irqsave(&u->lock);
    int rc = ctx_write(u, sid, d);
    spin_unlock_irqrestore(&u->lock, f);
    return rc;
}

/* RMRR identity maps for `dev` in a translated domain (d->lock taken). */
static int map_rmrrs(iommu_domain_t* d, const pci_dev_t* dev) {
    vtd_dom_t* h = (vtd_dom_t*)d->hw;
    if (h->pt || !h->root || d->kind != IDOM_TRANSLATED) return 0;
    const acpi_dmar_t* dm = acpi_dmar_get();
    uint16_t sid = dev_sid(dev);
    int rc = 0;
    uint64_t f = spin_lock_irqsave(&d->lock);
    for (uint32_t i = 0; i < dm->rmrr_count && i < DMAR_MAX_RMRR; i++) {
        const dmar_rmrr_t* r = &dm->rmrr[i];
        if (!r->valid || r->segment != dev->seg || r->limit > g_max_iova) continue;
        int mine = 0;
        for (uint32_t k = 0; k < r->scope_count; k++) {
            const dmar_scope_t* s = &dm->scopes[r->scope_first + k];
            if (s->resolved && s->type == DMAR_SCOPE_ENDPOINT && s->bdf == sid) mine = 1;
            if (s->resolved && s->type == DMAR_SCOPE_BRIDGE &&
                (uint8_t)(sid >> 8) >= s->sec_bus && (uint8_t)(sid >> 8) <= s->sub_bus) mine = 1;
        }
        if (!mine) continue;
        int e = vtd_pt_map(h, r->base, r->base, r->limit + 1 - r->base, IOMMU_READ | IOMMU_WRITE, 0, 1);
        if (e) { rc = e; break; }
    }
    spin_unlock_irqrestore(&d->lock, f);
    return rc;
}

static vtd_alias_t* alias_find(uint16_t sid, int create) {
    vtd_alias_t* fr = NULL;
    for (int i = 0; i < VTD_ALIAS_MAX; i++) {
        if (g_alias[i].refs && g_alias[i].sid == sid) return &g_alias[i];
        if (!g_alias[i].refs && !fr) fr = &g_alias[i];
    }
    if (create && fr) { fr->sid = sid; fr->dom = NULL; fr->refs = 0; return fr; }
    return NULL;
}

/* Called by kernel/iommu.c under the device lock. `d` NULL = blocked. The
 * caller knows the old domain (prev); aliases are shared by the group. */
static int vtd_set_context(iommu_domain_t* d, const pci_dev_t* dev) {
    uint16_t sid = dev_sid(dev), alias = 0;
    int has_alias = dev_alias(dev, &alias) && alias != sid;
    vtd_alias_t* a = NULL;
    if (d && has_alias) {
        a = alias_find(alias, 0);
        if (a && a->dom && a->dom != d) {
            iommu_line_t l; ln_begin(&l, NULL);
            iommu_ln_s(&l, "refuse attach of "); iommu_ln_bdf(&l, sid);
            iommu_ln_s(&l, ": its requester alias "); iommu_ln_bdf(&l, alias);
            iommu_ln_s(&l, " belongs to another domain");
            logline(&l);
            return IOMMU_EBUSY;
        }
    }
    if (d) {
        int rc = map_rmrrs(d, dev);
        if (rc) return rc;
    }
    int rc = ctx_set(dev->seg, sid, d);
    if (rc) return rc;
    if (has_alias) {
        /* Drop this function's reference on its old alias context... */
        vtd_alias_t* o = alias_find(alias, 0);
        if (d) {
            if (!o) {
                o = alias_find(alias, 1);
                if (!o) return 0;               /* table full: alias not programmed */
            }
            if (!o->dom) { if (ctx_set(dev->seg, alias, d) == 0) o->dom = d; }
            o->refs++;
        } else if (o && o->refs) {
            if (--o->refs == 0) { ctx_set(dev->seg, alias, NULL); o->dom = NULL; }
        }
    }
    return 0;
}

static int vtd_covers(const pci_dev_t* dev) { return unit_for(dev->seg, dev_sid(dev)) != NULL; }

static int vtd_irq_compose(const pci_dev_t* dev, int vec, uint32_t cpu, uint64_t* addr, uint32_t* data) {
    return vtd_ir_compose(dev, vec, cpu, addr, data);   /* M56: vtd_ir.c */
}

/* ------------------------------------- unit access for vtd_ir.c (M56) */
static vtd_unit_t* unit_at(int i) {
    return (i >= 0 && i < g_nunits && g_unit[i].used) ? &g_unit[i] : NULL;
}
int      vtd_unit_count(void) { return g_nunits; }
int      vtd_unit_used(int i) { vtd_unit_t* u = unit_at(i); return u && u->active; }
uint64_t vtd_unit_ecap(int i) { vtd_unit_t* u = unit_at(i); return u ? u->ecap : 0; }
int      vtd_unit_has_qi(int i) { vtd_unit_t* u = unit_at(i); return u && u->use_qi; }
uint32_t vtd_unit_index(int i) { vtd_unit_t* u = unit_at(i); return u ? u->drhd_idx : 0; }
uint32_t vtd_unit_gsts(int i) { vtd_unit_t* u = unit_at(i); return u ? r32(u, VTD_REG_GSTS) : 0; }

int vtd_unit_gcmd(int i, uint32_t bit, int on) {
    vtd_unit_t* u = unit_at(i);
    if (!u) return -1;
    uint64_t f = spin_lock_irqsave(&u->lock);
    int rc = gcmd(u, bit, on);
    spin_unlock_irqrestore(&u->lock, f);
    return rc;
}

static int iec_locked(vtd_unit_t* u, uint32_t index, uint32_t count) {
    if (!u->use_qi) return -1;                 /* IEC exists only as a QI descriptor */
    uint64_t lo = VTD_QI_IEC;
    if (count) {
        uint32_t im = 0;
        while ((1u << im) < count && im < 16) im++;
        lo |= VTD_QI_IEC_INDEX | ((uint64_t)im << 27) | ((uint64_t)(index & 0xFFFF) << 32);
    }
    if (qi_push(u, lo, 0)) return -1;
    return qi_sync(u);
}

int vtd_unit_iec(int i, uint32_t index, uint32_t count) {
    vtd_unit_t* u = unit_at(i);
    if (!u) return -1;
    uint64_t f = spin_lock_irqsave(&u->lock);
    int rc = iec_locked(u, index, count);
    spin_unlock_irqrestore(&u->lock, f);
    return rc;
}

int vtd_unit_set_irta(int i, uint64_t irta) {
    vtd_unit_t* u = unit_at(i);
    if (!u) return -1;
    uint64_t f = spin_lock_irqsave(&u->lock);
    w64(u, VTD_REG_IRTA, irta);
    w32(u, VTD_REG_GCMD, (r32(u, VTD_REG_GSTS) & VTD_GSTS_PERSIST) | VTD_GCMD_SIRTP);
    int rc = wait_gsts(u, VTD_GSTS_IRTPS, VTD_GSTS_IRTPS);
    /* The new table pointer is used only after a global IEC invalidation. */
    if (rc == 0) rc = iec_locked(u, 0, 0);
    spin_unlock_irqrestore(&u->lock, f);
    return rc;
}

int vtd_unit_for_dev(const pci_dev_t* dev) {
    vtd_unit_t* u = dev ? unit_for(dev->seg, dev_sid(dev)) : NULL;
    return u ? (int)(u - g_unit) : -1;
}

int vtd_unit_for_sid(uint16_t seg, uint16_t sid) {
    /* IOAPIC / HPET scopes: the DRHD that lists the special device. */
    const acpi_dmar_t* dm = acpi_dmar_get();
    for (int i = 0; i < g_nunits; i++) {
        vtd_unit_t* u = &g_unit[i];
        if (!u->used || !u->drhd || u->drhd->segment != seg) continue;
        const dmar_drhd_t* r = u->drhd;
        for (uint32_t k = 0; k < r->scope_count && r->scope_first + k < DMAR_MAX_SCOPES; k++) {
            const dmar_scope_t* sc = &dm->scopes[r->scope_first + k];
            if ((sc->type == DMAR_SCOPE_IOAPIC || sc->type == DMAR_SCOPE_HPET) &&
                sc->resolved && sc->bdf == sid)
                return i;
        }
    }
    /* An INCLUDE_PCI_ALL unit covers every source no other unit claims. */
    vtd_unit_t* u = unit_for(seg, sid);
    return u ? (int)(u - g_unit) : -1;
}

void vtd_dev_requester(const pci_dev_t* dev, uint16_t* sid, uint8_t* svt) {
    *sid = dev_sid(dev);
    *svt = 1;
    uint16_t alias = 0;
    if (!dev_alias(dev, &alias) || alias == *sid) return;
    /* The alias of a PCIe-to-PCI bridge is (secondary bus, 00.0): conventional
     * devices behind it may present any id on that bus range, so verify the
     * bus range (SVT=10b); a conventional bridge forwards with its own id. */
    int hops = 0;
    for (int16_t up = dev->parent; up >= 0 && hops++ < 32; ) {
        const pci_dev_t* b = pci_dev_at(up);
        if (!b) break;
        if (b->pcie_type == PCI_EXP_PCIE_TO_PCI && DMAR_BDF(b->sec_bus, 0, 0) == alias) {
            *sid = (uint16_t)(((uint16_t)b->sec_bus << 8) | b->sub_bus);
            *svt = 2;
            return;
        }
        up = b->parent;
    }
    *sid = alias;
}

static const char* vtd_fault_reason(uint8_t r) {
    switch (r) {
    case VTD_FR_ROOT_NP: return "root entry not present";
    case VTD_FR_CTX_NP: return "context entry not present (blocked)";
    case VTD_FR_CTX_INV: return "invalid context entry";
    case VTD_FR_BEYOND_AGAW: return "address beyond the AGAW";
    case VTD_FR_WRITE: return "write: page not present or not writable";
    case VTD_FR_READ: return "read: page not present or not readable";
    case VTD_FR_PT_ACCESS: return "page-table access error";
    case 0x08: return "root table address error";
    case 0x09: return "context table pointer error";
    case 0x0A: return "reserved bit in root entry";
    case 0x0B: return "reserved bit in context entry";
    case 0x0C: return "reserved bit in page-table entry";
    case 0x0D: return "translation type blocked";
    default:
        if (r >= VTD_FR_IR_FIRST && r <= VTD_FR_IR_LAST) return vtd_ir_fault_reason(r);
        return "other";
    }
}

/* ------------------------------------------------------------ faults */
#define FBATCH 16
static int drain_unit(vtd_unit_t* u, iommu_fault_t* out, int max, int* ovf) {
    int n = 0;
    *ovf = 0;
    uint64_t f = spin_lock_irqsave(&u->lock);
    for (int pass = 0; pass < 4 && n < max; pass++) {
        uint32_t fs = r32(u, VTD_REG_FSTS);
        if (fs & VTD_FSTS_PFO) { *ovf = 1; w32(u, VTD_REG_FSTS, VTD_FSTS_PFO); }
        if (!(fs & VTD_FSTS_PPF)) break;
        uint32_t start = VTD_FSTS_FRI(fs) % u->nfr;
        for (uint32_t k = 0; k < u->nfr && n < max; k++) {
            uint32_t i = (start + k) % u->nfr;
            uint32_t off = u->fro + i * 16;
            uint64_t hi = r64(u, off + 8);
            if (!(hi >> 63)) continue;
            uint64_t lo = r64(u, off);
            iommu_fault_t* r = &out[n++];
            r->seg = u->drhd ? u->drhd->segment : 0;
            r->bdf = (uint16_t)(hi & 0xFFFF);
            r->reason = (uint8_t)((hi >> 32) & 0xFF);
            r->is_write = ((hi >> 62) & 1) ? 0 : 1;      /* T: 1 = read */
            r->iova = lo & ~0xFFFull;
            r->unit = (uint8_t)u->drhd_idx;
            /* M56: interrupt-remapping faults carry the IRTE index in
             * FI[63:48] instead of a page address. */
            r->is_ir = (r->reason >= VTD_FR_IR_FIRST && r->reason <= VTD_FR_IR_LAST);
            r->ir_index = r->is_ir ? (uint16_t)(lo >> 48) : 0;
            if (r->is_ir) r->iova = 0;
            w32(u, off + 12, 0x80000000u);                /* F is RW1C */
        }
    }
    spin_unlock_irqrestore(&u->lock, f);
    return n;
}

static void drain_report(vtd_unit_t* u, int from_irq) {
    iommu_fault_t rec[FBATCH];
    for (int round = 0; round < 8; round++) {
        int ovf = 0;
        int n = drain_unit(u, rec, FBATCH, &ovf);
        if (ovf) iommu_note_overflow();
        for (int i = 0; i < n; i++) iommu_report_fault(&rec[i], from_irq);
        if (n < FBATCH) break;
    }
}

static void vtd_fault_irq(uint8_t vec, void* ctx) {
    (void)vec;
    iommu_note_fault_irq();
    drain_report((vtd_unit_t*)ctx, 1);
}

static void vtd_fault_poll(void) {
    for (int i = 0; i < g_nunits; i++) if (g_unit[i].used) drain_report(&g_unit[i], 0);
}

static int setup_fault_irq(vtd_unit_t* u) {
    u->vec = -1;
    static const char* names[4] = { "dmar0", "dmar1", "dmar2", "dmar3" };
    int v = irq_vec_alloc(0, vtd_fault_irq, u, u->drhd_idx < 4 ? names[u->drhd_idx] : "dmar");
    if (v < 0) return -1;
    cpu_t* c = cpu_by_index(0);
    if (!c || c->lapic_id > 0xFE) { irq_vec_free(v); return -1; }
    uint64_t f = spin_lock_irqsave(&u->lock);
    w32(u, VTD_REG_FECTL, VTD_FECTL_IM);
    w32(u, VTD_REG_FEDATA, (uint32_t)v);
    w32(u, VTD_REG_FEADDR, MSI_ADDR_BASE | (c->lapic_id << 12));
    w32(u, VTD_REG_FEUADDR, 0);
    spin_unlock_irqrestore(&u->lock, f);
    u->vec = v;
    return 0;
}

/* ----------------------------------------------------------- probing */
static int unit_probe(vtd_unit_t* u, const dmar_drhd_t* r, uint32_t idx, uint16_t haw) {
    iommu_line_t l;
    u->drhd = r; u->drhd_idx = idx; u->base = r->reg_base; u->vec = -1;
    spin_init_class(&u->lock, LK_S68_IOMMU_UNIT, "iommu_unit");
    uint64_t sz = 4096ull << (r->size_pages_log2 > 4 ? 4 : r->size_pages_log2);
    u->regs = (volatile uint8_t*)vmm_map_mmio(r->reg_base, sz, VMM_CACHE_UC);
    ln_begin(&l, u);
    if (!u->regs) { iommu_ln_s(&l, "cannot map registers at "); iommu_ln_hex(&l, r->reg_base); logline(&l); return -1; }
    u->reg_size = sz;
    u->ver = r32(u, VTD_REG_VER);
    u->cap = r64(u, VTD_REG_CAP);
    u->ecap = r64(u, VTD_REG_ECAP);
    if (u->cap == ~0ull || u->ecap == ~0ull || u->cap == 0) {
        iommu_ln_s(&l, "no unit answers at "); iommu_ln_hex(&l, r->reg_base); logline(&l); return -1;
    }
    u->iro = VTD_ECAP_IRO(u->ecap);
    u->fro = VTD_CAP_FRO(u->cap);
    u->nfr = VTD_CAP_NFR(u->cap);
    u->mamv = VTD_CAP_MAMV(u->cap);
    u->cm = !!(u->cap & VTD_CAP_CM);
    u->rwbf = !!(u->cap & VTD_CAP_RWBF);
    u->psi = !!(u->cap & VTD_CAP_PSI);
    u->drd = !!(u->cap & VTD_CAP_DRD);
    u->dwd = !!(u->cap & VTD_CAP_DWD);
    u->coherent = !!(u->ecap & VTD_ECAP_C);
    u->pt = !!(u->ecap & VTD_ECAP_PT);
    uint32_t nd = VTD_CAP_ND(u->cap);
    u->nd_domains = nd >= 6 ? 65536u : (1u << (4 + 2 * nd));
    /* Registers we touch must lie inside the mapped window. */
    uint64_t span = u->iro + 16;
    if ((uint64_t)u->fro + (uint64_t)u->nfr * 16 > span) span = (uint64_t)u->fro + (uint64_t)u->nfr * 16;
    if (span > sz) {
        uint64_t nsz = (span + 4095) & ~4095ull;
        if (nsz > 65536 || !(u->regs = (volatile uint8_t*)vmm_map_mmio(r->reg_base, nsz, VMM_CACHE_UC))) {
            iommu_ln_s(&l, "register set larger than its window: refused"); logline(&l); return -1;
        }
        u->reg_size = nsz;
    }
    uint32_t sagaw = VTD_CAP_SAGAW(u->cap);
    uint32_t gsts = r32(u, VTD_REG_GSTS);
    u->fw_te = !!(gsts & VTD_GSTS_TES);
    u->fw_qi = !!(gsts & VTD_GSTS_QIES);
    u->fw_ir = !!(gsts & VTD_GSTS_IRES);
    iommu_ln_s(&l, "unit at "); iommu_ln_hex(&l, r->reg_base);
    iommu_ln_s(&l, " ver "); iommu_ln_dec(&l, (u->ver >> 4) & 0xF); iommu_ln_s(&l, ".");
    iommu_ln_dec(&l, u->ver & 0xF);
    iommu_ln_s(&l, " cap "); iommu_ln_hex(&l, u->cap);
    iommu_ln_s(&l, " ecap "); iommu_ln_hex(&l, u->ecap);
    logline(&l);
    ln_begin(&l, u);
    iommu_ln_s(&l, "sagaw "); iommu_ln_hex(&l, sagaw);
    iommu_ln_s(&l, " mgaw "); iommu_ln_dec(&l, VTD_CAP_MGAW(u->cap));
    iommu_ln_s(&l, " haw "); iommu_ln_dec(&l, haw);
    iommu_ln_s(&l, " domains "); iommu_ln_dec(&l, u->nd_domains);
    iommu_ln_s(&l, u->cm ? " CM" : ""); iommu_ln_s(&l, u->rwbf ? " RWBF" : "");
    iommu_ln_s(&l, u->coherent ? " C" : " non-coherent");
    iommu_ln_s(&l, (u->ecap & VTD_ECAP_QI) ? " QI" : " no-QI");
    iommu_ln_s(&l, u->pt ? " PT" : " no-PT");
    iommu_ln_s(&l, (u->ecap & VTD_ECAP_IR) ? " IR" : "");
    iommu_ln_s(&l, " sllps "); iommu_ln_hex(&l, VTD_CAP_SLLPS(u->cap));
    iommu_ln_s(&l, " nfr "); iommu_ln_dec(&l, u->nfr);
    if (u->fw_te || u->fw_qi || u->fw_ir) {
        iommu_ln_s(&l, " firmware-left:"); iommu_ln_s(&l, u->fw_te ? " TE" : "");
        iommu_ln_s(&l, u->fw_qi ? " QIE" : ""); iommu_ln_s(&l, u->fw_ir ? " IRE" : "");
    }
    logline(&l);
    if (!(sagaw & 0x6)) {
        ln_begin(&l, u); iommu_ln_s(&l, "no 39/48-bit AGAW: unit not used"); logline(&l); return -1;
    }
    if (u->nfr == 0 || u->fro == 0) {
        ln_begin(&l, u); iommu_ln_s(&l, "no fault-recording registers: unit not used"); logline(&l); return -1;
    }
    return 0;
}

/* Firmware handoff, part 1 (before we own the queue): drain and disable the
 * firmware's queued invalidation so IQA can be reprogrammed. */
static void handoff_qi(vtd_unit_t* u) {
    if (!u->fw_qi) return;
    iommu_line_t l; ln_begin(&l, u);
    uint64_t dl = ktime_ns() + VTD_TIMEOUT_NS;
    while ((r64(u, VTD_REG_IQH) >> 4) != (r64(u, VTD_REG_IQT) >> 4) && ktime_ns() < dl) cpu_relax_poll();
    int rc = gcmd(u, VTD_GCMD_QIE, 0);
    iommu_ln_s(&l, rc ? "handoff: firmware queued invalidation would not stop"
                      : "handoff: firmware queued invalidation drained and disabled");
    logline(&l);
}

static int unit_setup(vtd_unit_t* u) {
    iommu_line_t l;
    u->root = vtd_table_alloc();
    if (!u->root) return -1;
    handoff_qi(u);
    u->use_qi = 0;
    if ((u->ecap & VTD_ECAP_QI) && !g_force_no_qi) {
        u->iq = vtd_table_alloc();
        u->stat_phys = vtd_table_alloc();
        if (u->iq && u->stat_phys) {
            u->stat = (volatile uint32_t*)phys_to_virt(u->stat_phys);
            u->iq_tail = 0;
            w64(u, VTD_REG_IQT, 0);
            w64(u, VTD_REG_IQA, u->iq);          /* QS=0: 256 entries, 128-bit */
            if (gcmd(u, VTD_GCMD_QIE, 1) == 0) u->use_qi = 1;
        }
        if (!u->use_qi) {
            ln_begin(&l, u); iommu_ln_s(&l, "queued invalidation failed to start: register-based"); logline(&l);
        }
    }
    if (setup_fault_irq(u)) {
        ln_begin(&l, u); iommu_ln_s(&l, "no fault-event vector: faults only logged when polled"); logline(&l);
    }
    /* Stale fault records left by the firmware. */
    drain_report(u, 0);
    return 0;
}

/* Switch to our root table and turn translation on (firmware handoff part 2:
 * a unit already translating keeps TE on across the switch). */
static int unit_enable(vtd_unit_t* u) {
    iommu_line_t l;
    uint64_t f = spin_lock_irqsave(&u->lock);
    w64(u, VTD_REG_RTADDR, u->root);                         /* legacy mode */
    w32(u, VTD_REG_GCMD, (r32(u, VTD_REG_GSTS) & VTD_GSTS_PERSIST) | VTD_GCMD_SRTP);
    int rc = wait_gsts(u, VTD_GSTS_RTPS, VTD_GSTS_RTPS);
    if (rc == 0) {
        inv(u, INV_CC_GLOBAL, 0, 0, 0, 0);
        inv(u, INV_IOTLB_GLOBAL, 0, 0, 0, 0);
        rc = inv_sync(u);
        write_buffer_flush(u);
    }
    if (rc == 0 && u->vec >= 0) w32(u, VTD_REG_FECTL, 0);   /* unmask fault events */
    if (rc == 0 && !u->fw_te) rc = gcmd(u, VTD_GCMD_TE, 1);
    spin_unlock_irqrestore(&u->lock, f);
    ln_begin(&l, u);
    if (rc) { iommu_ln_s(&l, "FAILED to enable translation"); logline(&l); return -1; }
    iommu_ln_s(&l, u->fw_te ? "handoff: root table switched with translation on"
                            : "translation enabled");
    iommu_ln_s(&l, u->use_qi ? ", queued invalidation" : ", register invalidation");
    iommu_ln_s(&l, ", fault vector ");
    if (u->vec >= 0) iommu_ln_hex(&l, (uint64_t)u->vec); else iommu_ln_s(&l, "none");
    logline(&l);
    /* Protected memory regions (DMA-protected boot): off once we translate. */
    if (u->cap & (VTD_CAP_PLMR | VTD_CAP_PHMR)) {
        uint32_t pm = r32(u, VTD_REG_PMEN);
        if (pm & (VTD_PMEN_EPM | VTD_PMEN_PRS)) {
            w32(u, VTD_REG_PMEN, pm & ~VTD_PMEN_EPM);
            uint64_t dl = ktime_ns() + VTD_TIMEOUT_NS;
            while ((r32(u, VTD_REG_PMEN) & VTD_PMEN_PRS) && ktime_ns() < dl) cpu_relax_poll();
            ln_begin(&l, u);
            iommu_ln_s(&l, (r32(u, VTD_REG_PMEN) & VTD_PMEN_PRS) ? "handoff: protected memory regions would not turn off"
                                                                 : "handoff: protected memory regions disabled");
            logline(&l);
        }
    }
    u->active = 1;
    return 0;
}

static int vtd_enable(void) {
    int ok = 0;
    for (int i = 0; i < g_nunits; i++)
        if (g_unit[i].used && unit_enable(&g_unit[i]) == 0) ok++;
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------ /proc */
static void vtd_generate(void (*emit)(void* ctx, const char* line), void* ctx) {
    iommu_line_t l;
    for (int i = 0; i < g_nunits; i++) {
        vtd_unit_t* u = &g_unit[i];
        if (!u->used) continue;
        l.n = 0;
        iommu_ln_s(&l, "unit "); iommu_ln_dec(&l, u->drhd_idx);
        iommu_ln_s(&l, " base "); iommu_ln_hex(&l, u->base);
        iommu_ln_s(&l, u->active ? " translating" : " off");
        iommu_ln_s(&l, " ver "); iommu_ln_dec(&l, (u->ver >> 4) & 0xF); iommu_ln_s(&l, ".");
        iommu_ln_dec(&l, u->ver & 0xF);
        iommu_ln_s(&l, " cap "); iommu_ln_hex(&l, u->cap);
        iommu_ln_s(&l, " ecap "); iommu_ln_hex(&l, u->ecap);
        emit(ctx, iommu_ln_end(&l));
        l.n = 0;
        iommu_ln_s(&l, "unit "); iommu_ln_dec(&l, u->drhd_idx);
        iommu_ln_s(&l, " levels "); iommu_ln_dec(&l, (uint64_t)g_levels);
        iommu_ln_s(&l, " cm "); iommu_ln_dec(&l, u->cm);
        iommu_ln_s(&l, " coherent "); iommu_ln_dec(&l, u->coherent);
        iommu_ln_s(&l, " invalidation "); iommu_ln_s(&l, u->use_qi ? "queued" : "register");
        iommu_ln_s(&l, " fault-vector ");
        if (u->vec >= 0) iommu_ln_hex(&l, (uint64_t)u->vec); else iommu_ln_s(&l, "none");
        emit(ctx, iommu_ln_end(&l));
        l.n = 0;
        iommu_ln_s(&l, "unit "); iommu_ln_dec(&l, u->drhd_idx);
        iommu_ln_s(&l, " inv cc "); iommu_ln_dec(&l, u->n_cc);
        iommu_ln_s(&l, " iotlb "); iommu_ln_dec(&l, u->n_iotlb);
        iommu_ln_s(&l, " (psi "); iommu_ln_dec(&l, u->n_psi);
        iommu_ln_s(&l, " dsi "); iommu_ln_dec(&l, u->n_dsi);
        iommu_ln_s(&l, ") waits "); iommu_ln_dec(&l, u->n_wait);
        iommu_ln_s(&l, " wbf "); iommu_ln_dec(&l, u->n_wbf);
        iommu_ln_s(&l, " timeouts "); iommu_ln_dec(&l, u->n_timeout);
        iommu_ln_s(&l, " iq-errors "); iommu_ln_dec(&l, u->n_iqe);
        iommu_ln_s(&l, " ctx set "); iommu_ln_dec(&l, u->n_ctx_set);
        iommu_ln_s(&l, " cleared "); iommu_ln_dec(&l, u->n_ctx_clear);
        emit(ctx, iommu_ln_end(&l));
    }
}

static const iommu_ops_t g_vtd_ops = {
    .name = "vt-d",
    .dom_init = vtd_dom_init,
    .dom_fini = vtd_dom_fini,
    .map = vtd_map,
    .unmap = vtd_unmap,
    .translate = vtd_translate,
    .set_context = vtd_set_context,
    .covers = vtd_covers,
    .max_iova = vtd_max_iova,
    .irq_compose = vtd_irq_compose,
    .fault_reason = vtd_fault_reason,
    .fault_poll = vtd_fault_poll,
    .enable = vtd_enable,
    .generate = vtd_generate,
    .ir_init = vtd_ir_init,
    .ir_enabled = vtd_ir_enabled,
    .ir_covers = vtd_ir_covers,
    .ir_free = vtd_ir_free,
    .ir_set_cpu = vtd_ir_set_cpu,
    .ir_generate = vtd_ir_generate,
};

/* Probe every usable DRHD, pick the common page-table format, set the
 * units up (root table, queue, fault vector; translation still off) and
 * register the ops. kernel/iommu.c then attaches the devices and calls
 * ops->enable(). */
void vtd_probe(void) {
    const acpi_dmar_t* dm = acpi_dmar_get();
    iommu_line_t l;
    g_force_no_qi = iommu_cmdline_has("iommu.qi=off");
    g_force_no_pt = iommu_cmdline_has("iommu.pt=off");
    if (dm->checksum_bad || dm->haw_bad || dm->truncated) {
        ln_begin(&l, NULL);
        iommu_ln_s(&l, "DMAR table unreliable (");
        iommu_ln_s(&l, dm->checksum_bad ? "checksum " : "");
        iommu_ln_s(&l, dm->haw_bad ? "haw " : "");
        iommu_ln_s(&l, dm->truncated ? "truncated" : "");
        iommu_ln_s(&l, "): DMA remapping not enabled");
        logline(&l);
        return;
    }
    uint16_t haw = dm->haw;
    g_vtd_table_limit = haw >= 52 ? ~0ull : (1ull << haw);
    uint32_t sagaw_all = 0x1F, sllps_all = 0xF, mgaw_min = 64;
    int all_pt = 1, clflush = 0;
    g_nunits = 0;
    for (uint32_t i = 0; i < dm->drhd_count && i < DMAR_MAX_DRHD; i++) {
        const dmar_drhd_t* r = &dm->drhd[i];
        if (!r->valid) continue;
        ln_begin(&l, NULL);
        if (r->scopes_incomplete) {
            iommu_ln_s(&l, "DRHD "); iommu_ln_dec(&l, i);
            iommu_ln_s(&l, ": device scopes not fully parsed: unit left untouched");
            logline(&l); continue;
        }
        if (r->gfx_only) {
            iommu_ln_s(&l, "DRHD "); iommu_ln_dec(&l, i);
            iommu_ln_s(&l, ": graphics-only unit: left untouched"); logline(&l); continue;
        }
        if (r->segment != 0) {
            iommu_ln_s(&l, "DRHD "); iommu_ln_dec(&l, i);
            iommu_ln_s(&l, ": PCI segment != 0 not supported: left untouched"); logline(&l); continue;
        }
        vtd_unit_t* u = &g_unit[g_nunits];
        { uint8_t* z = (uint8_t*)u; for (uint64_t k = 0; k < sizeof(*u); k++) z[k] = 0; }
        if (unit_probe(u, r, i, haw)) { u->regs = 0; continue; }
        sagaw_all &= VTD_CAP_SAGAW(u->cap);
        sllps_all &= VTD_CAP_SLLPS(u->cap);
        if (VTD_CAP_MGAW(u->cap) < mgaw_min) mgaw_min = VTD_CAP_MGAW(u->cap);
        if (!u->pt) all_pt = 0;
        if (!u->coherent) clflush = 1;
        if (u->nd_domains < g_did_limit) g_did_limit = u->nd_domains;
        u->used = 1;
        g_nunits++;
    }
    if (!g_nunits) { ln_begin(&l, NULL); iommu_ln_s(&l, "no usable DMA-remapping unit"); logline(&l); return; }
    if (sagaw_all & 0x4) g_levels = 4;            /* 48-bit, 4-level */
    else if (sagaw_all & 0x2) g_levels = 3;       /* 39-bit, 3-level */
    else { ln_begin(&l, NULL); iommu_ln_s(&l, "units share no AGAW: not enabled"); logline(&l);
           g_nunits = 0; return; }
    uint32_t agaw_bits = g_levels == 4 ? 48 : 39;
    if (mgaw_min < agaw_bits) agaw_bits = mgaw_min;
    g_max_iova = (1ull << agaw_bits) - 1;
    g_huge = sllps_all & 3;
    g_all_pt = all_pt && !g_force_no_pt;
    g_vtd_clflush = clflush;
    for (int i = 0; i < g_nunits; i++) {
        if (unit_setup(&g_unit[i])) {
            ln_begin(&l, &g_unit[i]); iommu_ln_s(&l, "setup failed: unit not used"); logline(&l);
            g_unit[i].used = 0;
        }
    }
    int any = 0;
    for (int i = 0; i < g_nunits; i++) any |= g_unit[i].used;
    if (!any) return;
    ln_begin(&l, NULL);
    iommu_ln_dec(&l, (uint64_t)g_nunits); iommu_ln_s(&l, " unit(s), ");
    iommu_ln_dec(&l, (uint64_t)g_levels); iommu_ln_s(&l, "-level tables, iova bits ");
    iommu_ln_dec(&l, agaw_bits);
    iommu_ln_s(&l, ", kernel domain ");
    iommu_ln_s(&l, g_all_pt ? "passthrough (TT=10b)" : "identity tables");
    iommu_ln_s(&l, g_force_no_pt ? " (iommu.pt=off)" : "");
    iommu_ln_s(&l, ", superpages "); iommu_ln_s(&l, (g_huge & 2) ? "1G+2M" : (g_huge & 1) ? "2M" : "none");
    iommu_ln_s(&l, g_force_no_qi ? ", iommu.qi=off" : "");
    logline(&l);
    iommu_register_ops(&g_vtd_ops);
}
