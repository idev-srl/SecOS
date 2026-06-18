/*
 * sysfs.c — [M23] /sys virtual filesystem (minimal).
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * A small Linux-like /sys: the block-device tree under /sys/block/<dev>/ with
 * the `size` (in 512-byte sectors, Linux convention) and `sector_size`
 * attributes, plus /sys/version. Generated on read; read-only namespace.
 */
#include "vfs.h"
#include "block.h"
#include <stddef.h>
#include <stdint.h>

static int seq(const char* a, const char* b){ int i=0; while(a[i]&&b[i]){ if(a[i]!=b[i]) return 0; i++; } return a[i]==b[i]; }

// Split rel ("/block/sda/size") into up to 3 components. Returns the count.
static int split(const char* rel, char comp[3][32]) {
    int nc=0; const char* p=rel; if(*p=='/') p++;
    while(*p && nc<3){
        int k=0; while(*p && *p!='/' && k<31) comp[nc][k++]=*p++; comp[nc][k]=0;
        if(k>0) nc++;
        while(*p=='/') p++;
    }
    return nc;
}

static char g_genbuf[64];
static vfs_inode_t g_ino;   // single scratch inode (lookups are serialized)

static int gen_attr(block_dev_t* b, const char* attr) {
    uint64_t v;
    if (seq(attr,"size")) v = b->sector_count * (uint64_t)b->sector_size / 512; // 512-byte units
    else if (seq(attr,"sector_size")) v = b->sector_size;
    else return -1;
    int n=0; char tmp[24]; if(v==0) tmp[n++]='0'; while(v){ tmp[n++]=(char)('0'+v%10); v/=10; }
    int len=0; while(n--) g_genbuf[len++]=tmp[n]; g_genbuf[len++]='\n'; g_genbuf[len]=0;
    return len;
}

static vfs_inode_t* mk(const char* path, vfs_node_type_t t, size_t size, void* fsd) {
    int k=0; for(int i=0; path[i] && k<255; i++) g_ino.path[k++]=path[i]; g_ino.path[k]=0;
    g_ino.type=t; g_ino.size=size; g_ino.fs_data=fsd;
    return &g_ino;
}

static vfs_inode_t* sysfs_lookup(const char* rel) {
    if (!rel || rel[0]!='/') return NULL;
    if (rel[1]==0) return mk("/", VFS_NODE_DIR, 0, NULL);
    char c[3][32]; int n=split(rel, c);
    if (n==1 && seq(c[0],"block")) return mk("/block", VFS_NODE_DIR, 0, NULL);
    if (n==1 && seq(c[0],"version")) return mk("/version", VFS_NODE_FILE, 0, NULL);
    if (n>=2 && seq(c[0],"block")) {
        block_dev_t* b = block_find(c[1]);
        if (!b) return NULL;
        if (n==2) return mk(rel, VFS_NODE_DIR, 0, NULL);             // /block/<dev>
        if (n==3 && (seq(c[2],"size")||seq(c[2],"sector_size"))) {
            int len = gen_attr(b, c[2]);
            if (len<0) return NULL;
            return mk(rel, VFS_NODE_FILE, (size_t)len, b);          // attribute file
        }
    }
    return NULL;
}

static int sysfs_readdir(const char* dir, vfs_iter_cb cb, void* user) {
    if (!cb || !dir) return -1;
    if (dir[0]=='/' && dir[1]==0) { cb(mk("/block", VFS_NODE_DIR,0,NULL), user); return 0; }
    char c[3][32]; int n=split(dir, c);
    if (n==1 && seq(c[0],"block")) {
        int bn=block_count();
        for (int i=0;i<bn;i++){ block_dev_t* b=block_get(i); if(!b||!b->name) continue;
            char path[64]; int k=0; const char* pre="/block/";
            for(int j=0;pre[j];j++) path[k++]=pre[j];
            for(int j=0;b->name[j]&&k<63;j++) path[k++]=b->name[j]; path[k]=0;
            cb(mk(path, VFS_NODE_DIR,0,NULL), user);
        }
        return 0;
    }
    if (n==2 && seq(c[0],"block") && block_find(c[1])) {
        char base[48]; int k=0; const char* pre="/block/";
        for(int j=0;pre[j];j++) base[k++]=pre[j];
        for(int j=0;c[1][j]&&k<40;j++) base[k++]=c[1][j];
        const char* attrs[2]={"size","sector_size"};
        for(int a=0;a<2;a++){ char p[64]; int m=0; for(int j=0;j<k;j++)p[m++]=base[j]; p[m++]='/';
            for(int j=0;attrs[a][j];j++)p[m++]=attrs[a][j]; p[m]=0; cb(mk(p,VFS_NODE_FILE,0,NULL),user); }
        return 0;
    }
    return -1;
}

static int sysfs_read(vfs_inode_t* inode, size_t off, void* buf, size_t len) {
    if (!inode || inode->type!=VFS_NODE_FILE) return -1;
    int total;
    if (inode->fs_data) {
        // block attribute: regenerate from the path's last component.
        const char* p = inode->path; const char* attr=p; for(const char* q=p;*q;q++) if(*q=='/') attr=q+1;
        total = gen_attr((block_dev_t*)inode->fs_data, attr);
    } else {
        // /sys/version
        const char* v = "SecOS " GIT_HASH "\n"; int k=0; while(v[k]){ g_genbuf[k]=v[k]; k++; } g_genbuf[k]=0; total=k;
    }
    if (total<0) return -1;
    if ((int)off>=total) return 0;
    size_t avail=(size_t)total-off; if(len>avail) len=avail;
    for(size_t i=0;i<len;i++) ((uint8_t*)buf)[i]=(uint8_t)g_genbuf[off+i];
    return (int)len;
}

static int sysfs_ro_w(vfs_inode_t* a, size_t b, const void* c, size_t d){ (void)a;(void)b;(void)c;(void)d; return -1; }
static int sysfs_ro_create(const char* p, const void* d, size_t s){ (void)p;(void)d;(void)s; return -1; }
static int sysfs_ro_mkdir(const char* p){ (void)p; return -1; }
static int sysfs_ro_remove(const char* p){ (void)p; return -1; }
static int sysfs_ro_rename(const char* a, const char* b){ (void)a;(void)b; return -1; }
static int sysfs_ro_trunc(const char* p, size_t s){ (void)p;(void)s; return -1; }

static vfs_fs_ops_t sysfs_ops = {
    .flags = VFS_FS_NOCACHE,
    .lookup = sysfs_lookup,
    .readdir = sysfs_readdir,
    .read = sysfs_read,
    .write = sysfs_ro_w,
    .create = sysfs_ro_create,
    .mkdir = sysfs_ro_mkdir,
    .remove = sysfs_ro_remove,
    .rename = sysfs_ro_rename,
    .truncate = sysfs_ro_trunc,
};

int sysfs_mount(const char* mount_point) {
    extern int vfs_mount(const char*, const vfs_fs_ops_t*, const char*);
    return vfs_mount(mount_point, &sysfs_ops, "sysfs");
}
