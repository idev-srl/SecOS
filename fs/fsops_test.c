/*
 * SecOS Kernel - inode-keyed VFS operations self-test (M52 D3, -DSECOS_FSOPS_TEST)
 *
 * Checks, on the ramfs root and on the disk filesystem at /mnt (ext2/ext4,
 * FAT32 or exFAT: whichever data disk is attached), that the operations an
 * open fd uses act on the fd's own object and that a new file carries its
 * attributes from the start:
 *
 *   1. ftruncate after a rename: open (look up) f1, rename it to f2, create
 *      a new f1; vfs_ftruncate on the old inode must shrink f2 and leave the
 *      new f1 alone.
 *   2. fsetattr after the same rename: chmod/chown through the old inode
 *      must reach f2 (ext2 and ramfs: mode/uid/gid visible on a fresh
 *      lookup; FAT/exFAT keep no owners: the call must succeed) and must not
 *      touch the new f1.
 *   3. create_attr / mkdir_attr: the new file / directory has the requested
 *      mode, uid and gid on its first lookup (ext2 and ramfs).
 *   4. An unlinked file: remove it, create another file (on ext2 it gets the
 *      freed inode number, on FAT/exFAT the freed directory slot, on ramfs
 *      the freed table entry); ftruncate/fsetattr through the stale inode
 *      must fail and the new file must keep its bytes and mode.
 *   5. ramfs inode cache: once it is full, lookups of new entries still work
 *      (dead inodes nobody holds are recycled).
 *
 * Runs once on the BSP from kernel_main. Output on debugcon: "[FSOPS] ..."
 * lines, ending with "[FSOPS] RESULT PASS|FAIL".
 *
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 */
#ifdef SECOS_FSOPS_TEST
#include "vfs.h"
#include "debugcon.h"
#include <stdint.h>
#include <stddef.h>

static int g_fail;

static void say(const char* s){ debugcon_writestring(s); }
static void say_u(uint64_t v){
    char b[24]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while(v && n < 23);
    while(n) { char c[2] = { b[--n], 0 }; say(c); }
}
static void check(const char* where, const char* what, int ok){
    say(ok ? "[FSOPS] ok   " : "[FSOPS] FAIL "); say(where); say(": "); say(what); say("\n");
    if(!ok) g_fail = 1;
}
static void join(char* out, const char* base, const char* name){
    size_t k = 0;
    if(!(base[0] == '/' && base[1] == 0)) for(size_t i = 0; base[i] && k < 200; i++) out[k++] = base[i];
    out[k++] = '/';
    for(size_t i = 0; name[i] && k < 250; i++) out[k++] = name[i];
    out[k] = 0;
}
/* The file's bytes equal `want` (length n)? */
static int content_is(const char* path, const char* want, int n){
    char buf[64];
    int r = vfs_read_all(path, buf, sizeof(buf));
    if(r != n) return 0;
    for(int i = 0; i < n; i++) if(buf[i] != want[i]) return 0;
    return 1;
}
static uint32_t perm_of(const char* path){
    vfs_inode_t* ino = vfs_lookup(path);
    return ino ? (ino->mode & 07777u) : 0xFFFFFFFFu;
}

struct left { int n; };
static void left_cb(const vfs_inode_t* c, void* u){
    const char* p = c->path; const char* b = p;
    for(; *p; p++) if(*p == '/') b = p + 1;
    const char* t = "fsops_";
    for(int i = 0; t[i]; i++) if(b[i] != t[i]) return;
    say("[FSOPS] left behind: "); say(c->path); say("\n");
    ((struct left*)u)->n++;
}

static void run_on(const char* base, const char* tag, int owners){
    char f1[256], f2[256], f3[256], d3[256], f4[256], f5[256];
    join(f1, base, "fsops_a"); join(f2, base, "fsops_b"); join(f3, base, "fsops_c");
    join(d3, base, "fsops_d"); join(f4, base, "fsops_e"); join(f5, base, "fsops_f");
    vfs_remove(f1); vfs_remove(f2); vfs_remove(f3); vfs_rmdir(d3); vfs_remove(f4); vfs_remove(f5);

    /* 1 + 2: rename, then a new file under the old name */
    check(tag, "create f1", vfs_create(f1, "AAAAAAAA", 8) == 0);
    vfs_inode_t* old = vfs_lookup(f1);
    check(tag, "lookup f1", old != NULL);
    if(!old) return;
    check(tag, "rename f1 -> f2", vfs_rename(f1, f2) == 0);
    check(tag, "create new f1", vfs_create(f1, "BBBBBBBB", 8) == 0);
    check(tag, "ftruncate(old inode) == 0", vfs_ftruncate(old, 3) == 0);
    check(tag, "renamed file f2 truncated to 3", content_is(f2, "AAA", 3));
    check(tag, "new f1 untouched", content_is(f1, "BBBBBBBB", 8));
    uint32_t f1_perm = perm_of(f1);
    vfs_attr_t a; a.mode = 0600; a.uid = 1234; a.gid = 4321; a.atime = a.mtime = 0;
    check(tag, "fsetattr(old inode) == 0", vfs_fsetattr(old, &a, VFS_ATTR_MODE | VFS_ATTR_UID | VFS_ATTR_GID) == 0);
    if(owners){
        vfs_inode_t* i2 = vfs_lookup(f2);
        check(tag, "f2 mode 0600 uid 1234 gid 4321",
              i2 && (i2->mode & 07777u) == 0600 && i2->uid == 1234 && i2->gid == 4321);
        vfs_inode_t* i1 = vfs_lookup(f1);
        check(tag, "new f1 attributes untouched", i1 && i1->uid != 1234 && (i1->mode & 07777u) == f1_perm);
    } else {
        check(tag, "new f1 mode untouched", perm_of(f1) == f1_perm);
    }

    /* 3: create/mkdir with attributes */
    vfs_attr_t c; c.mode = 0640; c.uid = 1000; c.gid = 1001; c.atime = c.mtime = 0;
    check(tag, "create_attr f3 == 0", vfs_create_attr(f3, &c, VFS_ATTR_MODE | VFS_ATTR_UID | VFS_ATTR_GID) == 0);
    check(tag, "create_attr existing f3 fails", vfs_create_attr(f3, &c, VFS_ATTR_MODE) != 0);
    vfs_inode_t* i3 = vfs_lookup(f3);
    check(tag, "f3 is an empty file", i3 && i3->type == VFS_NODE_FILE && i3->size == 0);
    if(owners && i3)
        check(tag, "f3 born 0640 1000:1001", (i3->mode & 0170777u) == 0100640u && i3->uid == 1000 && i3->gid == 1001);
    c.mode = 0710;
    check(tag, "mkdir_attr d3 == 0", vfs_mkdir_attr(d3, &c, VFS_ATTR_MODE | VFS_ATTR_UID | VFS_ATTR_GID) == 0);
    vfs_inode_t* id = vfs_lookup(d3);
    check(tag, "d3 is a directory", id && id->type == VFS_NODE_DIR);
    if(owners && id)
        check(tag, "d3 born 0710 1000:1001", (id->mode & 0170777u) == 0040710u && id->uid == 1000 && id->gid == 1001);
    check(tag, "ftruncate(directory) == -EISDIR", id && vfs_ftruncate(id, 0) == -21);

    /* 4: an fd on an unlinked file */
    check(tag, "create f4", vfs_create(f4, "DDDDDDDD", 8) == 0);
    vfs_inode_t* dead = vfs_lookup(f4);
    check(tag, "lookup f4", dead != NULL);
    if(!dead) return;
    uint64_t dead_no = dead->ino;
    check(tag, "remove f4", vfs_remove(f4) == 0);
    check(tag, "create f5 (reuses the freed object)", vfs_create(f5, "EEEEEEEE", 8) == 0);
    vfs_inode_t* i5 = vfs_lookup(f5);
    check(tag, "f5 has its own inode", i5 && i5 != dead);
    if(i5 && dead_no && i5->ino == dead_no){ say("[FSOPS] (" ); say(tag); say(": f5 reuses number "); say_u(dead_no); say(")\n"); }
    uint32_t f5_perm = perm_of(f5);
    check(tag, "ftruncate(unlinked) fails", vfs_ftruncate(dead, 0) != 0);
    a.mode = 0777;
    check(tag, "fsetattr(unlinked) fails", vfs_fsetattr(dead, &a, VFS_ATTR_MODE | VFS_ATTR_UID) != 0);
    check(tag, "f5 keeps its bytes", content_is(f5, "EEEEEEEE", 8));
    check(tag, "f5 keeps its mode", perm_of(f5) == f5_perm);
    if(owners){ vfs_inode_t* j5 = vfs_lookup(f5); check(tag, "f5 keeps its owner", j5 && j5->uid != 1234); }
    check(tag, "f2 still 3 bytes", content_is(f2, "AAA", 3));

    vfs_remove(f1); vfs_remove(f2); vfs_remove(f3); vfs_rmdir(d3); vfs_remove(f5);
    struct left l = { 0 };
    vfs_readdir(base, left_cb, &l);
    check(tag, "no test file left behind", l.n == 0);
}

/* 5: fill the ramfs inode cache with dead inodes, then keep creating. */
static void ramfs_recycle(const char* base){
    char p[256];
    int ok = 1;
    for(int i = 0; i < 400 && ok; i++){
        char nm[16] = "fsops_r";
        nm[7] = (char)('0' + (i / 100) % 10); nm[8] = (char)('0' + (i / 10) % 10); nm[9] = (char)('0' + i % 10); nm[10] = 0;
        join(p, base, nm);
        if(vfs_create(p, "R", 1) != 0) ok = 0;
        else if(!vfs_lookup(p)) ok = 0;
        else if(!content_is(p, "R", 1)) ok = 0;
        vfs_remove(p);
        if(!ok){ say("[FSOPS] ramfs recycle stopped at "); say_u((uint64_t)i); say("\n"); }
    }
    check("ramfs", "400 create/lookup/remove past a full inode cache", ok);
}

void fsops_selftest(void){
    say("[FSOPS] start\n");
    /* ramfs: the root when no system disk is attached */
    const char* rootfs = NULL; const char* mntfs = NULL;
    for(int i = 0; i < vfs_mount_count(); i++){
        const char* mp; const char* fs;
        if(vfs_mount_info(i, &mp, &fs) != 0) continue;
        if(mp[0] == '/' && mp[1] == 0) rootfs = fs;
        if(mp[0] == '/' && mp[1] == 'm' && mp[2] == 'n' && mp[3] == 't' && mp[4] == 0) mntfs = fs;
    }
    say("[FSOPS] / is "); say(rootfs ? rootfs : "?"); say(", /mnt is "); say(mntfs ? mntfs : "(none)"); say("\n");
    if(rootfs && rootfs[0] == 'r'){                       /* "ramfs" */
        vfs_mkdir("/fsops");
        run_on("/fsops", "ramfs", 1);
        ramfs_recycle("/fsops");
        vfs_rmdir("/fsops");
    }
    if(mntfs){
        const vfs_fs_ops_t* ops = NULL;
        for(int i = 0; i < vfs_mount_count(); i++){
            const char* mp; const char* fs;
            if(vfs_mount_info(i, &mp, &fs) == 0 && mp[0] == '/' && mp[1] == 'm' && mp[2] == 'n' && mp[3] == 't' && !mp[4])
                ops = vfs_mount_ops_at(i);
        }
        const char* ty = NULL;
        if(ops && ops->mntinfo) ops->mntinfo(NULL, &ty);
        if(!ty) ty = mntfs;
        int owners = (ty[0] == 'e' && ty[1] == 'x' && ty[2] == 't');   /* ext2/ext3/ext4 */
        run_on("/mnt", ty, owners);
    } else {
        say("[FSOPS] no disk filesystem at /mnt (attach one with -drive ...,if=virtio)\n");
        if(!rootfs || rootfs[0] != 'r') g_fail = 1;
    }
    vfs_sync_all();                                       /* leave the volumes clean */
    say(g_fail ? "[FSOPS] RESULT FAIL\n" : "[FSOPS] RESULT PASS\n");
}
#endif
