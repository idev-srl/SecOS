/*
 * pagecache_test.c — [M50 B2] boot self-test of the lock-free page-cache fill
 * (built only with -DSECOS_PAGECACHE_TEST; the selftest's "M50 PAGECACHE"
 * gate). The races it checks cannot happen while the BKL serializes every
 * syscall and fault, so they are forced: pagecache.c calls
 * pagecache_test_fill_hook between the filesystem read of a fill and its
 * publication, and the hook makes "the other side" act in that window —
 * on this CPU (deterministic) and, with >= 2 CPUs online, on an AP (real
 * cross-CPU interleaving, driven from the AP idle loop).
 *
 * Every line is "[PCTEST] <name>: OK|FAIL"; the run ends with
 * "[PCTEST] DONE ok=N fail=M". The negative control (stale bit ignored) must
 * serve the stale page, which shows the test can see the bug it guards
 * against.
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef SECOS_PAGECACHE_TEST
#include <stdint.h>
#include "pagecache.h"
#include "vfs.h"
#include "percpu.h"
#include "spinlock.h"
#include "debugcon.h"

extern uint64_t ktime_us(void);
extern uint32_t smp_online_count(void);
extern void* kmalloc(unsigned long);
extern void kfree(void*);

#define PCT_PATH  "/pctest.bin"
#define PCT_BIG   "/pctest_big.bin"
#define PCT_PAGES 2
#define PCT_BIGPG 140     /* > PC_ENTRIES: reading it evicts everything */

static int g_ok, g_fail;
static vfs_inode_t* g_ino;
static uint8_t g_page[4096], g_tmp[4096];

static void pct_check(const char* name, int good) {
    debugcon_writestring("[PCTEST] ");
    debugcon_writestring(name);
    debugcon_writestring(good ? ": OK\n" : ": FAIL\n");
    if (good) g_ok++; else g_fail++;
}

/* Page 'pg' of version 'v': every 8-byte word = v<<32 | pg<<16 | word. */
static void pct_pattern(uint8_t* buf, uint32_t v, uint32_t pg) {
    uint64_t* w = (uint64_t*)buf;
    for (uint32_t i = 0; i < 512; i++)
        w[i] = ((uint64_t)v << 32) | ((uint64_t)pg << 16) | i;
}
static int pct_is(const uint8_t* buf, uint32_t v, uint32_t pg) {
    const uint64_t* w = (const uint64_t*)buf;
    for (uint32_t i = 0; i < 512; i++)
        if (w[i] != (((uint64_t)v << 32) | ((uint64_t)pg << 16) | i)) return 0;
    return 1;
}
/* The file write path of ksys_write: write, then invalidate. */
static void pct_write_page(vfs_inode_t* ino, uint32_t v, uint32_t pg) {
    static uint8_t wb[4096];
    pct_pattern(wb, v, pg);
    ino->ops->write(ino, (size_t)pg * 4096, wb, 4096);
    pagecache_invalidate(ino, (uint64_t)pg * 4096, 4096);
}
static int pct_read_is(vfs_inode_t* ino, uint32_t v, uint32_t pg) {
    for (int i = 0; i < 4096; i++) g_tmp[i] = 0;
    if (pagecache_read(ino, (uint64_t)pg * 4096, g_tmp, 4096) != 4096) return 0;
    return pct_is(g_tmp, v, pg);
}
static pagecache_stats_t pct_stats(void) { pagecache_stats_t s; pagecache_get_stats(&s); return s; }

/* ---- hooks (run between fill and publish) --------------------------------- */
static volatile int g_mode;      /* what the hook does on its next call */
static volatile int g_fired;

static void hook(vfs_inode_t* ino, uint64_t off);

/* AP side: one AP (index 1) acts when g_ap.phase is set. */
static volatile struct {
    uint32_t phase;     /* 1 write+invalidate, 2 read (waits), 3 read (times out), 4 invalidate_fs */
    uint32_t done;      /* AP finished the phase                                         */
    uint32_t started;   /* AP entered the phase                                          */
    int      result;    /* AP's own check                                                */
} g_ap;

void pagecache_selftest_ap(cpu_t* c) {
    uint32_t ph = g_ap.phase;
    if (!ph || c->index != 1 || g_ap.done == ph || g_ap.started == ph) return;
    g_ap.started = ph;
    __sync_synchronize();
    static uint8_t buf[4096];
    int r = 1;
    if (ph == 1) {
        pct_write_page(g_ino, 40, 0);
    } else if (ph == 2 || ph == 3) {
        for (int i = 0; i < 4096; i++) buf[i] = 0;
        r = pagecache_read(g_ino, 0, buf, 4096) == 4096 && pct_is(buf, 50 + ph, 0);
    } else if (ph == 4) {
        pagecache_invalidate_fs(g_ino->ops);
    }
    g_ap.result = r;
    __sync_synchronize();
    g_ap.done = ph;
}

/* Wait (bounded) until the AP finished phase 'ph'. */
static int pct_ap_wait(uint32_t ph, uint64_t tmo_us) {
    uint64_t t0 = ktime_us();
    while (g_ap.done != ph && ktime_us() - t0 < tmo_us) cpu_relax_poll();
    return g_ap.done == ph;
}
/* Hand phase 'ph' to the AP and wait (bounded) until it finished. */
static int pct_ap_run(uint32_t ph, uint64_t tmo_us) {
    g_ap.result = 0;
    __sync_synchronize();
    g_ap.phase = ph;
    uint64_t t0 = ktime_us();
    while (g_ap.done != ph && ktime_us() - t0 < tmo_us) cpu_relax_poll();
    return g_ap.done == ph;
}

static volatile int g_ap_ran;
static void hook(vfs_inode_t* ino, uint64_t off) {
    if (ino != g_ino || off != 0 || g_fired) return;
    g_fired = 1;
    switch (g_mode) {
    case 1:     /* another writer completes during the fill */
        pct_write_page(ino, 2, 0);
        break;
    case 2:     /* the negative control's writer */
        pct_write_page(ino, 3, 0);
        break;
    case 3:     /* the FS is remounted (inode slots reused) during the fill */
        pagecache_invalidate_fs(ino->ops);
        break;
    case 4: {   /* the same task misses the same page while filling it */
        static uint8_t nb[4096];
        int r = pagecache_read(ino, 0, nb, 4096);
        g_ap_ran = (r == 4096 && pct_is(nb, 4, 0));
        break; }
    case 5: {   /* the filler reads more pages than the cache holds */
        vfs_inode_t* big = vfs_lookup(PCT_BIG);
        int good = big != 0;
        for (uint32_t pg = 0; big && pg < PCT_BIGPG; pg++)
            if (!pct_read_is(big, 7, pg)) good = 0;
        g_ap_ran = good;
        break; }
    case 11:    /* AP: write + invalidate during our fill */
        g_ap_ran = pct_ap_run(1, 2000000);
        break;
    case 12: {  /* AP: misses the same page, must WAIT for our publish */
        pagecache_stats_t s0 = pct_stats();
        g_ap.result = 0; __sync_synchronize(); g_ap.phase = 2;
        uint64_t t0 = ktime_us();
        while (pct_stats().waits == s0.waits && ktime_us() - t0 < 2000000) cpu_relax_poll();
        g_ap_ran = pct_stats().waits > s0.waits;     /* AP is now spinning on our slot */
        break; }    /* returning publishes the page; the AP then hits it */
    case 13: {  /* AP: misses the same page, we stall past its wait budget */
        g_ap.result = 0; __sync_synchronize(); g_ap.phase = 3;
        uint64_t t0 = ktime_us();
        while (g_ap.done != 3 && ktime_us() - t0 < 2000000) cpu_relax_poll();
        g_ap_ran = g_ap.done == 3;                   /* finished while we still fill */
        break; }
    case 14:    /* AP: invalidate_fs during our fill */
        g_ap_ran = pct_ap_run(4, 2000000);
        break;
    }
}

static void pct_fill_with_hook(int mode) {
    pagecache_invalidate(g_ino, 0, 1ULL << 62);
    g_mode = mode; g_fired = 0; g_ap_ran = 0;
    for (int i = 0; i < 4096; i++) g_page[i] = 0;
    pagecache_read(g_ino, 0, g_page, 4096);
    g_mode = 0;
}

void pagecache_selftest(void) {
    debugcon_writestring("[PCTEST] page-cache fill self-test\n");
    uint8_t* big = (uint8_t*)kmalloc(PCT_BIGPG * 4096);
    if (!big) { pct_check("allocate test buffers", 0); goto out; }
    for (uint32_t pg = 0; pg < PCT_BIGPG; pg++) pct_pattern(big + pg * 4096, 7, pg);
    vfs_remove(PCT_BIG);
    vfs_remove(PCT_PATH);
    int cb = vfs_create(PCT_BIG, big, PCT_BIGPG * 4096);
    for (uint32_t pg = 0; pg < PCT_PAGES; pg++) pct_pattern(big + pg * 4096, 1, pg);
    int cs = vfs_create(PCT_PATH, big, PCT_PAGES * 4096);
    kfree(big);
    g_ino = vfs_lookup(PCT_PATH);
    if (cb != 0 || cs != 0 || !g_ino || !vfs_lookup(PCT_BIG) || (g_ino->ops->flags & VFS_FS_NOCACHE)) {
        pct_check("create test files on the root FS", 0); goto out;
    }
    pagecache_test_fill_hook = hook;

    /* 1. plain miss then hit; copy_page (mmap faults) == read */
    {
        pagecache_invalidate(g_ino, 0, 1ULL << 62);
        pagecache_stats_t s0 = pct_stats();
        int a = pct_read_is(g_ino, 1, 0) && pct_read_is(g_ino, 1, 1);
        pagecache_stats_t s1 = pct_stats();
        int b = pct_read_is(g_ino, 1, 0);
        pagecache_stats_t s2 = pct_stats();
        pct_check("miss fills and publishes", a && s1.published == s0.published + 2);
        pct_check("second read hits", b && s2.hits == s1.hits + 1 && s2.misses == s1.misses);
        int r = pagecache_copy_page(g_ino, 4096 + 123, g_page) == 0 && pct_is(g_page, 1, 1);
        pct_check("copy_page (mmap fault path) == read", r);
    }
    /* 2. a write + invalidate during the fill: the fill is not published */
    {
        pagecache_stats_t s0 = pct_stats();
        pct_fill_with_hook(1);
        pagecache_stats_t s1 = pct_stats();
        pct_check("racing read returns the bytes it read (old)", pct_is(g_page, 1, 0));
        pct_check("racing fill is doomed and discarded", g_fired &&
                  s1.doomed == s0.doomed + 1 && s1.discarded == s0.discarded + 1 &&
                  s1.published == s0.published);
        pct_check("no stale page after the completed write", pct_read_is(g_ino, 2, 0));
    }
    /* 3. negative control: without the stale bit the old page stays cached */
    {
        pagecache_test_nostale = 1;
        pct_fill_with_hook(2);
        pagecache_test_nostale = 0;
        int stale = pct_read_is(g_ino, 2, 0);       /* the pre-write bytes, cached */
        pct_check("negative control: ignoring the stale bit serves a stale page", g_fired && stale);
        pagecache_invalidate(g_ino, 0, 1ULL << 62);
        pct_check("negative control cleaned up", pct_read_is(g_ino, 3, 0));
    }
    /* 4. invalidate_fs (mount/umount, inode-slot reuse) during the fill */
    {
        pagecache_stats_t s0 = pct_stats();
        pct_fill_with_hook(3);
        pagecache_stats_t s1 = pct_stats();
        pct_check("invalidate_fs dooms an in-flight fill", g_fired &&
                  s1.discarded == s0.discarded + 1 && s1.published == s0.published);
    }
    /* 5. the filler itself misses the same page: direct read, no self-wait */
    {
        pct_write_page(g_ino, 4, 0);
        pagecache_stats_t s0 = pct_stats();
        pct_fill_with_hook(4);
        pagecache_stats_t s1 = pct_stats();
        pct_check("recursive miss on own fill reads directly",
                  g_fired && g_ap_ran && s1.direct == s0.direct + 1 && s1.waits == s0.waits);
        pct_check("recursive fill still publishes", pct_is(g_page, 4, 0) &&
                  s1.published == s0.published + 1);
    }
    /* 6. heavy eviction during a fill: the FILLING slot is never evicted */
    {
        pagecache_stats_t s0 = pct_stats();
        pct_fill_with_hook(5);
        pagecache_stats_t s1 = pct_stats();
        pct_check("big file read through a full cache", g_fired && g_ap_ran &&
                  s1.evictions > s0.evictions);
        pct_check("filling slot survives eviction and publishes",
                  pct_is(g_page, 4, 0) && s1.published == s0.published + PCT_BIGPG + 1);
        pagecache_stats_t s2 = pct_stats();
        int h = pct_read_is(g_ino, 4, 0);
        pagecache_stats_t s3 = pct_stats();
        pct_check("published page is a hit afterwards", h && s3.hits == s2.hits + 1);
    }

    /* Cross-CPU: an AP acts while this CPU sits between fill and publish. */
    if (smp_online_count() < 2) {
        debugcon_writestring("[PCTEST] one CPU online: cross-CPU cases skipped\n");
    } else {
        pagecache_stats_t s0 = pct_stats();
        pct_fill_with_hook(11);                  /* AP writes v40 */
        pagecache_stats_t s1 = pct_stats();
        pct_check("AP: write+invalidate during fill ran", g_ap_ran);
        pct_check("AP: racing fill discarded", s1.discarded == s0.discarded + 1 &&
                  s1.published == s0.published);
        pct_check("AP: no stale page after the AP's write", pct_read_is(g_ino, 40, 0));

        pct_write_page(g_ino, 52, 0);
        s0 = pct_stats();
        pct_fill_with_hook(12);                  /* AP waits on our slot */
        int fin = pct_ap_wait(2, 2000000);          /* the AP's read completes */
        pagecache_stats_t s2 = pct_stats();
        pct_check("AP: second misser waits on the filling slot", g_ap_ran);
        pct_check("AP: waiter gets the published page", fin && g_ap.result &&
                  s2.published == s0.published + 1 && s2.hits >= s0.hits + 1 &&
                  s2.wait_timeouts == s0.wait_timeouts);

        pct_write_page(g_ino, 53, 0);
        s0 = pct_stats();
        pct_fill_with_hook(13);                  /* we stall, AP times out */
        s2 = pct_stats();
        pct_check("AP: stalled fill makes the waiter time out", g_ap_ran &&
                  s2.wait_timeouts == s0.wait_timeouts + 1);
        pct_check("AP: timed-out waiter read the page directly", g_ap.result &&
                  s2.direct >= s0.direct + 1 && s2.published == s0.published + 1);

        s0 = pct_stats();
        pct_fill_with_hook(14);                  /* AP: invalidate_fs */
        s2 = pct_stats();
        pct_check("AP: invalidate_fs during fill discards it", g_ap_ran &&
                  s2.discarded == s0.discarded + 1 && s2.published == s0.published);
        g_ap.phase = 0;
    }
    {
        pagecache_stats_t s = pct_stats();
        pct_check("no slot left filling", s.filling == 0);
    }
    pagecache_test_fill_hook = 0;
    pagecache_invalidate(g_ino, 0, 1ULL << 62);
    if (vfs_lookup(PCT_BIG)) pagecache_invalidate(vfs_lookup(PCT_BIG), 0, 1ULL << 62);
    vfs_remove(PCT_PATH);
    vfs_remove(PCT_BIG);
out:
    debugcon_writestring("[PCTEST] DONE ok=");
    debugcon_print_hex((uint64_t)g_ok);
    debugcon_writestring(" fail=");
    debugcon_print_hex((uint64_t)g_fail);
    debugcon_writestring("\n");
}
#endif /* SECOS_PAGECACHE_TEST */
