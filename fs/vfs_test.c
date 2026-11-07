/*
 * SecOS Kernel - VFS lock self-test (M51 C1, -DSECOS_VFS_TEST)
 *
 * Forced concurrency outside the BKL: the BSP (from kernel_main) and every AP
 * (from its idle loop, kernel/smp.c) run the jobs at the same time. The
 * filesystem drivers have no instance locks yet on this branch (C2-C4), so
 * every phase keeps each driver to one CPU at a time except through the VFS
 * locks under test:
 *
 *   1. Atomic O_APPEND: every CPU appends records (one, two or three 64-byte
 *      records per call) to one ramfs file and one file on /mnt through
 *      vfs_append. Afterwards the file must be exactly ncpu*N*64 bytes, every
 *      record well formed (no torn or overlapping data) and every (cpu, seq)
 *      present once, in order per CPU (no lost record).
 *   2. Negative control for 1: the same appends done the pre-M51 way ("look
 *      up the size", unlock, "write there"), each step serialized by a
 *      test-local lock so the driver still sees one writer at a time. Two
 *      CPUs pick the same offset and records get lost: the checker must see
 *      it, which shows that phase 1 can fail.
 *   3. Mount/umount race: the BSP unmounts and remounts /mnt in a loop
 *      (vfs_ns_lock + vfs_unmount_ex with drain/sync, then the driver's
 *      mount routine), with every 4th unmount refused by its busy callback
 *      (EBUSY: the mount must stay usable). One AP reads /mnt meanwhile:
 *      readdir with a slow callback (a ref held across the FS call) and
 *      vfs_read_all of a test file. Every reader call must either succeed
 *      with the right data or fail cleanly, and no callback may run inside
 *      the driver after the unmount returned (the drain).
 *   4. Negative control for 3: the same with the drain disabled
 *      (g_vfs_test_nodrain): the reader is caught inside the detached FS.
 *
 * Output on debugcon: "[VFST] ..." lines, ending with "[VFST] RESULT PASS|FAIL".
 *
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 */
#ifdef SECOS_VFS_TEST
#include "vfs.h"
#include "klock.h"
#include "heap.h"
#include "debugcon.h"
#include "percpu.h"
#include <stdint.h>
#include <stddef.h>

extern uint64_t ktime_ms(void);
extern uint64_t ktime_us(void);
extern int fat32_mount(const char*, const char*);
extern int ext2_mount(const char*, const char*);
extern int exfat_mount(const char*, const char*);
extern volatile int g_vfs_test_nodrain;              /* fs/vfs.c */
extern volatile uint32_t g_vfs_test_drain_waits;      /* fs/vfs.c */

#define REC        64u
#define NREC_RAM   300u
#define NREC_MNT   60u
#define MAXCPU     8u
#define RACE_ITERS 40u
#define TIMEOUT_MS 120000u

/* 0 idle, 1 append (ramfs), 2 negative append, 3 race, 4 negative race,
 * 5 append (/mnt). Each phase number is used once: an AP joins it once. */
static volatile uint32_t g_phase;
static volatile uint32_t g_in[8], g_out[8];
static volatile uint8_t  g_joined[8][SMP_MAX_CPUS];
static volatile uint32_t g_fail;
static volatile uint32_t g_reader_claim;
static volatile uint32_t g_stop_reader;
static const char*       g_app_path;
static vfs_inode_t*      g_app_ino;     /* looked up once by the BSP */
static uint32_t          g_app_nrec;
static klock_t           g_neg_lock = KLOCK_INIT(LK_K25_VFS_APPEND, "vfst_neg");

static void say(const char* s) { debugcon_writestring(s); }
static void dec(uint64_t v) {
    char b[24]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) debugcon_putchar(b[--n]);
}
static void fail(const char* what) {
    __atomic_add_fetch(&g_fail, 1, __ATOMIC_SEQ_CST);
    say("[VFST] FAIL "); say(what); say("\n");
}
static void put2(char* p, uint32_t v) { p[0] = (char)('0' + (v / 10) % 10); p[1] = (char)('0' + v % 10); }
static void put5(char* p, uint32_t v) { for (int i = 4; i >= 0; i--) { p[i] = (char)('0' + v % 10); v /= 10; } }
static uint32_t get_n(const char* p, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) { if (p[i] < '0' || p[i] > '9') return 0xFFFFFFFFu; v = v * 10 + (uint32_t)(p[i] - '0'); } return v; }

/* Record: "C<cc> N<nnnnn> " + filler (cpu letter) + '\n' (64 bytes). */
static void rec_fill(char* r, uint32_t cpu, uint32_t seq) {
    r[0] = 'C'; put2(r + 1, cpu); r[3] = ' '; r[4] = 'N'; put5(r + 5, seq); r[10] = ' ';
    for (uint32_t i = 11; i < REC - 1; i++) r[i] = (char)('a' + cpu);
    r[REC - 1] = '\n';
}

/* ---- phases 1/2: appenders -------------------------------------------- */
static void appender(uint32_t me, int negative) {
    vfs_inode_t* ino = g_app_ino;          /* drivers have no locks yet: no concurrent lookups */
    if (!ino) { fail("append: no file"); return; }
    char buf[3 * REC];
    uint32_t seq = 0, k = me;
    while (seq < g_app_nrec) {
        uint32_t n = 1 + (k++ % 3);
        if (n > g_app_nrec - seq) n = g_app_nrec - seq;
        for (uint32_t i = 0; i < n; i++) rec_fill(buf + i * REC, me, seq + i);
        int r;
        if (!negative) {
            uint64_t at = 0;
            r = vfs_append(ino, buf, n * REC, &at);
            if (at % REC) fail("append: unaligned offset");
        } else {
            /* pre-M51: size, then write, in two critical sections */
            klock_lock(&g_neg_lock);
            vfs_inode_t* cur = ino->ops->lookup(ino->path);
            size_t end = cur ? cur->size : ino->size;
            klock_unlock(&g_neg_lock);
            for (volatile int d = 0; d < 2000; d++) { }
            klock_lock(&g_neg_lock);
            r = ino->ops->write(ino, end, buf, n * REC);
            klock_unlock(&g_neg_lock);
        }
        if (r != (int)(n * REC)) { fail("append: short write"); return; }
        seq += n;
    }
}

/* 0 = consistent; else the number of problems found. */
static uint32_t check_append(const char* path, uint32_t ncpu, int quiet) {
    uint32_t want = ncpu * g_app_nrec * REC, bad = 0;
    char* b = (char*)kmalloc(want + REC);
    if (!b) { fail("check: kmalloc"); return 1; }
    int n = vfs_read_all(path, b, want + REC);
    if (n != (int)want) {
        bad++;
        if (!quiet) { say("[VFST] append "); say(path); say(": size "); dec((uint64_t)(int64_t)n); say(" want "); dec(want); say("\n"); }
    }
    uint32_t next[MAXCPU] = { 0 };
    uint32_t nr = n > 0 ? (uint32_t)n / REC : 0;
    for (uint32_t i = 0; i < nr; i++) {
        const char* r = b + i * REC;
        uint32_t cpu = get_n(r + 1, 2), seq = get_n(r + 5, 5);
        int ok = r[0] == 'C' && r[3] == ' ' && r[4] == 'N' && r[10] == ' ' && r[REC - 1] == '\n' && cpu < ncpu;
        for (uint32_t j = 11; ok && j < REC - 1; j++) if (r[j] != (char)('a' + cpu)) ok = 0;
        if (!ok) { bad++; continue; }
        if (seq != next[cpu]) bad++;                   /* lost / duplicated / reordered */
        next[cpu] = seq + 1;
    }
    for (uint32_t c = 0; c < ncpu; c++) if (next[c] != g_app_nrec) bad++;
    kfree(b);
    if (!quiet) {
        say("[VFST] append "); say(path); say(": "); dec(nr); say(" records, ");
        dec(bad); say(" problems\n");
    }
    return bad;
}

/* ---- phases 3/4: the reader ------------------------------------------- */
static volatile uint32_t g_gen;          /* odd = /mnt detached (between umount and mount) */
static volatile uint32_t g_inside_detached, g_rd_ok, g_rd_fail, g_rd_bad, g_dir_ok;
static uint8_t  g_ref[4096];
static uint32_t g_ref_len;

static int is_test_name(const char* p) {
    const char* b = p; for (const char* q = p; *q; q++) if (*q == '/') b = q + 1;
    const char* w = "vfstest.dat";
    for (int i = 0; w[i]; i++) {
        char c = b[i]; if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != w[i]) return 0;
    }
    return b[11] == 0;
}
static void slow_cb(const vfs_inode_t* child, void* u) {
    (void)u;
    if (!is_test_name(child->path)) return;        /* only the test FS has it */
    uint64_t t0 = ktime_us();
    while (ktime_us() - t0 < 2000) {         /* a ref held across a slow FS call */
        if (g_gen & 1) { __atomic_add_fetch(&g_inside_detached, 1, __ATOMIC_SEQ_CST); break; }
        __asm__ volatile("pause");
    }
    __atomic_add_fetch(&g_dir_ok, 1, __ATOMIC_SEQ_CST);
}
static void reader(void) {
    static uint8_t buf[4096];
    while (!g_stop_reader) {
        vfs_readdir("/mnt", slow_cb, 0);
        int n = vfs_read_all("/mnt/vfstest.dat", buf, sizeof(buf));
        if (n < 0) { g_rd_fail++; continue; }
        int same = (uint32_t)n == g_ref_len;
        for (int i = 0; same && i < n; i++) if (buf[i] != g_ref[i]) same = 0;
        if (same) g_rd_ok++; else g_rd_bad++;
    }
}

/* ---- the AP side ------------------------------------------------------ */
void vfstest_ap(cpu_t* c) {
    uint32_t ph = g_phase;
    if (!ph || ph > 7 || g_joined[ph][c->index]) return;
    g_joined[ph][c->index] = 1;
    __atomic_add_fetch(&g_in[ph], 1, __ATOMIC_SEQ_CST);
    if (ph == 1 || ph == 2 || ph == 5) appender(c->index, ph == 2);
    else if ((ph == 3 || ph == 4) && __atomic_fetch_add(&g_reader_claim, 1, __ATOMIC_SEQ_CST) == 0) reader();
    __atomic_add_fetch(&g_out[ph], 1, __ATOMIC_SEQ_CST);
}

static void wait_aps(uint32_t ph, uint32_t naps) {
    uint64_t t0 = ktime_ms();
    while ((g_in[ph] < naps || g_out[ph] < g_in[ph]) && ktime_ms() - t0 < TIMEOUT_MS)
        __asm__ volatile("pause");
    if (g_in[ph] < naps) fail("an AP never joined");
    if (g_out[ph] < g_in[ph]) fail("an AP did not finish");
}

static int run_append(uint32_t ph, const char* path, uint32_t nrec, uint32_t ncpu) {
    vfs_remove(path);
    if (vfs_create(path, "", 0) != 0) { say("[VFST] cannot create "); say(path); say("\n"); return -1; }
    g_app_path = path; g_app_nrec = nrec;
    g_app_ino = vfs_lookup(path);
    __sync_synchronize();
    g_phase = ph;
    appender(this_cpu()->index, ph == 2);       /* the BSP takes part too */
    wait_aps(ph, ncpu - 1);
    g_phase = 0;
    return 0;
}

/* Remount /mnt with the driver that had it. */
static int remount_mnt(const char* fstype, const char* dev) {
    vfs_ns_lock();
    int r = -1;
    if (fstype[0] == 'v' || fstype[0] == 'f') r = fat32_mount(dev, "/mnt");
    else if (fstype[0] == 'e' && fstype[1] == 'x' && fstype[2] == 't') r = ext2_mount(dev, "/mnt");
    else if (fstype[0] == 'e') r = exfat_mount(dev, "/mnt");
    vfs_ns_unlock();
    return r;
}
static volatile uint32_t g_busy_every;
static uint32_t g_busy_calls;
static int test_busy(const vfs_fs_ops_t* ops, void* ctx) {
    (void)ops; (void)ctx;
    return g_busy_every && (++g_busy_calls % g_busy_every) == 0;
}

/* 0 = completed (counts in the globals). */
static int run_race(uint32_t ph, const char* fstype, const char* dev, uint32_t iters, int negative) {
    g_inside_detached = g_rd_ok = g_rd_fail = g_rd_bad = g_dir_ok = 0;
    g_reader_claim = 0; g_stop_reader = 0; g_gen = 0;
    g_vfs_test_nodrain = negative;
    g_busy_every = negative ? 0 : 4; g_busy_calls = 0;
    g_vfs_test_drain_waits = 0;
    __sync_synchronize();
    g_phase = ph;
    uint64_t t0 = ktime_ms();
    while (!g_reader_claim && ktime_ms() - t0 < 5000) __asm__ volatile("pause");
    uint32_t umounts = 0, busy = 0, mounts = 0;
    for (uint32_t i = 0; i < iters; i++) {
        /* let the reader get going on the mounted FS */
        uint64_t t1 = ktime_us(); while (ktime_us() - t1 < 1500) __asm__ volatile("pause");
        vfs_ns_lock();
        int r = vfs_unmount_ex("/mnt", test_busy, 0, negative ? 0 : VFS_UMOUNT_SYNC);
        if (r == 0) __atomic_add_fetch(&g_gen, 1, __ATOMIC_SEQ_CST);     /* odd: detached */
        vfs_ns_unlock();
        if (r == -16) { busy++; continue; }
        if (r != 0) { fail("race: unmount failed"); break; }
        umounts++;
        if (negative) { t1 = ktime_us(); while (ktime_us() - t1 < 3000) __asm__ volatile("pause"); }
        __atomic_add_fetch(&g_gen, 1, __ATOMIC_SEQ_CST);                 /* even: mounting */
        if (remount_mnt(fstype, dev) != 0) { fail("race: remount failed"); break; }
        mounts++;
        if (negative && g_inside_detached) break;
    }
    g_stop_reader = 1;
    wait_aps(ph, 1);
    g_phase = 0;
    g_vfs_test_nodrain = 0;
    say("[VFST] race"); say(negative ? " (no drain)" : ""); say(": umount="); dec(umounts);
    say(" ebusy="); dec(busy); say(" mount="); dec(mounts);
    say(" reads ok="); dec(g_rd_ok); say(" failed="); dec(g_rd_fail); say(" wrong="); dec(g_rd_bad);
    say(" dir-callbacks="); dec(g_dir_ok); say(" inside-detached="); dec(g_inside_detached);
    say(" drains-that-waited="); dec(g_vfs_test_drain_waits); say("\n");
    if (!negative && busy == 0) fail("race: the busy callback never refused");
    if (!negative && g_vfs_test_drain_waits == 0) fail("race: no unmount ever had to wait for a reader");
    return 0;
}

void vfstest_run(void) {
    uint32_t ncpu = smp_online_count();
    say("[VFST] start cpus="); dec(ncpu); say("\n");
    if (ncpu < 2 || ncpu > MAXCPU) { say("[VFST] SKIP: needs 2..8 CPUs\n"); say("[VFST] RESULT FAIL\n"); return; }

    /* 1 + 2: appends on the root FS (ramfs) */
    if (run_append(1, "/vfst_append.log", NREC_RAM, ncpu) == 0) {
        if (check_append("/vfst_append.log", ncpu, 0)) fail("append ramfs: lost or torn records");
        else say("[VFST] append ramfs OK\n");
    } else fail("append ramfs: setup");
    if (run_append(2, "/vfst_neg.log", NREC_RAM, ncpu) == 0) {
        uint32_t bad = check_append("/vfst_neg.log", ncpu, 0);
        if (bad) say("[VFST] negative control: size-then-write loses records: OK\n");
        else fail("negative control: unlocked size-then-write lost nothing (test cannot see races)");
        vfs_remove("/vfst_neg.log");
    } else fail("negative control: setup");

    /* /mnt: which driver, which device */
    const char* dev = 0; const char* ty = 0;
    vfs_inode_t* root = vfs_lookup("/mnt");
    const vfs_fs_ops_t* mops = 0;
    for (int i = 0; i < vfs_mount_count(); i++) {
        const char* mp = 0; const char* fs = 0;
        if (vfs_mount_info(i, &mp, &fs) == 0 && mp && mp[0] == '/' && mp[1] == 'm' && mp[2] == 'n' && mp[3] == 't' && !mp[4])
            mops = vfs_mount_ops_at(i);
    }
    if (!root || !mops || !mops->mntinfo || mops->mntinfo(&dev, &ty) != 0 || !dev || !ty) {
        say("[VFST] no disk at /mnt: skipping the /mnt phases\n");
        fail("no /mnt");
    } else {
        say("[VFST] /mnt = "); say(ty); say(" on "); say(dev); say("\n");
        g_app_nrec = NREC_MNT;
        if (run_append(5, "/mnt/vfstapp.log", NREC_MNT, ncpu) == 0) {
            if (check_append("/mnt/vfstapp.log", ncpu, 0)) fail("append /mnt: lost or torn records");
            else say("[VFST] append /mnt OK\n");
            vfs_remove("/mnt/vfstapp.log");
        } else fail("append /mnt: setup");

        g_ref_len = sizeof(g_ref);
        for (uint32_t i = 0; i < g_ref_len; i++) g_ref[i] = (uint8_t)(i * 7 + 3);
        vfs_remove("/mnt/vfstest.dat");
        if (vfs_create("/mnt/vfstest.dat", g_ref, g_ref_len) != 0) fail("race: create test file");
        else {
            run_race(3, ty, dev, RACE_ITERS, 0);
            if (g_inside_detached) fail("race: an operation ran inside the unmounted FS");
            if (g_rd_bad) fail("race: a read returned wrong data");
            if (!g_rd_ok || !g_dir_ok) fail("race: the reader never got through");
            if (!g_fail) say("[VFST] mount/umount race OK\n");
            run_race(4, ty, dev, RACE_ITERS, 1);
            if (g_inside_detached) say("[VFST] negative control: without the drain the reader is caught inside the detached FS: OK\n");
            else fail("negative control: no reader caught without the drain (test cannot see the race)");
            /* Clean up while mounted. */
            if (vfs_lookup("/mnt/vfstest.dat")) vfs_remove("/mnt/vfstest.dat");
            else fail("race: /mnt not usable after the race");
        }
    }
    say(g_fail ? "[VFST] RESULT FAIL\n" : "[VFST] RESULT PASS\n");
}
#endif /* SECOS_VFS_TEST */
