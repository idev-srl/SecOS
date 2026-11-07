/*
 * SecOS Kernel - FAT32 / exFAT / NTFS lock stress self-test (M51 C4,
 * -DSECOS_C4_FSSTRESS)
 *
 * Every CPU (the BSP from kernel_main, each AP from its idle loop) pulls jobs
 * from one shared queue and runs them at the same time, outside the BKL — the
 * only way to exercise the per-driver K30 klocks with real concurrency while
 * the syscall path still serializes everything. Disks (tools/c4_fs_drive.py,
 * q35 AHCI): sda = fresh FAT32, sdb = fresh exFAT, sdc = the M42 NTFS image.
 * FAT32 is mounted at /c4f, exFAT at /c4x, the NTFS volume twice (/c4n and
 * /c4n2: two instances, two locks). Jobs, on FAT32 and exFAT at once:
 *   FILES   own directory: create 6 files, write a pattern in odd-sized
 *           chunks (+ page-cache invalidation, like write()), rename all,
 *           truncate half, unlink a third, mkdir/rmdir, verify every byte
 *           through read() and the page cache;
 *   APPEND  4 writers append 32-byte lines to one shared LOG.TXT through
 *           fat32_append / exfat_append (C1's ops->append; the returned
 *           offset must be line-aligned and grow): at the end the log must
 *           hold every line exactly once, in each writer's order, none torn;
 *   SHARED  4 writers create/write/rename/unlink in one shared directory
 *           (directory slot allocation, cluster allocation, FAT updates);
 *   SCAN    readdir of the shared directory and the root while they churn;
 * and on NTFS 6 readers hash every file of the image (lookup + read with
 * varying chunk sizes, the two instances alternating); all readers must
 * agree and the listing ("[C4N] F|E ...") is compared with the ntfs-3g
 * reference by the host. Afterwards every surviving file is verified again,
 * FAT32 and exFAT are synced and unmounted, and the host runs fsck.vfat -n,
 * fsck.exfat -n + exfat_img.py deepcheck and reads LOG.TXT back.
 * Output: "[C4] ..." lines ending with "[C4] RESULT PASS|FAIL".
 * -DSECOS_C4_NOLOCK (drivers without their locks) is the negative control.
 *
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 */
#ifdef SECOS_C4_FSSTRESS
#include "vfs.h"
#include "heap.h"
#include "debugcon.h"
#include "percpu.h"
#include "pagecache.h"
#include "fs_dirbuf.h"
#include "klock.h"
#include "fat32.h"
#include "exfat.h"
#include "ntfs.h"
#include <stdint.h>
#include <stddef.h>

extern uint64_t ktime_ms(void);
extern uint32_t smp_online_count(void);

#define TIMEOUT_MS  240000u
#define BUF_BYTES   65536u
#define NFILES      6
#define NFJOBS      8          /* FILES jobs per FS */
#define NAPPEND     4
#define LOG_LINES   150
#define LINE        32
#define NSHARED     4
#define SHR_K       20
#define NSCAN       2
#define NNTREAD     6
#define NT_MAXF     4096
#define NT_MAXD     512

enum { JK_FILES = 1, JK_APPEND, JK_SHARED, JK_SCAN, JK_NTREAD };
typedef struct { int kind, fs, w; } c4_job_t;    /* fs: 0 = FAT32, 1 = exFAT */

static const char* const g_root[2] = { "/c4f", "/c4x" };
static c4_job_t g_jobs[2 * (NFJOBS + NAPPEND + NSHARED + NSCAN) + NNTREAD];
static uint32_t g_njobs;

static volatile int      g_active, g_stop;
static volatile uint32_t g_next, g_in, g_out, g_fail, g_churn_left;
static volatile uint8_t  g_joined[SMP_MAX_CPUS];
static volatile uint32_t g_runs[SMP_MAX_CPUS];
static volatile uint32_t g_scans;

/* NTFS reference walk (BSP, before the run) and per-reader results. */
static char   (*g_ntp)[256];
static uint32_t g_ntn;
static uint64_t* g_nth[NNTREAD];          /* FNV-1a 64 of the file          */
static uint32_t* g_nte[NNTREAD];          /* read error (errno), 0 = none   */
static volatile uint8_t g_ntdone[NNTREAD];

static void say(const char* s) { debugcon_writestring(s); }
static void dec(uint64_t v) {
    char b[24]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) debugcon_putchar(b[--n]);
}
static void hex16(uint64_t v) {
    static const char* h = "0123456789abcdef";
    for (int i = 60; i >= 0; i -= 4) debugcon_putchar(h[(v >> i) & 15]);
}
static void fail(const char* what, const char* p) {
    __atomic_add_fetch(&g_fail, 1, __ATOMIC_SEQ_CST);
    say("[C4] FAIL "); say(what); say(" "); say(p ? p : "-"); say("\n");
}
static size_t sl(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void cat(char* d, const char* s) { size_t n = sl(d); while (*s) d[n++] = *s++; d[n] = 0; }
static void cat2(char* d, uint32_t v) { char t[3] = { (char)('0' + v / 10 % 10), (char)('0' + v % 10), 0 }; cat(d, t); }
static void pcpy(char* d, const char* s) { d[0] = 0; cat(d, s); }

static uint64_t mix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static uint8_t patb(uint32_t tag, uint64_t a) {
    return (uint8_t)(mix64(((uint64_t)tag << 48) ^ (a >> 3)) >> ((a & 7) * 8));
}

static const uint32_t g_chunks[] = { 1, 7, 512, 4096, 333, 2048, 1500, 65536, 513 };
#define NCHUNK (sizeof(g_chunks) / sizeof(g_chunks[0]))

/* Write `size` bytes of pattern `tag` to an existing empty file. */
static int put_file(const char* path, uint32_t tag, uint32_t size, uint8_t* buf, uint32_t k) {
    vfs_inode_t* ino = vfs_lookup(path);
    if (!ino || !ino->ops || !ino->ops->write) { fail("lookup after create", path); return -1; }
    for (uint32_t off = 0; off < size;) {
        uint32_t n = g_chunks[k++ % NCHUNK];
        if (n > size - off) n = size - off;
        if (n > BUF_BYTES) n = BUF_BYTES;
        for (uint32_t i = 0; i < n; i++) buf[i] = patb(tag, off + i);
        int r = ino->ops->write(ino, off, buf, n);
        if (r != (int)n) { fail("write", path); return -1; }
        pagecache_invalidate(ino, off, n);
        off += n;
    }
    return 0;
}
/* The file must be exactly `size` bytes of pattern `tag`. */
static int check_file(const char* path, uint32_t tag, uint32_t size, uint8_t* buf, int via_cache, uint32_t k) {
    vfs_inode_t* ino = vfs_lookup(path);
    if (!ino || ino->type != VFS_NODE_FILE) { fail("verify lookup", path); return -1; }
    if (ino->size != size) { say("[C4] size "); dec(ino->size); say(" want "); dec(size); say("\n"); fail("verify size", path); return -1; }
    for (uint32_t off = 0; off < size;) {
        uint32_t n = g_chunks[k++ % NCHUNK];
        if (n > size - off) n = size - off;
        if (n > BUF_BYTES) n = BUF_BYTES;
        int r = via_cache ? pagecache_read(ino, off, buf, n) : ino->ops->read(ino, off, buf, n);
        if (r != (int)n) { fail("verify read", path); return -1; }
        for (uint32_t i = 0; i < n; i++)
            if (buf[i] != patb(tag, off + i)) { say("[C4] at "); dec(off + i); say("\n"); fail("verify bytes", path); return -1; }
        off += n;
    }
    return 0;
}

/* ---- FILES ---- */
static void files_path(char* p, int fs, int w, char c, int i) {
    pcpy(p, g_root[fs]); cat(p, "/D"); cat2(p, (uint32_t)w);
    if (c) { char t[3] = { '/', c, 0 }; cat(p, t); cat2(p, (uint32_t)i); cat(p, ".DAT"); }
}
static uint32_t files_tag(int fs, int w, int i) { return 0x100u + (uint32_t)fs * 0x80u + (uint32_t)w * 8u + (uint32_t)i; }
static uint32_t files_size(int w, int i) { return 1000u + ((uint32_t)w * 7919u + (uint32_t)i * 3271u) % 30000u; }
static uint32_t files_final(int w, int i) { return (i & 1) ? files_size(w, i) / 2 : files_size(w, i); }
static int files_gone(int i) { return i % 3 == 0; }

static void job_files(int fs, int w, uint8_t* buf) {
    char d[64], p[64], q[64];
    files_path(d, fs, w, 0, 0);
    if (vfs_mkdir(d) != 0) { fail("mkdir", d); return; }
    for (int i = 0; i < NFILES; i++) {
        files_path(p, fs, w, 'F', i);
        if (vfs_create(p, "", 0) != 0) { fail("create", p); return; }
        if (put_file(p, files_tag(fs, w, i), files_size(w, i), buf, (uint32_t)(w + i)) != 0) return;
    }
    for (int i = 0; i < NFILES; i++) {
        files_path(p, fs, w, 'F', i); files_path(q, fs, w, 'G', i);
        if (vfs_rename(p, q) != 0) { fail("rename", p); return; }
        if (vfs_lookup(p)) { fail("old name still there", p); return; }
    }
    for (int i = 0; i < NFILES; i++) {
        files_path(q, fs, w, 'G', i);
        if ((i & 1) && vfs_truncate(q, files_size(w, i) / 2) != 0) { fail("truncate", q); return; }
        if (files_gone(i) && vfs_remove(q) != 0) { fail("unlink", q); return; }
    }
    pcpy(p, d); cat(p, "/SUB");
    if (vfs_mkdir(p) != 0 || !vfs_lookup(p) || vfs_rmdir(p) != 0 || vfs_lookup(p)) { fail("mkdir/rmdir", p); return; }
    for (int i = 0; i < NFILES; i++) {
        files_path(q, fs, w, 'G', i);
        if (files_gone(i)) { if (vfs_lookup(q)) fail("unlinked file still there", q); continue; }
        check_file(q, files_tag(fs, w, i), files_final(w, i), buf, i & 2, (uint32_t)i);
    }
}

/* ---- APPEND: lines "Axx Lyyyy <fs> ..................\n" (32 bytes) ---- */
static void mkline(char* l, int fs, int w, int k) {
    for (int i = 0; i < LINE; i++) l[i] = '.';
    l[0] = 'A'; l[1] = (char)('0' + w / 10); l[2] = (char)('0' + w % 10); l[3] = ' ';
    l[4] = 'L'; l[5] = (char)('0' + k / 1000 % 10); l[6] = (char)('0' + k / 100 % 10);
    l[7] = (char)('0' + k / 10 % 10); l[8] = (char)('0' + k % 10); l[9] = ' ';
    l[10] = fs ? 'X' : 'F'; l[11] = ' '; l[LINE - 1] = '\n';
}
/* The drivers' atomic-append entry points (C1's vfs_fs_ops.append). */
static int (*const g_append[2])(vfs_inode_t*, const void*, size_t, uint64_t*) = { fat32_append, exfat_append };
static void job_append(int fs, int w) {
    char p[64]; pcpy(p, g_root[fs]); cat(p, "/LOG.TXT");
    vfs_inode_t* ino = vfs_lookup(p);
    if (!ino || !ino->ops || !ino->ops->write) { fail("log lookup", p); return; }
    char l[LINE];
    uint64_t last = 0;
    for (int k = 0; k < LOG_LINES; k++) {
        mkline(l, fs, w, k);
        uint64_t at = ~0ULL;
        int r = g_append[fs](ino, l, LINE, &at);
        if (r != LINE || at == ~0ULL || at % LINE || (k && at <= last)) { fail("append", p); return; }
        last = at;
        pagecache_invalidate(ino, at, LINE);
    }
}
static void check_log(int fs, uint8_t* buf) {
    char p[64]; pcpy(p, g_root[fs]); cat(p, "/LOG.TXT");
    vfs_inode_t* ino = vfs_lookup(p);
    uint32_t want = NAPPEND * LOG_LINES * LINE;
    if (!ino || ino->size != want) {
        say("[C4] log size "); dec(ino ? ino->size : 0); say(" want "); dec(want); say("\n");
        fail("log size", p); return;
    }
    if (ino->ops->read(ino, 0, buf, want) != (int)want) { fail("log read", p); return; }
    int next[NAPPEND] = { 0 };
    for (uint32_t o = 0; o < want; o += LINE) {
        const char* l = (const char*)buf + o;
        int w = (l[1] - '0') * 10 + (l[2] - '0');
        int k = (l[5] - '0') * 1000 + (l[6] - '0') * 100 + (l[7] - '0') * 10 + (l[8] - '0');
        char ref[LINE];
        if (w < 0 || w >= NAPPEND || k < 0 || k >= LOG_LINES) { say("[C4] bad line at "); dec(o); say("\n"); fail("log torn line", p); return; }
        mkline(ref, fs, w, k);
        for (int i = 0; i < LINE; i++) if (ref[i] != l[i]) { say("[C4] torn at "); dec(o); say("\n"); fail("log torn line", p); return; }
        if (k != next[w]) { say("[C4] order w="); dec((uint64_t)w); say(" k="); dec((uint64_t)k); say("\n"); fail("log order", p); return; }
        next[w]++;
    }
    say("[C4] "); say(p); say(" "); dec(want / LINE); say(" lines, every one whole and in order\n");
}

/* ---- SHARED ---- */
static void shr_path(char* p, int fs, int w, char c, int k, const char* ext) {
    pcpy(p, g_root[fs]); cat(p, "/SHR/S"); cat2(p, (uint32_t)w);
    char t[2] = { c, 0 }; cat(p, t); cat2(p, (uint32_t)k); cat(p, ext);
}
static uint32_t shr_tag(int fs, int w, int k) { return 0x800u + (uint32_t)fs * 0x100u + (uint32_t)w * 0x20u + (uint32_t)k; }
static void job_shared(int fs, int w, uint8_t* buf) {
    char p[64], q[64];
    for (int k = 0; k < SHR_K; k++) {
        shr_path(p, fs, w, 'N', k, ".TMP"); shr_path(q, fs, w, 'R', k, ".DAT");
        if (vfs_create(p, "", 0) != 0) { fail("shared create", p); return; }
        if (put_file(p, shr_tag(fs, w, k), 100u + (uint32_t)k * 97u, buf, (uint32_t)k) != 0) return;
        if (vfs_rename(p, q) != 0) { fail("shared rename", p); return; }
        if ((k & 1) && vfs_remove(q) != 0) { fail("shared unlink", q); return; }
    }
}
struct scan_ctx { int n, bad; const char* pre; };
static void scan_cb(const vfs_inode_t* c, void* u) {
    struct scan_ctx* x = (struct scan_ctx*)u;
    x->n++;
    const char* a = c->path; const char* b = x->pre;
    while (*b && *a == *b) { a++; b++; }
    if (*b || !*a || (c->type != VFS_NODE_FILE && c->type != VFS_NODE_DIR)) x->bad++;
}
static void job_scan(int fs) {
    char d[64]; pcpy(d, g_root[fs]); cat(d, "/SHR");
    for (int it = 0; it < 4000 && __atomic_load_n(&g_churn_left, __ATOMIC_ACQUIRE); it++) {
        struct scan_ctx a = { 0, 0, "/SHR/" }, b = { 0, 0, "/" };
        if (vfs_readdir(d, scan_cb, &a) != 0) { fail("scan readdir", d); return; }
        if (vfs_readdir(g_root[fs], scan_cb, &b) != 0) { fail("scan readdir", g_root[fs]); return; }
        if (a.bad || b.bad) { fail("scan malformed entry", d); return; }
        __atomic_add_fetch(&g_scans, 1, __ATOMIC_RELAXED);
    }
}
static void check_shared(int fs, uint8_t* buf) {
    char d[64], q[64]; pcpy(d, g_root[fs]); cat(d, "/SHR");
    struct scan_ctx a = { 0, 0, "/SHR/" };
    if (vfs_readdir(d, scan_cb, &a) != 0 || a.bad || a.n != NSHARED * SHR_K / 2) {
        say("[C4] shared entries "); dec((uint64_t)a.n); say("\n"); fail("shared listing", d); return;
    }
    for (int w = 0; w < NSHARED; w++)
        for (int k = 0; k < SHR_K; k++) {
            shr_path(q, fs, w, 'R', k, ".DAT");
            if (k & 1) { if (vfs_lookup(q)) fail("shared unlinked still there", q); }
            else check_file(q, shr_tag(fs, w, k), 100u + (uint32_t)k * 97u, buf, k & 2, (uint32_t)k);
        }
}

/* ---- NTFS readers ---- */
#define NT_LINK 0x10000u
/* Each reader alternates between the two instances file by file, so both
 * volume locks are contended at once. */
static void job_ntread(int r, uint8_t* buf) {
    char p[300];
    uint32_t k = (uint32_t)r;
    for (uint32_t i = 0; i < g_ntn; i++) {
        uint32_t j = (i + (uint32_t)r * 577u) % g_ntn;
        const char* mp = ((i + (uint32_t)r) & 1) ? "/c4n2" : "/c4n";
        pcpy(p, mp); cat(p, g_ntp[j]);
        vfs_inode_t* ino = vfs_lookup(p);
        /* An Interix symlink is a system file in its index entry (readdir
         * says file), only its contents make it a link (lookup says link). */
        if (ino && ino->type == VFS_NODE_SYMLINK) { g_nth[r][j] = 0; g_nte[r][j] = NT_LINK; continue; }
        if (!ino || ino->type != VFS_NODE_FILE) { g_nth[r][j] = 0; g_nte[r][j] = 0xFFFF; continue; }
        uint64_t h = 0xCBF29CE484222325ULL;
        int err = 0;
        for (uint64_t off = 0; off < ino->size;) {
            uint32_t n = g_chunks[k++ % NCHUNK];
            if (n > ino->size - off) n = (uint32_t)(ino->size - off);
            if (n > BUF_BYTES) n = BUF_BYTES;
            int x = ino->ops->read(ino, off, buf, n);
            if (x < 0) { err = -x; break; }
            if (x == 0 || (uint32_t)x > n) { err = 999; break; }
            for (int b = 0; b < x; b++) { h ^= buf[b]; h *= 0x100000001B3ULL; }
            off += (uint64_t)x;
        }
        g_nth[r][j] = err ? 0 : h;
        g_nte[r][j] = (uint32_t)err;
        if ((i & 255) == 0) { struct scan_ctx a = { 0, 0, "/" }; vfs_readdir(mp, scan_cb, &a); }
    }
    g_ntdone[r] = 1;
}
static void nt_coll_cb(const vfs_inode_t* c, void* u) {
    char (*dirs)[256] = (char (*)[256])u;
    uint32_t* nd = (uint32_t*)dirs[NT_MAXD];          /* counter after the array */
    if (c->type == VFS_NODE_DIR) {
        if (*nd < NT_MAXD) { pcpy(dirs[*nd], c->path); (*nd)++; }
    } else if (c->type == VFS_NODE_FILE && g_ntn < NT_MAXF) {
        pcpy(g_ntp[g_ntn++], c->path);
    }
}
static int nt_walk(void) {
    char (*dirs)[256] = kmalloc((NT_MAXD + 1) * 256);
    g_ntp = kmalloc(NT_MAXF * 256);
    if (!dirs || !g_ntp) return -1;
    uint32_t* nd = (uint32_t*)dirs[NT_MAXD];
    *nd = 1; dirs[0][0] = 0;                           /* "" = the root */
    char p[300];
    for (uint32_t i = 0; i < *nd; i++) {
        pcpy(p, "/c4n"); cat(p, dirs[i]);
        if (vfs_readdir(p, nt_coll_cb, dirs) != 0) { fail("ntfs walk readdir", p); return -1; }
    }
    kfree(dirs);
    for (int r = 0; r < NNTREAD; r++) {
        g_nth[r] = kmalloc(sizeof(uint64_t) * (g_ntn + 1));
        g_nte[r] = kmalloc(sizeof(uint32_t) * (g_ntn + 1));
        if (!g_nth[r] || !g_nte[r]) return -1;
    }
    return 0;
}

/* ---- queue ---- */
static void run_job(const c4_job_t* j, uint8_t* buf) {
    switch (j->kind) {
    case JK_FILES:  job_files(j->fs, j->w, buf); break;
    case JK_APPEND: job_append(j->fs, j->w); break;
    case JK_SHARED: job_shared(j->fs, j->w, buf); break;
    case JK_SCAN:   job_scan(j->fs); break;
    case JK_NTREAD: job_ntread(j->w, buf); break;
    }
    if (j->kind != JK_SCAN && j->kind != JK_NTREAD) __atomic_sub_fetch(&g_churn_left, 1, __ATOMIC_RELEASE);
}
static void worker(void) {
    uint32_t me = this_cpu()->index;
    uint8_t* buf = (uint8_t*)kmalloc(BUF_BYTES);
    if (!buf) { fail("kmalloc", 0); return; }
    __atomic_add_fetch(&g_in, 1, __ATOMIC_SEQ_CST);
    uint64_t t0 = ktime_ms();
    while (!g_stop && ktime_ms() - t0 < TIMEOUT_MS) {
        uint32_t i = __atomic_fetch_add(&g_next, 1, __ATOMIC_SEQ_CST);
        if (i >= g_njobs) break;
        run_job(&g_jobs[i], buf);
        g_runs[me]++;
    }
    kfree(buf);
    __atomic_add_fetch(&g_out, 1, __ATOMIC_SEQ_CST);
}
/* Called from every AP's idle loop (kernel/smp.c). */
void c4_fsstress_ap(cpu_t* c) {
    if (!g_active || g_joined[c->index]) return;
    g_joined[c->index] = 1;
    worker();
}

static void add(int kind, int fs, int w) { g_jobs[g_njobs].kind = kind; g_jobs[g_njobs].fs = fs; g_jobs[g_njobs].w = w; g_njobs++; }

static const vfs_fs_ops_t* ops_at(const char* mp) {
    int n = vfs_mount_count();
    for (int i = 0; i < n; i++) {
        const char* m = 0; const char* f = 0;
        if (vfs_mount_info(i, &m, &f) == 0 && m) {
            const char* a = m; const char* b = mp;
            while (*a && *a == *b) { a++; b++; }
            if (!*a && !*b) return vfs_mount_ops_at(i);
        }
    }
    return NULL;
}
static void lockstat(const char* nm, const klock_t* l) {
    if (!l) return;
    say("[C4] lock "); say(nm); say(" acquisitions="); dec(l->n_acq);
    say(" contended="); dec(l->n_contended); say("\n");
}

void c4_fsstress_run(void) {
    /* The boot probe may have put sda on /mnt: take it off, then mount ours. */
    const vfs_fs_ops_t* old = ops_at("/mnt");
    if (old) { vfs_sync_ops(old); vfs_unmount("/mnt"); }
    int mf = fat32_mount("sda", "/c4f"), mx = exfat_mount("sdb", "/c4x");
    int mn = ntfs_mount("sdc", "/c4n"), mn2 = ntfs_mount("sdc", "/c4n2");
    if (mf || mx || mn || mn2) {
        say("[C4] SKIP: needs FAT32 sda, exFAT sdb, NTFS sdc (mount fat32/exfat/ntfs/ntfs2 = ");
        dec((uint64_t)(mf != 0)); dec((uint64_t)(mx != 0)); dec((uint64_t)(mn != 0)); dec((uint64_t)(mn2 != 0));
        say(")\n");
        return;
    }
    for (int fs = 0; fs < 2; fs++) {
        char p[64];
        pcpy(p, g_root[fs]); cat(p, "/LOG.TXT");
        if (vfs_create(p, "", 0) != 0) { fail("create log", p); }
        pcpy(p, g_root[fs]); cat(p, "/SHR");
        if (vfs_mkdir(p) != 0) { fail("mkdir shared", p); }
    }
    if (nt_walk() != 0) { fail("ntfs walk", 0); }
    say("[C4] ntfs files="); dec(g_ntn); say("\n");
    /* Interleave: every kind is in flight on both filesystems at once. */
    for (int w = 0; w < NFJOBS; w++) {
        add(JK_FILES, 0, w); add(JK_FILES, 1, w);
        if (w < NAPPEND) { add(JK_APPEND, 0, w); add(JK_APPEND, 1, w); }
        if (w < NSHARED) { add(JK_SHARED, 0, w); add(JK_SHARED, 1, w); }
        if (w == 1) { for (int s = 0; s < NSCAN; s++) { add(JK_SCAN, 0, s); add(JK_SCAN, 1, s); } }
        if (w < NNTREAD) add(JK_NTREAD, 0, w);
    }
    g_churn_left = 2 * (NFJOBS + NAPPEND + NSHARED);
    uint32_t ncpu = smp_online_count();
    say("[C4] stress start cpus="); dec(ncpu); say(" jobs="); dec(g_njobs); say("\n");
    uint64_t t0 = ktime_ms();
    __atomic_store_n(&g_active, 1, __ATOMIC_RELEASE);
    worker();
    g_stop = 1;
    while ((g_out < g_in || g_in < ncpu) && ktime_ms() - t0 < TIMEOUT_MS + 5000) __asm__ volatile("pause");
    g_active = 0;
    say("[C4] workers="); dec(g_in); say(" done="); dec(g_out); say(" jobs="); dec(g_next > g_njobs ? g_njobs : g_next);
    say(" scans="); dec(g_scans); say(" ms="); dec(ktime_ms() - t0); say("\n");
    for (uint32_t i = 0; i < ncpu && i < SMP_MAX_CPUS; i++) { say("[C4] cpu "); dec(i); say(" ran "); dec(g_runs[i]); say(" jobs\n"); }
    if (g_in < 2) fail("fewer than two CPUs took part", 0);
    if (g_out != g_in) fail("a worker did not finish", 0);
    if (g_next < g_njobs) fail("jobs left in the queue", 0);
#ifndef SECOS_C4_NOLOCK
    lockstat("fat32", (const klock_t*)fat32_fs_lock());
    lockstat("exfat", (const klock_t*)exfat_fs_lock());
    lockstat("ntfs /c4n", (const klock_t*)ntfs_fs_lock(ops_at("/c4n")));
    lockstat("ntfs /c4n2", (const klock_t*)ntfs_fs_lock(ops_at("/c4n2")));
#endif

    /* Everything again, single-threaded: nothing a later job did may have
     * damaged an earlier job's files. */
    uint8_t* buf = (uint8_t*)kmalloc(NAPPEND * LOG_LINES * LINE > BUF_BYTES ? NAPPEND * LOG_LINES * LINE : BUF_BYTES);
    if (!buf) { fail("kmalloc", 0); return; }
    for (int fs = 0; fs < 2; fs++) {
        char q[64];
        for (int w = 0; w < NFJOBS; w++)
            for (int i = 0; i < NFILES; i++) {
                files_path(q, fs, w, 'G', i);
                if (files_gone(i)) { if (vfs_lookup(q)) fail("final: unlinked file back", q); }
                else check_file(q, files_tag(fs, w, i), files_final(w, i), buf, 0, 3);
            }
        check_log(fs, buf);
        check_shared(fs, buf);
    }
    kfree(buf);
    /* NTFS: every reader got the same answer; print the listing. */
    uint32_t bad = 0, nerr = 0;
    for (uint32_t j = 0; j < g_ntn; j++) {
        for (int r = 1; r < NNTREAD; r++) if (g_nth[r][j] != g_nth[0][j] || g_nte[r][j] != g_nte[0][j]) bad++;
        uint64_t h = g_nth[0][j];
        vfs_inode_t* ino = 0;
        char p[300]; pcpy(p, "/c4n"); cat(p, g_ntp[j]);
        ino = vfs_lookup(p);
        if (g_nte[0][j] == NT_LINK) continue;
        if (g_nte[0][j]) { nerr++; say("[C4N] E\t"); dec(g_nte[0][j]); say("\t"); dec(ino ? ino->size : 0); }
        else { say("[C4N] F\t"); dec(ino ? ino->size : 0); say("\t"); hex16(h); }
        say("\t"); say(g_ntp[j]); say("\n");
    }
    for (int r = 0; r < NNTREAD; r++) if (!g_ntdone[r]) fail("ntfs reader did not finish", 0);
    say("[C4] ntfs readers="); dec(NNTREAD); say(" files="); dec(g_ntn); say(" errors="); dec(nerr);
    say(" disagreements="); dec(bad); say("\n");
    if (bad) fail("ntfs readers disagree", 0);
    if (g_ntn < 100) fail("ntfs walk too small", 0);

    /* Clean detach so the host checks consistent, clean volumes. */
    for (int fs = 0; fs < 2; fs++) {
        const vfs_fs_ops_t* o = ops_at(g_root[fs]);
        if (!o || vfs_sync_ops(o) != 0 || vfs_unmount(g_root[fs]) != 0) fail("sync+umount", g_root[fs]);
    }
    vfs_unmount("/c4n2"); vfs_unmount("/c4n");
    say(g_fail ? "[C4] RESULT FAIL\n" : "[C4] RESULT PASS\n");
}
#endif /* SECOS_C4_FSSTRESS */
