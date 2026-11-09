/*
 * dma.c — the kernel DMA API and the strict kernel DMA domain (Phase 5, M55).
 * See dma.h for the contract.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dma.h"
#include "iommu.h"
#include "acpi_dmar.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "spinlock.h"
#include "debugcon.h"
#include "panic.h"
#include <stddef.h>

#define DMA_PAGE        4096ull
#define DMA_MAX_MAPS    64

/* .dmabss, placed by linker.ld inside .bss (input section .bss.dma). */
extern uint8_t __dma_start[], __dma_end[];

struct dma_dev {
    const pci_dev_t* pd;
    uint64_t mask;
    uint8_t  used;
    uint8_t  strict;       /* attached to the strict kernel domain          */
};

typedef struct {
    uint64_t kva;          /* page-aligned kernel virtual base              */
    uint64_t phys;         /* == bus address (identity)                     */
    uint64_t pages;
    uint8_t  kind;         /* 0 free, 1 coherent, 2 streaming               */
    uint8_t  mapped;       /* in the strict domain                          */
} dma_map_t;
#define DMA_K_COHERENT  1
#define DMA_K_STREAM    2

static spinlock_t       g_dma_lock = SPINLOCK_INIT_CLASS(LK_S63_DMA, "dma");
static struct dma_dev   g_dd[PCI_MAX_DEVS];
static struct dma_dev   g_dd_none = { NULL, DMA_MASK_64, 1, 0 };
static dma_map_t        g_maps[DMA_MAX_MAPS];

static uint64_t         g_dma_kva, g_dma_end, g_dma_phys;   /* .dmabss */
static iommu_domain_t*  g_strict;     /* the strict kernel DMA domain (or NULL) */
static const char*      g_mode = "off";
static int              g_inited;   /* region bounds computed */
static volatile uint64_t g_violations, g_logged;
static volatile uint32_t g_ndev, g_nstrict;
static volatile uint64_t g_coh_pages, g_coh_allocs, g_stream_maps;

/* ------------------------------------------------------------ helpers */
static void region_init(void);
static void ln_dev(iommu_line_t* l, const dma_dev_t* dd) {
    if (dd && dd->pd) {
        iommu_ln_bdf(l, DMAR_BDF(dd->pd->bus, dd->pd->slot, dd->pd->func));
        iommu_ln_s(l, " "); iommu_ln_hexw(l, dd->pd->vendor_id, 4);
        iommu_ln_s(l, ":"); iommu_ln_hexw(l, dd->pd->device_id, 4);
    } else {
        iommu_ln_s(l, "(no pci function)");
    }
}

static int in_dmabss(uint64_t v, uint64_t len) {
    return v >= g_dma_kva && len <= g_dma_end - g_dma_kva && v - g_dma_kva <= (g_dma_end - g_dma_kva) - len;
}

static int fits_mask(const dma_dev_t* dd, uint64_t bus, uint64_t len) {
    uint64_t m = dd ? dd->mask : DMA_MASK_64;
    if (len == 0) len = 1;
    if (bus > m) return 0;
    return len - 1 <= m - bus;
}

/* Bus address of [v, v+len) in a live mapping (lock held), or DMA_ADDR_ERROR. */
static dma_addr_t map_lookup_locked(uint64_t v, uint64_t len) {
    for (int i = 0; i < DMA_MAX_MAPS; i++) {
        const dma_map_t* m = &g_maps[i];
        if (!m->kind) continue;
        uint64_t size = m->pages * DMA_PAGE;
        if (v >= m->kva && len <= size && v - m->kva <= size - len)
            return m->phys + (v - m->kva);
    }
    return DMA_ADDR_ERROR;
}

/* Bus address if reachable (no mask check), else DMA_ADDR_ERROR. */
static dma_addr_t bus_of(const void* kva, size_t len) {
    region_init();
    uint64_t v = (uint64_t)(uintptr_t)kva, n = len ? len : 1;
    if (in_dmabss(v, n)) return g_dma_phys + (v - g_dma_kva);
    uint64_t f = spin_lock_irqsave(&g_dma_lock);
    dma_addr_t a = map_lookup_locked(v, n);
    spin_unlock_irqrestore(&g_dma_lock, f);
    return a;
}

static void violation(const dma_dev_t* dd, const void* kva, size_t len, const char* why, void* caller) {
    uint64_t n = __atomic_add_fetch(&g_violations, 1, __ATOMIC_RELAXED);
    if (n <= 16) {
        __atomic_add_fetch(&g_logged, 1, __ATOMIC_RELAXED);
        iommu_line_t l; l.n = 0;
        iommu_ln_s(&l, "[DMA] BUG: "); iommu_ln_s(&l, why);
        iommu_ln_s(&l, " dev "); ln_dev(&l, dd);
        iommu_ln_s(&l, " kva "); iommu_ln_hex(&l, (uint64_t)(uintptr_t)kva);
        iommu_ln_s(&l, " len "); iommu_ln_hex(&l, len);
        iommu_ln_s(&l, " caller "); iommu_ln_hex(&l, (uint64_t)(uintptr_t)caller);
        debugcon_writestring(iommu_ln_end(&l));
    }
#ifdef SECOS_DMA_DEBUG
    PANIC("SECOS_DMA_DEBUG: DMA address outside the DMA regions");
#endif
}

/* ------------------------------------------------------------ boot */
static int strict_selftest(void) {
    /* One coherent page: mapped while alive, faulting (unmapped) once freed. */
    dma_addr_t a = 0;
    void* p = dma_alloc_coherent(&g_dd_none, 8192, &a, DMA_F_NONE);
    if (!p) return -1;
    int prot = 0, ok = 1;
    if (iommu_translate(g_strict, a, &prot) != a || iommu_translate(g_strict, a + 4096, &prot) != a + 4096 ||
        prot != (IOMMU_READ | IOMMU_WRITE)) ok = 0;
    for (int i = 0; i < 8192 && ok; i++) if (((volatile uint8_t*)p)[i]) ok = 0;
    if (dma_static(&g_dd_none, (uint8_t*)p + 100, 16) != a + 100) ok = 0;
    dma_free_coherent(&g_dd_none, p, 8192, a);
    if (iommu_translate(g_strict, a, &prot) != (uint64_t)-1) ok = 0;
    /* .dmabss start and end page are mapped, the page after it is not. */
    if (iommu_translate(g_strict, g_dma_phys, &prot) != g_dma_phys) ok = 0;
    if (g_dma_end > g_dma_kva &&
        iommu_translate(g_strict, g_dma_phys + (g_dma_end - g_dma_kva) - 1, &prot) !=
            g_dma_phys + (g_dma_end - g_dma_kva) - 1) ok = 0;
    if (iommu_translate(g_strict, g_dma_phys + (g_dma_end - g_dma_kva), &prot) != (uint64_t)-1) ok = 0;
    /* .text is not mapped. */
    extern char _text_start[];
    uint64_t tp = kvirt_to_phys(((uint64_t)(uintptr_t)_text_start + 0xFFF) & ~0xFFFull);
    if (iommu_translate(g_strict, tp, &prot) != (uint64_t)-1) ok = 0;
    return ok ? 0 : -1;
}

/* The region bounds (idempotent; dma_init calls it, so does the first
 * dma_static of a driver probed before it). */
static void region_init(void) {
    if (__atomic_load_n(&g_inited, __ATOMIC_ACQUIRE)) return;
    uint64_t s = (uint64_t)(uintptr_t)__dma_start, e = (uint64_t)(uintptr_t)__dma_end;
    uint64_t p = kvirt_to_phys(s);
    /* One physically contiguous run: the image loads contiguously and KASLR
     * slides only the virtual base. */
    if (e > s && kvirt_to_phys(e - 1) != p + (e - s) - 1)
        PANIC("[DMA] .dmabss is not physically contiguous");
    g_dma_kva = s; g_dma_end = e; g_dma_phys = p;
    __atomic_store_n(&g_inited, 1, __ATOMIC_RELEASE);
}

void dma_init(void) {
    iommu_line_t l; l.n = 0;
    region_init();
    uint64_t len = g_dma_end - g_dma_kva;
    iommu_ln_s(&l, "[DMA] .dmabss phys "); iommu_ln_hex(&l, g_dma_phys);
    iommu_ln_s(&l, " size "); iommu_ln_dec(&l, len / 1024); iommu_ln_s(&l, " KiB");
#ifdef SECOS_DMA_DEBUG
    iommu_ln_s(&l, " (SECOS_DMA_DEBUG: violations panic)");
#endif
    debugcon_writestring(iommu_ln_end(&l));

    if (!iommu_present()) { g_mode = "off"; return; }
    char v[16];
    int have = iommu_cmdline_value("iommu.kernel", v, sizeof(v));
    int pt = 0;
    if (have >= 0) {
        if (v[0] == 'p' && v[1] == 't' && v[2] == 0) pt = 1;
        else if (!(v[0] == 's' && v[1] == 't' && v[2] == 'r' && v[3] == 'i' && v[4] == 'c' && v[5] == 't' && v[6] == 0)) {
            iommu_ln_s(&l, "[DMA] unknown iommu.kernel="); iommu_ln_s(&l, v);
            iommu_ln_s(&l, " (want strict|pt), using strict");
            debugcon_writestring(iommu_ln_end(&l));
        }
    }
    if (pt) {
        g_mode = "pt";
        debugcon_writestring("[DMA] kernel DMA domain: passthrough (iommu.kernel=pt)\n");
        iommu_set_state("on (kernel drivers: passthrough, iommu.kernel=pt)");
        return;
    }
    iommu_domain_t* d = iommu_domain_new(IDOM_TRANSLATED);
    int rc = d ? iommu_map(d, g_dma_phys, g_dma_phys, (len + DMA_PAGE - 1) & ~(DMA_PAGE - 1),
                           IOMMU_READ | IOMMU_WRITE) : IOMMU_ENOMEM;
    if (rc) {
        if (d) iommu_domain_free(d);
        g_mode = "pt";
        iommu_ln_s(&l, "[DMA] cannot build the strict kernel DMA domain (rc ");
        iommu_ln_dec(&l, (uint64_t)(-rc)); iommu_ln_s(&l, "), kernel drivers stay in passthrough");
        debugcon_writestring(iommu_ln_end(&l));
        return;
    }
    g_strict = d;
    g_mode = "strict";
    iommu_set_state("on (kernel drivers: strict DMA domain)");
    if (strict_selftest()) {
        debugcon_writestring("[DMA] strict domain self-test FAIL\n");
    } else {
        debugcon_writestring("[DMA] strict domain self-test PASS\n");
    }
    iommu_ln_s(&l, "[DMA] kernel DMA domain: strict (");
    iommu_ln_dec(&l, (len + DMA_PAGE - 1) / DMA_PAGE);
    iommu_ln_s(&l, " .dmabss pages + coherent frames; iommu.kernel=pt = passthrough)");
    debugcon_writestring(iommu_ln_end(&l));
}

/* ------------------------------------------------------------ devices */
dma_dev_t* dma_dev(const pci_dev_t* pd) {
    if (!pd) return &g_dd_none;
    int idx = pci_dev_index(pd);
    if (idx < 0 || idx >= PCI_MAX_DEVS) return &g_dd_none;
    dma_dev_t* dd = &g_dd[idx];
    uint64_t f = spin_lock_irqsave(&g_dma_lock);
    if (dd->used) { spin_unlock_irqrestore(&g_dma_lock, f); return dd; }
    dd->pd = pd; dd->mask = DMA_MASK_64; dd->strict = 0;
    int rc = 1;   /* 1 = not covered, 2 = not in the passthrough domain */
    /* Only a function still in the M54 passthrough domain moves: one left
     * blocked (the iommu.block= test hook) or placed elsewhere stays put. */
    if (g_strict && iommu_dev_covered(pd)) {
        if (iommu_dev_domain(pd) != iommu_kernel_domain()) rc = 2;
        else {
            rc = iommu_attach(g_strict, pd);
            if (rc == 0) dd->strict = 1;
        }
    }
    dd->used = 1;
    __atomic_add_fetch(&g_ndev, 1, __ATOMIC_RELAXED);
    if (dd->strict) __atomic_add_fetch(&g_nstrict, 1, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&g_dma_lock, f);

    iommu_line_t l; l.n = 0;
    if (g_strict) {
        iommu_ln_s(&l, "[DMA] "); ln_dev(&l, dd);
        if (rc == 0) iommu_ln_s(&l, ": strict kernel DMA domain");
        else if (rc == 1) iommu_ln_s(&l, ": not translated by any IOMMU unit (DMA unconfined)");
        else if (rc == 2) iommu_ln_s(&l, ": not in the kernel passthrough domain (blocked or assigned), left as is");
        else { iommu_ln_s(&l, ": attach to the strict domain FAILED (rc -"); iommu_ln_dec(&l, (uint64_t)(-rc));
               iommu_ln_s(&l, "), stays in passthrough"); }
        debugcon_writestring(iommu_ln_end(&l));
    }
#ifdef SECOS_DMA_DEBUG
    else {
        iommu_ln_s(&l, "[DMA] "); ln_dev(&l, dd); iommu_ln_s(&l, ": registered (mode "); iommu_ln_s(&l, g_mode);
        iommu_ln_s(&l, ")");
        debugcon_writestring(iommu_ln_end(&l));
    }
#endif
    return dd;
}

dma_dev_t* dma_dev_legacy(const pci_device_t* d) {
    return dma_dev(d ? pci_dev_of(d) : NULL);
}

void dma_set_mask(dma_dev_t* dd, uint64_t mask) {
    if (!dd || dd == &g_dd_none || mask < 0xFFFFF) return;
    dd->mask = mask;
}
uint64_t dma_get_mask(const dma_dev_t* dd) { return dd ? dd->mask : DMA_MASK_64; }
int dma_dev_strict(const dma_dev_t* dd) { return dd && dd->strict; }
const char* dma_mode(void) { return g_mode; }

/* ------------------------------------------------------------ addresses */
int dma_reachable(const dma_dev_t* dd, const void* kva, size_t len) {
    dma_addr_t a = bus_of(kva, len);
    return a != DMA_ADDR_ERROR && fits_mask(dd, a, len);
}

dma_addr_t dma_static(const dma_dev_t* dd, const void* kva, size_t len) {
    dma_addr_t a = bus_of(kva, len);
    if (a == DMA_ADDR_ERROR) {
        violation(dd, kva, len, "dma_static outside .dmabss/coherent memory", __builtin_return_address(0));
        return kvirt_to_phys((uint64_t)(uintptr_t)kva);
    }
    if (!fits_mask(dd, a, len)) violation(dd, kva, len, "dma_static above the device DMA mask", __builtin_return_address(0));
    return a;
}

/* Record a mapping (lock held). -1 when the table is full. */
static int map_add_locked(uint64_t kva, uint64_t phys, uint64_t pages, int kind, int mapped) {
    for (int i = 0; i < DMA_MAX_MAPS; i++) {
        if (g_maps[i].kind) continue;
        g_maps[i].kva = kva; g_maps[i].phys = phys; g_maps[i].pages = pages;
        g_maps[i].kind = (uint8_t)kind; g_maps[i].mapped = (uint8_t)mapped;
        return i;
    }
    return -1;
}

void* dma_alloc_coherent(dma_dev_t* dd, size_t len, dma_addr_t* out, int flags) {
    (void)flags;
    if (!len || !out) return NULL;
    uint64_t pages = ((uint64_t)len + DMA_PAGE - 1) / DMA_PAGE;
    uint64_t mask = dd ? dd->mask : DMA_MASK_64;
    uint64_t f = spin_lock_irqsave(&g_dma_lock);
    void* fr = pmm_alloc_contiguous_below(pages, mask == DMA_MASK_64 ? ~0ULL : mask + 1);
    uint64_t phys = (uint64_t)(uintptr_t)fr;
    if (!fr || !fits_mask(dd, phys, pages * DMA_PAGE)) {
        if (fr) pmm_free_contiguous(fr, pages);
        spin_unlock_irqrestore(&g_dma_lock, f);
        return NULL;
    }
    uint64_t kva = phys_to_virt(phys);
    for (uint64_t i = 0; i < pages * DMA_PAGE; i += 8) *(volatile uint64_t*)(kva + i) = 0;
    int mapped = 0;
    if (g_strict) {
        if (iommu_map(g_strict, phys, phys, pages * DMA_PAGE, IOMMU_READ | IOMMU_WRITE)) {
            pmm_free_contiguous(fr, pages);
            spin_unlock_irqrestore(&g_dma_lock, f);
            return NULL;
        }
        mapped = 1;
    }
    if (map_add_locked(kva, phys, pages, DMA_K_COHERENT, mapped) < 0) {
        if (mapped) iommu_unmap(g_strict, phys, pages * DMA_PAGE);
        pmm_free_contiguous(fr, pages);
        spin_unlock_irqrestore(&g_dma_lock, f);
        return NULL;
    }
    g_coh_pages += pages; g_coh_allocs++;
    spin_unlock_irqrestore(&g_dma_lock, f);
    *out = phys;
    return (void*)(uintptr_t)kva;
}

void dma_free_coherent(dma_dev_t* dd, void* kva, size_t len, dma_addr_t addr) {
    if (!kva) return;
    uint64_t v = (uint64_t)(uintptr_t)kva;
    uint64_t f = spin_lock_irqsave(&g_dma_lock);
    for (int i = 0; i < DMA_MAX_MAPS; i++) {
        dma_map_t* m = &g_maps[i];
        if (m->kind != DMA_K_COHERENT || m->kva != v) continue;
        if (m->phys != addr || m->pages != ((uint64_t)len + DMA_PAGE - 1) / DMA_PAGE) break;
        /* The unmap returns after the IOTLB flush: the device can no longer
         * reach the frames when they go back to the PMM. */
        if (m->mapped) iommu_unmap(g_strict, m->phys, m->pages * DMA_PAGE);
        pmm_free_contiguous((void*)(uintptr_t)m->phys, m->pages);
        g_coh_pages -= m->pages;
        m->kind = 0;
        spin_unlock_irqrestore(&g_dma_lock, f);
        return;
    }
    spin_unlock_irqrestore(&g_dma_lock, f);
    violation(dd, kva, len, "dma_free_coherent of memory not from dma_alloc_coherent", __builtin_return_address(0));
}

dma_addr_t dma_map_single(dma_dev_t* dd, void* kva, size_t len, int dir) {
    (void)dir;
    if (!len) return DMA_ADDR_ERROR;
    uint64_t v = (uint64_t)(uintptr_t)kva;
    if (in_dmabss(v, len)) {
        dma_addr_t a = g_dma_phys + (v - g_dma_kva);
        return fits_mask(dd, a, len) ? a : DMA_ADDR_ERROR;
    }
    uint64_t p0 = v & ~(DMA_PAGE - 1);
    if (v + len < v) return DMA_ADDR_ERROR;
    uint64_t pages = (v + len - p0 + DMA_PAGE - 1) / DMA_PAGE;
    uint64_t phys = kvirt_to_phys(p0);
    if (!phys) return DMA_ADDR_ERROR;
    for (uint64_t i = 1; i < pages; i++)       /* physically contiguous only */
        if (kvirt_to_phys(p0 + i * DMA_PAGE) != phys + i * DMA_PAGE) return DMA_ADDR_ERROR;
    if (!fits_mask(dd, phys, pages * DMA_PAGE)) return DMA_ADDR_ERROR;
    uint64_t f = spin_lock_irqsave(&g_dma_lock);
    int mapped = 0;
    if (g_strict) {
        int prot;
        for (uint64_t i = 0; i < pages; i++)
            if (iommu_translate(g_strict, phys + i * DMA_PAGE, &prot) != (uint64_t)-1) {
                spin_unlock_irqrestore(&g_dma_lock, f);
                return DMA_ADDR_ERROR;      /* shares a page with another mapping */
            }
        if (iommu_map(g_strict, phys, phys, pages * DMA_PAGE, IOMMU_READ | IOMMU_WRITE)) {
            spin_unlock_irqrestore(&g_dma_lock, f);
            return DMA_ADDR_ERROR;
        }
        mapped = 1;
    }
    if (map_add_locked(p0, phys, pages, DMA_K_STREAM, mapped) < 0) {
        if (mapped) iommu_unmap(g_strict, phys, pages * DMA_PAGE);
        spin_unlock_irqrestore(&g_dma_lock, f);
        return DMA_ADDR_ERROR;
    }
    g_stream_maps++;
    spin_unlock_irqrestore(&g_dma_lock, f);
    return phys + (v - p0);
}

void dma_unmap_single(dma_dev_t* dd, dma_addr_t addr, size_t len, int dir) {
    (void)dir;
    if (addr == DMA_ADDR_ERROR) return;
    if (addr >= g_dma_phys && addr - g_dma_phys < g_dma_end - g_dma_kva) return;   /* .dmabss */
    uint64_t base = addr & ~(DMA_PAGE - 1);
    uint64_t f = spin_lock_irqsave(&g_dma_lock);
    for (int i = 0; i < DMA_MAX_MAPS; i++) {
        dma_map_t* m = &g_maps[i];
        if (m->kind != DMA_K_STREAM || m->phys != base) continue;
        if (m->mapped) iommu_unmap(g_strict, m->phys, m->pages * DMA_PAGE);
        m->kind = 0;
        g_stream_maps--;
        spin_unlock_irqrestore(&g_dma_lock, f);
        return;
    }
    spin_unlock_irqrestore(&g_dma_lock, f);
    violation(dd, (void*)(uintptr_t)addr, len, "dma_unmap_single of an address not mapped", __builtin_return_address(0));
}

/* ------------------------------------------------------------ /proc */
void dma_generate(void (*emit)(void* ctx, const char* line), void* ctx) {
    iommu_line_t l; l.n = 0;
    iommu_ln_s(&l, "dma mode "); iommu_ln_s(&l, g_mode);
    iommu_ln_s(&l, " dmabss "); iommu_ln_hex(&l, g_dma_phys);
    iommu_ln_s(&l, " "); iommu_ln_dec(&l, (g_dma_end - g_dma_kva) / 1024); iommu_ln_s(&l, " KiB");
    iommu_ln_s(&l, " devices "); iommu_ln_dec(&l, g_ndev);
    iommu_ln_s(&l, " strict "); iommu_ln_dec(&l, g_nstrict);
    iommu_ln_s(&l, " coherent "); iommu_ln_dec(&l, g_coh_pages);
    iommu_ln_s(&l, " pages streaming "); iommu_ln_dec(&l, g_stream_maps);
    iommu_ln_s(&l, " violations "); iommu_ln_dec(&l, g_violations);
    emit(ctx, iommu_ln_end(&l));
    for (int i = 0; i < PCI_MAX_DEVS; i++) {
        const dma_dev_t* dd = &g_dd[i];
        if (!dd->used) continue;
        iommu_ln_s(&l, "dma dev "); ln_dev(&l, dd);
        iommu_ln_s(&l, " mask "); iommu_ln_hex(&l, dd->mask);
        iommu_ln_s(&l, dd->strict ? " strict" : (g_strict ? " unconfined" : " untranslated-or-pt"));
        emit(ctx, iommu_ln_end(&l));
    }
}

/* ------------------------------------------------------------ tests */
#ifdef SECOS_DMA_TEST
#include "../boot/kaslr.h"
dma_addr_t dma_test_bad_addr(int which, void** kva) {
    if (kva) *kva = NULL;
    if (which == 0) {
        extern char _text_start[];
        uint64_t v = (((uint64_t)(uintptr_t)_text_start) + 0x10000 + 0xFFF) & ~0xFFFull;
        return kvirt_to_phys(v);
    }
    uint8_t* p = (uint8_t*)kmalloc(3 * DMA_PAGE);
    if (!p) return DMA_ADDR_ERROR;
    if (kva) *kva = p;
    uint64_t v = ((uint64_t)(uintptr_t)p + DMA_PAGE - 1) & ~(DMA_PAGE - 1);
    return kvirt_to_phys(v);
}
void dma_test_free(void* kva) { if (kva) kfree(kva); }
void dma_test_text_hash(uint8_t out[32]) {
    extern char _text_start[], _rodata_end[];
    kaslr_sha256_unrelocated((const uint8_t*)_text_start, (const uint8_t*)_rodata_end, out);
}
#endif
