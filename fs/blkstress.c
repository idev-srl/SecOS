/*
 * SecOS Kernel - block / USB lock stress self-test (M50 B3, -DSECOS_B3_BLKSTRESS)
 *
 * Every CPU (the BSP from kernel_main, each AP from its idle loop) pulls jobs
 * from one shared queue and runs them at the same time, outside the BKL —
 * the only way to exercise the per-disk klocks and the xHCI locks with real
 * concurrency while the syscall path still serializes everything:
 *   - SHA-256 of fixed regions of sda, sdb1 (a partition: its parent's lock),
 *     nvme0n1 and usb0 (USB Mass Storage, sharing the xHCI controller with the
 *     keyboard), with varying chunk sizes, repeated for several rounds; every
 *     round must give the same digest and the host compares round 0 with the
 *     image files (tools/blk_stress_drive.py);
 *   - a pattern written to nvme0n1 and read back;
 *   - two jobs writing interleaved 100-byte stripes of one region of
 *     /dev/sdb1 through devfs (partial-sector read-modify-write) while another
 *     job reads the same disk: the result must be the complete pattern, which
 *     only holds if each devfs request is atomic on its disk;
 *   - all the while every CPU polls the USB HID keyboard (usb_hid_poll, which
 *     only trylocks) between chunks, and the BSP collects the characters the
 *     host types through QMP. The run lasts until the host's line arrived.
 * Output on debugcon: "[B3] ..." lines, ending with "[B3] RESULT PASS|FAIL".
 *
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 */
#ifdef SECOS_B3_BLKSTRESS
#include "block.h"
#include "vfs.h"
#include "heap.h"
#include "debugcon.h"
#include "percpu.h"
#include "../crypto/sha256.h"
#include <stdint.h>
#include <stddef.h>

extern void usb_hid_poll(void);
extern int  keyboard_getc_raw(void);
extern uint64_t ktime_ms(void);
extern uint32_t smp_online_count(void);

#define SEC        512u
#define MIB        (1024u * 1024u)
#define MAX_ROUNDS 24
#define MIN_ROUNDS 3
#define BUF_BYTES  (128u * SEC)
#define STRIPE     100u
#define STRIPE_OFF (8u * MIB)          /* partition-relative byte offset */
#define STRIPE_LEN (256u * 1024u)
#define TIMEOUT_MS 150000u

enum { JK_HASH = 1, JK_WRITE = 2, JK_STRIPE = 3 };
typedef struct {
    int kind;
    const char* dev;
    uint64_t lba;          /* first sector (HASH/WRITE) */
    uint32_t nsec;
    uint32_t tag;          /* pattern tag (WRITE/STRIPE) */
    int parity;            /* STRIPE: which stripes */
} b3_job_t;

static const b3_job_t g_jobs[] = {
    { JK_STRIPE, "sdb1",    0, 0, 3, 0 },
    { JK_STRIPE, "sdb1",    0, 0, 3, 1 },
    { JK_HASH,   "sdb1",    0, (8u * MIB) / SEC, 0, 0 },
    { JK_HASH,   "usb0",    0, (8u * MIB) / SEC, 0, 0 },
    { JK_HASH,   "sda",     0, (16u * MIB) / SEC, 0, 0 },
    { JK_WRITE,  "nvme0n1", (16u * MIB) / SEC, (8u * MIB) / SEC, 2, 0 },
    { JK_HASH,   "nvme0n1", 0, (16u * MIB) / SEC, 0, 0 },
    { JK_HASH,   "sda",     (16u * MIB) / SEC, (8u * MIB) / SEC, 0, 0 },
};
#define NJOBS (sizeof(g_jobs) / sizeof(g_jobs[0]))

static volatile int      g_active, g_stop;
static volatile uint32_t g_next, g_in, g_out, g_fail;
static volatile uint8_t  g_joined[SMP_MAX_CPUS];
static uint8_t           g_dig[NJOBS][MAX_ROUNDS][32];
static volatile uint8_t  g_dig_ok[NJOBS][MAX_ROUNDS];
static volatile uint32_t g_runs[SMP_MAX_CPUS];
static char              g_keys[256];
static volatile uint32_t g_nkeys, g_keys_done;

static void say(const char* s) { debugcon_writestring(s); }
static void hex8(uint8_t v) {
    static const char* h = "0123456789abcdef";
    debugcon_putchar(h[v >> 4]); debugcon_putchar(h[v & 15]);
}
static void dec(uint64_t v) {
    char b[24]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) debugcon_putchar(b[--n]);
}
static void fail(const char* what, const char* dev) {
    __atomic_add_fetch(&g_fail, 1, __ATOMIC_SEQ_CST);
    say("[B3] FAIL "); say(what); say(" "); say(dev ? dev : "-"); say("\n");
}

/* Deterministic pattern: byte x of the region with tag t (host: same formula). */
static uint64_t mix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static void pat_fill(uint32_t tag, uint64_t x, uint8_t* out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint64_t a = x + i;
        out[i] = (uint8_t)(mix64(((uint64_t)tag << 48) ^ (a >> 3)) >> ((a & 7) * 8));
    }
}

/* The BSP collects typed characters; every CPU pumps the HID poll. */
static void pump(int bsp) {
    usb_hid_poll();
    if (!bsp) return;
    int c;
    while ((c = keyboard_getc_raw()) >= 0) {
        if (c == '\r' || c == '\n') { g_keys_done = 1; continue; }
        if (g_nkeys < sizeof(g_keys) - 1) g_keys[g_nkeys++] = (char)c;
    }
}

static const uint32_t g_chunks[] = { 1, 7, 64, 128, 33, 128, 2 };

static void job_hash(const b3_job_t* j, uint32_t ji, uint32_t round, uint8_t* buf, int bsp) {
    block_dev_t* d = block_find(j->dev);
    if (!d) { fail("no device", j->dev); return; }
    sha256_ctx c; sha256_init(&c);
    uint64_t done = 0; uint32_t k = round;
    while (done < j->nsec) {
        uint32_t n = g_chunks[k++ % (sizeof(g_chunks) / sizeof(g_chunks[0]))];
        if (n > j->nsec - done) n = (uint32_t)(j->nsec - done);
        if (block_read(d, j->lba + done, buf, n) != (int)n) { fail("read", j->dev); return; }
        sha256_update(&c, buf, (size_t)n * SEC);
        done += n;
        pump(bsp);
    }
    sha256_final(&c, g_dig[ji][round]);
    __atomic_store_n(&g_dig_ok[ji][round], 1, __ATOMIC_RELEASE);
}

static void job_write(const b3_job_t* j, uint8_t* buf, int bsp) {
    block_dev_t* d = block_find(j->dev);
    if (!d) { fail("no device", j->dev); return; }
    uint64_t done = 0; uint32_t k = 0;
    while (done < j->nsec) {
        uint32_t n = g_chunks[k++ % (sizeof(g_chunks) / sizeof(g_chunks[0]))];
        if (n > j->nsec - done) n = (uint32_t)(j->nsec - done);
        pat_fill(j->tag, (j->lba + done) * SEC, buf, n * SEC);   /* absolute offset */
        if (block_write(d, j->lba + done, buf, n) != (int)n) { fail("write", j->dev); return; }
        done += n;
        pump(bsp);
    }
}

static void job_stripe(const b3_job_t* j, uint8_t* buf, int bsp) {
    char path[24] = "/dev/";
    for (int i = 0; j->dev[i] && i < 16; i++) { path[5 + i] = j->dev[i]; path[6 + i] = 0; }
    vfs_inode_t* ino = vfs_lookup(path);
    if (!ino || !ino->ops || !ino->ops->write) { fail("no devfs node", path); return; }
    uint32_t nstripes = (STRIPE_LEN + STRIPE - 1) / STRIPE;
    for (uint32_t s = (uint32_t)j->parity; s < nstripes; s += 2) {
        uint64_t off = (uint64_t)STRIPE_OFF + (uint64_t)s * STRIPE;
        uint32_t len = STRIPE;
        if (s * STRIPE + len > STRIPE_LEN) len = STRIPE_LEN - s * STRIPE;
        pat_fill(j->tag, off, buf, len);
        if (ino->ops->write(ino, (size_t)off, buf, len) != (int)len) { fail("devfs write", path); return; }
        if ((s & 31) == 0) pump(bsp);
    }
}

static int keys_and_rounds_done(void) {
    return g_keys_done && __atomic_load_n(&g_next, __ATOMIC_ACQUIRE) >= MIN_ROUNDS * NJOBS;
}

static void worker(int bsp) {
    uint32_t me = this_cpu()->index;
    uint8_t* buf = (uint8_t*)kmalloc(BUF_BYTES);
    if (!buf) { fail("kmalloc", 0); return; }
    __atomic_add_fetch(&g_in, 1, __ATOMIC_SEQ_CST);
    uint64_t t0 = ktime_ms();
    while (!g_stop && ktime_ms() - t0 < TIMEOUT_MS) {
        uint32_t i = __atomic_fetch_add(&g_next, 1, __ATOMIC_SEQ_CST);
        uint32_t round = i / NJOBS, ji = i % NJOBS;
        if (round >= MAX_ROUNDS) break;
        const b3_job_t* j = &g_jobs[ji];
        if (round > 0 && j->kind != JK_HASH) continue;   /* writes run once */
        if (j->kind == JK_HASH) job_hash(j, ji, round, buf, bsp);
        else if (j->kind == JK_WRITE) job_write(j, buf, bsp);
        else job_stripe(j, buf, bsp);
        g_runs[me]++;
        if (bsp && keys_and_rounds_done()) g_stop = 1;
    }
    kfree(buf);
    __atomic_add_fetch(&g_out, 1, __ATOMIC_SEQ_CST);
}

/* Called from every AP's idle loop (kernel/smp.c). */
void blkstress_ap(cpu_t* c) {
    if (!g_active || g_joined[c->index]) return;
    g_joined[c->index] = 1;
    worker(0);
}

static void verify_region(const char* dev, uint64_t byte_off, uint32_t len, uint32_t tag,
                          uint8_t* buf, uint8_t* want, const char* what) {
    block_dev_t* d = block_find(dev);
    if (!d) { fail("no device", dev); return; }
    for (uint32_t done = 0; done < len; done += BUF_BYTES) {
        uint32_t n = len - done < BUF_BYTES ? len - done : BUF_BYTES;
        uint64_t off = byte_off + done;
        if (block_read(d, off / SEC, buf, (n + SEC - 1) / SEC) != (int)((n + SEC - 1) / SEC)) {
            fail("verify read", dev); return;
        }
        pat_fill(tag, off, want, n);
        for (uint32_t i = 0; i < n; i++) {
            if (buf[i] != want[i]) {
                say("[B3] mismatch "); say(what); say(" at byte "); dec(off + i); say("\n");
                fail(what, dev); return;
            }
        }
    }
    say("[B3] "); say(what); say(" verify OK\n");
}

void blkstress_run(void) {
    for (uint32_t ji = 0; ji < NJOBS; ji++) {
        if (!block_find(g_jobs[ji].dev)) {
            say("[B3] SKIP: needs sda, sdb1, nvme0n1, usb0 (missing ");
            say(g_jobs[ji].dev); say(")\n");
            return;
        }
    }
    uint32_t ncpu = smp_online_count();
    say("[B3] stress start cpus="); dec(ncpu); say("\n");
    uint64_t t0 = ktime_ms();
    __atomic_store_n(&g_active, 1, __ATOMIC_RELEASE);
    worker(1);
    /* Our share is done: keep collecting keys until the line arrived, then
     * stop everyone and wait for the APs to leave. */
    while (!keys_and_rounds_done() && ktime_ms() - t0 < TIMEOUT_MS) {
        pump(1);
        __asm__ volatile("pause");
    }
    g_stop = 1;
    while ((g_out < g_in || g_in < ncpu) && ktime_ms() - t0 < TIMEOUT_MS + 5000) {
        pump(1);
        __asm__ volatile("pause");
    }
    g_active = 0;
    say("[B3] workers="); dec(g_in); say(" done="); dec(g_out);
    say(" jobs="); dec(g_next > MAX_ROUNDS * NJOBS ? MAX_ROUNDS * NJOBS : g_next);
    say(" ms="); dec(ktime_ms() - t0); say("\n");
    for (uint32_t i = 0; i < ncpu && i < SMP_MAX_CPUS; i++) {
        say("[B3] cpu "); dec(i); say(" ran "); dec(g_runs[i]); say(" jobs\n");
    }
    if (g_in < 2) fail("fewer than two CPUs took part", 0);
    if (g_out != g_in) fail("a worker did not finish", 0);

    /* Digests: round 0 for the host, every later round must agree. */
    for (uint32_t ji = 0; ji < NJOBS; ji++) {
        const b3_job_t* j = &g_jobs[ji];
        if (j->kind != JK_HASH) continue;
        if (!g_dig_ok[ji][0]) { fail("no round-0 digest", j->dev); continue; }
        uint32_t rounds = 1;
        for (uint32_t r = 1; r < MAX_ROUNDS; r++) {
            if (!g_dig_ok[ji][r]) continue;
            rounds++;
            for (int b = 0; b < 32; b++)
                if (g_dig[ji][r][b] != g_dig[ji][0][b]) { fail("digest changed between rounds", j->dev); break; }
        }
        say("[B3] hash "); say(j->dev); say(" "); dec(j->lba); say(" "); dec(j->nsec); say(" ");
        for (int b = 0; b < 32; b++) hex8(g_dig[ji][0][b]);
        say(" rounds="); dec(rounds); say("\n");
        if (rounds < MIN_ROUNDS) fail("too few rounds", j->dev);
    }
    uint8_t* buf = (uint8_t*)kmalloc(BUF_BYTES);
    uint8_t* want = (uint8_t*)kmalloc(BUF_BYTES);
    if (buf && want) {
        verify_region("nvme0n1", 16ull * MIB, 8u * MIB, 2, buf, want, "nvme-write");
        verify_region("sdb1", STRIPE_OFF, STRIPE_LEN, 3, buf, want, "sdb1-stripes");
    } else fail("kmalloc", 0);
    if (buf) kfree(buf);
    if (want) kfree(want);
    g_keys[g_nkeys] = 0;
    extern uint32_t usb_hid_keydowns(void);
    say("[B3] keys=["); say(g_keys); say("] usb_hid_keydowns="); dec(usb_hid_keydowns()); say("\n");
    if (!g_keys_done) fail("key line never completed", 0);
    say(g_fail ? "[B3] RESULT FAIL\n" : "[B3] RESULT PASS\n");
}
#endif /* SECOS_B3_BLKSTRESS */
