/*
 * udrv_edu.c — [M57] ring-3 driver for QEMU's "edu" educational PCI device
 * (1234:11e8, QEMU docs/specs/edu.rst) on the Driver Space v2 API
 * (user/include/secos/udrv.h). The UIO/VFIO sample driver of SecOS: the
 * positive half of the M57 gate (tools/udrv_gate.sh).
 *
 *   1. CLAIM the device (by vendor:device, 28-bit DMA mask: edu's default)
 *      and print INFO;
 *   2. MAP_BAR 0: identification register (0x00), liveness check (0x04
 *      returns the inverted value), factorials (0x08, status 0x20 bit 0);
 *   3. IRQ_SETUP 1 vector (MSI): edu raises an interrupt on a write of 0x60;
 *      the driver blocks in read() of the claim fd, checks the interrupt
 *      status (0x24), acks the device (0x64) and writes the ack to the fd
 *      (which unmasks the vector). N rounds, plus one through poll();
 *   4. DMA_ALLOC 2 pages; edu's DMA engine copies 2 KiB RAM -> edu buffer
 *      (device address 0x40000) and back into the second page; completion
 *      by interrupt (status 0x100); the bytes must match and iova != phys is
 *      implied (the device only sees the IOVA the kernel gave us);
 *   5. DMA_FREE, RELEASE; the fd then reads -ENODEV.
 *
 * Copies stay at 2 KiB: QEMU's edu aborts on a DMA that ends exactly at the
 * end of its 4 KiB buffer (edu_check_range treats the end as exclusive).
 *
 * usage: udrv_edu [-n ROUNDS] [--serve | --hold]
 *   -n       IRQ round trips (default 10000);
 *   --hold   after a passing run take a second claim with the BAR, bus
 *            mastering, an armed vector and 16 KiB of DMA, and exit without
 *            RELEASE (the exit-path teardown test);
 *   --serve  the same claim, then sleep in read() of its fd until killed
 *            (the init respawn test).
 * Output: "[udrv_edu] <check> ok|FAIL ..." lines, then
 * "[udrv_edu] RESULT pass=N fail=M" and "[udrv_edu] PASS" / "FAIL" on stdout
 * (the kernel mirrors console output to debugcon). Exit status 0 = PASS.
 * SPDX-License-Identifier: MIT
 */
#include "libsecos.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <poll.h>
#include <secos/udrv.h>

#define EDU_ID        0x00
#define EDU_LIVE      0x04
#define EDU_FACT      0x08
#define EDU_STATUS    0x20
#define EDU_IRQ_STAT  0x24
#define EDU_IRQ_RAISE 0x60
#define EDU_IRQ_ACK   0x64
#define EDU_DMA_SRC   0x80
#define EDU_DMA_DST   0x88
#define EDU_DMA_CNT   0x90
#define EDU_DMA_CMD   0x98
#define EDU_ST_COMPUTING 0x01
#define EDU_DMA_RUN   0x1
#define EDU_DMA_TO_RAM 0x2            /* direction bit: edu buffer -> RAM */
#define EDU_DMA_IRQ   0x4
#define EDU_DMA_IRQV  0x100           /* interrupt status of a finished DMA */
#define EDU_BUF       0x40000ull      /* device address of edu's 4 KiB buffer */
#define EDU_DMA_MASK  0x0FFFFFFFull   /* edu's default dma_mask (28 bits) */
#define COPY_LEN      2048u

static int g_pass, g_fail;
static int g_fd = -1;
static volatile uint8_t* g_bar;

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(b) - 1) n = (int)sizeof(b) - 1;
    write(1, b, (size_t)n);                   /* one write per line */
}

static void check(const char* what, int ok, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void check(const char* what, int ok, const char* fmt, ...) {
    char d[160];
    d[0] = 0;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(d, sizeof(d), fmt, ap);
        va_end(ap);
    }
    if (ok) g_pass++; else g_fail++;
    say("[udrv_edu] %s %s %s\n", what, ok ? "ok" : "FAIL", d);
}

static long udrv(long op, void* arg, unsigned long size) { return secos_udrv(op, g_fd, arg, size); }
/* Raw read()/write() of the claim fd: the kernel's -errno, not libc's -1. */
static long fd_read(void* b, long n) { return secos_syscall(3 /*SYS_READ*/, g_fd, (long)b, n, 0, 0); }
static long fd_write(const void* b, long n) { return secos_syscall(2 /*SYS_WRITE*/, g_fd, (long)b, n, 0, 0); }

static uint32_t rd32(uint32_t off) { return *(volatile uint32_t*)(g_bar + off); }
static void wr32(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_bar + off) = v; }
static void wr64(uint32_t off, uint64_t v) { *(volatile uint64_t*)(g_bar + off) = v; }
static uint64_t rd64(uint32_t off) { return *(volatile uint64_t*)(g_bar + off); }

static uint32_t fact(uint32_t n) { uint32_t r = 1; while (n > 1) r *= n--; return r; }

/* Block for the next interrupt; returns the events read (count 0 = error). */
static struct udrv_events wait_irq(long* rc) {
    struct udrv_events ev;
    memset(&ev, 0, sizeof(ev));
    *rc = fd_read(&ev, sizeof(ev));
    return ev;
}

static int ack_fd(void) {
    struct udrv_ack a = { 1, 0 };
    return (int)fd_write(&a, sizeof(a));
}

static int claim(void) {
    struct udrv_claim c;
    memset(&c, 0, sizeof(c));
    c.vendor = 0x1234; c.device = 0x11e8;
    c.flags = UDRV_CLAIM_BY_ID;
    c.dma_mask = EDU_DMA_MASK;
    return (int)secos_udrv(UDRV_OP_CLAIM, 0, &c, sizeof(c));
}

static int map_bar0(void) {
    struct udrv_map_bar mb;
    memset(&mb, 0, sizeof(mb));
    mb.bar = 0;
    long r = udrv(UDRV_OP_MAP_BAR, &mb, sizeof(mb));
    check("map-bar0", r == 0 && mb.uva && mb.len >= 0x1000, "rc=%ld uva=0x%lx len=0x%lx", r,
          (unsigned long)mb.uva, (unsigned long)mb.len);
    if (r != 0) return -1;
    g_bar = (volatile uint8_t*)(uintptr_t)mb.uva;
    return 0;
}

static void test_info(struct udrv_info* in) {
    memset(in, 0, sizeof(*in));
    long r = udrv(UDRV_OP_INFO, in, sizeof(*in));
    check("info", r == 0 && in->vendor == 0x1234 && in->device == 0x11e8, "rc=%ld", r);
    say("[udrv_edu] info %02x:%02x.%x %04x:%04x class %02x.%02x.%02x rev %u bars %u bar0 0x%lx "
        "msix %u msi %u%s iommu %u ir %u dma_ok %u unsafe %u flr %u pm %u dma_max %lu cfgw 0x%x barmask 0x%x\n",
        in->bus, in->dev, in->fn, in->vendor, in->device, in->class_code, in->subclass, in->prog_if,
        in->revision, in->nbars, (unsigned long)in->bar_size[0], in->msix_count, in->msi_max,
        in->msi_pvm ? "+pvm" : "", in->iommu, in->ir, in->dma_ok, in->unsafe, in->flr, in->pm_reset,
        (unsigned long)in->dma_max, in->cfg_write_mask, in->bar_mask);
}

static void test_mmio(void) {
    uint32_t id = rd32(EDU_ID);
    check("id-register", (id & 0xFF) == 0xED, "id=0x%08x (version %u.%u)", id, id >> 24, (id >> 16) & 0xFF);
    int live_ok = 1;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t v = 0x9E3779B9u * (i + 1);
        wr32(EDU_LIVE, v);
        uint32_t g = rd32(EDU_LIVE);
        if (g != ~v) { live_ok = 0; bad = g; break; }
    }
    check("liveness", live_ok, live_ok ? "64 values inverted" : "got 0x%08x", bad);
    int fact_ok = 1;
    uint32_t fn = 0, fgot = 0;
    for (uint32_t n = 0; n <= 12 && fact_ok; n++) {
        wr32(EDU_FACT, n);
        int spins = 0;
        while ((rd32(EDU_STATUS) & EDU_ST_COMPUTING) && spins < 10000000) spins++;
        fgot = rd32(EDU_FACT);
        if (fgot != fact(n)) { fact_ok = 0; fn = n; }
    }
    check("factorial", fact_ok, fact_ok ? "0!..12! (12! = %u)" : "n=%u got %u", fact_ok ? fact(12) : fn,
          fgot);
}

/* Enable memory decode + bus mastering (needed for MSI and DMA on QEMU). */
static int enable_bm(void) {
    struct udrv_cfg cf = { 0x04, 2, 0x0006, 0 };
    long r = udrv(UDRV_OP_CFG_WRITE, &cf, sizeof(cf));
    struct udrv_cfg rd = { 0x04, 2, 0, 0 };
    long r2 = udrv(UDRV_OP_CFG_READ, &rd, sizeof(rd));
    check("busmaster-on", r == 0 && r2 == 0 && (rd.val & 0x6) == 0x6, "rc=%ld cmd=0x%04x", r, rd.val);
    return r == 0 ? 0 : -1;
}

static int test_irq(int rounds) {
    struct udrv_irq iq = { 1, 0 };
    long r = udrv(UDRV_OP_IRQ_SETUP, &iq, sizeof(iq));
    check("irq-setup", r == 0 && iq.mode == UDRV_IRQ_MSI, "rc=%ld mode=%u", r, iq.mode);
    if (r != 0) return -1;
    /* IRQ_SETUP leaves the vector masked: the first ack enables it. */
    int a0 = ack_fd();
    check("irq-unmask", a0 == (int)sizeof(struct udrv_ack), "rc=%d", a0);
    /* Nothing is pending yet: a non-blocking poll says "not readable". */
    struct pollfd pf = { g_fd, POLLIN, 0 };
    int pr = poll(&pf, 1, 0);
    check("poll-idle", pr == 0, "rc=%d revents=0x%x", pr, pf.revents);

    uint64_t last = 0;
    int bad = 0;
    long brc = 0;
    uint32_t bst = 0;
    uint64_t bcount = 0;
    for (int i = 0; i < rounds && !bad; i++) {
        uint32_t val = 1u << (16 + (i & 7));
        wr32(EDU_IRQ_RAISE, val);
        long rc;
        struct udrv_events ev = wait_irq(&rc);
        uint32_t st = rd32(EDU_IRQ_STAT);
        if (rc != (long)sizeof(ev) || ev.irq_count != last + 1 || !(ev.vec_pending_mask & 1) || st != val) {
            bad = i + 1; brc = rc; bst = st; bcount = ev.irq_count;
            break;
        }
        last = ev.irq_count;
        wr32(EDU_IRQ_ACK, val);
        if (ack_fd() != (int)sizeof(struct udrv_ack)) { bad = i + 1; brc = -1000; break; }
    }
    if (bad)
        check("irq-rounds", 0, "round %d: read rc=%ld count=%lu (want %lu) status=0x%x", bad, brc,
              (unsigned long)bcount, (unsigned long)(last + 1), bst);
    else
        check("irq-rounds", 1, "%d interrupts, one per raise, count=%lu", rounds, (unsigned long)last);
    if (bad) return -1;

    /* One more through poll(): the IRQ wakes the poller (POLLIN). */
    wr32(EDU_IRQ_RAISE, 0x1);
    pf.revents = 0;
    pr = poll(&pf, 1, 2000);
    check("irq-poll", pr == 1 && (pf.revents & POLLIN), "rc=%d revents=0x%x", pr, pf.revents);
    long rc;
    struct udrv_events ev = wait_irq(&rc);
    check("irq-poll-read", rc == (long)sizeof(ev) && ev.irq_count == last + 1, "rc=%ld count=%lu", rc,
          (unsigned long)ev.irq_count);
    wr32(EDU_IRQ_ACK, 0x1);
    ack_fd();
    struct udrv_info in;
    memset(&in, 0, sizeof(in));
    long ir = udrv(UDRV_OP_INFO, &in, sizeof(in));
    check("irq-count-info", ir == 0 && in.irq_count == last + 1 && in.irq_mode == UDRV_IRQ_MSI,
          "count=%lu mode=%u", (unsigned long)in.irq_count, in.irq_mode);
    return 0;
}

/* One edu DMA, completion by interrupt. */
static int edu_dma(uint64_t src, uint64_t dst, uint32_t cmd, const char* what) {
    wr64(EDU_DMA_SRC, src);
    wr64(EDU_DMA_DST, dst);
    wr64(EDU_DMA_CNT, COPY_LEN);
    wr64(EDU_DMA_CMD, EDU_DMA_RUN | EDU_DMA_IRQ | cmd);
    long rc;
    struct udrv_events ev = wait_irq(&rc);
    uint32_t st = rd32(EDU_IRQ_STAT);
    uint64_t c = rd64(EDU_DMA_CMD);
    int ok = rc == (long)sizeof(ev) && (st & EDU_DMA_IRQV) && !(c & EDU_DMA_RUN);
    check(what, ok, "rc=%ld irq_status=0x%x cmd=0x%lx src=0x%lx dst=0x%lx", rc, st, (unsigned long)c,
          (unsigned long)src, (unsigned long)dst);
    wr32(EDU_IRQ_ACK, st);
    ack_fd();
    return ok ? 0 : -1;
}

static int test_dma(void) {
    struct udrv_dma dm;
    memset(&dm, 0, sizeof(dm));
    dm.size = 2 * 4096;
    dm.dir = UDRV_DMA_BIDIR;
    long r = udrv(UDRV_OP_DMA_ALLOC, &dm, sizeof(dm));
    check("dma-alloc", r == 0 && dm.uva && dm.iova && dm.iova + dm.size - 1 <= EDU_DMA_MASK,
          "rc=%ld uva=0x%lx iova=0x%lx", r, (unsigned long)dm.uva, (unsigned long)dm.iova);
    if (r != 0) return -1;
    uint8_t* a = (uint8_t*)(uintptr_t)dm.uva;
    uint8_t* b = a + 4096;
    int zero = 1;
    for (uint32_t i = 0; i < 2 * 4096; i++) if (a[i]) { zero = 0; break; }
    check("dma-zeroed", zero, NULL);
    for (uint32_t i = 0; i < COPY_LEN; i++) a[i] = (uint8_t)(i * 7 + 0x5A + (i >> 8));
    int ok = edu_dma(dm.iova, EDU_BUF, 0, "dma-ram-to-edu") == 0 &&
             edu_dma(EDU_BUF, dm.iova + 4096, EDU_DMA_TO_RAM, "dma-edu-to-ram") == 0;
    if (ok) {
        int same = memcmp(a, b, COPY_LEN) == 0;
        int first = -1;
        for (uint32_t i = 0; i < COPY_LEN && !same; i++) if (a[i] != b[i]) { first = (int)i; break; }
        int tail = 1;
        for (uint32_t i = COPY_LEN; i < 4096; i++) if (b[i]) { tail = 0; break; }
        check("dma-data", same && tail, same ? "2048 bytes match, rest of page untouched" : "first diff at %d",
              first);
    }
    struct udrv_dma fr;
    memset(&fr, 0, sizeof(fr));
    fr.uva = dm.uva;
    long f = udrv(UDRV_OP_DMA_FREE, &fr, sizeof(fr));
    check("dma-free", f == 0, "rc=%ld", f);
    struct udrv_info in;
    memset(&in, 0, sizeof(in));
    udrv(UDRV_OP_INFO, &in, sizeof(in));
    check("dma-accounting", in.dma_used == 0, "dma_used=%lu", (unsigned long)in.dma_used);
    return ok ? 0 : -1;
}

static int run(int rounds) {
    g_fd = claim();
    check("claim", g_fd >= 0, "fd=%d", g_fd);
    if (g_fd < 0) return -1;
    struct udrv_info in;
    test_info(&in);
    if (map_bar0() != 0) return -1;
    test_mmio();
    if (!in.dma_ok) {
        /* No IOMMU (or no IR without an override): MMIO only. */
        struct udrv_dma dm;
        memset(&dm, 0, sizeof(dm));
        dm.size = 4096; dm.dir = UDRV_DMA_BIDIR;
        long r = udrv(UDRV_OP_DMA_ALLOC, &dm, sizeof(dm));
        say("[udrv_edu] mmio-only: dma_alloc rc=%ld\n", r);
        struct udrv_cfg cf = { 0x04, 2, 0x0006, 0 };
        long rb = udrv(UDRV_OP_CFG_WRITE, &cf, sizeof(cf));
        struct udrv_irq iq = { 1, 0 };
        long ri = udrv(UDRV_OP_IRQ_SETUP, &iq, sizeof(iq));
        say("[udrv_edu] mmio-only: busmaster rc=%ld irq_setup rc=%ld\n", rb, ri);
        check("mmio-only-refusals", r == -19 && rb == -1 && ri == -1, "dma_alloc %ld busmaster %ld irq_setup %ld",
              r, rb, ri);
    } else {
        if (enable_bm() == 0 && test_irq(rounds) == 0) test_dma();
    }
    long rr = udrv(UDRV_OP_RELEASE, NULL, 0);
    check("release", rr == 0, "rc=%ld", rr);
    struct udrv_events ev;
    long after = fd_read(&ev, sizeof(ev));
    check("fd-after-release", after == -19, "read rc=%ld", after);
    close(g_fd);
    g_fd = -1;
    return 0;
}

/* A second claim with everything a driver holds: the BAR, bus mastering, an
 * armed MSI vector and a DMA buffer. 0 = all granted. */
static int hold_claim(const char* what) {
    g_fd = claim();
    if (g_fd < 0) { say("[udrv_edu] %s claim rc=%d\n", what, g_fd); return -1; }
    struct udrv_irq iq = { 1, 0 };
    long m = map_bar0();
    long b = enable_bm();
    long r = udrv(UDRV_OP_IRQ_SETUP, &iq, sizeof(iq));
    struct udrv_dma dm;
    memset(&dm, 0, sizeof(dm));
    dm.size = 4 * 4096; dm.dir = UDRV_DMA_BIDIR;
    long d = udrv(UDRV_OP_DMA_ALLOC, &dm, sizeof(dm));
    long k = ack_fd();
    say("[udrv_edu] %s pid=%d fd=%d bar=%ld bm=%ld irq=%ld dma=%ld\n", what, getpid(), g_fd, m, b, r, d);
    return (m || b || r || d || k != (long)sizeof(struct udrv_ack)) ? -1 : 0;
}

int main(int argc, char** argv) {
    int rounds = 10000, serve = 0, hold = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) rounds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--serve")) serve = 1;
        else if (!strcmp(argv[i], "--hold")) hold = 1;
        else { say("usage: udrv_edu [-n ROUNDS] [--serve | --hold]\n"); return 2; }
    }
    if (rounds < 1) rounds = 1;
    say("[udrv_edu] start pid=%d rounds=%d%s\n", getpid(), rounds, serve ? " serve" : hold ? " hold" : "");
    run(rounds);
    say("[udrv_edu] RESULT pass=%d fail=%d\n", g_pass, g_fail);
    say("[udrv_edu] %s pid=%d\n", g_fail ? "FAIL" : "PASS", getpid());
    if (g_fail) return 1;
    if (hold) {
        /* Exit with the claim held: the kernel's exit path must undo it. */
        return hold_claim("holding") == 0 ? 0 : 1;
    }
    if (serve) {
        /* Hold a claim and sleep in read() of its fd until killed. */
        if (hold_claim("serving") != 0) return 1;
        for (;;) {
            struct udrv_events ev;
            long rc = fd_read(&ev, sizeof(ev));
            if (rc < 0 && rc != -4) { say("[udrv_edu] serve read rc=%ld\n", rc); return 1; }
            if (rc > 0) { wr32(EDU_IRQ_ACK, rd32(EDU_IRQ_STAT)); ack_fd(); }
        }
    }
    return 0;
}
