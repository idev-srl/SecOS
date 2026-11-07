/*
 * exec_test.c — [M51-C6] boot self-test of concurrent signed-image loads
 * (built only with -DSECOS_EXEC_TEST; the selftest's "M51 EXEC" gate).
 *
 * With the BKL on, every exec/spawn syscall is serialized, so the load path's
 * own reentrancy (no shared staging buffer, signature checked on the pinned
 * copy, lock-free revocation reads) cannot be exercised from user space yet.
 * This test runs the load path itself on every online CPU at once, outside the
 * BKL: the BSP from kernel_main and each AP from its idle loop
 * (kernel/smp.c), all creating processes from signed images and tearing them
 * down again, through both entry points:
 *   - process_create_from_elf_args (copy path: kernel shell, boot demos);
 *   - process_create_from_elf_owned (the SYS_SPAWN path: a private kmalloc'd
 *     buffer handed over as the pinned image);
 * on three signed images (hello 14 KB, coreutils 42 KB, bash 1.1 MB) and on
 * tampered copies of them (one flipped byte), which must be refused. Every
 * accepted image's pinned copy must equal the embedded original byte for byte
 * (a shared buffer refilled by another CPU would show up here), and the
 * copy path must not pin the caller's buffer. Meanwhile every worker also
 * revokes the same small set of unused key ids, which must end up in the
 * revocation list exactly once each.
 *
 * Leak check: one parallel warm-up round (heap growth happens there), then the
 * free PMM memory is sampled before and after a serial round and two parallel
 * rounds; it must not drop. A heap_check() runs at the end.
 *
 * Timing: the serial round (BSP alone) and a parallel round (every CPU, the
 * same per-CPU work) give the load throughput with one and with N CPUs; the
 * speedup is reported (it is what dropping the BKL around exec will buy).
 *
 * Output: "[EXECTEST] <check>: OK|FAIL", ending "[EXECTEST] DONE ok=N fail=M".
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef SECOS_EXEC_TEST
#include <stdint.h>
#include <stddef.h>
#include "process.h"
#include "percpu.h"
#include "debugcon.h"
#include "heap.h"
#include "lockdep.h"
#include "../mm/pmm.h"
#include "../mm/elf_sign.h"
#include "../crypto/user_hello_elf.h"
#include "../crypto/user_coreutils_elf.h"
#ifndef SECOS_NO_BASH
#include "../crypto/user_bash_elf.h"
#endif

extern uint64_t ktime_us(void);
extern uint32_t smp_online_count(void);
extern process_t* process_detach_unstarted(uint32_t pid);
extern int process_destroy(process_t* p);
extern void process_reap_done(void);

#define ET_ITERS      12          /* loads per CPU per round              */
#define ET_REVOKE_N   6           /* key ids every worker revokes         */
#define ET_REVOKE_ID0 0x5EC60000u /* unused by any signer (test keys: 0)  */
#define ET_TIMEOUT_US 120000000ull

typedef struct { const uint8_t* img; size_t len; uint8_t* bad; const char* name; } et_image_t;
static et_image_t g_img[3];
static int g_nimg;

static volatile uint32_t g_round;                 /* 0 = idle, else round id    */
static volatile uint32_t g_seen[SMP_MAX_CPUS];    /* last round a CPU joined    */
static volatile uint32_t g_done, g_joined;
static volatile uint32_t g_ok_loads, g_refused, g_bad;   /* per round          */
static volatile uint32_t g_ran[SMP_MAX_CPUS];
static int g_ok, g_fail;

static void say(const char* s) { debugcon_writestring(s); }
static void dec(uint64_t v) {
    char b[24]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) debugcon_putchar(b[--n]);
}
static void check(const char* name, int good) {
    say("[EXECTEST] "); say(name); say(good ? ": OK\n" : ": FAIL\n");
    if (good) g_ok++; else g_fail++;
}
static void bad(const char* what, const char* img) {
    __atomic_add_fetch(&g_bad, 1, __ATOMIC_SEQ_CST);
    say("[EXECTEST] error: "); say(what); say(" ("); say(img); say(")\n");
}

static int same(const uint8_t* a, const uint8_t* b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Load one image through one entry point and tear the process down again. */
static void et_one(uint32_t cpu, uint32_t i) {
    const et_image_t* im = &g_img[(cpu + i) % (uint32_t)g_nimg];
    int tampered = (i & 2) != 0;
    int owned = (i & 1) != 0;
    const uint8_t* src = tampered ? im->bad : im->img;
    process_t* p;
    uint8_t* buf = NULL;
    if (owned) {
        buf = (uint8_t*)kmalloc(im->len);
        if (!buf) { bad("kmalloc", im->name); return; }
        for (size_t k = 0; k < im->len; k++) buf[k] = src[k];
        p = process_create_from_elf_owned(buf, im->len, 0, NULL);
    } else {
        p = process_create_from_elf_args(src, im->len, 0, NULL);
    }
    if (tampered) {
        if (p) bad("tampered image accepted", im->name);
        else __atomic_add_fetch(&g_refused, 1, __ATOMIC_SEQ_CST);
    } else if (!p) {
        bad("signed image refused", im->name);
    } else {
        if (!p->image || p->image_size != im->len || !same(p->image, im->img, im->len))
            bad("pinned image differs from the signed original", im->name);
        else if (owned && p->image != buf)
            bad("owned buffer not pinned", im->name);
        else if (!owned && p->image == src)
            bad("copy path pinned the caller's buffer", im->name);
        else
            __atomic_add_fetch(&g_ok_loads, 1, __ATOMIC_SEQ_CST);
    }
    elf_sign_revoke_key(ET_REVOKE_ID0 + (cpu + i) % ET_REVOKE_N);
    if (p) {
        process_t* d = process_detach_unstarted(p->pid);
        if (!d) { bad("could not detach the unstarted process", im->name); return; }
        process_destroy(d);
        process_reap_done();
    }
}

static void et_worker(uint32_t cpu, uint32_t iters) {
    for (uint32_t i = 0; i < iters; i++) et_one(cpu, i);
    if (cpu < SMP_MAX_CPUS) g_ran[cpu] += iters;
}

/* AP idle hook: join a round once. */
void exectest_ap(cpu_t* c) {
    uint32_t r = __atomic_load_n(&g_round, __ATOMIC_ACQUIRE);
    if (!r || c->index >= SMP_MAX_CPUS || g_seen[c->index] == r) return;
    g_seen[c->index] = r;
    __atomic_add_fetch(&g_joined, 1, __ATOMIC_SEQ_CST);
    et_worker(c->index, ET_ITERS);
    __atomic_add_fetch(&g_done, 1, __ATOMIC_SEQ_CST);
}

/* One round: the BSP works too; with 'parallel' the online APs join. Returns
 * the wall time in us, or 0 on a timeout. */
static uint64_t et_round(int parallel, uint32_t naps) {
    static uint32_t rid;
    g_ok_loads = g_refused = 0;
    g_done = 0; g_joined = 0;
    uint64_t t0 = ktime_us();
    if (parallel) __atomic_store_n(&g_round, ++rid, __ATOMIC_RELEASE);
    et_worker(0, ET_ITERS);
    while (parallel && g_done < naps) {
        if (ktime_us() - t0 > ET_TIMEOUT_US) { g_round = 0; return 0; }
        __asm__ volatile("pause");
    }
    __atomic_store_n(&g_round, 0, __ATOMIC_RELEASE);
    uint64_t dt = ktime_us() - t0;
    return dt ? dt : 1;
}

#ifdef SECOS_LOCKDEP
static void et_lockstat_emit(void* ctx, const char* s) {
    (void)ctx;
    say("[EXECTEST] lockstat "); say(s);
}
#endif

void exectest_run(void) {
    say("[EXECTEST] concurrent signed-image load self-test\n");
    g_img[0] = (et_image_t){ user_hello_elf, user_hello_elf_len, NULL, "hello" };
    g_img[1] = (et_image_t){ user_coreutils_elf, user_coreutils_elf_len, NULL, "coreutils" };
    g_nimg = 2;
#ifndef SECOS_NO_BASH
    g_img[2] = (et_image_t){ user_bash_elf, user_bash_elf_len, NULL, "bash" };
    g_nimg = 3;
#endif
    for (int k = 0; k < g_nimg; k++) {
        g_img[k].bad = (uint8_t*)kmalloc(g_img[k].len);
        if (!g_img[k].bad) { check("allocate tampered copies", 0); return; }
        for (size_t b = 0; b < g_img[k].len; b++) g_img[k].bad[b] = g_img[k].img[b];
        g_img[k].bad[g_img[k].len / 2] ^= 0x01;           /* one flipped byte */
    }
    uint32_t ncpu = smp_online_count();
    uint32_t naps = ncpu > 1 ? ncpu - 1 : 0;
    if (naps > SMP_MAX_CPUS - 1) naps = SMP_MAX_CPUS - 1;
    say("[EXECTEST] cpus="); dec(ncpu); say(" images="); dec((uint64_t)g_nimg);
    say(" loads/cpu/round="); dec(ET_ITERS); say("\n");
    int revoked0 = elf_sign_revoked_count();

    /* Warm-up (parallel): the heap grows for N in-flight images here. The
     * heap never gives frames back, so the leak check below accounts its
     * footprint: PMM free + heap footprint must stay put (no frame leaked:
     * page tables, kernel stacks, image frames) and the bytes in use must
     * come back exactly (no kmalloc leaked). */
    g_bad = 0;
    int warm = et_round(naps > 0, naps) != 0;
    check("warm-up round completed", warm);
    uint64_t u0, h0;
    heap_usage(&u0, &h0);
    uint64_t f0 = pmm_get_free_memory() + h0;

    uint64_t ts = et_round(0, 0);
    uint32_t want_ok = ET_ITERS / 2, want_ref = ET_ITERS / 2;
    check("serial: every signed image loaded, every tampered one refused",
          ts && g_ok_loads == want_ok && g_refused == want_ref);

    uint64_t tp = 0;
    if (naps == 0) {
        say("[EXECTEST] one CPU online: parallel rounds skipped\n");
    } else {
        for (int r = 0; r < 2; r++) {
            uint64_t t = et_round(1, naps);
            uint32_t n = naps + 1;
            check(r ? "parallel round 2: all CPUs finished" : "parallel round 1: all CPUs finished",
                  t != 0 && g_done == naps);
            check(r ? "parallel round 2: loads and refusals as expected"
                    : "parallel round 1: loads and refusals as expected",
                  g_ok_loads == want_ok * n && g_refused == want_ref * n);
            if (r == 0) tp = t;
        }
        check("at least two CPUs loaded images at once", g_joined >= 1);
    }
    check("no wrong image, no accepted tampered image, no refused signed one", g_bad == 0);

    uint64_t u1, h1;
    heap_usage(&u1, &h1);
    uint64_t f1 = pmm_get_free_memory() + h1;
    say("[EXECTEST] pmm free + heap footprint before="); dec(f0 / 1024); say(" KiB after=");
    dec(f1 / 1024); say(" KiB (heap grew "); dec((h1 - h0) / 1024); say(" KiB); heap in use before=");
    dec(u0); say(" after="); dec(u1); say(" bytes\n");
    check("no frame leak (PMM free + heap footprint steady)", f1 == f0);
    check("no kmalloc leak (heap bytes in use steady)", u1 == u0);
    check("heap consistent", heap_check() == 0);

    int rv = elf_sign_revoked_count() - revoked0;
    int all = 1;
    for (uint32_t k = 0; k < ET_REVOKE_N; k++) all &= elf_sign_key_revoked(ET_REVOKE_ID0 + k);
    say("[EXECTEST] revocation list: +"); dec((uint64_t)(rv < 0 ? 0 : rv)); say(" entries\n");
    check("concurrent revocations: each key listed exactly once", all && rv == ET_REVOKE_N);
    check("unrevoked key 0 still trusted", !elf_sign_key_revoked(0));

    for (uint32_t c = 0; c < ncpu && c < SMP_MAX_CPUS; c++) {
        say("[EXECTEST] cpu "); dec(c); say(" loads="); dec(g_ran[c]); say("\n");
    }
    if (ts && tp) {
        uint64_t n = (uint64_t)naps + 1;
        /* loads/s ratio x100: (n*ITERS/tp) / (ITERS/ts) */
        uint64_t sp = (n * ts * 100) / tp;
        say("[EXECTEST] serial "); dec(ts / 1000); say(" ms for "); dec(ET_ITERS);
        say(" loads; parallel "); dec(tp / 1000); say(" ms for "); dec(n * ET_ITERS);
        say(" loads; throughput x"); dec(sp / 100); say("."); dec((sp % 100) / 10); dec(sp % 10);
        say("\n");
    }
#ifdef SECOS_LOCKDEP
    /* Where the parallel rounds waited (the lockdep build serializes on the
     * heap: its 0x6B poisoning fills/scans every MB-sized image block under
     * heap_lock). */
    lockstat_generate(et_lockstat_emit, NULL);
#endif
    for (int k = 0; k < g_nimg; k++) kfree(g_img[k].bad);
    say("[EXECTEST] DONE ok="); dec((uint64_t)g_ok); say(" fail="); dec((uint64_t)g_fail); say("\n");
}
#endif /* SECOS_EXEC_TEST */
