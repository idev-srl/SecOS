/*
 * sysfs.c — [M23] /sys virtual filesystem (minimal).
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * A small Linux-like /sys: the block-device tree under /sys/block/<dev>/ with
 * the `size` (in 512-byte sectors, Linux convention) and `sector_size`
 * attributes, the CPU tree under /sys/devices/system/cpu/ (online, possible,
 * present, offline, kernel_max, cpuN/online: the Linux cpulist format that
 * libcs and nproc read), plus /sys/version. Generated on read; read-only
 * namespace.
 */
#include "vfs.h"
#include "block.h"
#include "spinlock.h"
#include "percpu.h"   // CPU count + online flags (/sys/devices/system/cpu)
#include <stddef.h>
#include <stdint.h>

static int seq(const char* a, const char* b){ int i=0; while(a[i]&&b[i]){ if(a[i]!=b[i]) return 0; i++; } return a[i]==b[i]; }

#define SYSFS_MAXC 5   // deepest path: /devices/system/cpu/cpuN/online

// Split rel ("/block/sda/size") into up to SYSFS_MAXC components. Returns the
// count, or SYSFS_MAXC + 1 when the path is deeper (no such node).
static int split(const char* rel, char comp[SYSFS_MAXC][32]) {
    int nc=0; const char* p=rel; if(*p=='/') p++;
    while(*p){
        if(nc==SYSFS_MAXC) return SYSFS_MAXC+1;
        int k=0; while(*p && *p!='/' && k<31) comp[nc][k++]=*p++; comp[nc][k]=0;
        if(k>0) nc++;
        while(*p=='/') p++;
    }
    return nc;
}

// One lock guards the generation buffer and the inode pool: on SMP two
// processes can look up or read sysfs at the same time. An open fd keeps the
// inode pointer lookup returned, so lookups hand out pool slots keyed by path
// (an fd's inode is recycled only after SYSFS_NINO other distinct paths;
// readdir entries are transient — callers copy the name).
static spinlock_t g_sysfs_lock;
static char g_genbuf[1024];   // cpulist of 256 CPUs with gaps fits
#define SYSFS_NINO 32
static vfs_inode_t g_ipool[SYSFS_NINO];
static int g_inext;
static vfs_inode_t g_ent;     // readdir scratch entry

static int gen_attr(block_dev_t* b, const char* attr) {
    uint64_t v;
    if (seq(attr,"size")) v = b->sector_count * (uint64_t)b->sector_size / 512; // 512-byte units
    else if (seq(attr,"sector_size")) v = b->sector_size;
    else return -1;
    int n=0; char tmp[24]; if(v==0) tmp[n++]='0'; while(v){ tmp[n++]=(char)('0'+v%10); v/=10; }
    int len=0; while(n--) g_genbuf[len++]=tmp[n]; g_genbuf[len++]='\n'; g_genbuf[len]=0;
    return len;
}

// ---- /sys/devices/system/cpu ----
// "Present" (= possible) CPUs are the ones the kernel registered: the BSP plus
// every AP SMP bring-up tried; "online" are those running the kernel. With
// `nosmp` both are just CPU 0.
static uint32_t cpu_n(void){ uint32_t n=smp_cpu_count(); if(n==0) n=1; return n>SMP_MAX_CPUS?SMP_MAX_CPUS:n; }
static int cpu_on(uint32_t i){ return smp_cpu_online(i); }

static int put_u(char* o, int len, uint32_t v){
    char t[12]; int n=0; do{ t[n++]=(char)('0'+v%10); v/=10; }while(v);
    while(n--) o[len++]=t[n];
    return len;
}
// Linux cpulist ("0-3", "0,2-3", "" when empty) of the CPUs where want(i) holds,
// newline-terminated, into g_genbuf. Returns the length.
static int gen_cpulist(int which){   // 0 = present/possible, 1 = online, 2 = offline
    uint32_t n=cpu_n(); int len=0, first=1;
    for(uint32_t i=0;i<n;){
        int in = which==0 ? 1 : which==1 ? cpu_on(i) : !cpu_on(i);
        if(!in){ i++; continue; }
        uint32_t j=i;
        while(j+1<n && (which==0 ? 1 : which==1 ? cpu_on(j+1) : !cpu_on(j+1))) j++;
        if(!first) g_genbuf[len++]=',';
        first=0;
        len=put_u(g_genbuf,len,i);
        if(j>i){ g_genbuf[len++]='-'; len=put_u(g_genbuf,len,j); }
        i=j+1;
    }
    g_genbuf[len++]='\n'; g_genbuf[len]=0;
    return len;
}
// "cpuN" -> N when N is a present CPU, else -1.
static int cpu_index(const char* nm){
    if(nm[0]!='c'||nm[1]!='p'||nm[2]!='u'||!nm[3]) return -1;
    uint32_t v=0;
    for(int i=3;nm[i];i++){ if(nm[i]<'0'||nm[i]>'9'||i>6) return -1; v=v*10+(uint32_t)(nm[i]-'0'); }
    if(nm[3]=='0'&&nm[4]) return -1;   // no leading zeros
    return v<cpu_n() ? (int)v : -1;
}
static int is_cpu_dir(char c[SYSFS_MAXC][32], int n){
    return n>=3 && seq(c[0],"devices") && seq(c[1],"system") && seq(c[2],"cpu");
}
// Contents of a /devices/system/cpu file (rel = path inside sysfs), or -1.
static int gen_cpu_file(const char* rel){
    char c[SYSFS_MAXC][32]; int n=split(rel,c);
    if(!is_cpu_dir(c,n)) return -1;
    if(n==4){
        if(seq(c[3],"online")) return gen_cpulist(1);
        if(seq(c[3],"possible")||seq(c[3],"present")) return gen_cpulist(0);
        if(seq(c[3],"offline")) return gen_cpulist(2);
        if(seq(c[3],"kernel_max")){ int len=put_u(g_genbuf,0,SMP_MAX_CPUS-1); g_genbuf[len++]='\n'; g_genbuf[len]=0; return len; }
        return -1;
    }
    if(n==5 && seq(c[4],"online")){
        int i=cpu_index(c[3]); if(i<0) return -1;
        g_genbuf[0]=cpu_on((uint32_t)i)?'1':'0'; g_genbuf[1]='\n'; g_genbuf[2]=0; return 2;
    }
    return -1;
}
static char g_cpu_tag;   // fs_data of a CPU attribute file (vs a block_dev_t*)

static vfs_inode_t* fill(vfs_inode_t* ino, const char* path, vfs_node_type_t t, size_t size, void* fsd) {
    int k=0; for(int i=0; path[i] && k<255; i++) ino->path[k++]=path[i]; ino->path[k]=0;
    ino->type=t; ino->size=size; ino->fs_data=fsd;
    return ino;
}
// Lookup result: the pool slot already holding `path`, else the next one.
static vfs_inode_t* mk(const char* path, vfs_node_type_t t, size_t size, void* fsd) {
    int slot=-1;
    for(int i=0;i<SYSFS_NINO;i++) if(g_ipool[i].path[0] && seq(g_ipool[i].path,path)){ slot=i; break; }
    if(slot<0){ slot=g_inext; g_inext=(g_inext+1)%SYSFS_NINO; }
    return fill(&g_ipool[slot], path, t, size, fsd);
}
// out = a + b + c, truncated to 47 characters (every sysfs path is shorter).
static void join(char* out, const char* a, const char* b, const char* c) {
    int k=0;
    for (int j=0;a[j]&&k<47;j++) out[k++]=a[j];
    for (int j=0;b[j]&&k<47;j++) out[k++]=b[j];
    for (int j=0;c[j]&&k<47;j++) out[k++]=c[j];
    out[k]=0;
}
// Readdir entry (transient).
static vfs_inode_t* ent(const char* path, vfs_node_type_t t, void* fsd) {
    return fill(&g_ent, path, t, 0, fsd);
}

static vfs_inode_t* sysfs_lookup_locked(const char* rel) {
    if (!rel || rel[0]!='/') return NULL;
    if (rel[1]==0) return mk("/", VFS_NODE_DIR, 0, NULL);
    char c[SYSFS_MAXC][32]; int n=split(rel, c);
    if (n==1 && seq(c[0],"block")) return mk("/block", VFS_NODE_DIR, 0, NULL);
    if (n==1 && seq(c[0],"devices")) return mk(rel, VFS_NODE_DIR, 0, NULL);
    if (n==2 && seq(c[0],"devices") && seq(c[1],"system")) return mk(rel, VFS_NODE_DIR, 0, NULL);
    if (n==3 && is_cpu_dir(c,n)) return mk(rel, VFS_NODE_DIR, 0, NULL);
    if (n==4 && is_cpu_dir(c,n) && cpu_index(c[3])>=0) return mk(rel, VFS_NODE_DIR, 0, NULL);
    if (n>=4 && n<=5 && is_cpu_dir(c,n)) {
        int len = gen_cpu_file(rel);
        return len<0 ? NULL : mk(rel, VFS_NODE_FILE, (size_t)len, &g_cpu_tag);
    }
    if (n==1 && seq(c[0],"version")) return mk("/version", VFS_NODE_FILE, 0, NULL);
    if (n>=2 && n<=3 && seq(c[0],"block")) {
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

static int sysfs_readdir_locked(const char* dir, vfs_iter_cb cb, void* user) {
    if (!cb || !dir) return -1;
    if (dir[0]=='/' && dir[1]==0) {
        cb(ent("/block", VFS_NODE_DIR,NULL), user);
        cb(ent("/devices", VFS_NODE_DIR,NULL), user);
        return 0;
    }
    char c[SYSFS_MAXC][32]; int n=split(dir, c);
    if (n==1 && seq(c[0],"devices")) { cb(ent("/devices/system", VFS_NODE_DIR,NULL), user); return 0; }
    if (n==2 && seq(c[0],"devices") && seq(c[1],"system")) { cb(ent("/devices/system/cpu", VFS_NODE_DIR,NULL), user); return 0; }
    if (n==3 && is_cpu_dir(c,n)) {
        const char* attrs[5]={"online","possible","present","offline","kernel_max"};
        const char* pre="/devices/system/cpu/";
        char p[64];
        for (int a=0;a<5;a++) {
            join(p, pre, attrs[a], "");
            cb(ent(p,VFS_NODE_FILE,&g_cpu_tag),user);
        }
        for (uint32_t i=0;i<cpu_n();i++) {
            char nm[16]="cpu"; nm[put_u(nm,3,i)]=0;
            join(p, pre, nm, "");
            cb(ent(p,VFS_NODE_DIR,NULL),user);
        }
        return 0;
    }
    if (n==4 && is_cpu_dir(c,n) && cpu_index(c[3])>=0) {
        char p[64];
        join(p, "/devices/system/cpu/", c[3], "/online");
        cb(ent(p,VFS_NODE_FILE,&g_cpu_tag),user);
        return 0;
    }
    if (n==1 && seq(c[0],"block")) {
        int bn=block_count();
        for (int i=0;i<bn;i++) {
            block_dev_t* b=block_get(i);
            if (!b||!b->name) continue;
            char path[64];
            join(path, "/block/", b->name, "");
            cb(ent(path, VFS_NODE_DIR,NULL), user);
        }
        return 0;
    }
    if (n==2 && seq(c[0],"block") && block_find(c[1])) {
        const char* attrs[2]={"size","sector_size"};
        for (int a=0;a<2;a++) {
            char p[64], dir[48];
            join(dir, "/block/", c[1], "/");
            join(p, dir, attrs[a], "");
            cb(ent(p,VFS_NODE_FILE,NULL),user);
        }
        return 0;
    }
    return -1;
}

static int sysfs_read_locked(vfs_inode_t* inode, size_t off, void* buf, size_t len) {
    if (!inode || inode->type!=VFS_NODE_FILE) return -1;
    int total;
    if (inode->fs_data == &g_cpu_tag) {
        total = gen_cpu_file(inode->path);
    } else if (inode->fs_data) {
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

static vfs_inode_t* sysfs_lookup(const char* rel) {
    spin_lock(&g_sysfs_lock);
    vfs_inode_t* r = sysfs_lookup_locked(rel);
    spin_unlock(&g_sysfs_lock);
    return r;
}
static int sysfs_readdir(const char* dir, vfs_iter_cb cb, void* user) {
    spin_lock(&g_sysfs_lock);       // the callbacks only copy names / count
    int r = sysfs_readdir_locked(dir, cb, user);
    spin_unlock(&g_sysfs_lock);
    return r;
}
static int sysfs_read(vfs_inode_t* inode, size_t off, void* buf, size_t len) {
    spin_lock(&g_sysfs_lock);
    int r = sysfs_read_locked(inode, off, buf, len);
    spin_unlock(&g_sysfs_lock);
    return r;
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
