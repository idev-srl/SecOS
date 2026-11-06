/*
 * SecOS Kernel - FAT32 read-write filesystem
 *
 * A compact but functional FAT32 implementation over the block layer, wired
 * into the VFS. Supports: mount, path lookup, directory listing, file read,
 * file create/write (grow), truncate, remove, mkdir and rename. Short 8.3
 * names only — long-name (LFN) directory entries are skipped on read and never
 * generated. Sector size is assumed to be 512 bytes (virtio-blk guarantees it).
 *
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "fat32.h"
#include "vfs.h"
#include "block.h"
#include <stdint.h>
#include <stddef.h>

#define SECSZ           512
#define FAT_EOC         0x0FFFFFF8u
#define ATTR_DIR        0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LFN        0x0F
#define ATTR_VOLUME     0x08
#define DENT_FREE       0xE5
#define DENT_END        0x00

/* ── Mounted volume state (single FAT32 volume) ──────────────────────────── */
static block_dev_t* g_dev;
static uint32_t g_bytes_per_sec;
static uint32_t g_sec_per_clus;
static uint32_t g_reserved;
static uint32_t g_num_fats;
static uint32_t g_sec_per_fat;
static uint32_t g_root_clus;
static uint32_t g_fat_start;     /* LBA of first FAT */
static uint32_t g_data_start;    /* LBA of cluster 2 */
static uint32_t g_total_clusters;
static uint32_t g_clus_bytes;
static int      g_mounted;

/* Scratch sector buffer (single-threaded kernel). */
static uint8_t g_sec[SECSZ];

/* ── little-endian helpers ───────────────────────────────────────────────── */
static uint16_t rd16(const uint8_t* p){ return (uint16_t)(p[0] | (p[1]<<8)); }
static uint32_t rd32(const uint8_t* p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static void wr16(uint8_t* p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void wr32(uint8_t* p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

static int sec_read(uint64_t lba, uint8_t* buf){ return block_read(g_dev, lba, buf, 1)==1 ? 0 : -1; }
static int sec_write(uint64_t lba, const uint8_t* buf){ return block_write(g_dev, lba, buf, 1)==1 ? 0 : -1; }

static uint64_t clus_lba(uint32_t c){ return (uint64_t)g_data_start + (uint64_t)(c-2)*g_sec_per_clus; }
static int clus_valid(uint32_t c){ return c>=2 && c < g_total_clusters+2; }

/* ── FAT entry access ────────────────────────────────────────────────────── */
static uint32_t fat_get(uint32_t clus){
    uint32_t off = clus*4;
    uint64_t lba = g_fat_start + off / g_bytes_per_sec;
    uint32_t within = off % g_bytes_per_sec;
    if(sec_read(lba, g_sec)!=0) return FAT_EOC;
    return rd32(g_sec+within) & 0x0FFFFFFFu;
}

/* The FSInfo sector caches the free-cluster count and a next-free hint. SecOS
 * does not keep them up to date, so before the first FAT change of a mount it
 * marks both "unknown" (0xFFFFFFFF, valid per spec): fsck.fat and other OSes
 * then recount instead of trusting a stale value. */
static uint32_t g_fsinfo_sec;
static int      g_fsinfo_done;
static void fat_fsinfo_invalidate(void){
    static uint8_t fi[SECSZ];
    g_fsinfo_done = 1;
    if(g_fsinfo_sec==0 || g_fsinfo_sec==0xFFFF || g_fsinfo_sec>=g_reserved) return;
    if(sec_read(g_fsinfo_sec, fi)!=0) return;
    if(rd32(fi+0)!=0x41615252u || rd32(fi+484)!=0x61417272u) return;
    if(rd32(fi+488)==0xFFFFFFFFu && rd32(fi+492)==0xFFFFFFFFu) return;
    wr32(fi+488, 0xFFFFFFFFu); wr32(fi+492, 0xFFFFFFFFu);
    sec_write(g_fsinfo_sec, fi);
}

static int fat_set(uint32_t clus, uint32_t val){
    if(!g_fsinfo_done) fat_fsinfo_invalidate();
    uint32_t off = clus*4;
    uint32_t within = off % g_bytes_per_sec;
    uint64_t rel = off / g_bytes_per_sec;
    for(uint32_t f=0; f<g_num_fats; f++){
        uint64_t lba = g_fat_start + f*g_sec_per_fat + rel;
        if(sec_read(lba, g_sec)!=0) return -1;
        uint32_t cur = rd32(g_sec+within);
        cur = (cur & 0xF0000000u) | (val & 0x0FFFFFFFu);
        wr32(g_sec+within, cur);
        if(sec_write(lba, g_sec)!=0) return -1;
    }
    return 0;
}

/* Zero every sector of a cluster. */
static int clus_zero(uint32_t c){
    for(uint32_t i=0;i<SECSZ;i++) g_sec[i]=0;
    for(uint32_t s=0;s<g_sec_per_clus;s++) if(sec_write(clus_lba(c)+s, g_sec)!=0) return -1;
    return 0;
}

/* Allocate one free cluster, mark it EOC, zero it. Returns 0 if none free. */
static uint32_t fat_alloc(void){
    for(uint32_t c=2; c<g_total_clusters+2; c++){
        if(fat_get(c)==0){
            if(fat_set(c, FAT_EOC)!=0) return 0;
            if(clus_zero(c)!=0) return 0;
            return c;
        }
    }
    return 0;
}

/* Free a whole cluster chain starting at `c`. */
static void fat_free_chain(uint32_t c){
    while(clus_valid(c)){
        uint32_t next = fat_get(c);
        fat_set(c, 0);
        if(next < 0x0FFFFFF8u && next>=2) c = next; else break;
    }
}

/* ── DOS timestamps (local time == UTC here; 2 s resolution) ─────────────── */
extern uint64_t kclock_real_sec(void);
extern void kclock_unix_to_civil(uint64_t t, int* y, int* mo, int* d, int* h, int* mi, int* s);
extern uint64_t kclock_civil_to_unix(int y, int mo, int d, int h, int mi, int s);
static void fat_now(uint16_t* tm, uint16_t* dt){
    int y, mo, d, h, mi, sc;
    kclock_unix_to_civil(kclock_real_sec(), &y, &mo, &d, &h, &mi, &sc);
    if(y < 1980){ y = 1980; mo = 1; d = 1; h = mi = sc = 0; }
    if(y > 2107) y = 2107;
    *dt = (uint16_t)(((y-1980)<<9) | (mo<<5) | d);
    *tm = (uint16_t)((h<<11) | (mi<<5) | (sc/2));
}
static uint64_t fat_to_unix(uint16_t tm, uint16_t dt){
    if(dt==0) return 0;
    int y=1980+(dt>>9), mo=(dt>>5)&15, d=dt&31;
    if(mo<1||mo>12||d<1) return 0;
    return kclock_civil_to_unix(y, mo, d, tm>>11, (tm>>5)&63, (tm&31)*2);
}
/* Stamp a raw entry: create time (+date) when `create`, always write+access. */
static void fat_stamp(uint8_t* raw, int create){
    uint16_t tm, dt; fat_now(&tm, &dt);
    if(create){ raw[13]=0; wr16(raw+14,tm); wr16(raw+16,dt); }
    wr16(raw+18,dt); wr16(raw+22,tm); wr16(raw+24,dt);
}

/* ── 8.3 name conversion ─────────────────────────────────────────────────── */
static char up(char c){ return (c>='a'&&c<='z') ? (char)(c-'a'+'A') : c; }

/* Convert a path component to a raw 11-byte 8.3 name. Returns -1 if empty. */
static int name_to_83(const char* s, uint8_t out[11]){
    for(int i=0;i<11;i++) out[i]=' ';
    if(!s || !*s) return -1;
    int i=0, o=0;
    /* base name up to '.' (max 8) */
    while(s[i] && s[i]!='.' && o<8){ out[o++]=(uint8_t)up(s[i]); i++; }
    while(s[i] && s[i]!='.') i++; /* skip overflow */
    if(s[i]=='.'){
        i++; o=8;
        while(s[i] && o<11){ out[o++]=(uint8_t)up(s[i]); i++; }
    }
    return 0;
}

/* Convert a raw 11-byte 8.3 name to a display string "NAME.EXT". */
static void name_from_83(const uint8_t raw[11], char* out, size_t cap){
    size_t o=0;
    for(int i=0;i<8 && raw[i]!=' ';i++){ if(o<cap-1) out[o++]=(char)raw[i]; }
    if(raw[8]!=' '){ if(o<cap-1) out[o++]='.'; for(int i=8;i<11 && raw[i]!=' ';i++){ if(o<cap-1) out[o++]=(char)raw[i]; } }
    out[o]=0;
}

static int raw_eq(const uint8_t a[11], const uint8_t b[11]){ for(int i=0;i<11;i++) if(a[i]!=b[i]) return 0; return 1; }

/* ── Directory entry location ────────────────────────────────────────────── */
typedef struct {
    uint32_t clus;     /* cluster holding the entry */
    uint32_t off;      /* byte offset within that cluster */
    uint8_t  raw[32];  /* the entry bytes */
} dent_loc_t;

/* Iterate entries of a directory chain; find one matching name83.
 * Returns 0 and fills loc on hit, -1 if not found. */
static int dir_find(uint32_t dir_clus, const uint8_t name83[11], dent_loc_t* loc){
    uint32_t c = dir_clus;
    while(clus_valid(c)){
        for(uint32_t s=0;s<g_sec_per_clus;s++){
            if(sec_read(clus_lba(c)+s, g_sec)!=0) return -1;
            for(uint32_t e=0;e<g_bytes_per_sec;e+=32){
                uint8_t* ent = g_sec+e;
                if(ent[0]==DENT_END) return -1;
                if(ent[0]==DENT_FREE) continue;
                if(ent[11]==ATTR_LFN) continue;
                if(ent[11]&ATTR_VOLUME) continue;
                if(raw_eq(ent, name83)){
                    loc->clus = c; loc->off = s*g_bytes_per_sec + e;
                    for(int i=0;i<32;i++) loc->raw[i]=ent[i];
                    return 0;
                }
            }
        }
        uint32_t n = fat_get(c);
        if(n<0x0FFFFFF8u && n>=2) c=n; else break;
    }
    return -1;
}

/* Write back 32 bytes of a directory entry at loc->clus/off (uses loc->raw). */
static int dent_write_back(const dent_loc_t* loc){
    uint32_t s = loc->off / g_bytes_per_sec;
    uint32_t within = loc->off % g_bytes_per_sec;
    uint64_t lba = clus_lba(loc->clus) + s;
    if(sec_read(lba, g_sec)!=0) return -1;
    for(int i=0;i<32;i++) g_sec[within+i]=loc->raw[i];
    return sec_write(lba, g_sec);
}

/* Find a free directory slot (DENT_END or DENT_FREE), extending the directory
 * by one cluster if the chain is full. Returns 0, sets *out_clus/*out_off. */
static int dir_alloc_slot(uint32_t dir_clus, uint32_t* out_clus, uint32_t* out_off){
    uint32_t c = dir_clus, last = dir_clus;
    while(clus_valid(c)){
        last = c;
        for(uint32_t s=0;s<g_sec_per_clus;s++){
            if(sec_read(clus_lba(c)+s, g_sec)!=0) return -1;
            for(uint32_t e=0;e<g_bytes_per_sec;e+=32){
                uint8_t b0 = g_sec[e];
                if(b0==DENT_END || b0==DENT_FREE){ *out_clus=c; *out_off=s*g_bytes_per_sec+e; return 0; }
            }
        }
        uint32_t n = fat_get(c);
        if(n<0x0FFFFFF8u && n>=2) c=n; else break;
    }
    /* Extend directory with a fresh cluster. */
    uint32_t nc = fat_alloc();
    if(!nc) return -1;
    if(fat_set(last, nc)!=0) return -1;
    *out_clus = nc; *out_off = 0;
    return 0;
}

/* ── Path resolution ─────────────────────────────────────────────────────── */
/* Walk an absolute mount-relative path. On success fills *first_cluster,
 * *size, *is_dir and (if not root) the directory-entry location. */
static int path_resolve(const char* path, uint32_t* first_cluster, uint32_t* size,
                        int* is_dir, dent_loc_t* loc_out){
    if(!path) return -1;
    if(path[0]=='/') path++;
    /* root */
    if(path[0]==0){ if(first_cluster)*first_cluster=g_root_clus; if(size)*size=0; if(is_dir)*is_dir=1; if(loc_out) loc_out->clus=0; return 0; }
    uint32_t cur_dir = g_root_clus;
    char comp[64];
    dent_loc_t loc;
    int found_is_dir = 1; uint32_t found_fc = g_root_clus, found_sz = 0;
    while(*path){
        size_t k=0;
        while(*path && *path!='/' && k<sizeof(comp)-1) comp[k++]=*path++;
        comp[k]=0;
        while(*path=='/') path++;
        if(k==0) continue;
        uint8_t n83[11];
        if(name_to_83(comp, n83)!=0) return -1;
        if(dir_find(cur_dir, n83, &loc)!=0) return -1;
        found_fc = ((uint32_t)rd16(loc.raw+20)<<16) | rd16(loc.raw+26);
        found_sz = rd32(loc.raw+28);
        found_is_dir = (loc.raw[11]&ATTR_DIR)?1:0;
        if(*path){ /* more components: must be a directory */
            if(!found_is_dir) return -1;
            cur_dir = found_fc ? found_fc : g_root_clus;
        }
    }
    if(first_cluster)*first_cluster=found_fc;
    if(size)*size=found_sz;
    if(is_dir)*is_dir=found_is_dir;
    if(loc_out)*loc_out=loc;
    return 0;
}

/* Split path into parent dir cluster + final component name83. */
static int split_parent(const char* path, uint32_t* parent_clus, uint8_t name83[11], char* lastcomp, size_t lcsz){
    if(!path || path[0]!='/') return -1;
    /* find last '/' */
    const char* p = path; const char* last_slash = path;
    while(*p){ if(*p=='/') last_slash=p; p++; }
    /* parent path = path[..last_slash], child = last_slash+1 */
    const char* child = last_slash+1;
    if(*child==0) return -1;
    size_t k=0; while(child[k] && k<lcsz-1){ lastcomp[k]=child[k]; k++; } lastcomp[k]=0;
    if(name_to_83(lastcomp, name83)!=0) return -1;
    /* resolve parent */
    char parent[256]; size_t plen = (size_t)(last_slash - path);
    if(plen==0){ parent[0]='/'; parent[1]=0; }
    else { if(plen>sizeof(parent)-1) plen=sizeof(parent)-1; for(size_t i=0;i<plen;i++) parent[i]=path[i]; parent[plen]=0; }
    uint32_t fc; int isd;
    if(path_resolve(parent, &fc, NULL, &isd, NULL)!=0) return -1;
    if(!isd) return -1;
    *parent_clus = fc ? fc : g_root_clus;
    return 0;
}

/* ── VFS inode pool ──────────────────────────────────────────────────────── */
typedef struct { uint32_t first_cluster; uint32_t size; int is_dir; uint32_t ent_clus; uint32_t ent_off; } fat_node_t;
#define FAT_INODES 32
static vfs_inode_t g_inodes[FAT_INODES];
static fat_node_t  g_nodes[FAT_INODES];
static int         g_inode_used;

static vfs_inode_t* inode_alloc(const char* path){
    /* reuse by path */
    for(int i=0;i<g_inode_used;i++){ const char* a=g_inodes[i].path; size_t k=0; int eq=1; while(a[k]||path[k]){ if(a[k]!=path[k]){eq=0;break;} k++; } if(eq) return &g_inodes[i]; }
    int idx = (g_inode_used<FAT_INODES) ? g_inode_used++ : (g_inode_used-1); /* clobber last if full */
    vfs_inode_t* ino = &g_inodes[idx];
    size_t k=0; for(; path[k] && k<sizeof(ino->path)-1; k++) ino->path[k]=path[k]; ino->path[k]=0;
    ino->fs_data = &g_nodes[idx];
    return ino;
}

/* ── File read ───────────────────────────────────────────────────────────── */
static int fat_read_file(fat_node_t* nd, size_t offset, void* buf, size_t len){
    if(offset >= nd->size) return 0;
    if(offset+len > nd->size) len = nd->size - offset;
    uint8_t* out = (uint8_t*)buf;
    size_t copied = 0;
    /* walk to the cluster containing `offset` */
    uint32_t c = nd->first_cluster;
    uint32_t skip_clusters = (uint32_t)(offset / g_clus_bytes);
    uint32_t in_clus_off = (uint32_t)(offset % g_clus_bytes);
    for(uint32_t i=0;i<skip_clusters && clus_valid(c);i++){ uint32_t n=fat_get(c); c = (n<0x0FFFFFF8u&&n>=2)?n:0; }
    while(copied<len && clus_valid(c)){
        for(uint32_t s=0; s<g_sec_per_clus && copied<len; s++){
            uint32_t sec_base = s*g_bytes_per_sec;
            if(sec_base + g_bytes_per_sec <= in_clus_off) continue; /* before offset */
            if(sec_read(clus_lba(c)+s, g_sec)!=0) return -1;
            uint32_t start = (in_clus_off > sec_base) ? (in_clus_off - sec_base) : 0;
            for(uint32_t b=start; b<g_bytes_per_sec && copied<len; b++) out[copied++]=g_sec[b];
        }
        in_clus_off = 0;
        uint32_t n=fat_get(c); c=(n<0x0FFFFFF8u&&n>=2)?n:0;
    }
    return (int)copied;
}

/* Ensure the chain has at least `need_clusters`; returns first_cluster (may be
 * newly allocated) or 0 on failure. Updates *first if it was 0. */
static int fat_ensure_clusters(uint32_t* first, uint32_t need_clusters){
    if(need_clusters==0) return 0;
    uint32_t c = *first;
    if(!clus_valid(c)){
        uint32_t nc = fat_alloc(); if(!nc) return -1; *first = nc; c = nc;
    }
    uint32_t have = 1;
    while(have < need_clusters){
        uint32_t n = fat_get(c);
        if(n>=2 && n<0x0FFFFFF8u){ c=n; have++; continue; }
        uint32_t nc = fat_alloc(); if(!nc) return -1;
        if(fat_set(c, nc)!=0) return -1;
        c = nc; have++;
    }
    return 0;
}

/* ── File write (grow) ───────────────────────────────────────────────────── */
static int fat_write_file(fat_node_t* nd, size_t offset, const void* buf, size_t len){
    if(len==0) return 0;
    size_t new_size = offset+len;
    if(new_size < nd->size) new_size = nd->size;
    uint32_t need = (uint32_t)((new_size + g_clus_bytes - 1)/g_clus_bytes);
    if(fat_ensure_clusters(&nd->first_cluster, need)!=0) return -1;

    const uint8_t* in = (const uint8_t*)buf;
    size_t written = 0;
    uint32_t c = nd->first_cluster;
    uint32_t skip = (uint32_t)(offset / g_clus_bytes);
    uint32_t in_clus_off = (uint32_t)(offset % g_clus_bytes);
    for(uint32_t i=0;i<skip && clus_valid(c);i++){ uint32_t n=fat_get(c); c=(n<0x0FFFFFF8u&&n>=2)?n:0; }
    while(written<len && clus_valid(c)){
        for(uint32_t s=0; s<g_sec_per_clus && written<len; s++){
            uint32_t sec_base = s*g_bytes_per_sec;
            if(sec_base + g_bytes_per_sec <= in_clus_off) continue;
            uint32_t start = (in_clus_off > sec_base) ? (in_clus_off - sec_base) : 0;
            int full = (start==0 && (len-written)>=g_bytes_per_sec);
            if(!full){ if(sec_read(clus_lba(c)+s, g_sec)!=0) return -1; }
            for(uint32_t b=start; b<g_bytes_per_sec && written<len; b++) g_sec[b]=in[written++];
            if(sec_write(clus_lba(c)+s, g_sec)!=0) return -1;
        }
        in_clus_off = 0;
        if(written<len){ uint32_t n=fat_get(c); c=(n<0x0FFFFFF8u&&n>=2)?n:0; }
    }
    /* Update size + first cluster in the directory entry. */
    nd->size = (uint32_t)new_size;
    if(nd->ent_clus){
        dent_loc_t loc; loc.clus=nd->ent_clus; loc.off=nd->ent_off;
        uint32_t s = loc.off/g_bytes_per_sec, within=loc.off%g_bytes_per_sec;
        if(sec_read(clus_lba(loc.clus)+s, g_sec)!=0) return -1;
        for(int i=0;i<32;i++) loc.raw[i]=g_sec[within+i];
        wr16(loc.raw+20, (uint16_t)(nd->first_cluster>>16));
        wr16(loc.raw+26, (uint16_t)(nd->first_cluster&0xFFFF));
        wr32(loc.raw+28, nd->size);
        fat_stamp(loc.raw, 0);                       /* mtime/atime = now */
        if(dent_write_back(&loc)!=0) return -1;
    }
    return (int)written;
}

/* ── [M42] Ownership: FAT has none, so the mount supplies it ──────────────────
 * Every file and directory of the volume is owned by the mount's uid:gid with
 * mode 0777 & ~dmask (directories) / 0777 & ~fmask (files), as Linux vfat.
 * Options (mount(2) data, comma-separated): uid=N gid=N umask=OOO (both masks)
 * dmask=OOO fmask=OOO. Defaults: root:root, dmask 022, fmask 133 -> 0755 / 0644
 * (no exec bits: programs are not run from FAT). chmod/chown are accepted and
 * ignored (nothing to store), after the usual owner/root check. */
struct fat_opts { uint32_t uid, gid, dmask, fmask; };
static struct fat_opts g_fopt     = { 0, 0, 022, 0133 };   /* the mounted volume */
static struct fat_opts g_fopt_new = { 0, 0, 022, 0133 };   /* for the next mount */
static int fopt_num(const char** pp, int base, uint32_t* out){
    const char* p=*pp; uint32_t v=0; int n=0;
    while((*p>='0' && *p<=(base==8?'7':'9'))){ v=v*(uint32_t)base+(uint32_t)(*p-'0'); p++; n++; if(n>10) return -1; }
    if(!n) return -1;
    *out=v; *pp=p; return 0;
}
static int fopt_key(const char** pp, const char* k){
    const char* p=*pp; while(*k){ if(*p!=*k) return 0; p++; k++; }
    *pp=p; return 1;
}
int fat32_set_mount_options(const char* opts){
    struct fat_opts o = { 0, 0, 022, 0133 };
    const char* p = opts ? opts : "";
    while(*p){
        uint32_t v;
        if(fopt_key(&p,"uid=")){ if(fopt_num(&p,10,&v)) return -1; o.uid=v; }
        else if(fopt_key(&p,"gid=")){ if(fopt_num(&p,10,&v)) return -1; o.gid=v; }
        else if(fopt_key(&p,"umask=")){ if(fopt_num(&p,8,&v)) return -1; o.dmask=o.fmask=v&0777; }
        else if(fopt_key(&p,"dmask=")){ if(fopt_num(&p,8,&v)) return -1; o.dmask=v&0777; }
        else if(fopt_key(&p,"fmask=")){ if(fopt_num(&p,8,&v)) return -1; o.fmask=v&0777; }
        else if(fopt_key(&p,"rw") || fopt_key(&p,"defaults")){ }
        else return -1;                                   /* unknown option: refuse */
        if(*p==',') p++; else if(*p) return -1;
    }
    g_fopt_new = o;
    return 0;
}
static int fat32_setattr(const char* path, const vfs_attr_t* a, unsigned valid){
    (void)path; (void)a; (void)valid;
    return 0;                                             /* nothing to store (vfat "quiet") */
}

/* ── VFS operations ──────────────────────────────────────────────────────── */
static vfs_inode_t* fat32_lookup(const char* path){
    if(!g_mounted) return NULL;
    uint32_t fc, sz; int isd; dent_loc_t loc; loc.clus=0; loc.off=0;
    if(path_resolve(path, &fc, &sz, &isd, &loc)!=0) return NULL;
    vfs_inode_t* ino = inode_alloc(path && path[0] ? path : "/");
    if(!ino) return NULL;
    ino->type = isd ? VFS_NODE_DIR : VFS_NODE_FILE;
    ino->size = sz;
    fat_node_t* nd = (fat_node_t*)ino->fs_data;
    nd->first_cluster=fc; nd->size=sz; nd->is_dir=isd; nd->ent_clus=loc.clus; nd->ent_off=loc.off;
    ino->mode = isd ? (0040000u | (0777u & ~g_fopt.dmask)) : (0100000u | (0777u & ~g_fopt.fmask));
    ino->nlink = 1; ino->uid = g_fopt.uid; ino->gid = g_fopt.gid;      /* [M42] mount options */
    ino->atime = ino->mtime = ino->ctime = 0;
    if(loc.clus){                                  /* root has no entry: times stay 0 */
        ino->mtime = fat_to_unix(rd16(loc.raw+22), rd16(loc.raw+24));
        ino->ctime = fat_to_unix(rd16(loc.raw+14), rd16(loc.raw+16));
        ino->atime = fat_to_unix(0, rd16(loc.raw+18));
        if(!ino->ctime) ino->ctime = ino->mtime;
    }
    return ino;
}

static int fat32_readdir(const char* dir_path, vfs_iter_cb cb, void* user){
    if(!g_mounted || !cb) return -1;
    uint32_t fc; int isd;
    if(path_resolve(dir_path, &fc, NULL, &isd, NULL)!=0) return -1;
    if(!isd) return -1;
    uint32_t c = fc ? fc : g_root_clus;
    char base[256]; size_t bl=0;
    if(dir_path){ for(; dir_path[bl] && bl<sizeof(base)-1; bl++) base[bl]=dir_path[bl]; }
    base[bl]=0;
    if(bl==1 && base[0]=='/'){ bl=0; base[0]=0; } /* root: no trailing slash */
    while(clus_valid(c)){
        for(uint32_t s=0;s<g_sec_per_clus;s++){
            if(sec_read(clus_lba(c)+s, g_sec)!=0) return -1;
            for(uint32_t e=0;e<g_bytes_per_sec;e+=32){
                uint8_t* ent=g_sec+e;
                if(ent[0]==DENT_END){ return 0; }
                if(ent[0]==DENT_FREE) continue;
                if(ent[11]==ATTR_LFN) continue;
                if(ent[11]&ATTR_VOLUME) continue;
                char nm[16]; name_from_83(ent, nm, sizeof(nm));
                if(nm[0]=='.') continue; /* skip . and .. */
                vfs_inode_t child; /* transient */
                size_t k=0; for(size_t i=0;i<bl && k<sizeof(child.path)-1;i++) child.path[k++]=base[i];
                if(k<sizeof(child.path)-1) child.path[k++]='/';
                for(size_t i=0; nm[i] && k<sizeof(child.path)-1;i++) child.path[k++]=nm[i];
                child.path[k]=0;
                child.type = (ent[11]&ATTR_DIR)?VFS_NODE_DIR:VFS_NODE_FILE;
                child.size = rd32(ent+28);
                child.fs_data=NULL; child.ops=NULL;
                cb(&child, user);
            }
        }
        uint32_t n=fat_get(c); c=(n<0x0FFFFFF8u&&n>=2)?n:0;
    }
    return 0;
}

static int fat32_read(vfs_inode_t* ino, size_t off, void* buf, size_t len){
    if(!g_mounted || !ino || ino->type!=VFS_NODE_FILE) return -1;
    return fat_read_file((fat_node_t*)ino->fs_data, off, buf, len);
}

static int fat32_write(vfs_inode_t* ino, size_t off, const void* data, size_t len){
    if(!g_mounted || !ino || ino->type!=VFS_NODE_FILE) return -1;
    fat_node_t* nd = (fat_node_t*)ino->fs_data;
    int r = fat_write_file(nd, off, data, len);
    if(r>=0) ino->size = nd->size;
    return r;
}

static int fat32_create(const char* path, const void* initial, size_t size){
    if(!g_mounted || !path) return -1;
    /* fail if exists */
    if(path_resolve(path, NULL, NULL, NULL, NULL)==0) return -1;
    uint32_t parent; uint8_t n83[11]; char comp[64];
    if(split_parent(path, &parent, n83, comp, sizeof(comp))!=0) return -1;
    uint32_t ec, eo;
    if(dir_alloc_slot(parent, &ec, &eo)!=0) return -1;
    dent_loc_t loc; loc.clus=ec; loc.off=eo;
    for(int i=0;i<32;i++) loc.raw[i]=0;
    for(int i=0;i<11;i++) loc.raw[i]=n83[i];
    loc.raw[11]=ATTR_ARCHIVE;
    wr16(loc.raw+20,0); wr16(loc.raw+26,0); wr32(loc.raw+28,0);
    fat_stamp(loc.raw, 1);
    if(dent_write_back(&loc)!=0) return -1;
    if(initial && size>0){
        fat_node_t nd; nd.first_cluster=0; nd.size=0; nd.is_dir=0; nd.ent_clus=ec; nd.ent_off=eo;
        if(fat_write_file(&nd, 0, initial, size)<0) return -1;
    }
    return 0;
}

static int fat32_mkdir(const char* path){
    if(!g_mounted || !path) return -1;
    if(path_resolve(path, NULL, NULL, NULL, NULL)==0) return -1;
    uint32_t parent; uint8_t n83[11]; char comp[64];
    if(split_parent(path, &parent, n83, comp, sizeof(comp))!=0) return -1;
    uint32_t nc = fat_alloc(); if(!nc) return -1;
    /* write "." and ".." entries into the new cluster */
    for(uint32_t i=0;i<SECSZ;i++) g_sec[i]=0;
    uint8_t* dot=g_sec; for(int i=0;i<11;i++) dot[i]=' '; dot[0]='.'; dot[11]=ATTR_DIR;
    wr16(dot+20,(uint16_t)(nc>>16)); wr16(dot+26,(uint16_t)(nc&0xFFFF));
    fat_stamp(dot, 1);
    /* ".." of a top-level directory points at cluster 0 (the root), per spec */
    uint32_t pc = (parent==g_root_clus) ? 0 : parent;
    uint8_t* dd=g_sec+32; for(int i=0;i<11;i++) dd[i]=' '; dd[0]='.'; dd[1]='.'; dd[11]=ATTR_DIR;
    wr16(dd+20,(uint16_t)(pc>>16)); wr16(dd+26,(uint16_t)(pc&0xFFFF));
    fat_stamp(dd, 1);
    if(sec_write(clus_lba(nc), g_sec)!=0) return -1;
    /* parent entry */
    uint32_t ec,eo; if(dir_alloc_slot(parent,&ec,&eo)!=0) return -1;
    dent_loc_t loc; loc.clus=ec; loc.off=eo; for(int i=0;i<32;i++) loc.raw[i]=0;
    for(int i=0;i<11;i++) loc.raw[i]=n83[i];
    loc.raw[11]=ATTR_DIR;
    wr16(loc.raw+20,(uint16_t)(nc>>16)); wr16(loc.raw+26,(uint16_t)(nc&0xFFFF)); wr32(loc.raw+28,0);
    fat_stamp(loc.raw, 1);
    return dent_write_back(&loc);
}

static int fat32_remove(const char* path){
    if(!g_mounted || !path) return -1;
    uint32_t fc; uint32_t sz; int isd; dent_loc_t loc; loc.clus=0;
    if(path_resolve(path, &fc, &sz, &isd, &loc)!=0) return -1;
    if(loc.clus==0) return -1; /* cannot remove root */
    if(fc>=2) fat_free_chain(fc);
    loc.raw[0]=DENT_FREE;
    return dent_write_back(&loc);
}

static int fat32_truncate(const char* path, size_t new_size){
    if(!g_mounted || !path) return -1;
    uint32_t fc; uint32_t sz; int isd; dent_loc_t loc; loc.clus=0;
    if(path_resolve(path, &fc, &sz, &isd, &loc)!=0) return -1;
    if(isd || loc.clus==0) return -1;
    uint32_t need = (uint32_t)((new_size + g_clus_bytes - 1)/g_clus_bytes);
    if(new_size <= sz){
        /* shrink: free clusters beyond `need`, keep chain head */
        if(need==0){ if(fc>=2) fat_free_chain(fc); fc=0; }
        else {
            uint32_t c=fc; for(uint32_t i=1;i<need && clus_valid(c);i++){ uint32_t n=fat_get(c); c=(n<0x0FFFFFF8u&&n>=2)?n:c; }
            uint32_t tail = fat_get(c); fat_set(c, FAT_EOC);
            if(tail>=2 && tail<0x0FFFFFF8u) fat_free_chain(tail);
            /* zero the kept cluster past the new end: a later grow reads zeros */
            uint32_t keep = (uint32_t)(new_size % g_clus_bytes);
            if(keep && clus_valid(c)){
                for(uint32_t sct=keep/g_bytes_per_sec; sct<g_sec_per_clus; sct++){
                    if(sec_read(clus_lba(c)+sct, g_sec)!=0) return -1;
                    uint32_t from = (sct==keep/g_bytes_per_sec) ? keep%g_bytes_per_sec : 0;
                    for(uint32_t i=from;i<g_bytes_per_sec;i++) g_sec[i]=0;
                    if(sec_write(clus_lba(c)+sct, g_sec)!=0) return -1;
                }
            }
        }
    } else {
        if(fat_ensure_clusters(&fc, need)!=0) return -1;
    }
    wr16(loc.raw+20,(uint16_t)(fc>>16)); wr16(loc.raw+26,(uint16_t)(fc&0xFFFF)); wr32(loc.raw+28,(uint32_t)new_size);
    fat_stamp(loc.raw, 0);
    return dent_write_back(&loc);
}

/* rename: an existing target (the VFS checked the type rules) is deleted
 * first, then the entry is copied to the new name and the old one freed; a
 * directory moved to another parent gets its ".." re-pointed. Not crash-atomic
 * (FAT has no journal), but never leaves both names pointing at one chain. */
static int fat32_rename(const char* oldp, const char* newp){
    if(!g_mounted || !oldp || !newp) return -1;
    uint32_t fc, sz; int isd; dent_loc_t loc; loc.clus=0;
    if(path_resolve(oldp, &fc, &sz, &isd, &loc)!=0) return -1;
    if(loc.clus==0) return -1;
    uint32_t parent; uint8_t n83[11]; char comp[64];
    if(split_parent(newp, &parent, n83, comp, sizeof(comp))!=0) return -1;
    uint32_t oparent; uint8_t o83[11]; char ocomp[64];
    if(split_parent(oldp, &oparent, o83, ocomp, sizeof(ocomp))!=0) return -1;
    if(raw_eq(n83, o83) && parent==oparent) return 0;           /* same 8.3 name */
    uint32_t tfc, tsz; int tisd; dent_loc_t tl; tl.clus=0;
    if(path_resolve(newp, &tfc, &tsz, &tisd, &tl)==0){          /* replace target */
        if(tl.clus==0) return -1;
        if(tfc>=2 && tfc!=fc) fat_free_chain(tfc);
        tl.raw[0]=DENT_FREE;
        if(dent_write_back(&tl)!=0) return -1;
    }
    uint32_t ec,eo; if(dir_alloc_slot(parent,&ec,&eo)!=0) return -1;
    dent_loc_t nl; nl.clus=ec; nl.off=eo;
    for(int i=0;i<32;i++) nl.raw[i]=loc.raw[i];
    for(int i=0;i<11;i++) nl.raw[i]=n83[i];
    if(dent_write_back(&nl)!=0) return -1;
    loc.raw[0]=DENT_FREE;
    if(dent_write_back(&loc)!=0) return -1;
    if(isd && fc>=2 && parent!=oparent){                        /* fix ".." */
        if(sec_read(clus_lba(fc), g_sec)!=0) return -1;
        uint8_t* dd=g_sec+32;
        if(dd[0]=='.' && dd[1]=='.'){
            uint32_t pc = (parent==g_root_clus) ? 0 : parent;
            wr16(dd+20,(uint16_t)(pc>>16)); wr16(dd+26,(uint16_t)(pc&0xFFFF));
            if(sec_write(clus_lba(fc), g_sec)!=0) return -1;
        }
    }
    return 0;
}

/* statfs: clusters as blocks; free ones counted from the first FAT. */
static int fat32_statfs(vfs_statfs_t* st){
    if(!g_mounted || !st) return -1;
    uint64_t freec=0;
    uint32_t per = g_bytes_per_sec/4;
    for(uint32_t s=0; s<g_sec_per_fat; s++){
        if(sec_read(g_fat_start+s, g_sec)!=0) return -1;
        for(uint32_t i=0;i<per;i++){
            uint32_t c = s*per+i;
            if(c<2) continue;
            if(c>=g_total_clusters+2) goto done;
            if((rd32(g_sec+i*4)&0x0FFFFFFFu)==0) freec++;
        }
    }
done:
    st->type = 0x4d44;                   /* MSDOS_SUPER_MAGIC */
    st->bsize = st->frsize = g_clus_bytes;
    st->blocks = g_total_clusters;
    st->bfree = st->bavail = freec;
    st->files = 0; st->ffree = 0;        /* no inode table */
    st->namelen = 12;                    /* 8.3 */
    return 0;
}

/* FAT and directory updates are written through to the device as they happen
 * (no cached FAT sectors), so there is nothing left to flush at shutdown. */
static int fat32_sync(void){ return 0; }

static int fat32_mntinfo(const char** src, const char** type){
    if(!g_mounted) return -1;
    if(src) *src = (g_dev && g_dev->name) ? g_dev->name : "fat32";
    if(type) *type = "vfat";
    return 0;
}

static vfs_fs_ops_t fat32_ops = {
    .lookup=fat32_lookup, .readdir=fat32_readdir, .read=fat32_read, .write=fat32_write,
    .create=fat32_create, .mkdir=fat32_mkdir, .remove=fat32_remove, .rename=fat32_rename,
    .truncate=fat32_truncate, .sync=fat32_sync, .statfs=fat32_statfs, .mntinfo=fat32_mntinfo,
    .setattr=fat32_setattr
};

/* ── Mount ───────────────────────────────────────────────────────────────── */
int fat32_mount(const char* dev_name, const char* mount_point){
    block_dev_t* dev = block_find(dev_name);
    if(!dev) return -1;
    // Probe into locals: a rejected device must not clobber the geometry (or
    // g_dev) of a volume that is already mounted.
    static uint8_t bs[SECSZ];
    if(block_read(dev, 0, bs, 1)!=1) return -1;
    uint16_t bps   = rd16(bs+11);
    uint8_t  spc   = bs[13];
    uint16_t rsvd  = rd16(bs+14);
    uint8_t  nfats = bs[16];
    uint32_t spf   = rd32(bs+36);
    uint32_t root  = rd32(bs+44);
    uint32_t total_sectors = rd32(bs+32);
    if(bps!=SECSZ || spc==0 || nfats==0 || spf==0) return -1;
    if(bs[510]!=0x55 || bs[511]!=0xAA) return -1;
    // Only FAT32: a FAT12/16 BPB has a fixed root directory (BPB_RootEntCnt != 0)
    // and a 16-bit FAT size (BPB_FATSz16 != 0), and offsets 36/44 are not
    // BPB_FATSz32/BPB_RootClus there. Mounting one with FAT32 geometry read
    // garbage (and would have written garbage): QEMU's vvfat ESP (FAT16) used
    // in the UEFI smoke test was mounted at /mnt and failed the M10 readback.
    if(rd16(bs+17)!=0 || rd16(bs+22)!=0 || rd16(bs+19)!=0) return -1;
    if(total_sectors==0 || root<2) return -1;
    if((uint64_t)rsvd + (uint64_t)nfats*spf >= total_sectors) return -1;
    g_dev = dev;
    for(int i=0;i<SECSZ;i++) g_sec[i]=bs[i];
    g_bytes_per_sec = bps;
    g_sec_per_clus  = spc;
    g_reserved      = rsvd;
    g_num_fats      = nfats;
    g_sec_per_fat   = spf;
    g_root_clus     = root;
    g_fat_start  = g_reserved;
    g_data_start = g_reserved + g_num_fats*g_sec_per_fat;
    uint32_t data_sectors = total_sectors - g_data_start;
    g_total_clusters = data_sectors / g_sec_per_clus;
    g_clus_bytes = g_sec_per_clus * g_bytes_per_sec;
    g_inode_used = 0;
    g_fsinfo_sec = rd16(bs+48);
    g_fsinfo_done = 0;
    g_mounted = 1;
    g_fopt = g_fopt_new;                                  /* [M42] this mount's owner/masks */
    g_fopt_new.uid = g_fopt_new.gid = 0; g_fopt_new.dmask = 022; g_fopt_new.fmask = 0133;
    return vfs_mount(mount_point, &fat32_ops, "fat32");
}
