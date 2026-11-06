/*
 * SecOS Kernel - exFAT boot self-test (gate M41_EXFAT_DEMO)
 *
 * Two modes, picked from the attached disks:
 *  - "good" (only the virtio disk vda): the host-made image from
 *    `make disk-exfat` is mounted at /mnt. Reads host-written files (long
 *    Unicode names, case-insensitive lookup, a large and two fragmented
 *    files), then writes/mkdirs/unlinks/renames/truncates from SecOS, unmounts
 *    cleanly and remounts to check persistence. The host then runs
 *    fsck.exfat and compares every file (tools/exfat-host.sh verify).
 *  - "corrupt" (AHCI sda present): every attached disk holds a deliberately
 *    damaged image (tools/mk_exfat_corrupt.py). Each must be refused at mount
 *    or return errors from the damaged objects, never fault the kernel.
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "exfat.h"
#include "vfs.h"
#include "block.h"
#include "debugcon.h"
#include "../kernel/process.h"
#include "../kernel/cred.h"
#include <stdint.h>
#include <stddef.h>

#ifndef M41_EXFAT_DEMO
#define M41_EXFAT_DEMO 0
#endif

#if M41_EXFAT_DEMO

static void lg(const char* s){ debugcon_writestring(s); }
static void lgx(const char* s, uint64_t v){ debugcon_writestring(s); debugcon_print_hex(v); debugcon_writestring("\n"); }
static void ok(const char* what, int cond){
    debugcon_writestring("[M41] "); debugcon_writestring(what);
    debugcon_writestring(cond ? ": OK\n" : ": FAIL\n");
}
static size_t sl(const char* s){ size_t n = 0; while(s[n]) n++; return n; }
static int meq(const void* a, const void* b, size_t n){ const uint8_t* x = a; const uint8_t* y = b; for(size_t i = 0; i < n; i++) if(x[i] != y[i]) return 0; return 1; }

/* Deterministic test pattern shared with tools/exfat-host.sh (python). */
static uint8_t pat(uint64_t i, uint32_t seed){ return (uint8_t)((i * 131u + seed * 17u + (i >> 12)) & 0xFF); }

static uint8_t g_buf[65536];

/* Read a whole file in chunks and compare with pattern(seed); `len` expected. */
static int verify_pattern(const char* path, uint64_t len, uint32_t seed){
    vfs_inode_t* ino = vfs_lookup(path);
    if(!ino || ino->type != VFS_NODE_FILE || !ino->ops || ino->size != len) return 0;
    for(uint64_t off = 0; off < len; off += sizeof(g_buf)){
        size_t n = (len - off) > sizeof(g_buf) ? sizeof(g_buf) : (size_t)(len - off);
        if(ino->ops->read(ino, off, g_buf, n) != (int)n) return 0;
        for(size_t i = 0; i < n; i++) if(g_buf[i] != pat(off + i, seed)) return 0;
    }
    return 1;
}
static int write_pattern(const char* path, uint64_t off, uint64_t len, uint32_t seed){
    for(uint64_t done = 0; done < len;){
        size_t n = (len - done) > sizeof(g_buf) ? sizeof(g_buf) : (size_t)(len - done);
        for(size_t i = 0; i < n; i++) g_buf[i] = pat(off + done + i, seed);
        if(vfs_write(path, off + done, g_buf, n) != (int)n) return 0;
        done += n;
    }
    return 1;
}
static int read_eq(const char* path, const char* want){
    static char b[512];
    size_t wl = sl(want);
    int n = vfs_read_all(path, b, sizeof(b));
    return n == (int)wl && meq(b, want, wl);
}
static int dev_dirty_bit(const char* dev){
    static uint8_t s[4096];
    block_dev_t* d = block_find(dev);
    if(!d || block_read(d, 0, s, 1) != 1) return -1;
    return (s[106] >> 1) & 1;
}

struct cnt_ctx { int n; };
static void cnt_cb(const vfs_inode_t* c, void* u){
    struct cnt_ctx* x = (struct cnt_ctx*)u; x->n++;
    if(x->n <= 40){ lg("[M41]   ls: "); lg(c->path); lg("\n"); }
}

static const vfs_fs_ops_t* mnt_ops(const char* mp, const char** fsname){
    int n = vfs_mount_count();
    for(int i = 0; i < n; i++){
        const char* m = 0; const char* f = 0;
        if(vfs_mount_info(i, &m, &f) == 0 && m){
            size_t k = 0; while(m[k] && m[k] == mp[k]) k++;
            if(!m[k] && !mp[k]){ if(fsname) *fsname = f; return vfs_mount_ops_at(i); }
        }
    }
    return NULL;
}
static int umount_clean(const char* mp){
    const vfs_fs_ops_t* o = mnt_ops(mp, NULL);
    if(!o) return -1;
    vfs_sync_ops(o);
    return vfs_unmount(mp);
}

#define LONGNAME   "/mnt/Relazione è già pronta.txt"
#define LONGNAME_U "/mnt/RELAZIONE È GIÀ PRONTA.TXT"
#define LONGBODY   "Il documento è pronto per la revisione.\n"
#define WNAME      "/mnt/Scritto da SecOS – ñandú.txt"
#define WBODY      "Scritto da SecOS su exFAT: àèìòù ✓\n"
#define NDIR       "/mnt/Nuova cartella SecOS"

static void good_mode(const char* devp){
    static char dev[32];
    size_t dk = 0; for(; devp[dk] && dk < sizeof(dev) - 1; dk++) dev[dk] = devp[dk]; dev[dk] = 0;
    lg("[M41] --- exFAT good-image suite ---\n");
    const char* fsn = 0;
    const vfs_fs_ops_t* ops = mnt_ops("/mnt", &fsn);
    ok("/mnt is exfat", ops && fsn && fsn[0] == 'e' && fsn[1] == 'x');
    if(!ops) return;

    /* ---- host-written content ---- */
    ok("longname read", read_eq(LONGNAME, LONGBODY));
    ok("case-insensitive lookup", read_eq(LONGNAME_U, LONGBODY));
    ok("emoji (surrogate pair) name read", read_eq("/mnt/emoji_\xF0\x9F\x98\x80.txt", "smile\n"));
    ok("subdir longname read", read_eq("/mnt/Cartella con nome lungo àèìòù/dentro.txt", "inside\n"));
    ok("big.bin 3MiB host file verify", verify_pattern("/mnt/big.bin", 3u * 1024 * 1024, 1));
    ok("frag_a.bin fragmented host file verify", verify_pattern("/mnt/frag_a.bin", 40u * 4096, 2));
    ok("frag_b.bin fragmented host file verify", verify_pattern("/mnt/frag_b.bin", 40u * 4096, 3));
    struct cnt_ctx cc = { 0 };
    vfs_readdir("/mnt", cnt_cb, &cc);
    lgx("[M41] readdir /mnt entries=", (uint64_t)cc.n);
    ok("readdir lists host files", cc.n >= 10);

    /* ---- writes from SecOS ---- */
    vfs_remove(WNAME);
    ok("create unicode file", vfs_create(WNAME, WBODY, sl(WBODY)) == 0 && read_eq(WNAME, WBODY));
    ok("VolumeDirty set while mounted rw", dev_dirty_bit(dev) == 1);
    ok("mkdir", vfs_mkdir(NDIR) == 0 && vfs_lookup(NDIR) && vfs_lookup(NDIR)->type == VFS_NODE_DIR);
    ok("create in subdir", vfs_create(NDIR "/file interno.txt", "interno\n", 8) == 0 && read_eq(NDIR "/file interno.txt", "interno\n"));
    ok("mkdir nested", vfs_mkdir(NDIR "/sub") == 0);
    ok("rmdir non-empty refused", vfs_rmdir(NDIR) != 0 && vfs_lookup(NDIR));
    ok("mkdir+rmdir", vfs_mkdir("/mnt/tmpdir") == 0 && vfs_rmdir("/mnt/tmpdir") == 0 && !vfs_lookup("/mnt/tmpdir"));
    ok("unlink host file", vfs_remove("/mnt/delete_me.txt") == 0 && !vfs_lookup("/mnt/delete_me.txt"));
    ok("rename in dir", vfs_rename("/mnt/rename_me.txt", "/mnt/renamed.txt") == 0 &&
                        !vfs_lookup("/mnt/rename_me.txt") && read_eq("/mnt/renamed.txt", "rename payload\n"));
    ok("rename case-only", vfs_rename("/mnt/renamed.txt", "/mnt/RENAMED.txt") == 0 && read_eq("/mnt/RENAMED.txt", "rename payload\n"));
    ok("rename across dirs", vfs_rename("/mnt/RENAMED.txt", NDIR "/moved.txt") == 0 &&
                             !vfs_lookup("/mnt/RENAMED.txt") && read_eq(NDIR "/moved.txt", "rename payload\n"));
    vfs_create("/mnt/victim.txt", "victim\n", 7);
    vfs_create("/mnt/winner.txt", "winner\n", 7);
    ok("rename over existing target", vfs_rename("/mnt/winner.txt", "/mnt/victim.txt") == 0 &&
                                      read_eq("/mnt/victim.txt", "winner\n") && !vfs_lookup("/mnt/winner.txt"));
    /* truncate: shrink keeps the head, grow reads zeros */
    int t1 = vfs_truncate("/mnt/trunc.bin", 5000) == 0 && verify_pattern("/mnt/trunc.bin", 5000, 4);
    int t2 = vfs_truncate("/mnt/trunc.bin", 20000) == 0;
    vfs_inode_t* ti = vfs_lookup("/mnt/trunc.bin");
    int t3 = ti && ti->size == 20000 && ti->ops->read(ti, 0, g_buf, 20000) == 20000;
    for(int i = 0; t3 && i < 20000; i++) if(g_buf[i] != (i < 5000 ? pat((uint64_t)i, 4) : 0)) t3 = 0;
    ok("truncate shrink+grow (zero tail)", t1 && t2 && t3);
    /* large write */
    vfs_remove("/mnt/sbig.bin");
    vfs_create("/mnt/sbig.bin", 0, 0);
    ok("write 2MiB file", write_pattern("/mnt/sbig.bin", 0, 2u * 1024 * 1024, 5) &&
                          verify_pattern("/mnt/sbig.bin", 2u * 1024 * 1024, 5));
    /* fragmented write: two files appended alternately, one cluster at a time */
    vfs_create("/mnt/sfa.bin", 0, 0); vfs_create("/mnt/sfb.bin", 0, 0);
    int fw = 1;
    for(int k = 0; k < 32 && fw; k++){
        fw = write_pattern("/mnt/sfa.bin", (uint64_t)k * 4096, 4096, 6) &&
             write_pattern("/mnt/sfb.bin", (uint64_t)k * 4096, 4096, 7);
    }
    ok("interleaved (fragmenting) writes", fw && verify_pattern("/mnt/sfa.bin", 32u * 4096, 6) &&
                                           verify_pattern("/mnt/sfb.bin", 32u * 4096, 7));
    /* write past ValidDataLength: the gap must read as zeros */
    vfs_create("/mnt/sparse.bin", 0, 0);
    int sp = vfs_write("/mnt/sparse.bin", 70000, "END", 3) == 3;
    vfs_inode_t* si = vfs_lookup("/mnt/sparse.bin");
    sp = sp && si && si->size == 70003 && si->ops->read(si, 0, g_buf, 65536) == 65536;
    for(int i = 0; sp && i < 65536; i++) if(g_buf[i]) sp = 0;
    sp = sp && si->ops->read(si, 70000, g_buf, 3) == 3 && meq(g_buf, "END", 3);
    ok("write beyond VDL zero-fills gap", sp);
    /* overwrite in the middle of an existing file */
    ok("overwrite middle", vfs_write("/mnt/sbig.bin", 100000, "PATCHED!", 8) == 8);
    vfs_inode_t* bi = vfs_lookup("/mnt/sbig.bin");
    ok("overwrite readback", bi && bi->ops->read(bi, 99996, g_buf, 16) == 16 &&
                             g_buf[0] == pat(99996, 5) && meq(g_buf + 4, "PATCHED!", 8) && g_buf[12] == pat(100008, 5));
    /* directory growth across clusters, then delete half */
    vfs_mkdir("/mnt/many");
    int mk = 1;
    static char p[128];
    for(int i = 0; i < 80 && mk; i++){
        const char* pre = "/mnt/many/f_";
        size_t k = 0; while(pre[k]){ p[k] = pre[k]; k++; }
        p[k++] = (char)('0' + i / 10); p[k++] = (char)('0' + i % 10);
        const char* suf = "_with_a_reasonably_long_name.txt";
        for(size_t j = 0; suf[j]; j++) p[k++] = suf[j];
        p[k] = 0;
        mk = vfs_create(p, p, k) == 0;
        if(mk && (i % 2 == 0)) mk = vfs_remove(p) == 0;
    }
    struct cnt_ctx c2 = { 0 };
    vfs_readdir("/mnt/many", cnt_cb, &c2);
    ok("directory grows past one cluster (80 creates, 40 unlinks)", mk && c2.n == 40);
    ok("file in grown dir readable", read_eq("/mnt/many/f_79_with_a_reasonably_long_name.txt",
                                             "/mnt/many/f_79_with_a_reasonably_long_name.txt"));
    /* utimes */
    vfs_attr_t a; a.mode = 0; a.uid = a.gid = 0; a.atime = 1700000000; a.mtime = 1700000000;
    vfs_setattr(NDIR "/moved.txt", &a, VFS_ATTR_MTIME | VFS_ATTR_ATIME);
    vfs_inode_t* mi = vfs_lookup(NDIR "/moved.txt");
    ok("utimes mtime persisted", mi && mi->mtime == 1700000000);
    vfs_inode_t* wi = vfs_lookup(WNAME);
    ok("create time from RTC is sane (>2020)", wi && wi->mtime > 1577836800ull);
    /* procfs + statfs */
    static char mb[2048];
    for(size_t i = 0; i < sizeof(mb); i++) mb[i] = 0;
    vfs_inode_t* pm = vfs_lookup("/proc/mounts");
    int pr = pm && pm->ops ? pm->ops->read(pm, 0, mb, sizeof(mb) - 1) : -1;
    int has = 0;
    for(int i = 0; pr > 0 && i + 5 < pr; i++) if(meq(mb + i, " exfat ", 7)) has = 1;
    ok("/proc/mounts lists exfat", has);
    vfs_statfs_t sf;
    ok("statfs type=exfat", vfs_statfs("/mnt", &sf) == 0 && sf.type == 0x2011BAB0u && sf.blocks > 0 && sf.bfree < sf.blocks);

    /* ---- clean unmount + remount ---- */
    int um = umount_clean("/mnt");
    ok("umount", um == 0 && !exfat_is_mounted());
    ok("VolumeDirty cleared on clean unmount", dev_dirty_bit(dev) == 0);
    int rm = exfat_mount(dev, "/mnt");
    ok("remount", rm == 0);
    ok("persistence after remount", read_eq(WNAME, WBODY) && read_eq(NDIR "/moved.txt", "rename payload\n") &&
                                    verify_pattern("/mnt/sfb.bin", 32u * 4096, 7) && !vfs_lookup("/mnt/delete_me.txt"));
    /* ---- ownership: mount-supplied owner/modes + DAC (kernel/cred.c) ---- */
    static process_t u1000, u2000;             /* fake user processes (pid != 0) */
    u1000.pid = 0x7001; u1000.cred.ruid = u1000.cred.euid = 1000; u1000.cred.rgid = u1000.cred.egid = 100;
    u2000.pid = 0x7002; u2000.cred.ruid = u2000.cred.euid = 2000; u2000.cred.rgid = u2000.cred.egid = 2000;
    vfs_inode_t* df = vfs_lookup(WNAME);
    vfs_inode_t* dd = vfs_lookup(NDIR);
    ok("default owner root:root, files 0644, dirs 0755",
       df && dd && df->uid == 0 && df->gid == 0 && df->mode == 0100644u && dd->mode == 040755u);
    ok("default: a user may read, not write (EACCES)",
       df && cred_inode_perm(&u1000, df, MAY_READ) == 0 && cred_inode_perm(&u1000, df, MAY_WRITE) == -CRED_EACCES);
    umount_clean("/mnt");
    exfat_set_mount_options("uid=1000,gid=100,umask=027,ro");
    rm = exfat_mount(dev, "/mnt");
    df = vfs_lookup(WNAME); dd = vfs_lookup(NDIR);
    ok("mount -o uid=1000,gid=100,umask=027,ro: owner + modes",
       rm == 0 && df && dd && df->uid == 1000 && df->gid == 100 && df->mode == 0100750u && dd->mode == 040750u);
    ok("ro mount: write -> EROFS before EACCES (owner and other user)",
       df && cred_inode_perm(&u1000, df, MAY_WRITE) == -CRED_EROFS && cred_inode_perm(&u2000, df, MAY_WRITE) == -CRED_EROFS);
    ok("umask=027: owner reads, others get EACCES",
       df && cred_inode_perm(&u1000, df, MAY_READ) == 0 && cred_inode_perm(&u2000, df, MAY_READ) == -CRED_EACCES);
    ok("ro mount: create refused, statfs ST_RDONLY",
       vfs_create("/mnt/ro_new.txt", "x", 1) != 0 && vfs_statfs("/mnt", &sf) == 0 && (sf.flags & 1));
    umount_clean("/mnt");
    rm = exfat_mount(dev, "/mnt");
    df = vfs_lookup(WNAME);
    ok("options do not leak into the next mount", rm == 0 && df && df->uid == 0 && df->mode == 0100644u);
    umount_clean("/mnt");
    ok("final unmount clean", dev_dirty_bit(dev) == 0);
    lg("[M41] DONE\n");
}

/* ---- corrupt images ---- */
static void probe_files(void){
    static char b[256];
    vfs_inode_t* i;
    lgx("[M41C]   ok.txt read=", (uint64_t)(int64_t)vfs_read_all("/mnt/ok.txt", b, sizeof(b)));
    i = vfs_lookup("/mnt/oob.txt");
    lgx("[M41C]   oob.txt read=", (uint64_t)(int64_t)(i && i->ops ? i->ops->read(i, 0, b, 16) : -2));
    i = vfs_lookup("/mnt/cycle.bin");
    lgx("[M41C]   cycle.bin read=", (uint64_t)(int64_t)(i && i->ops ? i->ops->read(i, 0, b, 16) : -2));
    lgx("[M41C]   cycle.bin write=", (uint64_t)(int64_t)(i && i->ops ? i->ops->write(i, 0, "x", 1) : -2));
    lgx("[M41C]   badsum.txt visible=", (uint64_t)(vfs_lookup("/mnt/badsum.txt") != NULL));
    lgx("[M41C]   badcnt.txt visible=", (uint64_t)(vfs_lookup("/mnt/badcnt.txt") != NULL));
    lgx("[M41C]   loopdir readdir=", (uint64_t)(int64_t)vfs_readdir("/mnt/loopdir", cnt_cb, &(struct cnt_ctx){0}));
    lgx("[M41C]   unlink cycle.bin=", (uint64_t)(int64_t)vfs_remove("/mnt/cycle.bin"));
    struct cnt_ctx cc = { 0 };
    vfs_readdir("/mnt", cnt_cb, &cc);
    lgx("[M41C]   readdir entries=", (uint64_t)cc.n);
    lgx("[M41C]   create rc=", (uint64_t)(int64_t)vfs_create("/mnt/new.txt", "n\n", 2));
}

static void corrupt_mode(void){
    lg("[M41C] --- exFAT corrupt-image suite ---\n");
    if(mnt_ops("/mnt", NULL)) umount_clean("/mnt");
    static const char* devs[] = { "vda", "sda", "sdb", "sdc", "sdd", "nvme0n1", "usb0" };
    for(unsigned k = 0; k < sizeof(devs) / sizeof(devs[0]); k++){
        if(!block_find(devs[k])) continue;
        int r = exfat_mount(devs[k], "/mnt");
        lg("[M41C] dev="); lg(devs[k]); lgx(" mount rc=", (uint64_t)(int64_t)r);
        if(r == 0){
            probe_files();
            umount_clean("/mnt");
        }
    }
    lg("[M41C] DONE (kernel alive)\n");
}

/* Called once the boot mounts (/mnt, /proc, ...) are in place. */
void exfat_selftest(void){
    if(block_find("sda")) { corrupt_mode(); return; }
    const vfs_fs_ops_t* o = mnt_ops("/mnt", NULL);
    const char* dev = 0; const char* type = 0;
    if(!o || !o->mntinfo || o->mntinfo(&dev, &type) != 0 || !dev){ lg("[M41] no disk mounted at /mnt\n"); return; }
    good_mode(dev);
}

#else
void exfat_selftest(void){ }
#endif
