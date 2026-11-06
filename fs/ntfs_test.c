/*
 * SecOS Kernel - NTFS boot self-test (gate M42_NTFS_DEMO)
 *
 * Two modes, picked from the attached disks:
 *  - "good" (only the virtio disk vda): the host-made image from
 *    `make disk-ntfs` is auto-mounted at /mnt. Every object is walked and
 *    printed as an "[M42L]" listing line (type, size, FNV-1a hash of the
 *    content, path); tools/selftest.sh compares that with the listing ntfs-3g
 *    produced on the host. Targeted checks follow: Unicode and case-insensitive
 *    lookups, hidden DOS aliases and metafiles, the fragmented / sparse /
 *    compressed files, EFS -> EACCES, symlinks, every write op -> EROFS,
 *    /proc/mounts, statfs, a second simultaneous instance, remount.
 *  - "corrupt" (AHCI sda present): every attached disk holds a damaged image
 *    (tools/ntfs_img.py corrupt). Each must be refused at mount or return
 *    errors from the damaged objects, never fault the kernel.
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "ntfs.h"
#include "vfs.h"
#include "block.h"
#include "debugcon.h"
#include "heap.h"
#include "../kernel/process.h"
#include "../kernel/cred.h"
#include <stdint.h>
#include <stddef.h>

#ifndef M42_NTFS_DEMO
#define M42_NTFS_DEMO 0
#endif

#if M42_NTFS_DEMO

static void lg(const char* s){ debugcon_writestring(s); }
static void lgx(const char* s, uint64_t v){ debugcon_writestring(s); debugcon_print_hex(v); debugcon_writestring("\n"); }
static void ok(const char* what, int cond){
    debugcon_writestring("[M42] "); debugcon_writestring(what);
    debugcon_writestring(cond ? ": OK\n" : ": FAIL\n");
}
static size_t sl(const char* s){ size_t n = 0; while(s[n]) n++; return n; }
static int seq(const char* a, const char* b){ while(*a && *a == *b){ a++; b++; } return *a == *b; }
static int meq(const void* a, const void* b, size_t n){ const uint8_t* x = a; const uint8_t* y = b; for(size_t i = 0; i < n; i++) if(x[i] != y[i]) return 0; return 1; }
static void dec(uint64_t v){ char b[24]; int i = 23; b[i] = 0; do { b[--i] = (char)('0' + v % 10); v /= 10; } while(v); lg(b + i); }
static void hex16(uint64_t v){ char b[17]; for(int i = 15; i >= 0; i--){ b[i] = "0123456789abcdef"[v & 15]; v >>= 4; } b[16] = 0; lg(b); }

/* Deterministic test pattern shared with tools/ntfs_img.py. */
static uint8_t pat(uint64_t i, uint32_t seed){ return (uint8_t)((i * 131u + seed * 17u + (i >> 12)) & 0xFF); }
static uint64_t fnv(uint64_t h, const uint8_t* p, size_t n){ for(size_t i = 0; i < n; i++){ h ^= p[i]; h *= 0x100000001B3ULL; } return h; }

static uint8_t g_buf[65536];

static int read_eq(const char* path, const char* want){
    static char b[512];
    size_t wl = sl(want);
    int n = vfs_read_all(path, b, sizeof(b));
    return n == (int)wl && meq(b, want, wl);
}
/* File of `len` bytes: pattern(seed) on [a0,a0+n0) etc., zero elsewhere. */
struct isl { uint64_t off, len; uint32_t seed; };
static int verify_islands(const char* path, uint64_t len, const struct isl* is, int nis){
    vfs_inode_t* ino = vfs_lookup(path);
    if(!ino || ino->type != VFS_NODE_FILE || ino->size != len) return 0;
    for(uint64_t off = 0; off < len; off += sizeof(g_buf)){
        size_t n = (len - off) > sizeof(g_buf) ? sizeof(g_buf) : (size_t)(len - off);
        if(ino->ops->read(ino, off, g_buf, n) != (int)n) return 0;
        for(size_t i = 0; i < n; i++){
            uint64_t p = off + i; uint8_t want = 0;
            for(int k = 0; k < nis; k++) if(p >= is[k].off && p < is[k].off + is[k].len) want = pat(p - is[k].off, is[k].seed);
            if(g_buf[i] != want) return 0;
        }
    }
    return 1;
}

static const vfs_fs_ops_t* mnt_ops(const char* mp){
    int n = vfs_mount_count();
    for(int i = 0; i < n; i++){
        const char* m = 0; const char* f = 0;
        if(vfs_mount_info(i, &m, &f) == 0 && m && seq(m, mp)) return vfs_mount_ops_at(i);
    }
    return NULL;
}

/* ---- full walk: one listing line per object ---- */
typedef struct { char p[256]; int t; } went_t;
struct coll { went_t* e; int n, cap; const char* dir; };
/* Children's paths are filesystem-relative: keep the last component and
 * rebuild the absolute path under `dir` (struct coll.dir). */
static void coll_cb(const vfs_inode_t* c, void* u){
    struct coll* x = (struct coll*)u;
    if(x->n >= x->cap) return;
    const char* b = c->path;
    for(const char* q = c->path; *q; q++) if(*q == '/') b = q + 1;
    size_t k = 0;
    for(const char* d = x->dir; *d && k < 254; d++) x->e[x->n].p[k++] = *d;
    if(k && x->e[x->n].p[k - 1] != '/') x->e[x->n].p[k++] = '/';
    for(; *b && k < 255; b++) x->e[x->n].p[k++] = *b;
    if(*b) return;                                   /* too long for the VFS */
    x->e[x->n].p[k] = 0;
    x->e[x->n].t = c->type;
    x->n++;
}
/* Prints "[M42L] T\tA\tB\t/rel" lines; returns objects walked, -1 on error. */
static int walk(const char* mp, int* nfiles, int* errors){
    enum { MAXD = 512, MAXE = 4096 };
    char (*stack)[256] = kmalloc(MAXD * 256);
    struct coll c; c.e = kmalloc(sizeof(went_t) * MAXE); c.cap = MAXE;
    if(!stack || !c.e) return -1;
    int sp = 0, total = 0;
    size_t ml = sl(mp);
    for(size_t k = 0; k <= ml; k++) stack[0][k] = mp[k];
    sp = 1;
    *nfiles = 0; *errors = 0;
    while(sp > 0){
        static char dir[256];
        sp--;
        for(size_t k = 0; k < 256; k++){ dir[k] = stack[sp][k]; if(!dir[k]) break; }
        c.n = 0; c.dir = dir;
        if(vfs_readdir(dir, coll_cb, &c) != 0){ lg("[M42L] READDIR-FAIL "); lg(dir); lg("\n"); (*errors)++; continue; }
        for(int i = 0; i < c.n; i++){
            const char* p = c.e[i].p;
            const char* rel = p + ml;
            total++;
            vfs_inode_t* ino = vfs_lookup(p);
            if(!ino){ lg("[M42L] LOOKUP-FAIL "); lg(rel); lg("\n"); (*errors)++; continue; }
            if(ino->type == VFS_NODE_DIR){
                lg("[M42L] D\t-\t-\t"); lg(rel); lg("\n");
                if(sp < MAXD){ size_t k = 0; for(; p[k] && k < 255; k++) stack[sp][k] = p[k]; stack[sp][k] = 0; sp++; }
                continue;
            }
            if(ino->type == VFS_NODE_SYMLINK){
                static char t[256];
                int n = vfs_readlink(p, t, sizeof(t));
                lg("[M42L] L\t-\t"); lg(n >= 0 ? t : "?"); lg("\t"); lg(rel); lg("\n");
                continue;
            }
            uint64_t h = 0xCBF29CE484222325ULL;
            int err = 0;
            for(uint64_t off = 0; off < ino->size;){
                size_t want = (ino->size - off) > sizeof(g_buf) ? sizeof(g_buf) : (size_t)(ino->size - off);
                int r = ino->ops->read(ino, off, g_buf, want);
                if(r < 0){ err = -r; break; }
                if(r == 0 || (size_t)r > want){ err = 999; break; }
                h = fnv(h, g_buf, (size_t)r);
                off += (uint64_t)r;
            }
            if(err){ lg("[M42L] E\t"); dec((uint64_t)err); lg("\t"); dec(ino->size); lg("\t"); lg(rel); lg("\n"); }
            else { lg("[M42L] F\t"); dec(ino->size); lg("\t"); hex16(h); lg("\t"); lg(rel); lg("\n"); (*nfiles)++; }
        }
    }
    kfree(c.e); kfree(stack);
    return total;
}

static int count_dir(const char* path, const char* needle, int* found){
    static struct coll c; static went_t* e;
    if(!e) e = kmalloc(sizeof(went_t) * 4096);
    if(!e) return -1;
    c.e = e; c.cap = 4096; c.n = 0; c.dir = path;
    if(vfs_readdir(path, coll_cb, &c) != 0) return -1;
    if(found){
        *found = 0;
        for(int i = 0; i < c.n; i++){
            const char* b = c.e[i].p; const char* q = b;
            for(; *q; q++) if(*q == '/') b = q + 1;
            if(needle[0] == '$' ? b[0] == '$' : seq(b, needle)) (*found)++;
        }
    }
    return c.n;
}

#define LONG_UNI "/mnt/Relazione è già pronta — ünïcødé 😀.txt"
#define LONG_UNI_UP "/mnt/RELAZIONE È GIÀ PRONTA — ÜNÏCØDÉ 😀.TXT"
#define CJK "/mnt/日本語のファイル名.txt"

static void good_mode(void){
    lg("[M42] --- NTFS good-image suite ---\n");
    const vfs_fs_ops_t* o = mnt_ops("/mnt");
    const char* dev = 0; const char* type = 0;
    ok("/mnt is ntfs", o && o->mntinfo && o->mntinfo(&dev, &type) == 0 && type && seq(type, "ntfs"));
    if(!o) return;
    static char devc[32];
    { size_t k = 0; for(; dev && dev[k] && k < 31; k++) devc[k] = dev[k]; devc[k] = 0; }

    /* ---- full listing for the host comparison ---- */
    int nfiles = 0, errors = 0;
    int total = walk("/mnt", &nfiles, &errors);
    lg("[M42] walk objects="); dec((uint64_t)total); lg(" files="); dec((uint64_t)nfiles); lg(" errors="); dec((uint64_t)errors); lg("\n");
    ok("full tree walk without readdir/lookup errors", total > 3000 && errors == 0);

    /* ---- names ---- */
    ok("hello.txt read", read_eq("/mnt/hello.txt", "hello from ntfs\n"));
    vfs_inode_t* a = vfs_lookup("/mnt/hello.txt");
    vfs_inode_t* b = vfs_lookup("/mnt/HeLLo.TXT");
    ok("case-insensitive lookup ($UpCase)", a && a == b);
    ok("unicode long name read", read_eq(LONG_UNI, "Il documento è pronto per la revisione.\n"));
    ok("case-insensitive non-ASCII lookup", vfs_lookup(LONG_UNI_UP) == vfs_lookup(LONG_UNI) && vfs_lookup(LONG_UNI));
    ok("CJK name read", read_eq(CJK, "こんにちは NTFS\n"));
    ok("nested dirs (7 levels)", read_eq("/mnt/a/b/c/d/e/f/g/deep.txt", "deep down\n"));
    int f8 = -1, fdol = -1;
    count_dir("/mnt", "LONGDO~1.TXT", &f8);
    count_dir("/mnt", "$", &fdol);
    ok("DOS 8.3 alias hidden from readdir", f8 == 0 && vfs_lookup("/mnt/Long Document Name.txt") && !vfs_lookup("/mnt/LONGDO~1.TXT"));
    ok("metafiles hidden ($MFT, $Volume, ...)", fdol == 0 && !vfs_lookup("/mnt/$MFT") && !vfs_lookup("/mnt/$UpCase"));
    int nmany = count_dir("/mnt/many", "", NULL);
    lgx("[M42] /mnt/many entries=", (uint64_t)nmany);
    ok("2000-entry multi-level directory lists everything", nmany == 2000);
    ok("lookup deep in the big directory", read_eq("/mnt/many/entry_01999_with_a_reasonably_long_name.dat", "1999\n") &&
                                          read_eq("/mnt/many/ENTRY_00000_WITH_A_REASONABLY_LONG_NAME.DAT", "0\n"));

    /* ---- data ---- */
    struct isl fr = { 0, 5u << 20, 9 }, bg = { 0, 5u << 20, 11 };
    ok("fragmented.bin ($ATTRIBUTE_LIST, ~1300 runs) verify", verify_islands("/mnt/fragmented.bin", 5u << 20, &fr, 1));
    ok("big.bin 5MiB verify", verify_islands("/mnt/big.bin", 5u << 20, &bg, 1));
    struct isl sp[3] = { { 0, 10000, 3 }, { 20u << 20, 70000, 4 }, { (48u << 20) - 5000, 5000, 5 } };
    ok("sparse.bin 48MiB (holes read as zero) verify", verify_islands("/mnt/sparse.bin", 48u << 20, sp, 3));
    vfs_inode_t* ct = vfs_lookup("/mnt/compressed/text.txt");
    ok("compressed (LZNT1) file readable", ct && ct->size == 300000 && ct->ops->read(ct, 123457, g_buf, 1000) == 1000);
    ok("tiny resident file", read_eq("/mnt/tiny_resident.txt", "r"));
    vfs_inode_t* em = vfs_lookup("/mnt/empty.txt");
    ok("empty file", em && em->size == 0 && em->ops->read(em, 0, g_buf, 10) == 0);
    ok("file with an alternate data stream reads its main stream", read_eq("/mnt/with_ads.txt", "main stream\n"));
    vfs_inode_t* ef = vfs_lookup("/mnt/secret.efs");
    ok("EFS-encrypted file -> EACCES", ef && ef->type == VFS_NODE_FILE && ef->ops->read(ef, 0, g_buf, 10) == -13);

    /* ---- metadata ---- */
    ok("hello.txt mtime plausible (2023..2040)", a && a->mtime > 1672531200ULL && a->mtime < 2208988800ULL);
    vfs_inode_t* la = vfs_lookup("/mnt/linked_a.txt");
    ok("hard link: nlink=2", la && la->nlink == 2);
    vfs_inode_t* dd = vfs_lookup("/mnt/many");
    ok("directory type + mode", dd && dd->type == VFS_NODE_DIR && (dd->mode & 0170000) == 040000);
    static char t[256];
    int n = vfs_readlink("/mnt/winlink_rel", t, sizeof(t));
    ok("Windows relative symlink", n > 0 && seq(t, "a/b/c/d/e/f/g/deep.txt"));
    vfs_inode_t* fl = vfs_lookup_follow("/mnt/winlink_rel");
    ok("symlink followed by lookup", fl && fl->type == VFS_NODE_FILE && fl == vfs_lookup("/mnt/a/b/c/d/e/f/g/deep.txt"));
    n = vfs_readlink("/mnt/junction", t, sizeof(t));
    ok("junction -> path under the mount point", n > 0 && seq(t, "/mnt/a/b"));
    ok("WSL + Interix symlinks", vfs_readlink("/mnt/wsl_link", t, sizeof(t)) > 0 && seq(t, "hello.txt") &&
                                 vfs_readlink("/mnt/interix_link", t, sizeof(t)) > 0 && seq(t, "hello.txt"));

    /* ---- read-only ---- */
    int r1 = vfs_create("/mnt/new.txt", "x", 1), r2 = vfs_mkdir("/mnt/newdir"), r3 = vfs_remove("/mnt/hello.txt");
    int r4 = vfs_rename("/mnt/hello.txt", "/mnt/h2.txt"), r5 = vfs_truncate("/mnt/hello.txt", 0);
    int r6 = vfs_write("/mnt/hello.txt", 0, "x", 1), r7 = vfs_symlink("hello.txt", "/mnt/sl");
    int r8 = vfs_link("/mnt/hello.txt", "/mnt/hl"), r9 = vfs_rmdir("/mnt/emptydir");
    vfs_attr_t at; at.mode = 0777;
    int r10 = vfs_setattr("/mnt/hello.txt", &at, VFS_ATTR_MODE);
    lg("[M42] write rcs:"); int rr[10] = { r1, r2, r3, r4, r5, r6, r7, r8, r9, r10 };
    int allro = 1;
    for(int i = 0; i < 10; i++){ lg(rr[i] < 0 ? " -" : " "); dec((uint64_t)(rr[i] < 0 ? -rr[i] : rr[i])); if(rr[i] != -30) allro = 0; }
    lg("\n");
    ok("every write op refused with EROFS (create/mkdir/unlink/rename/truncate/write/symlink/link/rmdir/chmod)", allro);
    ok("content unchanged after refused writes", read_eq("/mnt/hello.txt", "hello from ntfs\n") && !vfs_lookup("/mnt/new.txt"));
    ok("read-write mount request refused", ntfs_mount_opts(devc, "/mnt3", 1) == -30);

    /* ---- integration ---- */
    static char pm[2048];
    for(size_t i = 0; i < sizeof(pm); i++) pm[i] = 0;
    int pn = vfs_read_all("/proc/mounts", pm, sizeof(pm) - 1);
    int found = 0;
    for(int i = 0; pn > 0 && i < pn; i++) if(meq(pm + i, " /mnt ntfs ro,", 14)) found = 1;
    ok("/proc/mounts lists /mnt ntfs ro", found);
    static char pf[512];
    for(size_t i = 0; i < sizeof(pf); i++) pf[i] = 0;
    int fn = vfs_read_all("/proc/filesystems", pf, sizeof(pf) - 1);
    found = 0;
    for(int i = 0; fn > 0 && i < fn; i++) if(meq(pf + i, "\tntfs\n", 6)) found = 1;
    ok("/proc/filesystems lists ntfs", found);
    vfs_statfs_t st;
    int sr = vfs_statfs("/mnt/hello.txt", &st);
    ok("statfs type=NTFS, read-only, sane counts", sr == 0 && st.type == 0x5346544E && (st.flags & 1) &&
                                                   st.blocks > 0 && st.bfree < st.blocks && st.bsize == 4096);

    /* ---- second instance + remount ---- */
    int m2 = ntfs_mount(devc, "/mnt2");
    ok("second simultaneous NTFS instance", m2 == 0 && read_eq("/mnt2/hello.txt", "hello from ntfs\n") && ntfs_mounted_count() == 2);
    if(m2 == 0) vfs_unmount("/mnt2");

    /* ---- ownership: mount-supplied owner/modes + EROFS before EACCES ---- */
    static process_t u1000, u2000;             /* fake user processes (pid != 0) */
    u1000.pid = 0x7001; u1000.cred.ruid = u1000.cred.euid = 1000; u1000.cred.rgid = u1000.cred.egid = 100;
    u2000.pid = 0x7002; u2000.cred.ruid = u2000.cred.euid = 2000; u2000.cred.rgid = u2000.cred.egid = 2000;
    vfs_inode_t* hf = vfs_lookup("/mnt/hello.txt");
    ok("default owner root:root, files 0644, dirs 0755",
       hf && hf->uid == 0 && hf->mode == 0100644u && (dd = vfs_lookup("/mnt/many")) && dd->mode == 040755u);
    ok("a user may read; any write -> EROFS (not EACCES)",
       hf && cred_inode_perm(&u2000, hf, MAY_READ) == 0 && cred_inode_perm(&u2000, hf, MAY_WRITE) == -CRED_EROFS &&
       cred_may_create(&u2000, "/mnt/new.txt") == -CRED_EROFS && cred_chmod(&u2000, "/mnt/hello.txt", 0777) == -CRED_EROFS);
    ok("mount option rw -> EROFS", ntfs_set_mount_options("rw") == 0 && ntfs_mount(devc, "/mnt2") == -30);
    ok("bad mount option refused", ntfs_set_mount_options("uid=x") != 0 && ntfs_set_mount_options("bogus") != 0);
    ntfs_set_mount_options("uid=1000,gid=100,umask=077");
    m2 = ntfs_mount(devc, "/mnt2");
    hf = vfs_lookup("/mnt2/hello.txt");
    vfs_inode_t* d2 = vfs_lookup("/mnt2/many");
    ok("mount -o uid=1000,gid=100,umask=077: owner + modes",
       m2 == 0 && hf && d2 && hf->uid == 1000 && hf->gid == 100 && hf->mode == 0100700u && d2->mode == 040700u);
    ok("umask=077: owner reads, other user EACCES",
       hf && cred_inode_perm(&u1000, hf, MAY_READ) == 0 && cred_inode_perm(&u2000, hf, MAY_READ) == -CRED_EACCES);
    hf = vfs_lookup("/mnt/hello.txt");
    ok("per-volume options (first volume unchanged)", hf && hf->uid == 0 && hf->mode == 0100644u);
    if(m2 == 0) vfs_unmount("/mnt2");
    vfs_inode_t* old = vfs_lookup("/mnt/big.bin");
    int um = vfs_unmount("/mnt");
    ok("unmount", um == 0 && !vfs_lookup("/mnt/hello.txt"));
    ok("stale inode after unmount fails cleanly", old && old->ops->read(old, 0, g_buf, 16) < 0);
    int rm = ntfs_mount(devc, "/mnt");
    ok("remount + read", rm == 0 && read_eq("/mnt/hello.txt", "hello from ntfs\n") &&
                         verify_islands("/mnt/fragmented.bin", 5u << 20, &fr, 1));
    lg("[M42] DONE\n");
}

/* ---- corrupt images ---- */
static void probe(const char* rel){
    static char p[128];
    size_t k = 0; const char* m = "/mnt/";
    for(; *m; m++) p[k++] = *m;
    for(; *rel && k < sizeof(p) - 1; rel++) p[k++] = *rel;
    p[k] = 0;
    vfs_inode_t* ino = vfs_lookup(p);
    const char* what = " read="; int r = 0;
    if(!ino){ lg("[M42C]   "); lg(p); lg(" lookup=NULL\n"); return; }
    if(ino->type == VFS_NODE_DIR){ what = " readdir="; r = count_dir(p, "", NULL); }
    else r = ino->ops->read(ino, 0, g_buf, sizeof(g_buf));
    lg("[M42C]   "); lg(p); lgx(what, (uint64_t)(int64_t)r);
}
static void corrupt_mode(void){
    lg("[M42C] --- NTFS corrupt-image suite ---\n");
    if(mnt_ops("/mnt")) vfs_unmount("/mnt");
    static const char* devs[] = { "vda", "sda", "sdb", "sdc", "sdd", "sde", "sdf", "nvme0n1", "usb0" };
    static const char* items[] = { "ok.txt", "runbad.bin", "attrbad.bin", "compressed/c.txt", "secret.efs",
                                   "loopdir", "indxbad", "hiberfil.sys", "compressed" };
    for(unsigned d = 0; d < sizeof(devs) / sizeof(devs[0]); d++){
        if(!block_find(devs[d])) continue;
        int r = ntfs_mount(devs[d], "/mnt");
        lg("[M42C] dev="); lg(devs[d]); lgx(" mount rc=", (uint64_t)(int64_t)r);
        if(r == 0){
            for(unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++) probe(items[i]);
            int c = count_dir("/mnt", "", NULL);
            lgx("[M42C]   root entries=", (uint64_t)(int64_t)c);
            vfs_unmount("/mnt");
        }
    }
    lg("[M42C] DONE (kernel alive)\n");
}

/* Called once the boot mounts (/mnt, /proc, ...) are in place. */
void ntfs_selftest(void){
    if(block_find("sda")){ corrupt_mode(); return; }
    if(!mnt_ops("/mnt")){ lg("[M42] no disk mounted at /mnt\n"); return; }
    good_mode();
}

#else
void ntfs_selftest(void){ }
#endif
