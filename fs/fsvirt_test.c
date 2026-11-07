/*
 * fsvirt_test.c — [M51 C2] boot self-test of the virtual-filesystem locks
 * (built only with -DSECOS_FSVIRT_TEST; the selftest's "M51 FSVIRT" gate).
 *
 * With the BKL on, no two syscalls ever run at once, so a userland stress
 * cannot race ramfs or procfs. This test runs the same operations on every
 * CPU at once outside the BKL: the BSP from kernel_main, each AP from its idle
 * loop (kernel/smp.c), all pulling the same phase.
 *
 *   ramfs:  every CPU creates, writes (growing in chunks), reads back,
 *           truncates, renames, chmods, appends to one shared log, writes its
 *           stripe of one shared file, lists the test directory, makes and
 *           removes a subdirectory and unlinks -- through the ramfs vfs_fs_ops
 *           (the ones the VFS calls), plus atomic appends through the
 *           ramfs_fs_lock() accessor. Every byte read back is checked, the log
 *           must hold every record exactly once and intact, the heap is
 *           validated (heap_check: headers, canaries and, with lockdep, the
 *           0x6B free poison) after each pass, and MemFree must be the same
 *           after the second pass as after the first (no leak).
 *   procfs: a few held (never started) processes; one CPU rewrites their cwd
 *           and fd 5 the way the syscalls do (under mm_lock) while the others
 *           read /proc/<pid>/cwd, /proc/<pid>/fd/5, status, stat and maps;
 *           every cwd must be one of the two strings written and every fd
 *           link one of the objects installed. Then the processes are
 *           destroyed while the readers keep reading.
 *   negative control: the same cwd rewrite WITHOUT mm_lock -- the readers
 *           must now see torn strings, which shows the test can see the bug.
 *
 * Output: "[FSVT] <check>: OK|FAIL" lines, "[FSVT] RESULT PASS|FAIL".
 *
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 */
#ifdef SECOS_FSVIRT_TEST
#include <stdint.h>
#include <stddef.h>
#include "vfs.h"
#include "ramfs.h"
#include "heap.h"
#include "debugcon.h"
#include "percpu.h"
#include "process.h"
#include "../crypto/user_hello_elf.h"

extern uint64_t ktime_ms(void);
extern uint32_t smp_online_count(void);
extern uint64_t pmm_get_free_memory(void);
extern const vfs_fs_ops_t* ramfs_vfs_ops(void);
extern int vfs_readlink(const char* path, char* buf, size_t len);

#define FSVT_ITERS      300     /* ramfs iterations per CPU and pass        */
#define FSVT_PROC_MS    4000    /* procfs phase length                      */
#define FSVT_NEG_MS     3000    /* negative-control phase length            */
#define FSVT_HELD       3       /* held processes                          */
#define FSVT_REC        32      /* log record size                          */
#define FSVT_STRIPE     512     /* shared-file stripe per CPU               */
#define FSVT_MAXCPU     16
#define CWD_LEN         200

enum { PH_NONE = 0, PH_RAMFS, PH_PROC, PH_PROC_NOLOCK, PH_PROC_READ };

static volatile uint32_t g_phase, g_gen, g_stop, g_pass;
static volatile uint32_t g_in, g_out;
static volatile uint32_t g_seen_gen[SMP_MAX_CPUS];
static volatile uint32_t g_fail, g_ok;
static volatile uint32_t g_torn, g_reads, g_badfd, g_fdreads, g_gone;
static uint32_t g_ncpu;
static process_t* g_held[FSVT_HELD];
static volatile uint32_t g_held_pid[FSVT_HELD];
static vfs_inode_t* g_fd_ino;                 /* /fsvt/fdA: a real ramfs inode   */
static char g_fake_pipe[256];                 /* never a valid path if mis-read */
static char g_cwd_a[CWD_LEN + 2], g_cwd_b[CWD_LEN + 2];

static void say(const char* s) { debugcon_writestring(s); }
static void dec(uint64_t v) {
    char b[24]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) debugcon_putchar(b[--n]);
}
static void check(const char* what, int good) {
    say("[FSVT] "); say(what); say(good ? ": OK\n" : ": FAIL\n");
    if (good) __atomic_add_fetch(&g_ok, 1, __ATOMIC_SEQ_CST);
    else __atomic_add_fetch(&g_fail, 1, __ATOMIC_SEQ_CST);
}
/* The same check, once per ramfs pass: "<what> (pass N)". */
static void checkp(const char* what, uint32_t pass, int good) {
    char b[96]; int o = 0;
    for (int i = 0; what[i] && o < 80; i++) b[o++] = what[i];
    const char* t = " (pass ";
    for (int i = 0; t[i]; i++) b[o++] = t[i];
    b[o++] = (char)('0' + pass % 10); b[o++] = ')'; b[o] = 0;
    check(b, good);
}
static void fail_once(const char* what, uint32_t cpu, uint32_t it) {
    if (__atomic_add_fetch(&g_fail, 1, __ATOMIC_SEQ_CST) > 40) return;   /* rate-limit */
    say("[FSVT] FAIL "); say(what); say(" cpu="); dec(cpu); say(" it="); dec(it); say("\n");
}
static int seq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* ------------------------------------------------------------------ ramfs */
static uint8_t pat(uint32_t w, uint32_t it, uint32_t off) { return (uint8_t)(w * 131u + it * 7u + off * 13u + (off >> 8)); }

static void mkname(char* out, const char* pre, uint32_t w, uint32_t k) {
    int o = 0;
    for (int i = 0; pre[i]; i++) out[o++] = pre[i];
    char d[12]; int n = 0; uint32_t v = w; do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out[o++] = d[--n];
    out[o++] = '_';
    v = k; do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out[o++] = d[--n];
    out[o] = 0;
}

struct cnt_ctx { uint32_t n, bad; };
static void cnt_cb(const vfs_inode_t* c, void* u) {
    struct cnt_ctx* x = (struct cnt_ctx*)u;
    x->n++;
    const char* p = c->path;
    if (!(p[0] == '/' && p[1] == 'f' && p[2] == 's' && p[3] == 'v' && p[4] == 't' && p[5] == '/')) x->bad++;
    for (int i = 6; p[i]; i++) { char ch = p[i]; if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '/')) { x->bad++; break; } }
}

/* Atomic append the way C1's VFS_OFF_APPEND will reach ramfs: the size is
 * read and the bytes written in one ramfs lock section. */
static int log_append(const uint8_t* rec) {
    klock_lock(ramfs_fs_lock());
    ramfs_entry_t* e = (ramfs_entry_t*)ramfs_find_locked("fsvt/log");
    int r = e ? ramfs_write_entry_locked(e, e->size, rec, FSVT_REC) : -1;
    klock_unlock(ramfs_fs_lock());
    return r;
}

static void ramfs_job(uint32_t w, uint32_t pass) {
    const vfs_fs_ops_t* o = ramfs_vfs_ops();
    uint8_t* buf = (uint8_t*)kmalloc(8192);
    uint8_t* rd = (uint8_t*)kmalloc(8192);
    if (!buf || !rd) { fail_once("kmalloc", w, 0); if (buf) kfree(buf); if (rd) kfree(rd); return; }
    char a[48], b[48], d[48], df[64];
    vfs_inode_t* shared = o->lookup("/fsvt/shared");
    for (uint32_t it = 0; it < FSVT_ITERS; it++) {
        uint32_t k = it % 2, tag = pass * 1000u + it;
        mkname(a, "/fsvt/a", w, k);
        mkname(b, "/fsvt/b", w, k);
        uint32_t l0 = 100u + (it * 37u) % 900u, len = l0 + 1000u + (it * 911u) % 5000u;
        for (uint32_t i = 0; i < len; i++) buf[i] = pat(w, tag, i);
        if (o->create(a, buf, l0) != 0) { fail_once("create", w, it); continue; }
        vfs_inode_t* ino = o->lookup(a);
        if (!ino) { fail_once("lookup after create", w, it); continue; }
        for (uint32_t off = l0; off < len; ) {                 /* grow in chunks */
            uint32_t n = 1u + (off * 7u + it) % 700u; if (n > len - off) n = len - off;
            if (o->write(ino, off, buf + off, n) != (int)n) { fail_once("write", w, it); break; }
            off += n;
        }
        if (ino->size != len) fail_once("inode size after write", w, it);
        if (o->read(ino, 0, rd, 8192) != (int)len) fail_once("read length", w, it);
        else for (uint32_t i = 0; i < len; i++) if (rd[i] != buf[i]) { fail_once("read content", w, it); break; }
        uint32_t half = len / 2;
        if (o->truncate(a, half) != 0) fail_once("truncate", w, it);
        if (o->read(ino, 0, rd, 8192) != (int)half) fail_once("read after truncate", w, it);
        vfs_attr_t at = { 0640, 1000 + w, 1000 + w, 0, 0 };
        if (o->setattr(a, &at, VFS_ATTR_MODE | VFS_ATTR_UID | VFS_ATTR_GID) != 0) fail_once("setattr", w, it);
        if ((ino->mode & 07777) != 0640 || ino->uid != 1000 + w) fail_once("setattr result", w, it);
        if (o->rename(a, b) != 0) { fail_once("rename", w, it); o->remove(a); continue; }
        if (o->lookup(a)) fail_once("old name still there", w, it);
        vfs_inode_t* nb = o->lookup(b);
        if (!nb || nb != ino) fail_once("renamed inode", w, it);
        else if (o->read(nb, 0, rd, 8192) != (int)half) fail_once("read after rename", w, it);
        else for (uint32_t i = 0; i < half; i++) if (rd[i] != buf[i]) { fail_once("content after rename", w, it); break; }
        /* the shared log: one 32-byte record, appended atomically */
        uint8_t rec[FSVT_REC];
        rec[0] = (uint8_t)w; rec[1] = (uint8_t)pass;
        rec[2] = (uint8_t)it; rec[3] = (uint8_t)(it >> 8);
        for (int i = 4; i < FSVT_REC; i++) rec[i] = pat(w, it, (uint32_t)i);
        if (log_append(rec) != FSVT_REC) fail_once("log append", w, it);
        /* my stripe of the shared file (a pre-sized inode everybody holds) */
        if (shared) {
            for (uint32_t i = 0; i < FSVT_STRIPE; i++) buf[i] = pat(w + 77, tag, i);
            if (o->write(shared, w * FSVT_STRIPE, buf, FSVT_STRIPE) != FSVT_STRIPE) fail_once("stripe write", w, it);
            if (o->read(shared, w * FSVT_STRIPE, rd, FSVT_STRIPE) != FSVT_STRIPE) fail_once("stripe read", w, it);
            else for (uint32_t i = 0; i < FSVT_STRIPE; i++) if (rd[i] != buf[i]) { fail_once("stripe content", w, it); break; }
        }
        /* listing: every entry is a well-formed /fsvt/ name */
        struct cnt_ctx c = { 0, 0 };
        if (o->readdir("/fsvt", cnt_cb, &c) != 0 || c.bad || c.n < 3) fail_once("readdir", w, it);
        if ((it & 7) == 3) {                                    /* a subdirectory */
            mkname(d, "/fsvt/d", w, 0);
            int pl = 0; while (d[pl]) { df[pl] = d[pl]; pl++; }
            df[pl++] = '/'; df[pl++] = 'x'; df[pl] = 0;
            if (o->mkdir(d) != 0) fail_once("mkdir", w, it);
            else {
                if (o->create(df, "xyz", 3) != 0) fail_once("create in dir", w, it);
                struct cnt_ctx c2 = { 0, 0 };
                o->readdir(d, cnt_cb, &c2);
                if (c2.n != 1) fail_once("subdir listing", w, it);
                if (o->remove(df) != 0) fail_once("remove in dir", w, it);
                if (o->remove(d) != 0) fail_once("rmdir", w, it);
            }
        }
        if (o->remove(b) != 0) fail_once("remove", w, it);
    }
    kfree(buf);
    kfree(rd);
}

/* ----------------------------------------------------------------- procfs */
static void pidpath(char* out, uint32_t pid, const char* leaf) {
    int o = 0; const char* pre = "/proc/";
    while (*pre) out[o++] = *pre++;
    char d[12]; int n = 0; do { d[n++] = (char)('0' + pid % 10); pid /= 10; } while (pid);
    while (n) out[o++] = d[--n];
    out[o++] = '/';
    while (*leaf) out[o++] = *leaf++;
    out[o] = 0;
}

/* The writer: what ksys_chdir and the fd install/close paths do. */
static void proc_mutate(int locked, uint32_t round) {
    for (int h = 0; h < FSVT_HELD; h++) {
        process_t* p = g_held[h];
        if (!p) continue;
        const char* src = (round + (uint32_t)h) & 1 ? g_cwd_a : g_cwd_b;
        uint64_t fl = 0;
        if (locked) fl = mm_lock(p);
        for (int i = 0; i <= CWD_LEN + 1; i++) ((volatile char*)p->cwd)[i] = src[i];
        if (locked) {
            struct proc_fd_entry* e = &p->fds[5];
            switch (round % 3) {
            case 0: e->used = 1; e->is_pipe = 0; e->is_sock = 0; e->inode = g_fd_ino; break;
            case 1: e->used = 1; e->is_pipe = 1; e->pipe_w = 1; e->inode = g_fake_pipe; break;
            default: e->used = 0; e->inode = NULL; e->is_pipe = 0; break;
            }
            mm_unlock(p, fl);
        }
    }
}

/* Read until g_stop, or until `until` (ms, 0 = no deadline). */
static void proc_reader(uint32_t w, int check_fd, uint64_t until) {
    char path[64], out[300];
    uint32_t it = 0;
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) && (!until || ktime_ms() < until)) {
        int h = (int)((it + w) % FSVT_HELD);
        uint32_t pid = g_held_pid[h];
        it++;
        if (!pid) continue;
        pidpath(path, pid, "cwd");
        int n = vfs_readlink(path, out, sizeof(out));
        if (n < 0) { __atomic_add_fetch(&g_gone, 1, __ATOMIC_RELAXED); continue; }
        __atomic_add_fetch(&g_reads, 1, __ATOMIC_RELAXED);
        if (!seq(out, g_cwd_a) && !seq(out, g_cwd_b) && !seq(out, "/")) __atomic_add_fetch(&g_torn, 1, __ATOMIC_RELAXED);
        if (!check_fd) continue;
        pidpath(path, pid, "fd/5");
        n = vfs_readlink(path, out, sizeof(out));
        if (n >= 0) {
            __atomic_add_fetch(&g_fdreads, 1, __ATOMIC_RELAXED);
            int pipe = out[0] == 'p' && out[1] == 'i' && out[2] == 'p' && out[3] == 'e' && out[4] == ':';
            if (!pipe && !seq(out, "/fsvt/fda")) __atomic_add_fetch(&g_badfd, 1, __ATOMIC_RELAXED);
        }
        static const char* leaves[3] = { "status", "stat", "maps" };
        pidpath(path, pid, leaves[it % 3]);
        vfs_inode_t* ino = vfs_lookup(path);
        if (ino && ino->ops && ino->ops->read) ino->ops->read(ino, 0, out, sizeof(out) - 1);
    }
}

/* ---------------------------------------------------------------- driver */
/* PH_PROC / PH_PROC_NOLOCK: CPU 1 is the writer, everybody else reads.
 * PH_PROC_READ: everybody reads (the held processes are being destroyed). */
static void run_job(uint32_t phase, uint32_t me) {
    if (phase == PH_RAMFS) {
        ramfs_job(me, g_pass);
    } else if ((phase == PH_PROC || phase == PH_PROC_NOLOCK) && me == 1) {
        uint32_t r = 0;
        while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) proc_mutate(phase == PH_PROC, r++);
    } else if (phase != PH_NONE) {
        proc_reader(me, phase != PH_PROC_NOLOCK, 0);
    }
}

/* Called from every AP's idle loop (kernel/smp.c). */
void fsvirt_selftest_ap(cpu_t* c) {
    uint32_t gen = __atomic_load_n(&g_gen, __ATOMIC_ACQUIRE);
    uint32_t ph = __atomic_load_n(&g_phase, __ATOMIC_ACQUIRE);
    if (!ph || g_seen_gen[c->index] == gen) return;
    g_seen_gen[c->index] = gen;
    __atomic_add_fetch(&g_in, 1, __ATOMIC_SEQ_CST);
    run_job(ph, c->index);
    __atomic_add_fetch(&g_out, 1, __ATOMIC_SEQ_CST);
}

/* Start `phase` on every AP and wait until they all joined. */
static void phase_start(uint32_t phase) {
    g_in = 0; g_out = 0; g_stop = 0;
    __atomic_store_n(&g_phase, phase, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_gen, 1, __ATOMIC_SEQ_CST);
    uint64_t t0 = ktime_ms();
    while (g_in < g_ncpu - 1 && ktime_ms() - t0 < 3000) __asm__ volatile("pause");
}
/* Stop the APs and wait until they all left. */
static void phase_end(uint32_t phase, uint64_t t0) {
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    while (g_out < g_in && ktime_ms() - t0 < 120000) __asm__ volatile("pause");
    __atomic_store_n(&g_phase, PH_NONE, __ATOMIC_RELEASE);
    say("[FSVT] phase "); dec(phase); say(" cpus="); dec(g_in + 1); say(" done="); dec(g_out + 1);
    say(" ms="); dec(ktime_ms() - t0); say("\n");
    if (g_out != g_in || g_in != g_ncpu - 1) check("every CPU took part in the phase and finished", 0);
}
/* Run `phase` on every CPU; the BSP does its own share (ramfs) or reads for
 * `ms` (procfs) and then stops the others. */
static void run_phase(uint32_t phase, uint32_t ms) {
    uint64_t t0 = ktime_ms();
    phase_start(phase);
    if (ms) proc_reader(0, phase != PH_PROC_NOLOCK, ktime_ms() + ms);
    else run_job(phase, 0);
    phase_end(phase, t0);
}

static int ramfs_free_slots(void) {
    static const ramfs_entry_t* all[RAMFS_MAX_FILES];
    klock_lock(ramfs_fs_lock());
    size_t n = ramfs_list_locked(all, RAMFS_MAX_FILES);
    klock_unlock(ramfs_fs_lock());
    return (int)(RAMFS_MAX_FILES - n);
}

static void verify_log(uint32_t pass, uint32_t ncpu) {
    const vfs_fs_ops_t* o = ramfs_vfs_ops();
    vfs_inode_t* ino = o->lookup("/fsvt/log");
    uint32_t want = ncpu * FSVT_ITERS * FSVT_REC;
    checkp("log holds one record per append", pass, ino && ino->size == want);
    if (!ino || ino->size != want) return;
    uint8_t* buf = (uint8_t*)kmalloc(want);
    if (!buf) { check("log kmalloc", 0); return; }
    int good = o->read(ino, 0, buf, want) == (int)want;
    static uint32_t next[FSVT_MAXCPU];
    for (uint32_t c = 0; c < FSVT_MAXCPU; c++) next[c] = 0;
    for (uint32_t r = 0; good && r < want / FSVT_REC; r++) {
        const uint8_t* rec = buf + r * FSVT_REC;
        uint32_t w = rec[0], it = (uint32_t)rec[2] | ((uint32_t)rec[3] << 8);
        if (w >= ncpu || rec[1] != (uint8_t)pass || it != next[w]) { good = 0; break; }
        for (int i = 4; i < FSVT_REC; i++) if (rec[i] != pat(w, it, (uint32_t)i)) { good = 0; break; }
        next[w]++;
    }
    for (uint32_t c = 0; good && c < ncpu; c++) if (next[c] != FSVT_ITERS) good = 0;
    checkp("log records intact, each CPU's in order, none lost", pass, good);
    kfree(buf);
}

void fsvirt_selftest(void) {
    g_ncpu = smp_online_count();
    if (g_ncpu == 0) g_ncpu = 1;
    if (g_ncpu > FSVT_MAXCPU) g_ncpu = FSVT_MAXCPU;
    say("[FSVT] start cpus="); dec(g_ncpu); say("\n");
    const vfs_fs_ops_t* o = ramfs_vfs_ops();
    int need = (int)g_ncpu * 3 + 6;
    int freeslots = ramfs_free_slots();
    if (freeslots < need) { say("[FSVT] not enough free ramfs slots\n"); check("ramfs slots", 0); goto out; }
    if (o->mkdir("/fsvt") != 0 && !o->lookup("/fsvt")) { check("mkdir /fsvt", 0); goto out; }

    /* ---- ramfs: two passes, MemFree must not move between them ---- */
    uint64_t mf[3];
    for (uint32_t pass = 1; pass <= 2; pass++) {
        o->remove("/fsvt/log"); o->remove("/fsvt/shared");
        static uint8_t zero[FSVT_MAXCPU * FSVT_STRIPE];
        o->create("/fsvt/log", "", 0);
        o->create("/fsvt/shared", zero, g_ncpu * FSVT_STRIPE);
        if (pass == 1) mf[0] = pmm_get_free_memory();
        g_pass = pass;
        run_phase(PH_RAMFS, 0);
        verify_log(pass, g_ncpu);
        checkp("heap sound after the ramfs pass (heap_check)", pass, heap_check() == 0);
        checkp("ramfs slots all released", pass, ramfs_free_slots() == freeslots - 3);
        mf[pass] = pmm_get_free_memory();
    }
    say("[FSVT] MemFree KiB before="); dec(mf[0] / 1024); say(" pass1="); dec(mf[1] / 1024);
    say(" pass2="); dec(mf[2] / 1024); say("\n");
    check("MemFree steady across ramfs passes", mf[2] + 64 * 1024 >= mf[1]);

    /* ---- procfs readers against cwd/fd rewrites, then exits ---- */
    g_cwd_a[0] = g_cwd_b[0] = '/';
    for (int i = 1; i <= CWD_LEN; i++) { g_cwd_a[i] = 'a'; g_cwd_b[i] = 'b'; }
    g_cwd_a[CWD_LEN + 1] = g_cwd_b[CWD_LEN + 1] = 0;
    for (int i = 0; i < (int)sizeof(g_fake_pipe) - 1; i++) g_fake_pipe[i] = 'Z';
    o->create("/fsvt/fda", "x", 1);
    g_fd_ino = o->lookup("/fsvt/fda");
    for (int h = 0; h < FSVT_HELD; h++) {
        g_held[h] = process_create_from_elf(user_hello_elf, user_hello_elf_len);
        g_held_pid[h] = g_held[h] ? g_held[h]->pid : 0;
    }
    check("held processes created", g_held[0] && g_held[FSVT_HELD - 1] && g_fd_ino);
    if (g_ncpu < 2) { say("[FSVT] one CPU: procfs race phases skipped\n"); goto destroy; }
    g_torn = g_reads = g_badfd = g_fdreads = g_gone = 0;
    run_phase(PH_PROC, FSVT_PROC_MS);
    say("[FSVT] proc reads="); dec(g_reads); say(" torn="); dec(g_torn);
    say(" fd_reads="); dec(g_fdreads); say(" bad_fd="); dec(g_badfd); say("\n");
    check("procfs: every cwd read whole (locked writer)", g_reads > 100 && g_torn == 0);
    check("procfs: every fd link a real object (locked writer)", g_fdreads > 50 && g_badfd == 0);
    g_torn = g_reads = 0;
    run_phase(PH_PROC_NOLOCK, FSVT_NEG_MS);
    say("[FSVT] negative control reads="); dec(g_reads); say(" torn="); dec(g_torn); say("\n");
    check("negative control: an unlocked cwd writer is seen torn", g_torn > 0);
destroy:
    /* Put fd 5 back (the writer installed a fake pipe end that teardown would
     * unref), then destroy the held processes while every CPU keeps reading
     * their /proc entries. */
    for (int h = 0; h < FSVT_HELD; h++) {
        process_t* p = g_held[h];
        if (!p) continue;
        uint64_t fl = mm_lock(p);
        p->fds[5].used = 0; p->fds[5].inode = NULL; p->fds[5].is_pipe = 0;
        mm_unlock(p, fl);
    }
    uint64_t t0 = ktime_ms();
    g_gone = 0; g_reads = 0;
    if (g_ncpu >= 2) {
        phase_start(PH_PROC_READ);
        while (ktime_ms() - t0 < 300) __asm__ volatile("pause");
    }
    for (int h = 0; h < FSVT_HELD; h++) {
        if (!g_held[h]) continue;
        process_t* t = process_detach_unstarted(g_held_pid[h]);
        g_held[h] = NULL;
        if (!t) { check("held process detached", 0); continue; }
        process_destroy(t);
        process_reap_done();
        uint64_t t1 = ktime_ms();
        while (ktime_ms() - t1 < 100) __asm__ volatile("pause");   /* readers hit the gone pid */
    }
    if (g_ncpu >= 2) {
        uint64_t t1 = ktime_ms();
        while (ktime_ms() - t1 < 300) __asm__ volatile("pause");
        phase_end(PH_PROC_READ, t0);
        say("[FSVT] exits: reads="); dec(g_reads); say(" failed-cleanly="); dec(g_gone); say("\n");
        check("procfs: readers of exiting processes fail cleanly", g_gone > 0);
    }
    check("heap sound after the procfs phases (heap_check)", heap_check() == 0);
    o->remove("/fsvt/fda"); o->remove("/fsvt/log"); o->remove("/fsvt/shared"); o->remove("/fsvt");
out:
    say(g_fail ? "[FSVT] RESULT FAIL\n" : "[FSVT] RESULT PASS\n");
}
#endif /* SECOS_FSVIRT_TEST */
