/*
 * iommu_test.c — in-kernel DMA-remapping tests with QEMU's `edu` device
 * (Phase 5, M54; built only with -DSECOS_IOMMU_TEST, run from iommu_init()
 * when `iommu=on` found a unit).
 *
 * edu (PCI 1234:11e8) has a DMA engine that copies (here 2 KiB) between its
 * internal buffer (device address 0x40000) and any bus address we give it,
 * one transfer at a time (QEMU completes it ~100 ms later). Its DMA goes
 * through the vIOMMU, so we can check exactly what the IOMMU lets through:
 *
 *   positive     map pages at IOVAs, RAM -> edu -> RAM, data intact, 0 faults;
 *   negative 1   DMA write to an unmapped IOVA: exactly one fault with edu's
 *                BDF, that IOVA, reason 5 (write), the kernel continues;
 *   negative 2   map, DMA OK, unmap, DMA again: must fault and leave the page
 *                untouched (proves iommu_unmap() flushed the IOTLB: QEMU
 *                caches translations, so a missing flush lets it through —
 *                -DSECOS_IOMMU_TEST_NOFLUSH is the negative control);
 *   negative 3   read-only mapping: device write faults, device read works;
 *   negative 4   a 28-bit DMA mask: IOVAs above 256 MiB are refused by the
 *                domain and by the IOVA allocator; IOVAs past the hardware
 *                width likewise;
 *   blocked      detached: DMA faults with "context not present";
 *   passthrough  back in the kernel domain: IOVA == phys works;
 *   loop         N times (`iommu.testloops=N`, default 1000) map/DMA/unmap/
 *                DMA-must-fault, recreating the domain every 100 rounds;
 *                PMM free frames identical before and after;
 *   storm        back-to-back faulting DMAs: every fault counted, the log
 *                rate-limited, the kernel responsive.
 * Lines: `[IOMMU-TEST] ok|FAIL <name> ...`, then `[IOMMU-TEST] RESULT
 * pass=N fail=M`.
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef SECOS_IOMMU_TEST
#include "iommu.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "timer.h"
#include "debugcon.h"
#include <stddef.h>

#define EDU_ID        0x00
#define EDU_DMA_SRC   0x80
#define EDU_DMA_DST   0x88
#define EDU_DMA_CNT   0x90
#define EDU_DMA_CMD   0x98
#define EDU_CMD_RUN   0x1
#define EDU_CMD_TO_RAM 0x2       /* direction: edu buffer -> RAM */
#define EDU_BUF       0x40000ull
#define PG            4096ull
#define XFER          2048u      /* edu refuses a transfer ending at its
                                    buffer end (QEMU off-by-one: hw_error) */

static volatile uint8_t* g_edu;
static const pci_dev_t*  g_dev;
static uint16_t          g_bdf;
static int g_pass, g_fail;

static void line(const char* tag, const char* name, const char* extra, uint64_t v, int hv) {
    iommu_line_t l; l.n = 0;
    iommu_ln_s(&l, "[IOMMU-TEST] "); iommu_ln_s(&l, tag); iommu_ln_s(&l, " "); iommu_ln_s(&l, name);
    if (extra) { iommu_ln_s(&l, " "); iommu_ln_s(&l, extra); }
    if (hv) { iommu_ln_s(&l, " "); iommu_ln_hex(&l, v); }
    debugcon_writestring(iommu_ln_end(&l));
}
static void check(int ok, const char* name, const char* extra, uint64_t v, int hv) {
    if (ok) g_pass++; else g_fail++;
    line(ok ? "ok" : "FAIL", name, extra, v, hv);
}

static uint64_t faults(void) { iommu_fault_stats_t s; iommu_fault_stats(&s); return s.total; }

/* Run one DMA and wait for it; 0 or -1 (edu stuck). */
static int edu_dma(uint64_t src, uint64_t dst, uint32_t cnt, int to_ram) {
    volatile uint64_t* r64 = (volatile uint64_t*)(g_edu + EDU_DMA_SRC);
    r64[0] = src;
    r64[1] = dst;
    r64[2] = cnt;
    *(volatile uint64_t*)(g_edu + EDU_DMA_CMD) = EDU_CMD_RUN | (to_ram ? EDU_CMD_TO_RAM : 0);
    uint64_t t0 = timer_get_ticks();
    while (*(volatile uint64_t*)(g_edu + EDU_DMA_CMD) & EDU_CMD_RUN) {
        if (timer_get_ticks() - t0 > 3000) return -1;
        __asm__ volatile ("pause");
    }
    return 0;
}
/* Device write of the edu buffer to `iova` (XFER bytes). */
static int to_ram(uint64_t iova) { return edu_dma(EDU_BUF, iova, XFER, 1); }
/* Device read of `iova` into the edu buffer (XFER bytes). */
static int from_ram(uint64_t iova) { return edu_dma(iova, EDU_BUF, XFER, 0); }

/* Wait until the fault count reaches `want` (the fault IRQ normally lands
 * at once); poll the records as a fallback. */
static uint64_t wait_faults(uint64_t want) {
    uint64_t t0 = timer_get_ticks();
    while (faults() < want && timer_get_ticks() - t0 < 300) __asm__ volatile ("pause");
    if (faults() < want) iommu_fault_poll();
    return faults();
}

static uint8_t* va(uint64_t phys) { return (uint8_t*)phys_to_virt(phys); }
static void fill(uint64_t phys, uint8_t seed) { uint8_t* p = va(phys); for (uint64_t i = 0; i < PG; i++) p[i] = (uint8_t)(seed + i * 7); }
static int same(uint64_t phys, uint8_t seed) { uint8_t* p = va(phys); for (uint64_t i = 0; i < XFER; i++) if (p[i] != (uint8_t)(seed + i * 7)) return 0; return 1; }
static void set(uint64_t phys, uint8_t v) { uint8_t* p = va(phys); for (uint64_t i = 0; i < PG; i++) p[i] = v; }
static int all(uint64_t phys, uint8_t v) { uint8_t* p = va(phys); for (uint64_t i = 0; i < PG; i++) if (p[i] != v) return 0; return 1; }

static int last_is(uint64_t iova, uint8_t reason, int w) {
    iommu_fault_stats_t s; iommu_fault_stats(&s);
    return s.last.bdf == g_bdf && s.last.iova == iova && s.last.reason == reason && s.last.is_write == w;
}

static uint64_t frame(void) { return (uint64_t)pmm_alloc_below(0x10000000ull); /* edu: 28-bit */ }

static iommu_domain_t* new_dom(void) {
    iommu_domain_t* d = iommu_domain_new(IDOM_TRANSLATED);
    if (!d) return NULL;
    iommu_domain_set_dma_mask(d, 0x0FFFFFFFull);
    if (iommu_attach(d, g_dev)) { iommu_domain_free(d); return NULL; }
    return d;
}
static void drop_dom(iommu_domain_t* d) {
    iommu_attach(iommu_kernel_domain(), g_dev);   /* moves edu out of d */
    iommu_domain_free(d);
}

static uint32_t parse_u32(const char* s, uint32_t dflt) {
    uint32_t v = 0; int n = 0;
    while (*s >= '0' && *s <= '9' && n < 9) { v = v * 10 + (uint32_t)(*s - '0'); s++; n++; }
    return n ? v : dflt;
}

void iommu_test_run(void) {
    pci_device_t pd;
    if (pci_find(0x1234, 0x11e8, &pd) != 0) { debugcon_writestring("[IOMMU-TEST] no edu device: skipped\n"); return; }
    g_dev = pci_dev_of(&pd);
    if (!g_dev || !iommu_dev_covered(g_dev)) { debugcon_writestring("[IOMMU-TEST] edu not translated: skipped\n"); return; }
    g_bdf = (uint16_t)((pd.bus << 8) | (pd.slot << 3) | pd.func);
    pci_enable_mem_and_busmaster(&pd);
    g_edu = (volatile uint8_t*)pci_bar_map(&pd, 0, 0x100000);
    if (!g_edu) { check(0, "edu-bar", NULL, 0, 0); return; }
    uint32_t id = *(volatile uint32_t*)(g_edu + EDU_ID);
    char buf[16];
    uint32_t loops = 1000;
    if (iommu_cmdline_value("iommu.testloops", buf, sizeof(buf)) > 0) loops = parse_u32(buf, 1000);
    line("start", "edu", "ident", id, 1);
    check((id & 0xFFFF) == 0x00ed, "edu-ident", NULL, id, 1);

    uint64_t src = frame(), dst = frame(), pg = frame(), ro = frame(), ptp = frame();
    if (!src || !dst || !pg || !ro || !ptp) { check(0, "frames", NULL, 0, 0); return; }
    uint64_t f0 = faults();
    iommu_domain_t* d = new_dom();
    check(d != NULL, "domain-attach", NULL, 0, 0);
    if (!d) return;

    /* positive: RAM(src) -> edu -> RAM(dst) through IOVAs != phys */
    const uint64_t IS = 0x100000, ID = 0x200000, IU = 0x300000, IP = 0x400000, IR = 0x500000;
    fill(src, 0x11); set(dst, 0);
    int rc = iommu_map(d, IS, src, PG, IOMMU_READ);
    rc |= iommu_map(d, ID, dst, PG, IOMMU_READ | IOMMU_WRITE);
    check(rc == 0, "map", NULL, 0, 0);
    int prot = 0;
    check(iommu_translate(d, ID, &prot) == dst && prot == (IOMMU_READ | IOMMU_WRITE), "translate", NULL, 0, 0);
    check(iommu_map(d, ID, pg, PG, IOMMU_READ) == IOMMU_EEXIST, "remap-refused", NULL, 0, 0);
    int e1 = from_ram(IS), e2 = to_ram(ID);
    check(!e1 && !e2 && same(dst, 0x11) && faults() == f0, "positive-dma", "faults", faults() - f0, 1);

    /* negative 1: unmapped IOVA */
    uint64_t f1 = faults();
    to_ram(IU);
    uint64_t f1b = wait_faults(f1 + 1);
    check(f1b == f1 + 1 && last_is(IU, 0x05, 1), "unmapped-faults-once", "count", f1b - f1, 1);

    /* negative 2: unmap must flush the IOTLB */
    set(pg, 0);
    rc = iommu_map(d, IP, pg, PG, IOMMU_READ | IOMMU_WRITE);
    to_ram(IP);
    int ok_before = (rc == 0) && same(pg, 0x11);
    rc = iommu_unmap(d, IP, PG);
    set(pg, 0xEE);
    uint64_t f2 = faults();
    to_ram(IP);
    uint64_t f2b = wait_faults(f2 + 1);
    check(ok_before && rc == 0, "map-dma-unmap", NULL, 0, 0);
    check(f2b == f2 + 1 && all(pg, 0xEE) && last_is(IP, 0x05, 1), "dma-after-unmap-faults", "count", f2b - f2, 1);

    /* negative 3: read-only page */
    fill(ro, 0x5A);
    rc = iommu_map(d, IR, ro, PG, IOMMU_READ);
    uint64_t f3 = faults();
    to_ram(IR);
    uint64_t f3b = wait_faults(f3 + 1);
    check(rc == 0 && f3b == f3 + 1 && same(ro, 0x5A) && last_is(IR, 0x05, 1), "ro-write-faults", NULL, 0, 0);
    set(dst, 0);
    from_ram(IR); to_ram(ID);
    check(same(dst, 0x5A) && faults() == f3b, "ro-read-works", NULL, 0, 0);

    /* negative 4: DMA mask / hardware width */
    check(iommu_map(d, 0x10000000ull, pg, PG, IOMMU_READ) == IOMMU_ERANGE, "mask-28bit-refused", NULL, 0, 0);
    check(iommu_iova_alloc(d, 0x10000000ull) == 0, "iova-alloc-mask", NULL, 0, 0);
    uint64_t a1 = iommu_iova_alloc(d, PG);
    check(a1 && a1 + PG - 1 <= 0x0FFFFFFFull, "iova-alloc-below-mask", NULL, a1, 1);
    check(iommu_map(d, 1ull << 48, pg, PG, IOMMU_READ) == IOMMU_ERANGE, "beyond-agaw-refused", NULL, 0, 0);

    /* blocked */
    iommu_unmap(d, IS, PG); iommu_unmap(d, ID, PG); iommu_unmap(d, IR, PG);
    iommu_detach(g_dev);
    check(iommu_dev_domain(g_dev) == NULL, "detached", NULL, 0, 0);
    uint64_t f4 = faults();
    to_ram(ID);
    uint64_t f4b = wait_faults(f4 + 1);
    iommu_fault_stats_t st; iommu_fault_stats(&st);
    check(f4b == f4 + 1 && st.last.bdf == g_bdf && st.last.reason == 0x02, "blocked-faults", "reason", st.last.reason, 1);
    check(iommu_domain_free(d) == 0, "domain-free", NULL, 0, 0);

    /* passthrough: back in the kernel domain, IOVA == phys */
    check(iommu_attach(iommu_kernel_domain(), g_dev) == 0, "reattach-kernel", NULL, 0, 0);
    set(ptp, 0);
    uint64_t f5 = faults();
    to_ram(ptp);
    check(same(ptp, 0x5A) && faults() == f5, "passthrough-dma", NULL, 0, 0);

    /* loop: map/DMA/unmap/DMA-must-fault; domain recreated every 100 */
    iommu_fault_stats_t s0; iommu_fault_stats(&s0);
    d = new_dom(); drop_dom(d);                    /* warm the heap */
    uint64_t free0 = pmm_get_free_memory();
    d = new_dom();
    uint32_t bad = 0, i;
    uint64_t lf0 = faults();
    for (i = 0; i < loops && d; i++) {
        if (i && i % 100 == 0) { drop_dom(d); d = new_dom(); if (!d) break; }
        uint64_t io = iommu_iova_alloc(d, PG);
        if (!io) { bad++; break; }
        set(pg, 0);
        if (iommu_map(d, io, pg, PG, IOMMU_READ | IOMMU_WRITE)) { bad++; break; }
        to_ram(io);
        if (!same(pg, 0x5A)) bad++;
        if (iommu_unmap(d, io, PG)) bad++;
        set(pg, 0xEE);
        uint64_t fb = faults();
        to_ram(io);
        if (wait_faults(fb + 1) != fb + 1 || !all(pg, 0xEE)) bad++;
        if (bad > 3) break;
    }
    if (d) drop_dom(d);
    uint64_t free1 = pmm_get_free_memory();
    iommu_fault_stats_t s1; iommu_fault_stats(&s1);
    check(bad == 0 && i == loops, "loop", "rounds", i, 1);
    check(faults() - lf0 == loops, "loop-fault-count", NULL, faults() - lf0, 1);
    check(free1 == free0, "loop-memfree-steady", "delta-bytes", free0 - free1, 1);

    /* storm: back-to-back faults, the log stays rate-limited */
    d = new_dom();
    uint64_t sf0 = faults(), t0 = timer_get_ticks();
    iommu_fault_stats_t a; iommu_fault_stats(&a);
    for (uint32_t k = 0; k < 60 && d; k++) to_ram(0x7000000 + k * PG);
    wait_faults(sf0 + 60);
    iommu_fault_stats_t b; iommu_fault_stats(&b);
    uint64_t secs = (timer_get_ticks() - t0) / 1000 + 1;
    check(b.total - a.total == 60, "storm-all-counted", NULL, b.total - a.total, 1);
    check(b.logged - a.logged <= 5 * (secs + 1) && b.suppressed > a.suppressed, "storm-rate-limited",
          "logged", b.logged - a.logged, 1);
    check(b.irqs > s0.irqs, "fault-irq-delivered", "irqs", b.irqs, 1);
    if (d) drop_dom(d);
    pmm_free_frame((void*)src); pmm_free_frame((void*)dst); pmm_free_frame((void*)pg);
    pmm_free_frame((void*)ro); pmm_free_frame((void*)ptp);

    iommu_line_t l; l.n = 0;
    iommu_ln_s(&l, "[IOMMU-TEST] RESULT pass="); iommu_ln_dec(&l, (uint64_t)g_pass);
    iommu_ln_s(&l, " fail="); iommu_ln_dec(&l, (uint64_t)g_fail);
    iommu_ln_s(&l, " faults="); iommu_ln_dec(&l, b.total);
    iommu_ln_s(&l, " irqs="); iommu_ln_dec(&l, b.irqs);
    iommu_ln_s(&l, " polled="); iommu_ln_dec(&l, b.polled);
    iommu_ln_s(&l, " suppressed="); iommu_ln_dec(&l, b.suppressed);
    debugcon_writestring(iommu_ln_end(&l));
}
#endif /* SECOS_IOMMU_TEST */
