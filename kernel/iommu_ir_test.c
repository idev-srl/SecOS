/*
 * iommu_ir_test.c — in-kernel interrupt-remapping tests with QEMU's `edu`
 * device (Phase 5, M56; built only with -DSECOS_IOMMU_TEST, run from
 * iommu_init() after the M54 DMA tests, when interrupt remapping is on).
 *
 * edu has an MSI capability (a message raised by a register write: a
 * *genuine* MSI, which QEMU sends with edu's requester id) and a DMA engine
 * that can write any 4 bytes to any bus address (an MSI *injected* by DMA,
 * the attack interrupt remapping exists for). Its DMA must reach 0xFEExxxxx,
 * so the guest needs `-device edu,dma_mask=0xffffffff`: QEMU clamps every
 * edu address to the mask (28 bits by default). The test probes the mask
 * first (a write to IOVA 0x80000000 with pages mapped there and at the
 * clamped IOVA 0, in a translated domain: never RAM, never a fault) and
 * skips the DMA-injection checks with a log line when it is too small (the
 * M54 gate's edu).
 *
 *   ir-cfis-clear          GSTS.CFIS is 0 (compatibility format blocked).
 *   ioapic-entry           the keyboard line (0x21) has a remapping entry.
 *   msi-delivered          edu MSI on an allocated vector, remappable format
 *                          (address bit 4), delivered 10 times out of 10.
 *   msi-retarget-ap        irq_vec_set_cpu() moves it to CPU 1 (the entry's
 *                          destination is rewritten; the device untouched).
 *   msi-sid-blocked        edu's MSI pointed at an entry owned by another
 *                          function (source-id check): not delivered.
 *   stale-entry-blocked    irq_vec_free() with edu's MSI still enabled, then
 *                          an MSI: not delivered (entry cleared), not even as
 *                          a spurious interrupt.
 *   dma-free-entry-blocked DMA write of a remappable message naming a free
 *                          entry: not delivered.
 *   dma-index-over-blocked the same with a handle past the table: blocked.
 *   dma-foreign-entry      DMA write naming another function's live entry:
 *                          blocked on hardware (source-id check). QEMU does
 *                          not give DMA writes a requester id and skips the
 *                          check: logged as LIMIT, not a failure.
 *   dma-compat             DMA write of a compatibility-format message for a
 *                          test vector: blocked on hardware (CFIS = 0). QEMU
 *                          6.2/9.2 do not implement the check and deliver
 *                          it: logged as LIMIT. Vectors 0x80 / 0xF0 are
 *                          deliberately not injected (on QEMU they would be
 *                          delivered and run the syscall / shootdown paths).
 *   ir-storm               60 back-to-back blocked injections: no delivery,
 *                          the timer keeps ticking, IR faults (when the
 *                          platform records them: QEMU 9.2 yes, 6.2 no) all
 *                          counted and the log rate-limited.
 * Every check also verifies the DMA-fault count did not move (the interrupt
 * window is not a DMA target) and the spurious-interrupt count is unchanged.
 * Lines: `[IR-TEST] ok|FAIL|LIMIT <name> ...`, then `[IR-TEST] RESULT ...`.
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef SECOS_IOMMU_TEST
#include "iommu.h"
#include "pci.h"
#include "pci_msi.h"
#include "irq.h"
#include "percpu.h"
#include "pmm.h"
#include "vmm.h"
#include "timer.h"
#include "debugcon.h"
#include <stddef.h>

extern int  vtd_ir_test_cfis(void);
extern void vtd_ir_test_entry(uint32_t idx, uint64_t* lo, uint64_t* hi);

#define EDU_IRQ_STATUS 0x24
#define EDU_IRQ_RAISE  0x60
#define EDU_IRQ_ACK    0x64
#define EDU_DMA_SRC    0x80
#define EDU_DMA_DST    0x88
#define EDU_DMA_CNT    0x90
#define EDU_DMA_CMD    0x98
#define EDU_CMD_RUN    0x1
#define EDU_CMD_TO_RAM 0x2
#define EDU_BUF        0x40000ull
#define PG             4096ull

static volatile uint8_t* g_edu;
static pci_device_t      g_pd;
static const pci_dev_t*  g_dev;
static uint16_t          g_bdf;
static int g_pass, g_fail, g_limit;

typedef struct { volatile uint32_t count; volatile int32_t cpu; } hit_t;
static hit_t g_hit_a, g_hit_b, g_hit_c;

static void on_irq(uint8_t vec, void* ctx) {
    (void)vec;
    hit_t* h = (hit_t*)ctx;
    h->cpu = (int32_t)this_cpu()->index;
    __atomic_add_fetch(&h->count, 1, __ATOMIC_RELEASE);
}

static void out(const char* tag, const char* name, const char* extra, uint64_t v, int hv) {
    iommu_line_t l; l.n = 0;
    iommu_ln_s(&l, "[IR-TEST] "); iommu_ln_s(&l, tag); iommu_ln_s(&l, " "); iommu_ln_s(&l, name);
    if (extra) { iommu_ln_s(&l, " "); iommu_ln_s(&l, extra); }
    if (hv) { iommu_ln_s(&l, " "); iommu_ln_hex(&l, v); }
    debugcon_writestring(iommu_ln_end(&l));
}
static void check(int ok, const char* name, const char* extra, uint64_t v, int hv) {
    if (ok) g_pass++; else g_fail++;
    out(ok ? "ok" : "FAIL", name, extra, v, hv);
}
/* Blocked on hardware, possibly delivered by the emulator. */
static void check_hw(int blocked, int sane, const char* name, const char* why) {
    if (!sane) { g_fail++; out("FAIL", name, "side effect (DMA fault / spurious)", 0, 0); return; }
    if (blocked) { g_pass++; out("ok", name, "blocked", 0, 0); return; }
    g_limit++;
    out("LIMIT", name, why, 0, 0);
}

static uint64_t dma_faults(void) { iommu_fault_stats_t s; iommu_fault_stats(&s); return s.total; }
static uint64_t ir_faults(void) { iommu_ir_fault_stats_t s; iommu_ir_fault_stats(&s); return s.total; }

static void edu_w32(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_edu + off) = v; }

static int edu_dma(uint64_t src, uint64_t dst, uint32_t cnt, int to_ram) {
    volatile uint64_t* r = (volatile uint64_t*)(g_edu + EDU_DMA_SRC);
    r[0] = src; r[1] = dst; r[2] = cnt;
    *(volatile uint64_t*)(g_edu + EDU_DMA_CMD) = EDU_CMD_RUN | (to_ram ? EDU_CMD_TO_RAM : 0);
    uint64_t t0 = timer_get_ticks();
    while (*(volatile uint64_t*)(g_edu + EDU_DMA_CMD) & EDU_CMD_RUN) {
        if (timer_get_ticks() - t0 > 3000) return -1;
        __asm__ volatile ("pause");
    }
    return 0;
}

/* Wait up to `ms` for h->count to reach `want`. */
static int wait_hit(hit_t* h, uint32_t want, uint32_t ms) {
    uint64_t t0 = timer_get_ticks();
    while (__atomic_load_n(&h->count, __ATOMIC_ACQUIRE) < want && timer_get_ticks() - t0 < ms)
        __asm__ volatile ("pause");
    return __atomic_load_n(&h->count, __ATOMIC_ACQUIRE) >= want;
}
static void settle(uint32_t ms) {
    uint64_t t0 = timer_get_ticks();
    while (timer_get_ticks() - t0 < ms) __asm__ volatile ("pause");
}

/* One genuine edu MSI (raise, then ack so the next raise notifies again). */
static void edu_msi(void) {
    edu_w32(EDU_IRQ_RAISE, 1);
    settle(20);
    edu_w32(EDU_IRQ_ACK, 1);
}

/* Write edu's MSI capability by hand (address / data), MSI on. */
static void edu_msi_raw(uint64_t addr, uint32_t data) {
    uint8_t cap = pci_find_capability(&g_pd, PCI_CAP_ID_MSI);
    uint16_t mc = (uint16_t)(pci_config_read32(g_pd.bus, g_pd.slot, g_pd.func, cap) >> 16);
    uint32_t w = pci_config_read32(g_pd.bus, g_pd.slot, g_pd.func, cap);
    pci_config_write32(g_pd.bus, g_pd.slot, g_pd.func, cap, w & ~(1u << 16));      /* MSI off */
    pci_config_write32(g_pd.bus, g_pd.slot, g_pd.func, (uint8_t)(cap + 4), (uint32_t)addr);
    uint8_t doff = (uint8_t)(cap + 8);
    if (mc & (1u << 7)) {                                                          /* 64-bit */
        pci_config_write32(g_pd.bus, g_pd.slot, g_pd.func, (uint8_t)(cap + 8), (uint32_t)(addr >> 32));
        doff = (uint8_t)(cap + 12);
    }
    uint32_t dw = pci_config_read32(g_pd.bus, g_pd.slot, g_pd.func, doff);
    pci_config_write32(g_pd.bus, g_pd.slot, g_pd.func, doff, (dw & 0xFFFF0000u) | (data & 0xFFFF));
    w = pci_config_read32(g_pd.bus, g_pd.slot, g_pd.func, cap);
    pci_config_write32(g_pd.bus, g_pd.slot, g_pd.func, cap, (w & ~(7u << 20)) | (1u << 16));
}
static uint32_t edu_msi_addr_lo(void) {
    uint8_t cap = pci_find_capability(&g_pd, PCI_CAP_ID_MSI);
    return pci_config_read32(g_pd.bus, g_pd.slot, g_pd.func, (uint8_t)(cap + 4));
}

/* A remappable-format message address naming `handle` (SHV=1). */
static uint32_t remap_addr(uint32_t handle) {
    return 0xFEE00000u | ((handle & 0x7FFF) << 5) | (1u << 4) | (1u << 3) | (((handle >> 15) & 1) << 2);
}

/* Inject {addr, data} by an edu DMA write of 4 bytes (edu in domain `d`,
 * the page `src` mapped at IOVA `isrc`). */
static void inject(uint64_t src, uint64_t isrc, uint32_t addr, uint32_t data) {
    *(volatile uint32_t*)phys_to_virt(src) = data;
    edu_dma(isrc, EDU_BUF, 64, 0);              /* RAM -> edu buffer */
    edu_dma(EDU_BUF, addr, 4, 1);               /* edu buffer -> "RAM" = the MSI window */
}

static const pci_dev_t* other_dev(void) {
    int n = pci_dev_count();
    for (int i = 0; i < n; i++) {
        const pci_dev_t* d = pci_dev_at(i);
        if (d && d != g_dev && iommu_ir_covers(d)) return d;
    }
    return NULL;
}

void iommu_ir_test_run(void) {
    if (!iommu_ir_enabled()) { debugcon_writestring("[IR-TEST] interrupt remapping off: skipped\n"); return; }
    if (pci_find(0x1234, 0x11e8, &g_pd) != 0) { debugcon_writestring("[IR-TEST] no edu device: skipped\n"); return; }
    g_dev = pci_dev_of(&g_pd);
    if (!g_dev || !iommu_ir_covers(g_dev)) { debugcon_writestring("[IR-TEST] edu not remapped: skipped\n"); return; }
    g_bdf = (uint16_t)((g_pd.bus << 8) | (g_pd.slot << 3) | g_pd.func);
    pci_enable_mem_and_busmaster(&g_pd);
    g_edu = (volatile uint8_t*)pci_bar_map(&g_pd, 0, 0x100000);
    if (!g_edu) { check(0, "edu-bar", NULL, 0, 0); return; }
    debugcon_writestring("[IR-TEST] start\n");

    uint64_t sp0 = irq_spurious_count(), df0 = dma_faults(), irf0 = ir_faults();
    check(vtd_ir_test_cfis() == 0, "ir-cfis-clear", NULL, 0, 0);
    uint64_t lo, hi;
    vtd_ir_test_entry(0x21, &lo, &hi);
    check((lo & 1) && ((lo >> 16) & 0xFF) == 0x21 && ((hi >> 18) & 3) == 1, "ioapic-entry", "sid", hi & 0xFFFF, 1);

    /* positive: genuine MSI through the remapping entry */
    int va = irq_vec_alloc(0, on_irq, &g_hit_a, "irtest-a");
    int vb = irq_vec_alloc(0, on_irq, &g_hit_b, "irtest-b");
    int vc = irq_vec_alloc(0, on_irq, &g_hit_c, "irtest-c");
    if (va < 0 || vb < 0 || vc < 0) { check(0, "vectors", NULL, 0, 0); return; }
    int rc = pci_msi_enable(&g_pd, 1, (uint8_t)va, 0);
    uint32_t alo = edu_msi_addr_lo();
    check(rc == 0 && (alo & 0x18) == 0x18 && ((alo >> 5) & 0x7FFF) == (uint32_t)va, "msi-remappable-format",
          "addr", alo, 1);
    vtd_ir_test_entry((uint32_t)va, &lo, &hi);
    check((lo & 1) && (hi & 0xFFFF) == g_bdf && ((hi >> 18) & 3) == 1, "msi-entry-sid", NULL, hi, 1);
    for (int i = 0; i < 10; i++) { edu_msi(); wait_hit(&g_hit_a, (uint32_t)i + 1, 300); }
    check(g_hit_a.count == 10 && g_hit_a.cpu == 0, "msi-delivered", "count", g_hit_a.count, 1);

    if (smp_cpu_count() > 1) {
        irq_vec_set_cpu(va, 1);
        uint32_t c0 = g_hit_a.count;
        edu_msi();
        int got = wait_hit(&g_hit_a, c0 + 1, 300);
        check(got && g_hit_a.cpu == 1, "msi-retarget-ap", "cpu", (uint64_t)(int64_t)g_hit_a.cpu, 1);
        irq_vec_set_cpu(va, 0);
        c0 = g_hit_a.count;
        edu_msi();
        got = wait_hit(&g_hit_a, c0 + 1, 300);
        check(got && g_hit_a.cpu == 0, "msi-retarget-back", NULL, 0, 0);
    }

    /* source-id check: edu's genuine MSI naming an entry owned by another function */
    const pci_dev_t* od = other_dev();
    if (od) {
        uint64_t oa; uint32_t odata;
        rc = iommu_irq_compose(od, vb, 0, &oa, &odata);
        uint64_t irf = ir_faults();
        edu_msi_raw(oa, odata);
        edu_msi();
        settle(200);
        iommu_fault_poll();
        iommu_ir_fault_stats_t s; iommu_ir_fault_stats(&s);
        int fault_ok = (s.total == irf) || (s.last_reason == 0x26 && s.last_index == (uint16_t)vb && s.last_bdf == g_bdf);
        check(rc == 0 && g_hit_b.count == 0 && irq_spurious_count() == sp0 && fault_ok,
              "msi-sid-blocked", "ir-faults", s.total - irf, 1);
    } else {
        check(0, "msi-sid-blocked", "no second remapped function", 0, 0);
    }

    /* stale entry: vector freed while edu's MSI still names it */
    rc = pci_msi_enable(&g_pd, 1, (uint8_t)va, 0);
    uint32_t c1 = g_hit_a.count;
    edu_msi();
    int live = wait_hit(&g_hit_a, c1 + 1, 300);
    irq_vec_free(va);
    vtd_ir_test_entry((uint32_t)va, &lo, &hi);
    uint64_t irf = ir_faults();
    edu_msi();
    settle(200);
    iommu_fault_poll();
    iommu_ir_fault_stats_t s2; iommu_ir_fault_stats(&s2);
    int fault_ok = (s2.total == irf) || (s2.last_reason == 0x22 && s2.last_index == (uint16_t)va && s2.last_bdf == g_bdf);
    check(rc == 0 && live && !(lo & 1) && g_hit_a.count == c1 + 1 && irq_spurious_count() == sp0 && fault_ok,
          "stale-entry-blocked", "ir-faults", s2.total - irf, 1);
    pci_msi_disable(&g_pd);

    /* DMA injection: edu in a translated domain with one page mapped */
    iommu_domain_t* d = iommu_domain_new(IDOM_TRANSLATED);
    uint64_t src = (uint64_t)pmm_alloc_below(0x100000000ull);
    const uint64_t ISRC = 0x100000;
    /* edu clamps every DMA address to its dma_mask (28 bits by default).
     * Probe it without a fault: pages mapped at IOVA 0x80000000 and at the
     * clamped address 0; a write to 0x80000000 lands in the first only when
     * the mask has at least 32 bits. */
    uint64_t phi = (uint64_t)pmm_alloc_below(0x100000000ull), plo = (uint64_t)pmm_alloc_below(0x100000000ull);
    int mask32 = 0;
    if (d && src && phi && plo && iommu_map(d, ISRC, src, PG, IOMMU_READ) == 0 &&
        iommu_map(d, 0x80000000ull, phi, PG, IOMMU_READ | IOMMU_WRITE) == 0 &&
        iommu_map(d, 0, plo, PG, IOMMU_READ | IOMMU_WRITE) == 0 && iommu_attach(d, g_dev) == 0) {
        *(volatile uint32_t*)phys_to_virt(phi) = 0; *(volatile uint32_t*)phys_to_virt(plo) = 0;
        *(volatile uint32_t*)phys_to_virt(src) = 0x5EC05EC0u;
        edu_dma(ISRC, EDU_BUF, 64, 0);
        edu_dma(EDU_BUF, 0x80000000ull, 4, 1);
        mask32 = *(volatile uint32_t*)phys_to_virt(phi) == 0x5EC05EC0u;
        iommu_unmap(d, 0x80000000ull, PG);
        iommu_unmap(d, 0, PG);
    }
    if (!mask32) {
        debugcon_writestring("[IR-TEST] edu dma_mask < 32 bits: DMA-injection checks skipped "
                             "(use -device edu,dma_mask=0xffffffff)\n");
        if (d) iommu_attach(iommu_kernel_domain(), g_dev);
    } else if (!d || !src) {
        check(0, "dma-domain", NULL, 0, 0);
    } else {
        int vfree = -1;
        for (int v = IRQ_VEC_LAST; v >= IRQ_VEC_FIRST; v--) if (irq_vec_cpu(v) < 0) { vfree = v; break; }
        uint64_t df = dma_faults();
        uint32_t ca = g_hit_c.count;
        inject(src, ISRC, remap_addr((uint32_t)vfree), 0);
        settle(100);
        check(irq_spurious_count() == sp0 && dma_faults() == df, "dma-free-entry-blocked", "handle",
              (uint64_t)vfree, 1);

        inject(src, ISRC, remap_addr(0x1000), 0);
        settle(100);
        check(irq_spurious_count() == sp0 && dma_faults() == df, "dma-index-over-blocked", NULL, 0, 0);

        /* vc owned by the other function (source id != edu) */
        if (od) {
            uint64_t oa; uint32_t odata;
            iommu_irq_compose(od, vc, 0, &oa, &odata);
            inject(src, ISRC, (uint32_t)oa, odata);
            settle(100);
            check_hw(g_hit_c.count == ca, irq_spurious_count() == sp0 && dma_faults() == df,
                     "dma-foreign-entry", "delivered: the platform does not source-check DMA writes (QEMU)");
        }

        /* compatibility format, harmless test vector vc on the BSP */
        cpu_t* bsp = cpu_by_index(0);
        uint32_t caddr = 0xFEE00000u | ((bsp ? (bsp->lapic_id & 0xFF) : 0) << 12);
        uint32_t cc = g_hit_c.count;
        inject(src, ISRC, caddr, (uint32_t)vc);
        settle(100);
        check_hw(g_hit_c.count == cc, irq_spurious_count() == sp0 && dma_faults() == df,
                 "dma-compat", "delivered: the platform ignores CFIS=0 (QEMU)");
        if (dma_faults() != df) {
            iommu_fault_stats_t fs; iommu_fault_stats(&fs);
            out("FAIL", "edu-dma-mask", "a DMA fault: is edu dma_mask < 32 bits? iova", fs.last.iova, 1);
        }

        /* storm: back-to-back blocked injections */
        uint64_t t0 = timer_get_ticks();
        iommu_ir_fault_stats_t a; iommu_ir_fault_stats(&a);
        for (int k = 0; k < 60; k++) inject(src, ISRC, remap_addr(0x2000 + (uint32_t)k), 0);
        settle(200);
        iommu_fault_poll();
        iommu_ir_fault_stats_t b; iommu_ir_fault_stats(&b);
        uint64_t secs = (timer_get_ticks() - t0) / 1000 + 1;
        int ticking = timer_get_ticks() > t0;
        int recorded = b.total > a.total;
        int rl_ok = !recorded || (b.logged - a.logged <= 5 * (secs + 1) && b.suppressed > a.suppressed);
        check(ticking && irq_spurious_count() == sp0 && rl_ok, recorded ? "ir-storm-rate-limited" : "ir-storm-not-recorded",
              "ir-faults", b.total - a.total, 1);
        iommu_attach(iommu_kernel_domain(), g_dev);
    }
    if (d) iommu_domain_free(d);
    if (src) pmm_free_frame((void*)src);
    if (phi) pmm_free_frame((void*)phi);
    if (plo) pmm_free_frame((void*)plo);
    irq_vec_free(vb);
    irq_vec_free(vc);
    vtd_ir_test_entry((uint32_t)vb, &lo, &hi);
    check(!(lo & 1) && !hi, "freed-entries-cleared", NULL, 0, 0);
    check(dma_faults() == df0, "no-dma-faults", NULL, dma_faults() - df0, 1);

    iommu_line_t l; l.n = 0;
    iommu_ln_s(&l, "[IR-TEST] RESULT pass="); iommu_ln_dec(&l, (uint64_t)g_pass);
    iommu_ln_s(&l, " fail="); iommu_ln_dec(&l, (uint64_t)g_fail);
    iommu_ln_s(&l, " limit="); iommu_ln_dec(&l, (uint64_t)g_limit);
    iommu_ln_s(&l, " ir-faults="); iommu_ln_dec(&l, ir_faults() - irf0);
    debugcon_writestring(iommu_ln_end(&l));
}
#endif /* SECOS_IOMMU_TEST */
