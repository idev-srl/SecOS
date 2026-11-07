/*
 * SecOS Kernel - ext2/ext4 driver-lock stress self-test (M51 C3,
 * -DSECOS_EXT4_LOCKTEST)
 *
 * The BKL still serializes every syscall, so userland cannot race the ext2
 * driver yet. This test does it from the kernel: every CPU (the BSP from
 * kernel_main, each AP from its idle loop, all outside the BKL) runs a worker
 * that drives the mounted ext volume through its vfs_fs_ops directly
 * (mount-relative paths), so the only thing keeping the driver's global
 * state (scratch buffers, JBD2 transaction, bitmaps, inode pool) consistent
 * is g_e2_lock. Each worker owns /e4lt/w<N>/f0..f3 and keeps a model of each
 * (version, size, pattern prefix); per iteration it does a mix of
 *   create-with-data, grow by write, rewrite in place, truncate shrink/grow,
 *   rename into the shared dir and back, rename over another slot, remove,
 *   mkdir + nested file + rmdir, symlink + readlink, link + remove, setattr,
 *   readdir of its own dir (must equal the model) and of the shared dir,
 *   statfs, lookup + read (must equal the model),
 * and appends one record to the shared /e4lt/log through ext2_append()
 * (the atomic O_APPEND path, C1's ->append). At the end the BSP checks every worker's model and the log (each
 * record whole, per-worker sequence numbers gap-free and in order, total
 * length = sum of the records), syncs, and the host runs `e2fsck -fn` on the
 * image. The run must show real contention on the driver lock (some
 * acquisitions had to wait), else it proves nothing.
 *
 * Needs an ext2/ext4 volume mounted (root or /mnt) on which /e4lt does not
 * exist yet. Output: "[E4LT] ..." lines ending with "[E4LT] RESULT PASS|FAIL".
 *
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 */
#ifdef SECOS_EXT4_LOCKTEST
#include "ext2.h"
#include "vfs.h"
#include "heap.h"
#include "debugcon.h"
#include "percpu.h"
#include "klock.h"
#include <stdint.h>
#include <stddef.h>

extern uint64_t ktime_ms(void);
extern uint32_t smp_online_count(void);


#define NSLOT      4
#define MAXF       (20u * 1024u)
#define ITERS      120
#define MAXW       SMP_MAX_CPUS
#define RECMAX     700u
#define TIMEOUT_MS 300000u

typedef struct { uint32_t ver, size, valid, pid; int exists; } lt_slot_t;   /* pid: pattern id (the slot it was created in) */
typedef struct {
    int w;
    uint64_t rng;
    lt_slot_t sl[NSLOT];
    uint8_t* buf;           /* MAXF + 4096 */
    uint8_t* rd;            /* MAXF + 4096 */
    uint32_t ops, fails;
} lt_worker_t;

static const vfs_fs_ops_t* g_ops;
static volatile int      g_active, g_stop;
static volatile uint32_t g_nextw, g_in, g_out, g_fail, g_records;
static volatile uint8_t  g_joined[SMP_MAX_CPUS];
static volatile uint32_t g_cpu_ops[SMP_MAX_CPUS];
static volatile uint32_t g_wdone[MAXW];

static void say(const char* s) { debugcon_writestring(s); }
static void dec(uint64_t v) {
    char b[24]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) debugcon_putchar(b[--n]);
}
static size_t lt_strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void lt_cat(char* o, const char* s) { size_t n = lt_strlen(o); while (*s) o[n++] = *s++; o[n] = 0; }
static void lt_catn(char* o, uint64_t v) {
    char b[24]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    size_t k = lt_strlen(o);
    while (n) o[k++] = b[--n];
    o[k] = 0;
}
static void lt_catpad(char* o, uint64_t v, int width) {
    char b[24]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    size_t k = lt_strlen(o);
    for (int i = n; i < width; i++) o[k++] = '0';
    while (n) o[k++] = b[--n];
    o[k] = 0;
}
static int lt_streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static uint32_t rnd(lt_worker_t* t, uint32_t n) {
    t->rng ^= t->rng << 13; t->rng ^= t->rng >> 7; t->rng ^= t->rng << 17;
    return n ? (uint32_t)(t->rng % n) : 0;
}
static uint8_t pat(uint32_t w, uint32_t s, uint32_t v, uint32_t x) {
    uint32_t h = x * 2654435761u ^ (v * 40503u + w * 977u + s * 131u);
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    uint8_t c = (uint8_t)h;
    return c ? c : 0x5A;
}

static void lt_fail(lt_worker_t* t, const char* what, uint32_t s, uint64_t a, uint64_t b) {
    t->fails++;
    __atomic_add_fetch(&g_fail, 1, __ATOMIC_SEQ_CST);
    if (g_fail > 30) return;
    say("[E4LT] FAIL w="); dec((uint64_t)t->w); say(" slot="); dec(s);
    say(" "); say(what); say(" a="); dec(a); say(" b="); dec(b); say("\n");
}

static void wdir(char* o, int w) { o[0] = 0; lt_cat(o, "/e4lt/w"); lt_catn(o, (uint64_t)w); }
static void spath(char* o, int w, uint32_t s) { wdir(o, w); lt_cat(o, "/f"); lt_catn(o, s); }

static uint8_t expect_byte(lt_worker_t* t, uint32_t s, uint32_t x) {
    return x < t->sl[s].valid ? pat((uint32_t)t->w, t->sl[s].pid, t->sl[s].ver, x) : 0;
}

/* ---- operations ---------------------------------------------------------- */
static void op_check(lt_worker_t* t, uint32_t s) {
    char p[96]; spath(p, t->w, s);
    vfs_inode_t* v = g_ops->lookup(p);
    t->ops++;
    if (!t->sl[s].exists) { if (v) lt_fail(t, "exists but model says removed", s, 0, 0); return; }
    if (!v) { lt_fail(t, "lookup failed", s, 0, 0); return; }
    if (v->size != t->sl[s].size) { lt_fail(t, "lookup size", s, v->size, t->sl[s].size); return; }
    int n = g_ops->read(v, 0, t->rd, MAXF + 4096);
    if (n != (int)t->sl[s].size) { lt_fail(t, "read size", s, (uint64_t)(int64_t)n, t->sl[s].size); return; }
    for (uint32_t x = 0; x < (uint32_t)n; x++)
        if (t->rd[x] != expect_byte(t, s, x)) { lt_fail(t, "read content", s, x, t->sl[s].ver); return; }
    /* a read at an offset, to exercise the block map from the middle */
    if (n > 100) {
        uint32_t off = rnd(t, (uint32_t)n - 50);
        int m = g_ops->read(v, off, t->rd, 50);
        if (m != 50) lt_fail(t, "partial read", s, (uint64_t)(int64_t)m, off);
        else for (uint32_t x = 0; x < 50; x++)
            if (t->rd[x] != expect_byte(t, s, off + x)) { lt_fail(t, "partial read content", s, off + x, 0); break; }
    }
}

static void op_create(lt_worker_t* t, uint32_t s, uint32_t ver) {
    char p[96]; spath(p, t->w, s);
    if (t->sl[s].exists) {
        if (g_ops->remove(p) != 0) { lt_fail(t, "remove before create", s, 0, 0); return; }
        t->sl[s].exists = 0;
    }
    uint32_t init = rnd(t, 6000);
    for (uint32_t x = 0; x < init; x++) t->buf[x] = pat((uint32_t)t->w, s, ver, x);
    if (g_ops->create(p, init ? t->buf : NULL, init) != 0) { lt_fail(t, "create", s, init, 0); return; }
    t->sl[s].exists = 1; t->sl[s].ver = ver; t->sl[s].size = init; t->sl[s].valid = init; t->sl[s].pid = s;
    t->ops++;
    /* grow by writes at the end, in chunks */
    uint32_t more = rnd(t, MAXF - init);
    if (rnd(t, 3) == 0) more = 0;
    vfs_inode_t* v = g_ops->lookup(p);
    if (!v) { lt_fail(t, "lookup after create", s, 0, 0); return; }
    uint32_t done = 0;
    while (done < more) {
        uint32_t c = 1 + rnd(t, 5000); if (c > more - done) c = more - done;
        uint32_t at = init + done;
        for (uint32_t x = 0; x < c; x++) t->buf[x] = pat((uint32_t)t->w, s, ver, at + x);
        int r = g_ops->write(v, at, t->buf, c);
        if (r != (int)c) { lt_fail(t, "grow write", s, (uint64_t)(int64_t)r, c); return; }
        done += c; t->sl[s].size = at + c; t->sl[s].valid = t->sl[s].size;
        t->ops++;
    }
}

static void op_overwrite(lt_worker_t* t, uint32_t s) {
    lt_slot_t* m = &t->sl[s];
    if (!m->exists || m->valid != m->size || m->size < 2) return;
    char p[96]; spath(p, t->w, s);
    vfs_inode_t* v = g_ops->lookup(p);
    if (!v) { lt_fail(t, "lookup for overwrite", s, 0, 0); return; }
    uint32_t off = rnd(t, m->size), n = 1 + rnd(t, m->size - off);
    for (uint32_t x = 0; x < n; x++) t->buf[x] = pat((uint32_t)t->w, m->pid, m->ver, off + x);
    int r = g_ops->write(v, off, t->buf, n);       /* same bytes: content unchanged */
    if (r != (int)n) lt_fail(t, "overwrite", s, (uint64_t)(int64_t)r, n);
    t->ops++;
}

static void op_truncate(lt_worker_t* t, uint32_t s) {
    lt_slot_t* m = &t->sl[s];
    if (!m->exists) return;
    char p[96]; spath(p, t->w, s);
    uint32_t n = rnd(t, MAXF);
    if (g_ops->truncate(p, n) != 0) { lt_fail(t, "truncate", s, n, m->size); return; }
    if (n < m->valid) m->valid = n;
    m->size = n;
    t->ops++;
}

static void op_rename_shared(lt_worker_t* t, uint32_t s) {
    if (!t->sl[s].exists) return;
    char a[96], b[96]; spath(a, t->w, s);
    b[0] = 0; lt_cat(b, "/e4lt/shared/w"); lt_catn(b, (uint64_t)t->w); lt_cat(b, ".f"); lt_catn(b, s);
    if (g_ops->rename(a, b) != 0) { lt_fail(t, "rename to shared", s, 0, 0); return; }
    if (g_ops->lookup(a)) lt_fail(t, "old name still there", s, 0, 0);
    if (g_ops->rename(b, a) != 0) { lt_fail(t, "rename back", s, 0, 0); t->sl[s].exists = 0; return; }
    t->ops += 3;
}

static void op_rename_slot(lt_worker_t* t, uint32_t s, uint32_t d) {
    if (!t->sl[s].exists || s == d) return;
    char a[96], b[96]; spath(a, t->w, s); spath(b, t->w, d);
    if (g_ops->rename(a, b) != 0) { lt_fail(t, "rename over slot", s, d, 0); return; }
    t->sl[d] = t->sl[s]; t->sl[s].exists = 0;
    t->ops++;
}

static void op_remove(lt_worker_t* t, uint32_t s) {
    if (!t->sl[s].exists) return;
    char p[96]; spath(p, t->w, s);
    if (g_ops->remove(p) != 0) { lt_fail(t, "remove", s, 0, 0); return; }
    t->sl[s].exists = 0;
    t->ops++;
}

static void op_dirs(lt_worker_t* t, uint32_t i) {
    char d1[96], d2[96], f[112];
    wdir(d1, t->w); lt_cat(d1, "/d"); lt_catn(d1, i);
    d2[0] = 0; lt_cat(d2, d1); lt_cat(d2, "/sub");
    f[0] = 0; lt_cat(f, d2); lt_cat(f, "/x");
    if (g_ops->mkdir(d1) != 0) { lt_fail(t, "mkdir", 0, i, 0); return; }
    if (g_ops->mkdir(d2) != 0) { lt_fail(t, "mkdir sub", 0, i, 0); g_ops->remove(d1); return; }
    uint32_t n = 1 + rnd(t, 3000);
    for (uint32_t x = 0; x < n; x++) t->buf[x] = pat((uint32_t)t->w, 99, i, x);
    if (g_ops->create(f, t->buf, n) != 0) lt_fail(t, "create in sub", 0, i, n);
    else {
        vfs_inode_t* v = g_ops->lookup(f);
        if (!v || v->size != n) lt_fail(t, "lookup in sub", 0, i, n);
        if (g_ops->remove(f) != 0) lt_fail(t, "remove in sub", 0, i, 0);
    }
    if (g_ops->remove(d2) != 0) lt_fail(t, "rmdir sub", 0, i, 0);
    if (g_ops->remove(d1) != 0) lt_fail(t, "rmdir", 0, i, 0);
    t->ops += 6;
}

static void op_posix(lt_worker_t* t, uint32_t s, uint32_t i) {
    if (!t->sl[s].exists) return;
    char p[96], l[96], h[96], tg[16] = "f";
    spath(p, t->w, s);
    lt_catn(tg, s);
    wdir(l, t->w); lt_cat(l, "/l"); lt_catn(l, i);
    if (g_ops->symlink(tg, l) != 0) lt_fail(t, "symlink", s, i, 0);
    else {
        char got[32];
        int n = g_ops->readlink(l, got, sizeof(got));
        if (n != (int)lt_strlen(tg) || !lt_streq(got, tg)) lt_fail(t, "readlink", s, (uint64_t)(int64_t)n, 0);
        if (g_ops->remove(l) != 0) lt_fail(t, "remove symlink", s, i, 0);
    }
    vfs_attr_t a; a.mode = 0600; a.uid = 0; a.gid = 0; a.atime = 0; a.mtime = 0;
    if (g_ops->setattr(p, &a, VFS_ATTR_MODE) != 0) lt_fail(t, "setattr", s, 0, 0);
    else {
        vfs_inode_t* v = g_ops->lookup(p);
        if (!v || (v->mode & 0777) != 0600) lt_fail(t, "mode after setattr", s, v ? (v->mode & 0777) : 0, 0600);
    }
    wdir(h, t->w); lt_cat(h, "/h"); lt_catn(h, i);
    if (g_ops->link(p, h) != 0) lt_fail(t, "link", s, i, 0);
    else {
        vfs_inode_t* v = g_ops->lookup(h);
        if (!v || v->nlink != 2) lt_fail(t, "nlink after link", s, v ? v->nlink : 0, 2);
        if (g_ops->remove(h) != 0) lt_fail(t, "remove hard link", s, i, 0);
        v = g_ops->lookup(p);
        if (!v || v->nlink != 1) lt_fail(t, "nlink after remove", s, v ? v->nlink : 0, 1);
    }
    t->ops += 9;
}

typedef struct { lt_worker_t* t; int seen[NSLOT]; int bad; uint32_t n; } lt_dirctx_t;
static void own_cb(const vfs_inode_t* c, void* u) {
    lt_dirctx_t* d = (lt_dirctx_t*)u;
    /* c->path is mount-relative "/e4lt/wN/<name>" */
    const char* nm = c->path; const char* q = c->path;
    while (*q) { if (*q == '/') nm = q + 1; q++; }
    if (nm[0] == 'f' && nm[1] >= '0' && nm[1] < '0' + NSLOT && nm[2] == 0) d->seen[nm[1] - '0']++;
    else d->bad++;
}
static void count_cb(const vfs_inode_t* c, void* u) { (void)c; ((lt_dirctx_t*)u)->n++; }

static void op_listing(lt_worker_t* t) {
    char d[96]; wdir(d, t->w);
    lt_dirctx_t c; c.t = t; c.bad = 0; c.n = 0;
    for (int s = 0; s < NSLOT; s++) c.seen[s] = 0;
    if (g_ops->readdir(d, own_cb, &c) != 0) { lt_fail(t, "readdir", 0, 0, 0); return; }
    if (c.bad) lt_fail(t, "unexpected entry in own dir", 0, (uint64_t)c.bad, 0);
    for (uint32_t s = 0; s < NSLOT; s++)
        if (c.seen[s] != (t->sl[s].exists ? 1 : 0)) lt_fail(t, "readdir differs from model", s, (uint64_t)c.seen[s], (uint64_t)t->sl[s].exists);
    if (g_ops->readdir("/e4lt/shared", count_cb, &c) != 0) lt_fail(t, "readdir shared", 0, 0, 0);
    vfs_statfs_t st;
    if (g_ops->statfs(&st) != 0 || st.bfree == 0 || st.bfree > st.blocks) lt_fail(t, "statfs", 0, st.bfree, st.blocks);
    t->ops += 3;
}

/* Record: "R w=WW i=IIIII n=NNNN " + payload + "\n", n bytes. */
static uint32_t make_rec(char* o, uint32_t w, uint32_t i, uint32_t n) {
    o[0] = 0; lt_cat(o, "R w="); lt_catpad(o, w, 2); lt_cat(o, " i="); lt_catpad(o, i, 5);
    lt_cat(o, " n="); lt_catpad(o, n, 4); lt_cat(o, " ");
    uint32_t h = (uint32_t)lt_strlen(o);
    for (uint32_t j = h; j < n - 1; j++) o[j] = (char)('a' + (w * 7 + i * 3 + j) % 26);
    o[n - 1] = '\n';
    return n;
}

static void op_log(lt_worker_t* t, uint32_t i) {
    static const char* lp = "/e4lt/log";
    char* rec = (char*)t->buf;
    uint32_t n = 32 + rnd(t, RECMAX - 32);
    make_rec(rec, (uint32_t)t->w, i, n);
    vfs_inode_t* v = g_ops->lookup(lp);
    if (!v) { lt_fail(t, "lookup log", 0, i, 0); return; }
    uint64_t at = ~0ull;
    int r = ext2_append(v, rec, n, &at);
    if (r != (int)n) { lt_fail(t, "log append", 0, (uint64_t)(int64_t)r, n); return; }
    if (at == ~0ull || at + n > (uint64_t)1 << 30) { lt_fail(t, "append offset", 0, at, n); return; }
    __atomic_add_fetch(&g_records, 1, __ATOMIC_SEQ_CST);
    t->ops++;
}

/* ---- workers ------------------------------------------------------------ */
static void worker(void) {
    uint32_t me = this_cpu()->index;
    uint32_t w = __atomic_fetch_add(&g_nextw, 1, __ATOMIC_SEQ_CST);
    if (w >= MAXW) return;
    lt_worker_t* t = (lt_worker_t*)kmalloc(sizeof(lt_worker_t));
    uint8_t* bufs = (uint8_t*)kmalloc(2 * (MAXF + 4096));
    __atomic_add_fetch(&g_in, 1, __ATOMIC_SEQ_CST);
    if (!t || !bufs) { say("[E4LT] FAIL kmalloc\n"); __atomic_add_fetch(&g_fail, 1, __ATOMIC_SEQ_CST); goto out; }
    for (size_t k = 0; k < sizeof(*t); k++) ((uint8_t*)t)[k] = 0;
    t->w = (int)w; t->buf = bufs; t->rd = bufs + MAXF + 4096;
    t->rng = 0x9E3779B97F4A7C15ull * (w + 1) + 12345;
    uint64_t t0 = ktime_ms();
    for (uint32_t i = 0; i < ITERS && !g_stop && t->fails == 0; i++) {
        if (ktime_ms() - t0 > TIMEOUT_MS) { lt_fail(t, "timeout", 0, i, 0); break; }
        uint32_t s = rnd(t, NSLOT);
        uint32_t opk = rnd(t, 12);
#ifdef SECOS_EXT4_LOCKTEST_TRACE
        say("[E4LT] T w="); dec(w); say(" i="); dec(i); say(" op="); dec(opk); say(" s="); dec(s);
        for (uint32_t z = 0; z < NSLOT; z++) { say(" ["); dec(t->sl[z].exists); say(","); dec(t->sl[z].ver); say(","); dec(t->sl[z].size); say(","); dec(t->sl[z].valid); say("]"); }
        say("\n");
#endif
        switch (opk) {
        case 0: case 1: case 2: op_create(t, s, i + 1); break;
        case 3: op_overwrite(t, s); break;
        case 4: op_truncate(t, s); break;
        case 5: op_rename_shared(t, s); break;
        case 6: op_rename_slot(t, s, rnd(t, NSLOT)); break;
        case 7: op_remove(t, s); break;
        case 8: op_dirs(t, i); break;
        case 9: op_posix(t, s, i); break;
        case 10: op_listing(t); break;
        default: op_check(t, s); break;
        }
        if (rnd(t, 2) == 0) op_check(t, rnd(t, NSLOT));
        op_log(t, i);
        g_wdone[w] = i + 1;
    }
    for (uint32_t s = 0; s < NSLOT; s++) op_check(t, s);
    op_listing(t);
    g_cpu_ops[me] += t->ops;
    say("[E4LT] worker "); dec(w); say(" cpu "); dec(me); say(t->fails ? " FAIL" : " ok");
    say(" ops="); dec(t->ops); say("\n");
out:
    if (bufs) kfree(bufs);
    if (t) kfree(t);
    __atomic_add_fetch(&g_out, 1, __ATOMIC_SEQ_CST);
}

/* Called from every AP's idle loop (kernel/smp.c). */
void ext4locktest_ap(cpu_t* c) {
    if (!g_active || g_joined[c->index]) return;
    g_joined[c->index] = 1;
    worker();
}

/* ---- final checks ------------------------------------------------------- */
static uint32_t lt_num(const char* p, int digits, int* bad) {
    uint32_t v = 0;
    for (int k = 0; k < digits; k++) { if (p[k] < '0' || p[k] > '9') { *bad = 1; return 0; } v = v * 10 + (uint32_t)(p[k] - '0'); }
    return v;
}
static int check_log(uint32_t nworkers) {
    vfs_inode_t* v = g_ops->lookup("/e4lt/log");
    if (!v) { say("[E4LT] FAIL log missing\n"); return 1; }
    size_t size = v->size;
    uint8_t* b = (uint8_t*)kmalloc(size + 1);
    char* want = (char*)kmalloc(RECMAX + 8);
    if (!b || !want) { say("[E4LT] FAIL kmalloc log\n"); return 1; }
    int r = g_ops->read(v, 0, b, size);
    int bad = 0;
    if (r != (int)size) { say("[E4LT] FAIL log read\n"); bad = 1; }
    uint32_t next[MAXW]; for (uint32_t w = 0; w < MAXW; w++) next[w] = 0;
    size_t off = 0; uint32_t recs = 0;
    while (!bad && off < size) {
        if (size - off < 22) { say("[E4LT] FAIL log ends in a partial record\n"); bad = 1; break; }
        const char* p = (const char*)b + off;
        int pb = 0;
        uint32_t w = lt_num(p + 4, 2, &pb), i = lt_num(p + 9, 5, &pb), n = lt_num(p + 17, 4, &pb);
        if (pb || p[0] != 'R' || w >= nworkers || n < 32 || n > RECMAX || off + n > size) {
            say("[E4LT] FAIL malformed record at byte "); dec(off); say("\n"); bad = 1; break;
        }
        make_rec(want, w, i, n);
        for (uint32_t k = 0; k < n; k++) if ((char)p[k] != want[k]) {
            say("[E4LT] FAIL torn/interleaved record at byte "); dec(off + k); say("\n"); bad = 1; break;
        }
        if (bad) break;
        if (i != next[w]) { say("[E4LT] FAIL record order w="); dec(w); say(" i="); dec(i); say(" want "); dec(next[w]); say("\n"); bad = 1; }
        next[w] = i + 1;
        off += n; recs++;
    }
    for (uint32_t w = 0; w < nworkers && !bad; w++)
        if (next[w] != g_wdone[w]) { say("[E4LT] FAIL worker "); dec(w); say(" records "); dec(next[w]); say(" want "); dec(g_wdone[w]); say("\n"); bad = 1; }
    say("[E4LT] log bytes="); dec(size); say(" records="); dec(recs); say(" appended="); dec(g_records);
    say(bad ? " BAD\n" : " ok\n");
    if (recs != g_records) bad = 1;
    kfree(b); kfree(want);
    return bad;
}

void ext4locktest_run(void) {
    g_ops = ext2_vfs_ops();
    if (!vfs_mount_point_of(g_ops)) { say("[E4LT] SKIP: no ext2/ext4 volume mounted\n"); return; }
    if (g_ops->lookup("/e4lt")) { say("[E4LT] SKIP: /e4lt already exists (use a fresh volume)\n"); return; }
    uint32_t ncpu = smp_online_count();
    if (ncpu > MAXW) ncpu = MAXW;
    if (g_ops->mkdir("/e4lt") != 0 || g_ops->mkdir("/e4lt/shared") != 0 || g_ops->create("/e4lt/log", NULL, 0) != 0) {
        say("[E4LT] FAIL setup\n"); say("[E4LT] RESULT FAIL\n"); return;
    }
    for (uint32_t w = 0; w < ncpu; w++) {
        char d[64]; wdir(d, (int)w);
        if (g_ops->mkdir(d) != 0) { say("[E4LT] FAIL mkdir worker dir\n"); say("[E4LT] RESULT FAIL\n"); return; }
    }
    klock_t* l = ext2_fs_lock();
    uint64_t acq0 = l->n_acq, cont0 = l->n_contended;
    say("[E4LT] start cpus="); dec(ncpu); say(" iters="); dec(ITERS); say(" on "); say(vfs_mount_point_of(g_ops)); say("\n");
    uint64_t t0 = ktime_ms();
    __atomic_store_n(&g_active, 1, __ATOMIC_RELEASE);
    worker();
    while ((g_out < g_in || g_in < ncpu) && ktime_ms() - t0 < TIMEOUT_MS + 30000) __asm__ volatile("pause");
    g_stop = 1;
    g_active = 0;
    uint64_t acq = l->n_acq - acq0, cont = l->n_contended - cont0;
    say("[E4LT] workers="); dec(g_in); say(" done="); dec(g_out); say(" ms="); dec(ktime_ms() - t0); say("\n");
    say("[E4LT] ext2 lock acquisitions="); dec(acq); say(" contended="); dec(cont);
    say(" max_wait_cyc="); dec(l->max_wait_cyc); say("\n");
    for (uint32_t i = 0; i < ncpu; i++) { say("[E4LT] cpu "); dec(i); say(" ops "); dec(g_cpu_ops[i]); say("\n"); }
    int bad = 0;
    if (g_in < 2 || g_in < ncpu) { say("[E4LT] FAIL not every CPU took part\n"); bad = 1; }
    if (g_out != g_in) { say("[E4LT] FAIL a worker did not finish\n"); bad = 1; }
    if (cont == 0) { say("[E4LT] FAIL no contention on the ext2 lock: nothing ran concurrently\n"); bad = 1; }
    if (check_log(g_in)) bad = 1;
    if (g_ops->sync() != 0) { say("[E4LT] FAIL sync\n"); bad = 1; }
    if (g_fail) bad = 1;
    say(bad ? "[E4LT] RESULT FAIL\n" : "[E4LT] RESULT PASS\n");
}
#endif /* SECOS_EXT4_LOCKTEST */
