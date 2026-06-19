/*
 * procfs.c — [M23] /proc virtual filesystem.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Generated-on-read kernel introspection as text files: /proc/{meminfo,uptime,
 * version,mounts,cpuinfo,self}. Each file's content is produced fresh on every
 * read into a static buffer, then the requested byte slice is returned (so the
 * file size is always current). A pure namespace: no create/write.
 */
#include "vfs.h"
#include "pmm.h"
#include "process.h"
#include <stddef.h>
#include <stdint.h>

extern uint64_t timer_get_ticks(void);
extern uint64_t timer_get_uptime_seconds(void);
extern uint32_t timer_get_frequency(void);
extern process_t* sched_get_current(void);
extern uint64_t ktime_ns(void);   /* [M28-3] TSC monotonic clock */
extern uint64_t tsc_hz(void);

// ---- tiny text builder ----
struct sb { char* p; int cap; int len; };
static void sb_init(struct sb* s, char* b, int c){ s->p=b; s->cap=c; s->len=0; }
static void sb_putc(struct sb* s, char c){ if(s->len < s->cap-1) s->p[s->len++]=c; }
static void sb_str(struct sb* s, const char* t){ while(*t) sb_putc(s, *t++); }
static void sb_u64(struct sb* s, uint64_t v){
    char tmp[24]; int n=0; if(v==0) tmp[n++]='0';
    while(v){ tmp[n++]=(char)('0'+(v%10)); v/=10; }
    while(n--) sb_putc(s, tmp[n]);
}

typedef int (*proc_gen)(struct sb*);

static int gen_meminfo(struct sb* s){
    uint64_t total=pmm_get_total_memory(), used=pmm_get_used_memory(), free=pmm_get_free_memory();
    sb_str(s,"MemTotal: "); sb_u64(s,total/1024); sb_str(s," kB\n");
    sb_str(s,"MemFree:  "); sb_u64(s,free/1024);  sb_str(s," kB\n");
    sb_str(s,"MemUsed:  "); sb_u64(s,used/1024);  sb_str(s," kB\n");
    return s->len;
}
static int gen_uptime(struct sb* s){
    // [M28-3] Sub-second precision from the TSC monotonic clock when calibrated;
    // fall back to whole-second tick uptime otherwise.
    uint64_t ns = ktime_ns();
    if(ns){
        uint64_t sec = ns / 1000000000ull;
        uint64_t cs  = (ns % 1000000000ull) / 10000000ull;   // centiseconds
        sb_u64(s, sec); sb_putc(s,'.');
        if(cs < 10) sb_putc(s,'0');
        sb_u64(s, cs); sb_str(s," s\n");
    } else {
        sb_u64(s, timer_get_uptime_seconds()); sb_str(s," s ("); sb_u64(s, timer_get_ticks()); sb_str(s," ticks)\n");
    }
    return s->len;
}
static int gen_version(struct sb* s){
    sb_str(s,"SecOS version 0.1.0-dev build "); sb_str(s, BUILD_TS);
    sb_str(s," git "); sb_str(s, GIT_HASH); sb_putc(s,'\n');
    return s->len;
}
static int gen_cpuinfo(struct sb* s){
    sb_str(s,"arch        : x86_64\n");
    sb_str(s,"mode        : long (64-bit)\n");
    sb_str(s,"timer_hz    : "); sb_u64(s, timer_get_frequency()); sb_putc(s,'\n');
    sb_str(s,"tsc_mhz     : "); sb_u64(s, tsc_hz()/1000000ull); sb_putc(s,'\n');
    return s->len;
}
static int gen_mounts(struct sb* s){
    int n = vfs_mount_count();
    for(int i=0;i<n;i++){
        const char* mp=0; const char* fs=0;
        if(vfs_mount_info(i,&mp,&fs)!=0) continue;
        sb_str(s, fs?fs:"?"); sb_putc(s,' '); sb_str(s, mp?mp:"?"); sb_str(s," rw\n");
    }
    return s->len;
}
static int gen_self(struct sb* s){
    process_t* c = sched_get_current();
    sb_str(s,"Pid:   "); sb_u64(s, c?c->pid:0); sb_putc(s,'\n');
    sb_str(s,"State: "); sb_u64(s, c?(uint64_t)c->state:0); sb_putc(s,'\n');
    return s->len;
}

static const struct { const char* name; proc_gen gen; } g_files[] = {
    {"meminfo", gen_meminfo}, {"uptime", gen_uptime}, {"version", gen_version},
    {"cpuinfo", gen_cpuinfo}, {"mounts", gen_mounts}, {"self", gen_self},
};
#define NFILES (int)(sizeof(g_files)/sizeof(g_files[0]))

static int seq(const char* a, const char* b){ int i=0; while(a[i]&&b[i]){ if(a[i]!=b[i]) return 0; i++; } return a[i]==b[i]; }
static char g_genbuf[2048];
static vfs_inode_t g_inodes[NFILES + 1];

static int gen_into(int idx){ struct sb s; sb_init(&s, g_genbuf, sizeof(g_genbuf)); g_files[idx].gen(&s); g_genbuf[s.len]=0; return s.len; }

static vfs_inode_t* fill(int idx){
    vfs_inode_t* ino = &g_inodes[idx<0 ? NFILES : idx];
    if(idx<0){ ino->path[0]='/'; ino->path[1]=0; ino->type=VFS_NODE_DIR; ino->size=0; ino->fs_data=NULL; return ino; }
    ino->path[0]='/'; int k=1; const char* s=g_files[idx].name;
    for(int j=0;s[j]&&k<255;j++) ino->path[k++]=s[j]; ino->path[k]=0;
    ino->type=VFS_NODE_FILE;
    ino->size=(size_t)gen_into(idx);
    ino->fs_data=(void*)(uintptr_t)(idx+1);   // 1-based index
    return ino;
}

static vfs_inode_t* procfs_lookup(const char* rel){
    if(!rel||rel[0]!='/') return NULL;
    if(rel[1]==0) return fill(-1);
    for(int i=0;i<NFILES;i++) if(seq(g_files[i].name, rel+1)) return fill(i);
    return NULL;
}
static int procfs_readdir(const char* dir, vfs_iter_cb cb, void* user){
    if(!cb || !(dir&&dir[0]=='/'&&dir[1]==0)) return -1;
    for(int i=0;i<NFILES;i++) cb(fill(i), user);
    return 0;
}
static int procfs_read(vfs_inode_t* inode, size_t off, void* buf, size_t len){
    if(!inode||!inode->fs_data) return -1;
    int idx=(int)(uintptr_t)inode->fs_data - 1;
    if(idx<0||idx>=NFILES) return -1;
    int total=gen_into(idx);                 // regenerate live
    if((int)off>=total) return 0;
    size_t avail=(size_t)total-off; if(len>avail) len=avail;
    for(size_t i=0;i<len;i++) ((uint8_t*)buf)[i]=(uint8_t)g_genbuf[off+i];
    return (int)len;
}
static int procfs_ro_w(vfs_inode_t* a, size_t b, const void* c, size_t d){ (void)a;(void)b;(void)c;(void)d; return -1; }
static int procfs_ro_create(const char* p, const void* d, size_t s){ (void)p;(void)d;(void)s; return -1; }
static int procfs_ro_mkdir(const char* p){ (void)p; return -1; }
static int procfs_ro_remove(const char* p){ (void)p; return -1; }
static int procfs_ro_rename(const char* a, const char* b){ (void)a;(void)b; return -1; }
static int procfs_ro_trunc(const char* p, size_t s){ (void)p;(void)s; return -1; }

static vfs_fs_ops_t procfs_ops = {
    .flags = VFS_FS_NOCACHE,
    .lookup = procfs_lookup,
    .readdir = procfs_readdir,
    .read = procfs_read,
    .write = procfs_ro_w,
    .create = procfs_ro_create,
    .mkdir = procfs_ro_mkdir,
    .remove = procfs_ro_remove,
    .rename = procfs_ro_rename,
    .truncate = procfs_ro_trunc,
};

int procfs_mount(const char* mount_point){
    extern int vfs_mount(const char*, const vfs_fs_ops_t*, const char*);
    return vfs_mount(mount_point, &procfs_ops, "procfs");
}
