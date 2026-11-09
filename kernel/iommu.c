/*
 * iommu.c — vendor-neutral DMA remapping core (Phase 5, M54): boot policy,
 * domains, device attachment, fault accounting, /proc/iommu. See iommu.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "iommu.h"
#include "acpi_dmar.h"
#include "heap.h"
#include "pmm.h"
#include "timer.h"
#include "debugcon.h"
#include "pci_msi.h"
#include <stddef.h>

extern void vtd_probe(void);
extern int  kernel_cmdline_has(const char* word);
extern char g_kcmdline[256];
#ifdef SECOS_IOMMU_TEST
extern void iommu_test_run(void);
#endif

static const iommu_ops_t* g_ops;
static int                g_on;            /* translation enabled            */
static iommu_domain_t*    g_kdom;          /* the shared passthrough domain  */
static volatile uint32_t  g_dom_ids;
static volatile uint32_t  g_ndomains;
static const char*        g_state = "off (default; boot word iommu=on enables it)";

/* Device -> domain, by registry index (LK_S65_IOMMU_DEV). */
static spinlock_t         g_dev_lock = SPINLOCK_INIT_CLASS(LK_S65_IOMMU_DEV, "iommu_dev");
static iommu_domain_t*    g_devdom[PCI_MAX_DEVS];
static uint8_t            g_devatt[PCI_MAX_DEVS];   /* context programmed */

/* Faults. */
static volatile uint64_t  g_f_total, g_f_irqs, g_f_polled, g_f_logged, g_f_supp, g_f_ovf;
static iommu_fault_t      g_f_last;
static iommu_fault_hook_t volatile g_f_hook;
static volatile uint64_t  g_rl_window;      /* tick the current window began  */
static volatile uint32_t  g_rl_count;       /* lines printed in the window    */
static volatile uint64_t  g_rl_pending;     /* suppressed, not yet reported   */
#define IOMMU_RL_WINDOW_MS  1000
#define IOMMU_RL_BURST      5

/* ------------------------------------------------------------ lines */
void iommu_ln_s(iommu_line_t* l, const char* s) {
    while (s && *s && l->n + 2 < sizeof(l->b)) l->b[l->n++] = *s++;
}
void iommu_ln_hexw(iommu_line_t* l, uint64_t v, int digits) {
    static const char h[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--)
        if (l->n + 2 < sizeof(l->b)) l->b[l->n++] = h[(v >> (i * 4)) & 0xF];
}
void iommu_ln_hex(iommu_line_t* l, uint64_t v) {
    iommu_ln_s(l, "0x");
    int d = 1;
    for (uint64_t t = v >> 4; t; t >>= 4) d++;
    iommu_ln_hexw(l, v, d);
}
void iommu_ln_dec(iommu_line_t* l, uint64_t v) {
    char t[24]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 23);
    while (n && l->n + 2 < sizeof(l->b)) l->b[l->n++] = t[--n];
}
void iommu_ln_bdf(iommu_line_t* l, uint16_t bdf) {
    iommu_ln_hexw(l, bdf >> 8, 2); iommu_ln_s(l, ":");
    iommu_ln_hexw(l, (bdf >> 3) & 0x1F, 2); iommu_ln_s(l, ".");
    iommu_ln_hexw(l, bdf & 7, 1);
}
const char* iommu_ln_end(iommu_line_t* l) {
    if (l->n + 1 >= sizeof(l->b)) l->n = sizeof(l->b) - 2;
    l->b[l->n++] = '\n';
    l->b[l->n] = 0;
    l->n = 0;
    return l->b;
}

int iommu_cmdline_has(const char* word) { return kernel_cmdline_has(word); }

int iommu_cmdline_value(const char* key, char* out, int cap) {
    const char* s = g_kcmdline;
    int kl = 0; while (key[kl]) kl++;
    for (int i = 0; s[i] && i < 256; ) {
        while (s[i] == ' ') i++;
        int j = 0;
        while (j < kl && s[i + j] == key[j]) j++;
        if (j == kl && s[i + j] == '=') {
            int k = 0; i += j + 1;
            while (s[i] && s[i] != ' ' && k + 1 < cap) out[k++] = s[i++];
            out[k] = 0;
            return k;
        }
        while (s[i] && s[i] != ' ') i++;
    }
    return -1;
}

/* ------------------------------------------------------------ domains */
void iommu_register_ops(const iommu_ops_t* ops) { g_ops = ops; }
int  iommu_present(void) { return g_on; }
void iommu_set_state(const char* st) { if (g_on && st) g_state = st; }   /* [M55] */
iommu_domain_t* iommu_kernel_domain(void) { return g_kdom; }
int  iommu_domain_kind(const iommu_domain_t* d) { return d ? d->kind : -1; }

/* The vendor state lives right after the domain (one allocation). */
#define IOMMU_HW_BYTES 192

iommu_domain_t* iommu_domain_new(int kind) {
    if (!g_ops || kind < IDOM_BLOCKED || kind > IDOM_TRANSLATED) return NULL;
    uint8_t* m = (uint8_t*)kmalloc(sizeof(iommu_domain_t) + IOMMU_HW_BYTES);
    if (!m) return NULL;
    for (uint64_t i = 0; i < sizeof(iommu_domain_t) + IOMMU_HW_BYTES; i++) m[i] = 0;
    iommu_domain_t* d = (iommu_domain_t*)m;
    d->kind = kind;
    d->id = __atomic_add_fetch(&g_dom_ids, 1, __ATOMIC_RELAXED);
    spin_init_class_dynamic(&d->lock, LK_S67_IOMMU_DOM, "iommu_dom");
    d->dma_mask = g_ops->max_iova();
    d->iova_next = 0x100000;              /* never hand out IOVA 0 */
    d->hw = m + sizeof(iommu_domain_t);
    if (g_ops->dom_init(d)) { kfree(m); return NULL; }
    __atomic_add_fetch(&g_ndomains, 1, __ATOMIC_RELAXED);
    return d;
}

int iommu_domain_free(iommu_domain_t* d) {
    if (!d || !g_ops) return IOMMU_EINVAL;
    if (d == g_kdom) return IOMMU_EBUSY;
    if (__atomic_load_n(&d->ndev, __ATOMIC_ACQUIRE)) return IOMMU_EBUSY;
    /* Every detach already waited for its context + IOTLB invalidation, so
     * no unit can still walk these tables. */
    g_ops->dom_fini(d);
    kfree(d);
    __atomic_sub_fetch(&g_ndomains, 1, __ATOMIC_RELAXED);
    return 0;
}

int iommu_domain_set_dma_mask(iommu_domain_t* d, uint64_t mask) {
    if (!d || mask < 0xFFF) return IOMMU_EINVAL;
    uint64_t hw = g_ops ? g_ops->max_iova() : 0;
    uint64_t f = spin_lock_irqsave(&d->lock);
    d->dma_mask = mask < hw ? mask : hw;
    spin_unlock_irqrestore(&d->lock, f);
    return 0;
}

uint64_t iommu_iova_alloc(iommu_domain_t* d, uint64_t len) {
    if (!d || !len) return 0;
    len = (len + 0xFFF) & ~0xFFFull;
    uint64_t r = 0;
    uint64_t f = spin_lock_irqsave(&d->lock);
    uint64_t a = d->iova_next;
    if (a + len > a && a + len - 1 <= d->dma_mask) { r = a; d->iova_next = a + len; }
    spin_unlock_irqrestore(&d->lock, f);
    return r;
}

int iommu_map(iommu_domain_t* d, uint64_t iova, uint64_t phys, uint64_t len, int prot) {
    if (!d || !g_ops) return IOMMU_ENODEV;
    if (d->kind != IDOM_TRANSLATED && d->kind != IDOM_IDENTITY) return IOMMU_EINVAL;
    if (!len || ((iova | phys | len) & 0xFFF) || iova + len < iova) return IOMMU_EINVAL;
    if (!(prot & (IOMMU_READ | IOMMU_WRITE)) || (prot & ~(IOMMU_READ | IOMMU_WRITE))) return IOMMU_EINVAL;
    uint64_t f = spin_lock_irqsave(&d->lock);
    int rc;
    if (iova + len - 1 > d->dma_mask) rc = IOMMU_ERANGE;
    else rc = g_ops->map(d, iova, phys, len, prot);
    if (rc == 0) d->mapped_pages += len >> 12;
    spin_unlock_irqrestore(&d->lock, f);
    return rc;
}

int iommu_unmap(iommu_domain_t* d, uint64_t iova, uint64_t len) {
    if (!d || !g_ops) return IOMMU_ENODEV;
    if (d->kind != IDOM_TRANSLATED && d->kind != IDOM_IDENTITY) return IOMMU_EINVAL;
    if (!len || ((iova | len) & 0xFFF) || iova + len < iova) return IOMMU_EINVAL;
    uint64_t f = spin_lock_irqsave(&d->lock);
    int rc = g_ops->unmap(d, iova, len);
    if (rc == 0) d->mapped_pages = d->mapped_pages > (len >> 12) ? d->mapped_pages - (len >> 12) : 0;
    spin_unlock_irqrestore(&d->lock, f);
    return rc;
}

uint64_t iommu_translate(iommu_domain_t* d, uint64_t iova, int* prot) {
    if (prot) *prot = 0;
    if (!d || !g_ops || d->kind == IDOM_BLOCKED) return (uint64_t)-1;
    uint64_t f = spin_lock_irqsave(&d->lock);
    uint64_t r = g_ops->translate(d, iova, prot);
    spin_unlock_irqrestore(&d->lock, f);
    return r;
}

/* ------------------------------------------------------------ devices */
int iommu_dev_covered(const pci_dev_t* dev) {
    return dev && g_ops && g_ops->covers(dev);
}

int iommu_attach(iommu_domain_t* d, const pci_dev_t* dev) {
    if (!d || !dev || !g_ops) return IOMMU_ENODEV;
    if (d->kind == IDOM_BLOCKED) return IOMMU_EINVAL;      /* iommu_detach() blocks */
    int i = pci_dev_index(dev);
    if (i < 0 || i >= PCI_MAX_DEVS) return IOMMU_EINVAL;
    if (!g_ops->covers(dev)) return IOMMU_ENODEV;
    uint64_t f = spin_lock_irqsave(&g_dev_lock);
    iommu_domain_t* old = g_devdom[i];
    int rc = 0;
    if (old != d || !g_devatt[i]) {
        rc = g_ops->set_context(d, dev);
        if (rc == 0) {
            if (old && g_devatt[i]) __atomic_sub_fetch(&old->ndev, 1, __ATOMIC_ACQ_REL);
            g_devdom[i] = d;
            g_devatt[i] = 1;
            __atomic_add_fetch(&d->ndev, 1, __ATOMIC_ACQ_REL);
        }
    }
    spin_unlock_irqrestore(&g_dev_lock, f);
    return rc;
}

void iommu_detach(const pci_dev_t* dev) {
    if (!dev || !g_ops) return;
    int i = pci_dev_index(dev);
    if (i < 0 || i >= PCI_MAX_DEVS) return;
    uint64_t f = spin_lock_irqsave(&g_dev_lock);
    if (g_devatt[i]) {
        if (g_ops->set_context(NULL, dev) == 0) {
            if (g_devdom[i]) __atomic_sub_fetch(&g_devdom[i]->ndev, 1, __ATOMIC_ACQ_REL);
            g_devdom[i] = NULL;
            g_devatt[i] = 0;
        }
    }
    spin_unlock_irqrestore(&g_dev_lock, f);
}

iommu_domain_t* iommu_dev_domain(const pci_dev_t* dev) {
    int i = dev ? pci_dev_index(dev) : -1;
    if (i < 0 || i >= PCI_MAX_DEVS) return NULL;
    return g_devatt[i] ? g_devdom[i] : NULL;
}

int iommu_irq_compose(const pci_dev_t* dev, int vec, uint32_t cpu, uint64_t* addr, uint32_t* data) {
    if (g_ops && g_on && g_ops->irq_compose) return g_ops->irq_compose(dev, vec, cpu, addr, data);
    pci_device_t pd = { dev->bus, dev->slot, dev->func, dev->vendor_id, dev->device_id };
    return msi_compose(&pd, (uint8_t)vec, cpu, addr, data);
}

/* ------------------------------------------------------------ faults */
void iommu_set_fault_hook(iommu_fault_hook_t hook) { g_f_hook = hook; }
void iommu_note_fault_irq(void) { __atomic_add_fetch(&g_f_irqs, 1, __ATOMIC_RELAXED); }
void iommu_note_overflow(void) {
    __atomic_add_fetch(&g_f_ovf, 1, __ATOMIC_RELAXED);
}

/* Rate limit: at most IOMMU_RL_BURST lines per second; the number dropped
 * is reported with the first line of the next window. */
static int rl_allow(uint64_t* dropped) {
    uint64_t now = timer_get_ticks();
    uint64_t w = __atomic_load_n(&g_rl_window, __ATOMIC_RELAXED);
    *dropped = 0;
    if (now - w >= IOMMU_RL_WINDOW_MS &&
        __atomic_compare_exchange_n(&g_rl_window, &w, now, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g_rl_count, 0, __ATOMIC_RELEASE);
        *dropped = __atomic_exchange_n(&g_rl_pending, 0, __ATOMIC_ACQ_REL);
    }
    if (__atomic_add_fetch(&g_rl_count, 1, __ATOMIC_ACQ_REL) <= IOMMU_RL_BURST) return 1;
    __atomic_add_fetch(&g_rl_pending, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_f_supp, 1, __ATOMIC_RELAXED);
    return 0;
}

void iommu_report_fault(const iommu_fault_t* f, int from_irq) {
    __atomic_add_fetch(&g_f_total, 1, __ATOMIC_RELAXED);
    if (!from_irq) __atomic_add_fetch(&g_f_polled, 1, __ATOMIC_RELAXED);
    g_f_last = *f;
    uint64_t dropped = 0;
    if (rl_allow(&dropped)) {
        iommu_line_t l; l.n = 0;
        if (dropped) {
            iommu_ln_s(&l, "[IOMMU] "); iommu_ln_dec(&l, dropped);
            iommu_ln_s(&l, " DMA fault message(s) suppressed (rate limit)");
            debugcon_writestring(iommu_ln_end(&l));
        }
        iommu_ln_s(&l, "[IOMMU] DMA fault bdf=");
        if (f->seg) { iommu_ln_hexw(&l, f->seg, 4); iommu_ln_s(&l, ":"); }
        iommu_ln_bdf(&l, f->bdf);
        iommu_ln_s(&l, " iova="); iommu_ln_hex(&l, f->iova);
        iommu_ln_s(&l, " reason="); iommu_ln_hex(&l, f->reason);
        iommu_ln_s(&l, " ("); iommu_ln_s(&l, g_ops ? g_ops->fault_reason(f->reason) : "?");
        iommu_ln_s(&l, f->is_write ? ") W" : ") R");
        debugcon_writestring(iommu_ln_end(&l));
        __atomic_add_fetch(&g_f_logged, 1, __ATOMIC_RELAXED);
    }
    iommu_fault_hook_t h = g_f_hook;
    if (h) h(f);
}

void iommu_fault_stats(iommu_fault_stats_t* o) {
    o->total = g_f_total; o->irqs = g_f_irqs; o->polled = g_f_polled;
    o->logged = g_f_logged; o->suppressed = g_f_supp; o->overflow = g_f_ovf;
    o->last = g_f_last;
}

void iommu_fault_poll(void) { if (g_ops && g_ops->fault_poll) g_ops->fault_poll(); }

/* ------------------------------------------------------------ /proc */
void iommu_generate(void (*emit)(void* ctx, const char* line), void* ctx) {
    iommu_line_t l; l.n = 0;
    iommu_ln_s(&l, "iommu "); iommu_ln_s(&l, g_on ? (g_ops ? g_ops->name : "?") : "off");
    iommu_ln_s(&l, " state "); iommu_ln_s(&l, g_state);
    emit(ctx, iommu_ln_end(&l));
    if (!g_ops) return;
    if (g_ops->generate) g_ops->generate(emit, ctx);
    iommu_ln_s(&l, "domains "); iommu_ln_dec(&l, g_ndomains);
    if (g_kdom) {
        iommu_ln_s(&l, " kernel ");
        iommu_ln_s(&l, g_kdom->kind == IDOM_PT ? "passthrough" : "identity");
        iommu_ln_s(&l, " devices "); iommu_ln_dec(&l, g_kdom->ndev);
    }
    emit(ctx, iommu_ln_end(&l));
    { extern void dma_generate(void (*)(void*, const char*), void*); dma_generate(emit, ctx); }   /* [M55] */
    int n = pci_dev_count();
    for (int i = 0; i < n && i < PCI_MAX_DEVS; i++) {
        const pci_dev_t* d = pci_dev_at(i);
        if (!d) break;
        iommu_domain_t* dd = g_devatt[i] ? g_devdom[i] : NULL;
        if (!g_ops->covers(d)) continue;
        iommu_ln_s(&l, "dev "); iommu_ln_bdf(&l, DMAR_BDF(d->bus, d->slot, d->func));
        iommu_ln_s(&l, " "); iommu_ln_hexw(&l, d->vendor_id, 4); iommu_ln_s(&l, ":");
        iommu_ln_hexw(&l, d->device_id, 4);
        iommu_ln_s(&l, " domain ");
        if (!dd) iommu_ln_s(&l, "blocked");
        else {
            iommu_ln_dec(&l, dd->id); iommu_ln_s(&l, " ");
            static const char* kn[4] = { "blocked", "identity", "passthrough", "translated" };
            iommu_ln_s(&l, kn[dd->kind & 3]);
        }
        emit(ctx, iommu_ln_end(&l));
    }
    iommu_ln_s(&l, "faults "); iommu_ln_dec(&l, g_f_total);
    iommu_ln_s(&l, " irqs "); iommu_ln_dec(&l, g_f_irqs);
    iommu_ln_s(&l, " polled "); iommu_ln_dec(&l, g_f_polled);
    iommu_ln_s(&l, " logged "); iommu_ln_dec(&l, g_f_logged);
    iommu_ln_s(&l, " suppressed "); iommu_ln_dec(&l, g_f_supp);
    iommu_ln_s(&l, " overflow "); iommu_ln_dec(&l, g_f_ovf);
    emit(ctx, iommu_ln_end(&l));
    if (g_f_total) {
        iommu_fault_t f = g_f_last;
        iommu_ln_s(&l, "last fault bdf "); iommu_ln_bdf(&l, f.bdf);
        iommu_ln_s(&l, " iova "); iommu_ln_hex(&l, f.iova);
        iommu_ln_s(&l, " reason "); iommu_ln_hex(&l, f.reason);
        iommu_ln_s(&l, f.is_write ? " W" : " R");
        emit(ctx, iommu_ln_end(&l));
    }
}

/* ------------------------------------------------------------ boot */
void iommu_init(void) {
    iommu_line_t l; l.n = 0;
    const acpi_dmar_t* dm = acpi_dmar_get();
    const acpi_ivrs_t* iv = acpi_ivrs_get();
    if (!kernel_cmdline_has("iommu=on")) {
        if (dm->present || iv->present)
            debugcon_writestring("[IOMMU] DMA remapping off (default; boot word iommu=on enables it)\n");
        return;
    }
    if (!dm->present) {
        g_state = iv->present ? "off (AMD-Vi: not supported before M64)" : "off (no DMA-remapping hardware)";
        iommu_ln_s(&l, "[IOMMU] iommu=on: "); iommu_ln_s(&l, g_state);
        iommu_ln_s(&l, ", continuing without DMA remapping");
        debugcon_writestring(iommu_ln_end(&l));
        return;
    }
    vtd_probe();
    if (!g_ops) {
        g_state = "off (no usable VT-d unit)";
        debugcon_writestring("[IOMMU] iommu=on: no usable VT-d unit, continuing without DMA remapping\n");
        return;
    }
    g_kdom = iommu_domain_new(IDOM_PT);
    if (!g_kdom) {
        g_state = "off (kernel domain allocation failed)";
        debugcon_writestring("[IOMMU] iommu=on: cannot build the kernel domain, DMA remapping off\n");
        g_ops = NULL;
        return;
    }
    int n = pci_dev_count(), att = 0, skip = 0, fail = 0;
#ifdef SECOS_IOMMU_TEST
    /* Test hook: `iommu.block=bb:dd.f` leaves one function blocked. */
    char blk[16]; int have_blk = iommu_cmdline_value("iommu.block", blk, sizeof(blk)) == 7;
#endif
    for (int i = 0; i < n; i++) {
        const pci_dev_t* d = pci_dev_at(i);
        if (!d) break;
        if (!g_ops->covers(d)) { skip++; continue; }
#ifdef SECOS_IOMMU_TEST
        if (have_blk) {
            iommu_line_t b; b.n = 0; iommu_ln_bdf(&b, DMAR_BDF(d->bus, d->slot, d->func)); b.b[b.n] = 0;
            int same = 1; for (int k = 0; k < 7; k++) if (b.b[k] != blk[k]) same = 0;
            if (same) {
                debugcon_writestring("[IOMMU-TEST] iommu.block: leaving ");
                debugcon_writestring(blk); debugcon_writestring(" blocked\n");
                continue;
            }
        }
#endif
        if (iommu_attach(g_kdom, d) == 0) att++;
        else {
            fail++;
            iommu_ln_s(&l, "[IOMMU] attach FAILED for "); iommu_ln_bdf(&l, DMAR_BDF(d->bus, d->slot, d->func));
            debugcon_writestring(iommu_ln_end(&l));
        }
    }
    if (g_ops->enable()) {
        g_state = "off (translation would not turn on)";
        debugcon_writestring("[IOMMU] iommu=on: enabling translation FAILED, DMA remapping off\n");
        return;
    }
    g_on = 1;
    g_state = g_kdom->kind == IDOM_PT ? "on (kernel drivers: passthrough)" : "on";
    iommu_ln_s(&l, "[IOMMU] on: "); iommu_ln_s(&l, g_ops->name);
    iommu_ln_s(&l, ", "); iommu_ln_dec(&l, (uint64_t)att);
    iommu_ln_s(&l, " function(s) in the kernel passthrough domain, ");
    iommu_ln_dec(&l, (uint64_t)skip); iommu_ln_s(&l, " not translated, ");
    iommu_ln_dec(&l, (uint64_t)fail); iommu_ln_s(&l, " failed");
    debugcon_writestring(iommu_ln_end(&l));
#ifdef SECOS_IOMMU_TEST
    iommu_test_run();
#endif
}
