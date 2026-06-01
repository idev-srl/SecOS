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

static int fat_set(uint32_t clus, uint32_t val){
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
        if(dent_write_back(&loc)!=0) return -1;
    }
    return (int)written;
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
    uint8_t* dd=g_sec+32; for(int i=0;i<11;i++) dd[i]=' '; dd[0]='.'; dd[1]='.'; dd[11]=ATTR_DIR;
    wr16(dd+20,(uint16_t)(parent>>16)); wr16(dd+26,(uint16_t)(parent&0xFFFF));
    if(sec_write(clus_lba(nc), g_sec)!=0) return -1;
    /* parent entry */
    uint32_t ec,eo; if(dir_alloc_slot(parent,&ec,&eo)!=0) return -1;
    dent_loc_t loc; loc.clus=ec; loc.off=eo; for(int i=0;i<32;i++) loc.raw[i]=0;
    for(int i=0;i<11;i++) loc.raw[i]=n83[i];
    loc.raw[11]=ATTR_DIR;
    wr16(loc.raw+20,(uint16_t)(nc>>16)); wr16(loc.raw+26,(uint16_t)(nc&0xFFFF)); wr32(loc.raw+28,0);
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
        }
    } else {
        if(fat_ensure_clusters(&fc, need)!=0) return -1;
    }
    wr16(loc.raw+20,(uint16_t)(fc>>16)); wr16(loc.raw+26,(uint16_t)(fc&0xFFFF)); wr32(loc.raw+28,(uint32_t)new_size);
    return dent_write_back(&loc);
}

static int fat32_rename(const char* oldp, const char* newp){
    if(!g_mounted || !oldp || !newp) return -1;
    uint32_t fc, sz; int isd; dent_loc_t loc; loc.clus=0;
    if(path_resolve(oldp, &fc, &sz, &isd, &loc)!=0) return -1;
    if(loc.clus==0) return -1;
    if(path_resolve(newp, NULL,NULL,NULL,NULL)==0) return -1; /* dest exists */
    uint32_t parent; uint8_t n83[11]; char comp[64];
    if(split_parent(newp, &parent, n83, comp, sizeof(comp))!=0) return -1;
    uint32_t ec,eo; if(dir_alloc_slot(parent,&ec,&eo)!=0) return -1;
    dent_loc_t nl; nl.clus=ec; nl.off=eo;
    for(int i=0;i<32;i++) nl.raw[i]=loc.raw[i];
    for(int i=0;i<11;i++) nl.raw[i]=n83[i];
    if(dent_write_back(&nl)!=0) return -1;
    loc.raw[0]=DENT_FREE;
    return dent_write_back(&loc);
}

static vfs_fs_ops_t fat32_ops = {
    .lookup=fat32_lookup, .readdir=fat32_readdir, .read=fat32_read, .write=fat32_write,
    .create=fat32_create, .mkdir=fat32_mkdir, .remove=fat32_remove, .rename=fat32_rename,
    .truncate=fat32_truncate
};

/* ── Mount ───────────────────────────────────────────────────────────────── */
int fat32_mount(const char* dev_name, const char* mount_point){
    block_dev_t* dev = block_find(dev_name);
    if(!dev) return -1;
    g_dev = dev;
    if(sec_read(0, g_sec)!=0) return -1;
    g_bytes_per_sec = rd16(g_sec+11);
    g_sec_per_clus  = g_sec[13];
    g_reserved      = rd16(g_sec+14);
    g_num_fats      = g_sec[16];
    g_sec_per_fat   = rd32(g_sec+36);
    g_root_clus     = rd32(g_sec+44);
    uint32_t total_sectors = rd32(g_sec+32);
    if(g_bytes_per_sec!=SECSZ || g_sec_per_clus==0 || g_num_fats==0 || g_sec_per_fat==0) return -1;
    if(g_sec[510]!=0x55 || g_sec[511]!=0xAA) return -1;
    g_fat_start  = g_reserved;
    g_data_start = g_reserved + g_num_fats*g_sec_per_fat;
    uint32_t data_sectors = total_sectors - g_data_start;
    g_total_clusters = data_sectors / g_sec_per_clus;
    g_clus_bytes = g_sec_per_clus * g_bytes_per_sec;
    g_inode_used = 0;
    g_mounted = 1;
    return vfs_mount(mount_point, &fat32_ops, "fat32");
}
