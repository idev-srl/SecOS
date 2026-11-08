/*
 * pci_msi.c — MSI / MSI-X programming (M53c). See pci_msi.h.
 *
 * Moved from drivers/pci.c (pci_enable_msix / pci_enable_msi, which programmed
 * MSI-X entry 0 / one MSI message to LAPIC ID 0 at a fixed vector) and
 * generalized: any entry, any CPU, per-entry masking, multi-message MSI with
 * per-vector masking.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pci_msi.h"
#include "irq.h"
#include "iommu.h"
#include "percpu.h"
#include "spinlock.h"
#include "io.h"
#include "vmm.h"
#include "debugcon.h"
#include <stddef.h>

/* MSI-X capability. */
#define MSIX_MC_ENABLE      (1u << 15)
#define MSIX_MC_FMASK       (1u << 14)
#define MSIX_MC_SIZE(mc)    (((mc) & 0x7FFu) + 1u)
#define MSIX_ENTRY_SIZE     16u
#define MSIX_VCTRL_MASK     1u

/* MSI capability. */
#define MSI_MC_ENABLE       (1u << 0)
#define MSI_MC_MMC(mc)      (((mc) >> 1) & 7u)     /* log2 of messages capable */
#define MSI_MC_MME_SHIFT    4
#define MSI_MC_MME_MASK     (7u << MSI_MC_MME_SHIFT)
#define MSI_MC_64BIT        (1u << 7)
#define MSI_MC_PVM          (1u << 8)

#define MSI_MAX_DEV 16

typedef struct msi_rec {
    volatile uint8_t used;          /* published last; BDF immutable after   */
    uint8_t bus, slot, func;
    uint8_t msix_cap, msi_cap;
    uint16_t msix_size;             /* table entries                          */
    volatile uint32_t* msix_tbl;    /* kernel mapping (UC), never unmapped    */
    uint8_t msix_on;
    uint8_t msi_on, msi_nvec;
} msi_rec_t;

static msi_rec_t g_msi[MSI_MAX_DEV];
static spinlock_t g_msi_lock = SPINLOCK_INIT_CLASS(LK_S58_MSI, "msi");

static inline uint16_t cfg16(const pci_device_t* d, uint8_t off) {
    return pci_config_read16(d->bus, d->slot, d->func, off);
}
static inline uint32_t cfg32(const pci_device_t* d, uint8_t off) {
    return pci_config_read32(d->bus, d->slot, d->func, off);
}
static inline void wcfg16(const pci_device_t* d, uint8_t off, uint16_t v) {
    pci_config_write16(d->bus, d->slot, d->func, off, v);
}
static inline void wcfg32(const pci_device_t* d, uint8_t off, uint32_t v) {
    pci_config_write32(d->bus, d->slot, d->func, off, v);
}

/* Lock-free lookup (records are never removed). */
static msi_rec_t* rec_find(const pci_device_t* d) {
    for (int i = 0; i < MSI_MAX_DEV; i++) {
        msi_rec_t* r = &g_msi[i];
        if (__atomic_load_n(&r->used, __ATOMIC_ACQUIRE) &&
            r->bus == d->bus && r->slot == d->slot && r->func == d->func)
            return r;
    }
    return NULL;
}

/* Find or create; caller holds g_msi_lock. */
static msi_rec_t* rec_get_locked(const pci_device_t* d) {
    msi_rec_t* r = rec_find(d);
    if (r) return r;
    for (int i = 0; i < MSI_MAX_DEV; i++) {
        r = &g_msi[i];
        if (r->used) continue;
        r->bus = d->bus; r->slot = d->slot; r->func = d->func;
        r->msix_cap = pci_find_capability(d, PCI_CAP_ID_MSIX);
        r->msi_cap = pci_find_capability(d, PCI_CAP_ID_MSI);
        r->msix_size = 0; r->msix_tbl = NULL; r->msix_on = 0;
        r->msi_on = 0; r->msi_nvec = 0;
        __atomic_store_n(&r->used, 1, __ATOMIC_RELEASE);
        return r;
    }
    debugcon_writestring("[MSI] device table full\n");
    return NULL;
}

/* ======================================================================== */
int msi_compose_compat(const pci_device_t* d, uint8_t vec, uint32_t cpu,
                       uint64_t* addr, uint32_t* data) {
    (void)d;
    if (vec < 0x20 || vec == 0xFF) return -1;
    int c = irq_pick_cpu(cpu);
    if (c < 0) return -1;
    cpu_t* cp = cpu_by_index((uint32_t)c);
    if (!cp) return -1;
    /* Compatibility format: 8-bit physical destination in address bits 19:12;
     * 0xFF is the broadcast ID in both xAPIC and x2APIC modes. */
    if (cp->lapic_id > 0xFE) return -1;
    *addr = (uint64_t)MSI_ADDR_BASE | ((uint64_t)cp->lapic_id << 12);
    *data = (uint32_t)vec;          /* fixed delivery, edge trigger */
    return 0;
}

/* [M56] With interrupt remapping on, the message is in remappable format:
 * the IOMMU programs the interrupt-remapping entry for `vec` (owner = the
 * device's requester id, destination = `cpu`) and returns its handle. A
 * device no remapping unit covers keeps the compatibility format. */
int msi_compose(const pci_device_t* d, uint8_t vec, uint32_t cpu,
                uint64_t* addr, uint32_t* data) {
    int rc = iommu_ir_msi_compose(d, vec, cpu, addr, data);
    if (rc <= 0) return rc;
    return msi_compose_compat(d, vec, cpu, addr, data);
}

int pci_msi_any_enabled(void) {
    int n = 0;
    for (int i = 0; i < MSI_MAX_DEV; i++) {
        msi_rec_t* r = &g_msi[i];
        if (__atomic_load_n(&r->used, __ATOMIC_ACQUIRE) && (r->msix_on || r->msi_on)) n++;
    }
    return n;
}

/* ======================================================================== MSI-X */
/* Read the MSI-X geometry; -1 on a missing cap or a table outside its BAR. */
static int msix_geometry(const pci_device_t* d, uint8_t cap, uint16_t* size, uint64_t* phys) {
    uint16_t mc = cfg16(d, (uint8_t)(cap + 2));
    uint32_t tbl = cfg32(d, (uint8_t)(cap + 4));
    uint32_t bir = tbl & 7u;
    uint64_t off = tbl & ~7u;
    if (bir > 5) return -1;
    uint64_t bar = pci_bar_mem64(d, (int)bir);
    if (bar == 0) return -1;
    /* [M53a] The BAR size comes from the boot registry (sized once, before any
     * driver ran): never re-probe a live BAR whose MSI-X table may be in use.
     * A BAR the registry does not know at this base is refused; only when the
     * registry overflowed (function not recorded) is the BAR sized here. */
    uint64_t barsz = 0;
    const pci_dev_t* r = pci_dev_of(d);
    if (r) {
        if (bir < r->nbars && (r->bar_flags[bir] & PCI_BAR_PRESENT) &&
            !(r->bar_flags[bir] & (PCI_BAR_IO | PCI_BAR_UPPER)) && r->bar_base[bir] == bar)
            barsz = r->bar_size[bir];
    } else if (!pci_registry_complete()) {
        barsz = pci_bar_size(d, (int)bir);
    }
    uint64_t bytes = (uint64_t)MSIX_MC_SIZE(mc) * MSIX_ENTRY_SIZE;
    if (barsz == 0 || off >= barsz || bytes > barsz - off) {
        debugcon_writestring("[MSIX] table outside its BAR, refused\n");
        return -1;
    }
    *size = (uint16_t)MSIX_MC_SIZE(mc);
    *phys = bar + off;
    return 0;
}

int pci_msix_table_size(const pci_device_t* d) {
    uint8_t cap = pci_find_capability(d, PCI_CAP_ID_MSIX);
    if (!cap) return -1;
    return (int)MSIX_MC_SIZE(cfg16(d, (uint8_t)(cap + 2)));
}

int pci_msix_enable(const pci_device_t* d, int nvec) {
    uint8_t cap = pci_find_capability(d, PCI_CAP_ID_MSIX);
    if (!cap) return -1;
    uint16_t size; uint64_t phys;
    if (msix_geometry(d, cap, &size, &phys) != 0) return -1;
    if (nvec < 1 || nvec > (int)size) return -1;
    /* Map outside the lock (the physmap lock ranks above g_msi_lock). The
     * mapping is idempotent and stays for the device's lifetime. */
    volatile uint32_t* tbl = (volatile uint32_t*)vmm_map_mmio(phys, (uint64_t)size * MSIX_ENTRY_SIZE,
                                                             VMM_CACHE_UC);
    if (!tbl) return -1;

    uint64_t fl = spin_lock_irqsave(&g_msi_lock);
    msi_rec_t* r = rec_get_locked(d);
    if (!r) { spin_unlock_irqrestore(&g_msi_lock, fl); return -1; }
    /* MSI and MSI-X must never be on together. */
    if (r->msi_cap) {
        uint16_t mmc = cfg16(d, (uint8_t)(r->msi_cap + 2));
        if (mmc & MSI_MC_ENABLE) wcfg16(d, (uint8_t)(r->msi_cap + 2), (uint16_t)(mmc & ~MSI_MC_ENABLE));
        r->msi_on = 0;
    }
    /* Enable with the function masked, mask every entry, then unmask the
     * function: no entry can fire before the driver programs and unmasks it. */
    uint16_t mc = cfg16(d, (uint8_t)(cap + 2));
    wcfg16(d, (uint8_t)(cap + 2), (uint16_t)(mc | MSIX_MC_ENABLE | MSIX_MC_FMASK));
    for (uint32_t i = 0; i < size; i++) {
        volatile uint32_t* e = tbl + i * 4;
        e[3] = e[3] | MSIX_VCTRL_MASK;
    }
    (void)tbl[3];                                    /* flush posted writes */
    r->msix_size = size;
    __atomic_store_n(&r->msix_tbl, tbl, __ATOMIC_RELEASE);
    r->msix_on = 1;
    mc = cfg16(d, (uint8_t)(cap + 2));
    wcfg16(d, (uint8_t)(cap + 2), (uint16_t)((mc | MSIX_MC_ENABLE) & ~MSIX_MC_FMASK));
    spin_unlock_irqrestore(&g_msi_lock, fl);

    debugcon_writestring("[MSIX] enabled cap=0x"); debugcon_print_hex(cap);
    debugcon_writestring(" entries="); debugcon_print_hex(size);
    debugcon_writestring(" tblphys=0x"); debugcon_print_hex(phys);
    debugcon_writestring("\n");
    return 0;
}

static volatile uint32_t* msix_entry(const pci_device_t* d, int entry) {
    msi_rec_t* r = rec_find(d);
    if (!r || !r->msix_on || entry < 0 || entry >= (int)r->msix_size) return NULL;
    volatile uint32_t* t = __atomic_load_n(&r->msix_tbl, __ATOMIC_ACQUIRE);
    return t ? t + (uint32_t)entry * 4 : NULL;
}

int pci_msix_set(const pci_device_t* d, int entry, uint8_t vec, uint32_t cpu) {
    uint64_t addr; uint32_t data;
    if (msi_compose(d, vec, cpu, &addr, &data) != 0) return -1;
    uint64_t fl = spin_lock_irqsave(&g_msi_lock);
    volatile uint32_t* e = msix_entry(d, entry);
    if (!e) { spin_unlock_irqrestore(&g_msi_lock, fl); return -1; }
    /* Rewrite the entry masked (a half-written address/data pair must never
     * be sent), then restore its mask state. */
    uint32_t vc = e[3];
    e[3] = vc | MSIX_VCTRL_MASK;
    e[0] = (uint32_t)addr;
    e[1] = (uint32_t)(addr >> 32);
    e[2] = data;
    io_mfence();
    e[3] = vc;
    (void)e[3];
    spin_unlock_irqrestore(&g_msi_lock, fl);
    return 0;
}

int pci_msix_mask(const pci_device_t* d, int entry) {
    volatile uint32_t* e = msix_entry(d, entry);
    if (!e) return -1;
    e[3] = e[3] | MSIX_VCTRL_MASK;
    (void)e[3];              /* the read completes the masking write */
    return 0;
}

int pci_msix_unmask(const pci_device_t* d, int entry) {
    volatile uint32_t* e = msix_entry(d, entry);
    if (!e) return -1;
    e[3] = e[3] & ~MSIX_VCTRL_MASK;   /* a latched pending bit fires now */
    (void)e[3];
    return 0;
}

int pci_msix_disable(const pci_device_t* d) {
    uint64_t fl = spin_lock_irqsave(&g_msi_lock);
    msi_rec_t* r = rec_find(d);
    if (!r || !r->msix_on || !r->msix_cap) { spin_unlock_irqrestore(&g_msi_lock, fl); return -1; }
    for (uint32_t i = 0; i < r->msix_size; i++) {
        volatile uint32_t* e = r->msix_tbl + i * 4;
        e[3] = e[3] | MSIX_VCTRL_MASK;
    }
    (void)r->msix_tbl[3];
    uint16_t mc = cfg16(d, (uint8_t)(r->msix_cap + 2));
    wcfg16(d, (uint8_t)(r->msix_cap + 2), (uint16_t)(mc & ~MSIX_MC_ENABLE));
    r->msix_on = 0;
    spin_unlock_irqrestore(&g_msi_lock, fl);
    return 0;
}

/* ======================================================================== MSI */
/* Offsets inside the MSI capability (they depend on the 64-bit bit). */
static inline uint8_t msi_data_off(uint8_t cap, uint16_t mc) { return (uint8_t)(cap + ((mc & MSI_MC_64BIT) ? 0x0C : 0x08)); }
static inline uint8_t msi_mask_off(uint8_t cap, uint16_t mc) { return (uint8_t)(cap + ((mc & MSI_MC_64BIT) ? 0x10 : 0x0C)); }

int pci_msi_max_vectors(const pci_device_t* d) {
    uint8_t cap = pci_find_capability(d, PCI_CAP_ID_MSI);
    if (!cap) return -1;
    uint32_t mmc = MSI_MC_MMC(cfg16(d, (uint8_t)(cap + 2)));
    if (mmc > 5) mmc = 5;                 /* 6, 7 are reserved encodings */
    return 1 << mmc;
}

int pci_msi_has_pvm(const pci_device_t* d) {
    uint8_t cap = pci_find_capability(d, PCI_CAP_ID_MSI);
    if (!cap) return -1;
    return (cfg16(d, (uint8_t)(cap + 2)) & MSI_MC_PVM) ? 1 : 0;
}

int pci_msi_enable(const pci_device_t* d, int nvec, uint8_t vec, uint32_t cpu) {
    int maxv = pci_msi_max_vectors(d);
    if (maxv < 0 || nvec < 1 || nvec > maxv || (nvec & (nvec - 1))) return -1;
    if (vec & (uint32_t)(nvec - 1)) return -1;       /* block must be aligned */
    uint64_t addr; uint32_t data;
    if (msi_compose(d, vec, cpu, &addr, &data) != 0) return -1;
    /* [M56] Multi-message: the device ORs the message number into the low
     * data bits, which a remappable message (SHV=1, subhandle 0) turns into
     * entry handle + i. Program entries vec+1 .. vec+nvec-1 too (with IR
     * off this only re-checks the target). */
    for (int i = 1; i < nvec; i++) {
        uint64_t a2; uint32_t d2;
        if (msi_compose(d, (uint8_t)(vec + i), cpu, &a2, &d2) != 0) return -1;
    }
    uint32_t mme = 0;
    while ((1 << mme) < nvec) mme++;

    uint64_t fl = spin_lock_irqsave(&g_msi_lock);
    msi_rec_t* r = rec_get_locked(d);
    if (!r || !r->msi_cap) { spin_unlock_irqrestore(&g_msi_lock, fl); return -1; }
    uint8_t cap = r->msi_cap;
    if (r->msix_on && r->msix_cap) {                  /* never both on */
        uint16_t xmc = cfg16(d, (uint8_t)(r->msix_cap + 2));
        wcfg16(d, (uint8_t)(r->msix_cap + 2), (uint16_t)(xmc & ~MSIX_MC_ENABLE));
        r->msix_on = 0;
    }
    uint16_t mc = cfg16(d, (uint8_t)(cap + 2));
    wcfg16(d, (uint8_t)(cap + 2), (uint16_t)(mc & ~MSI_MC_ENABLE));   /* off while rewriting */
    wcfg32(d, (uint8_t)(cap + 4), (uint32_t)addr);
    if (mc & MSI_MC_64BIT) wcfg32(d, (uint8_t)(cap + 8), (uint32_t)(addr >> 32));
    wcfg16(d, msi_data_off(cap, mc), (uint16_t)data);
    if (mc & MSI_MC_PVM) {
        /* Enabled messages start unmasked (as before M53c); the rest masked. */
        uint32_t m = (nvec >= 32) ? 0u : ~((1u << nvec) - 1u);
        wcfg32(d, msi_mask_off(cap, mc), m);
    }
    mc = (uint16_t)((mc & ~MSI_MC_MME_MASK) | (mme << MSI_MC_MME_SHIFT) | MSI_MC_ENABLE);
    wcfg16(d, (uint8_t)(cap + 2), mc);
    r->msi_on = 1;
    r->msi_nvec = (uint8_t)nvec;
    spin_unlock_irqrestore(&g_msi_lock, fl);

    debugcon_writestring("[MSI] enabled cap=0x"); debugcon_print_hex(cap);
    debugcon_writestring(" nvec="); debugcon_print_hex((uint64_t)nvec);
    debugcon_writestring((mc & MSI_MC_PVM) ? " pvm=1\n" : " pvm=0\n");
    return 0;
}

static int msi_mask_bit(const pci_device_t* d, int idx, int set) {
    uint64_t fl = spin_lock_irqsave(&g_msi_lock);
    msi_rec_t* r = rec_find(d);
    int rc = -1;
    if (r && r->msi_cap && r->msi_on && idx >= 0 && idx < (int)r->msi_nvec) {
        uint16_t mc = cfg16(d, (uint8_t)(r->msi_cap + 2));
        if (mc & MSI_MC_PVM) {
            uint8_t off = msi_mask_off(r->msi_cap, mc);
            uint32_t m = cfg32(d, off);
            m = set ? (m | (1u << idx)) : (m & ~(1u << idx));
            wcfg32(d, off, m);
            rc = 0;
        }
    }
    spin_unlock_irqrestore(&g_msi_lock, fl);
    return rc;
}

int pci_msi_mask(const pci_device_t* d, int idx)   { return msi_mask_bit(d, idx, 1); }
int pci_msi_unmask(const pci_device_t* d, int idx) { return msi_mask_bit(d, idx, 0); }

int pci_msi_disable(const pci_device_t* d) {
    uint64_t fl = spin_lock_irqsave(&g_msi_lock);
    msi_rec_t* r = rec_find(d);
    int rc = -1;
    if (r && r->msi_cap) {
        uint16_t mc = cfg16(d, (uint8_t)(r->msi_cap + 2));
        wcfg16(d, (uint8_t)(r->msi_cap + 2), (uint16_t)(mc & ~MSI_MC_ENABLE));
        r->msi_on = 0;
        rc = 0;
    }
    spin_unlock_irqrestore(&g_msi_lock, fl);
    return rc;
}

/* ======================================================================== */
int pci_irq_setup_single(const pci_device_t* d, uint8_t vec, uint32_t cpu) {
    uint64_t a; uint32_t dt;
    if (msi_compose(d, vec, cpu, &a, &dt) != 0) return -1;
    if (pci_msix_enable(d, 1) == 0) {
        if (pci_msix_set(d, 0, vec, cpu) == 0 && pci_msix_unmask(d, 0) == 0) return 1;
        pci_msix_disable(d);
        return -1;
    }
    if (pci_msi_enable(d, 1, vec, cpu) == 0) return 2;
    return -1;
}
