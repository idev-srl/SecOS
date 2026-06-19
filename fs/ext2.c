/*
 * SecOS Kernel - ext2/ext4 read-write filesystem
 *
 * A compact read-write driver for ext2 and (no-journal) ext4 images, wired into
 * the VFS. One driver handles both: block mapping branches on the per-inode
 * EXTENTS flag (ext4 extent tree) vs. the classic indirect-block scheme (ext2).
 * Journaling is NOT implemented — ext4 images must be created with
 * `-O ^has_journal`. 64-bit block counts beyond 2^32 are not supported (test
 * images are small). Long-name directory entries are native ext2/4 linked-list
 * entries.
 *
 * Supports: mount, path lookup, readdir, file read, create, write(grow),
 * truncate, remove, mkdir, rename.
 *
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "ext2.h"
#include "vfs.h"
#include "block.h"
#include "debugcon.h"
#include <stdint.h>
#include <stddef.h>

#define SECSZ            512
#define EXT2_MAGIC       0xEF53
#define EXT2_ROOT_INO    2
#define EXT4_EXTENTS_FL  0x00080000u  /* i_flags: inode uses extents */
#define EXT4_EXTENT_MAGIC 0xF30A

#define S_IFMT  0xF000
#define S_IFDIR 0x4000
#define S_IFREG 0x8000

#define DT_REG 1
#define DT_DIR 2
#define DT_LNK 7          /* [M26] symlink dir-entry file type */
#define S_IFLNK 0xA000    /* [M26] symlink mode bits */

#define MAX_BLK 4096

/* ── Mounted volume state ────────────────────────────────────────────────── */
static block_dev_t* g_dev;
static uint32_t g_blk_size, g_sec_per_blk;
static uint32_t g_inodes_per_group, g_blocks_per_group;
static uint32_t g_inode_size;
static uint32_t g_first_data_block;
static uint32_t g_total_inodes, g_total_blocks;
static uint32_t g_bgdt_block;
static uint32_t g_num_groups;
static uint32_t g_desc_size;        /* 32 (ext2) or 64 (ext4 64bit) */
static int      g_use_extents;      /* INCOMPAT_EXTENTS: new files use extents */
static int      g_mounted;

/* Scratch block buffers for distinct purposes (single-threaded kernel). */
static uint8_t g_b[MAX_BLK];        /* data / directory block */
static uint8_t g_mb[MAX_BLK];       /* metadata: inode table, bitmap, gd */
static uint8_t g_xb[MAX_BLK];       /* indirect / extent block */

/* ── little-endian helpers ───────────────────────────────────────────────── */
static uint16_t rd16(const uint8_t* p){ return (uint16_t)(p[0] | (p[1]<<8)); }
static uint32_t rd32(const uint8_t* p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static void wr16(uint8_t* p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void wr32(uint8_t* p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

static int g_jt_active;                                  /* [M27b] a metadata txn is open */
static int jt_read_cached(uint32_t b, uint8_t* buf);     /* fwd: 1 if served from txn buffer */
static int blk_read(uint32_t b, uint8_t* buf){
    /* [M27b] Read-your-writes: inside a transaction, a block modified earlier in
     * the same txn lives only in the buffer (not yet on disk). Serve it from there
     * so read-modify-write of shared metadata (group desc, superblock, bitmaps)
     * accumulates instead of clobbering. */
    if(g_jt_active && jt_read_cached(b, buf)) return 0;
    return block_read(g_dev, (uint64_t)b*g_sec_per_blk, buf, g_sec_per_blk)==(int)g_sec_per_blk ? 0 : -1;
}

/* [M27b] Write-side journaling. A metadata transaction buffers metadata block
 * writes and commits them atomically through the JBD2 journal (commit -> in-place
 * checkpoint, done synchronously), so a crash mid-operation leaves the fs either
 * fully-old or fully-new (M27a recovery replays a committed-but-uncheckpointed
 * txn on the next mount). When a txn is active, blk_write() buffers; file DATA
 * blocks and freshly-allocated-block zeroing use blk_write_direct() (ordered
 * mode: data hits disk before the metadata commit). With no journal on the
 * volume, transactions degrade to plain in-place writes (legacy behaviour). */
static int jt_buffer(uint32_t b, const uint8_t* buf);    /* fwd */
static void jt_begin(void);                              /* fwd */
static void jt_commit(void);                             /* fwd */
static int  jt_finish(int r, int ok);                    /* fwd */

/* [M27b] Fault injection: when "power is cut" (g_blk_dead) all further writes are
 * dropped, simulating a crash. Used by the crash-consistency test to verify that
 * the journal commit ordering makes every operation atomic across a power loss. */
static int g_blk_dead;
static int g_crash_mode;             /* 0=none, 1=before commit block, 2=after publish */
void ext2_test_set_crash(int mode){ g_crash_mode=mode; g_blk_dead=0; }
static int blk_write_direct(uint32_t b, const uint8_t* buf){
    if(g_blk_dead) return 0;         /* write lost to the (simulated) crash */
    return block_write(g_dev, (uint64_t)b*g_sec_per_blk, buf, g_sec_per_blk)==(int)g_sec_per_blk ? 0 : -1;
}
static int blk_write(uint32_t b, const uint8_t* buf){
    if(g_jt_active) return jt_buffer(b, buf);            /* metadata -> transaction */
    return blk_write_direct(b, buf);
}

static void mem_zero(uint8_t* p, size_t n){ while(n--) *p++=0; }
static void mem_copy(uint8_t* d, const uint8_t* s, size_t n){ while(n--) *d++=*s++; }
static size_t e2_strlen(const char* s){ size_t n=0; while(s[n]) n++; return n; }

/* ── Group descriptor access (read into g_mb) ────────────────────────────── */
/* Copy a group descriptor into a caller buffer (avoids aliasing g_mb). */
static int gd_read(uint32_t group, uint8_t* gd /* >= g_desc_size */){
    uint32_t off = group*g_desc_size;
    uint32_t blk = g_bgdt_block + off/g_blk_size;
    uint32_t within = off % g_blk_size;
    if(blk_read(blk, g_mb)!=0) return -1;
    mem_copy(gd, g_mb+within, g_desc_size);
    return 0;
}
static int gd_write(uint32_t group, const uint8_t* gd){
    uint32_t off = group*g_desc_size;
    uint32_t blk = g_bgdt_block + off/g_blk_size;
    uint32_t within = off % g_blk_size;
    if(blk_read(blk, g_mb)!=0) return -1;
    mem_copy(g_mb+within, gd, g_desc_size);
    return blk_write(blk, g_mb);
}
static uint32_t gd_inode_table(uint32_t group){ uint8_t gd[64]; return gd_read(group,gd)==0?rd32(gd+8):0; }
static uint32_t gd_block_bitmap(uint32_t group){ uint8_t gd[64]; return gd_read(group,gd)==0?rd32(gd+0):0; }
static uint32_t gd_inode_bitmap(uint32_t group){ uint8_t gd[64]; return gd_read(group,gd)==0?rd32(gd+4):0; }

/* ── Inode read/write (inode buffer is g_inode_size bytes) ───────────────── */
static int inode_read(uint32_t ino, uint8_t* out){
    if(ino==0) return -1;
    uint32_t group = (ino-1)/g_inodes_per_group;
    uint32_t index = (ino-1)%g_inodes_per_group;
    uint32_t itab = gd_inode_table(group);
    if(!itab) return -1;
    uint32_t byte = index*g_inode_size;
    uint32_t blk = itab + byte/g_blk_size;
    uint32_t within = byte % g_blk_size;
    if(blk_read(blk, g_mb)!=0) return -1;
    mem_copy(out, g_mb+within, g_inode_size);
    return 0;
}
static int inode_write(uint32_t ino, const uint8_t* in){
    uint32_t group = (ino-1)/g_inodes_per_group;
    uint32_t index = (ino-1)%g_inodes_per_group;
    uint32_t itab = gd_inode_table(group);
    if(!itab) return -1;
    uint32_t byte = index*g_inode_size;
    uint32_t blk = itab + byte/g_blk_size;
    uint32_t within = byte % g_blk_size;
    if(blk_read(blk, g_mb)!=0) return -1;
    mem_copy(g_mb+within, in, g_inode_size);
    return blk_write(blk, g_mb);
}

/* ── Superblock free-count maintenance (best effort) ─────────────────────── */
static void sb_adjust_free_blocks(int delta){
    if(blk_read(g_first_data_block, g_mb)!=0) return; /* SB lives in block holding offset 1024 */
    uint8_t* sb = (g_blk_size==1024) ? g_mb : g_mb+1024;
    uint32_t fb = rd32(sb+12); fb=(uint32_t)((int)fb+delta); wr32(sb+12, fb);
    blk_write(g_first_data_block, g_mb);
}
static void sb_adjust_free_inodes(int delta){
    if(blk_read(g_first_data_block, g_mb)!=0) return;
    uint8_t* sb = (g_blk_size==1024) ? g_mb : g_mb+1024;
    uint32_t fi = rd32(sb+16); fi=(uint32_t)((int)fi+delta); wr32(sb+16, fi);
    blk_write(g_first_data_block, g_mb);
}

/* ── Bitmap-based allocation ─────────────────────────────────────────────── */
static uint32_t alloc_block(void){
    for(uint32_t g=0; g<g_num_groups; g++){
        uint32_t bmp = gd_block_bitmap(g);
        if(!bmp) continue;
        if(blk_read(bmp, g_xb)!=0) continue;
        uint32_t nbits = g_blocks_per_group;
        for(uint32_t i=0;i<nbits;i++){
            if(!(g_xb[i>>3] & (1u<<(i&7)))){
                g_xb[i>>3] |= (1u<<(i&7));
                if(blk_write(bmp, g_xb)!=0) return 0;
                /* update group free count */
                { uint8_t gd[64]; if(gd_read(g,gd)==0){ uint16_t fc=rd16(gd+12); if(fc) wr16(gd+12,fc-1); gd_write(g,gd); } }
                sb_adjust_free_blocks(-1);
                uint32_t bno = g*g_blocks_per_group + i + g_first_data_block;
                /* zero the new block (direct: a freshly-allocated block's prior
                 * content is irrelevant, and journalling a data block's zero would
                 * clobber the direct data write on checkpoint) */
                mem_zero(g_b, g_blk_size); blk_write_direct(bno, g_b);
                return bno;
            }
        }
    }
    return 0;
}
static void free_block(uint32_t bno){
    if(bno<g_first_data_block) return;
    uint32_t rel = bno - g_first_data_block;
    uint32_t g = rel / g_blocks_per_group;
    uint32_t i = rel % g_blocks_per_group;
    uint32_t bmp = gd_block_bitmap(g); if(!bmp) return;
    if(blk_read(bmp, g_xb)!=0) return;
    if(g_xb[i>>3] & (1u<<(i&7))){
        g_xb[i>>3] &= ~(1u<<(i&7));
        blk_write(bmp, g_xb);
        { uint8_t gd[64]; if(gd_read(g,gd)==0){ uint16_t fc=rd16(gd+12); wr16(gd+12,fc+1); gd_write(g,gd); } }
        sb_adjust_free_blocks(+1);
    }
}
static uint32_t alloc_inode(int is_dir){
    for(uint32_t g=0; g<g_num_groups; g++){
        uint32_t bmp = gd_inode_bitmap(g);
        if(!bmp) continue;
        if(blk_read(bmp, g_xb)!=0) continue;
        uint32_t nbits = g_inodes_per_group;
        for(uint32_t i=0;i<nbits;i++){
            if(!(g_xb[i>>3] & (1u<<(i&7)))){
                g_xb[i>>3] |= (1u<<(i&7));
                if(blk_write(bmp, g_xb)!=0) return 0;
                { uint8_t gd[64]; if(gd_read(g,gd)==0){ uint16_t fc=rd16(gd+14); if(fc) wr16(gd+14,fc-1); if(is_dir){ uint16_t ud=rd16(gd+16); wr16(gd+16,ud+1);} gd_write(g,gd); } }
                sb_adjust_free_inodes(-1);
                return g*g_inodes_per_group + i + 1;
            }
        }
    }
    return 0;
}
static void free_inode(uint32_t ino, int is_dir){
    if(ino==0) return;
    uint32_t g=(ino-1)/g_inodes_per_group, i=(ino-1)%g_inodes_per_group;
    uint32_t bmp=gd_inode_bitmap(g); if(!bmp) return;
    if(blk_read(bmp,g_xb)!=0) return;
    if(g_xb[i>>3] & (1u<<(i&7))){
        g_xb[i>>3] &= ~(1u<<(i&7)); blk_write(bmp,g_xb);
        { uint8_t gd[64]; if(gd_read(g,gd)==0){ uint16_t fc=rd16(gd+14); wr16(gd+14,fc+1); if(is_dir){ uint16_t ud=rd16(gd+16); if(ud) wr16(gd+16,ud-1);} gd_write(g,gd); } }
        sb_adjust_free_inodes(+1);
    }
}

/* ── Block mapping: file block index -> physical block ───────────────────── */
/* ext4 extent tree (read-only walk; depth supported). inode buffer in `inode`. */
static uint32_t extent_bmap(const uint8_t* node /*60-byte i_block or ext block*/, uint32_t fb, int top){
    const uint8_t* hdr = node;
    if(rd16(hdr+0)!=EXT4_EXTENT_MAGIC) return 0;
    uint16_t entries = rd16(hdr+2);
    uint16_t depth   = rd16(hdr+6);
    const uint8_t* e = node + 12;
    if(depth==0){
        for(uint16_t i=0;i<entries;i++, e+=12){
            uint32_t first = rd32(e+0);
            uint16_t len   = rd16(e+4);
            uint32_t start_lo = rd32(e+8);
            if(fb>=first && fb < first+len){
                return start_lo + (fb-first);
            }
        }
        return 0;
    }
    /* interior node: find the index entry covering fb */
    uint32_t child = 0;
    for(uint16_t i=0;i<entries;i++, e+=12){
        uint32_t first = rd32(e+0);
        uint32_t leaf_lo = rd32(e+4);
        if(i+1<entries){
            uint32_t next_first = rd32(e+12);
            if(fb>=first && fb<next_first){ child=leaf_lo; break; }
        } else {
            if(fb>=first){ child=leaf_lo; }
        }
    }
    (void)top;
    if(!child) return 0;
    if(blk_read(child, g_xb)!=0) return 0;
    return extent_bmap(g_xb, fb, 0);
}

static uint32_t bmap_read(const uint8_t* inode, uint32_t fb){
    uint32_t flags = rd32(inode+32);
    if(flags & EXT4_EXTENTS_FL){
        /* i_block (offset 40) holds the extent header + root. Copy 60 bytes. */
        return extent_bmap(inode+40, fb, 1);
    }
    uint32_t ppb = g_blk_size/4;
    if(fb<12) return rd32(inode+40+fb*4);
    fb-=12;
    if(fb<ppb){
        uint32_t ind = rd32(inode+40+12*4); if(!ind) return 0;
        if(blk_read(ind, g_xb)!=0) return 0;
        return rd32(g_xb+fb*4);
    }
    fb-=ppb;
    if(fb<ppb*ppb){
        uint32_t dind = rd32(inode+40+13*4); if(!dind) return 0;
        if(blk_read(dind, g_xb)!=0) return 0;
        uint32_t i1 = rd32(g_xb+(fb/ppb)*4); if(!i1) return 0;
        if(blk_read(i1, g_xb)!=0) return 0;
        return rd32(g_xb+(fb%ppb)*4);
    }
    return 0; /* triple indirect not supported */
}

/* Allocating bmap for WRITE (ext2 indirect: direct + single indirect). Returns
 * physical block for file-block fb, allocating along the way; updates `inode`
 * (i_block pointers) in memory. Returns 0 on failure. */
static uint32_t bmap_alloc(uint8_t* inode, uint32_t fb){
    uint32_t flags = rd32(inode+32);
    if(flags & EXT4_EXTENTS_FL) return 0; /* extent write path handled separately */
    uint32_t ppb = g_blk_size/4;
    if(fb<12){
        uint32_t b = rd32(inode+40+fb*4);
        if(!b){ b=alloc_block(); if(!b) return 0; wr32(inode+40+fb*4, b); }
        return b;
    }
    fb-=12;
    if(fb<ppb){
        uint32_t ind = rd32(inode+40+12*4);
        if(!ind){ ind=alloc_block(); if(!ind) return 0; wr32(inode+40+12*4, ind); }
        if(blk_read(ind, g_xb)!=0) return 0;
        uint32_t b = rd32(g_xb+fb*4);
        if(!b){ b=alloc_block(); if(!b) return 0; wr32(g_xb+fb*4, b); if(blk_write(ind,g_xb)!=0) return 0; }
        return b;
    }
    return 0; /* beyond single indirect not supported for writes */
}

/* ── File read ───────────────────────────────────────────────────────────── */
static uint32_t inode_size_lo(const uint8_t* inode){ return rd32(inode+4); }

static int file_read(uint32_t ino, size_t offset, void* buf, size_t len){
    uint8_t inode[256];
    if(inode_read(ino, inode)!=0) return -1;
    uint32_t fsize = inode_size_lo(inode);
    if(offset>=fsize) return 0;
    if(offset+len>fsize) len=fsize-offset;
    uint8_t* out=(uint8_t*)buf; size_t done=0;
    while(done<len){
        uint32_t fb = (uint32_t)((offset+done)/g_blk_size);
        uint32_t bo = (uint32_t)((offset+done)%g_blk_size);
        uint32_t pb = bmap_read(inode, fb);
        uint32_t chunk = g_blk_size - bo; if(chunk>len-done) chunk=(uint32_t)(len-done);
        if(pb==0){ for(uint32_t i=0;i<chunk;i++) out[done+i]=0; } /* sparse hole */
        else { if(blk_read(pb, g_b)!=0) return -1; for(uint32_t i=0;i<chunk;i++) out[done+i]=g_b[bo+i]; }
        done+=chunk;
    }
    return (int)done;
}

/* Append (file block fb -> physical pb) to an inline (depth-0) extent tree in
 * the inode. Coalesces with the last extent when contiguous; up to 4 inline
 * extents. Returns 0, or -1 if the inline tree is full / not depth-0. */
static int extent_append(uint8_t* inode, uint32_t fb, uint32_t pb){
    uint8_t* hdr = inode+40;
    if(rd16(hdr+0)!=EXT4_EXTENT_MAGIC) return -1;
    if(rd16(hdr+6)!=0) return -1;            /* only inline depth-0 supported */
    uint16_t entries = rd16(hdr+2);
    uint16_t maxent  = rd16(hdr+4);
    uint8_t* base = inode+40+12;
    if(entries>0){
        uint8_t* last = base + (entries-1)*12;
        uint32_t lfirst = rd32(last+0);
        uint16_t llen   = rd16(last+4);
        uint32_t llo    = rd32(last+8);
        if(fb==lfirst+llen && pb==llo+llen && llen<32768){ wr16(last+4,(uint16_t)(llen+1)); return 0; }
    }
    if(entries>=maxent) return -1;
    uint8_t* e = base + entries*12;
    wr32(e+0, fb); wr16(e+4, 1); wr16(e+6, 0); wr32(e+8, pb);
    wr16(hdr+2, (uint16_t)(entries+1));
    return 0;
}

/* Map file block fb to a physical block for WRITE, allocating as needed.
 * Handles both extent (ext4) and indirect (ext2) inodes. Returns 0 on fail. */
static uint32_t block_for_write(uint8_t* inode, uint32_t fb){
    if(rd32(inode+32) & EXT4_EXTENTS_FL){
        uint32_t pb = extent_bmap(inode+40, fb, 1);
        if(pb) return pb;
        pb = alloc_block(); if(!pb) return 0;
        if(extent_append(inode, fb, pb)!=0) return 0;
        return pb;
    }
    return bmap_alloc(inode, fb);
}

/* ── File write (grow); supports ext2 indirect and ext4 inline extents ───── */
static int file_write(uint32_t ino, size_t offset, const void* buf, size_t len){
    uint8_t inode[256];
    if(inode_read(ino, inode)!=0) return -1;
    uint32_t fsize = inode_size_lo(inode);
    const uint8_t* in=(const uint8_t*)buf; size_t done=0;
    while(done<len){
        uint32_t fb=(uint32_t)((offset+done)/g_blk_size);
        uint32_t bo=(uint32_t)((offset+done)%g_blk_size);
        uint32_t pb=block_for_write(inode, fb);
        if(!pb) return -1;
        uint32_t chunk=g_blk_size-bo; if(chunk>len-done) chunk=(uint32_t)(len-done);
        if(bo!=0 || chunk!=g_blk_size){ if(blk_read(pb,g_b)!=0) return -1; }
        for(uint32_t i=0;i<chunk;i++) g_b[bo+i]=in[done+i];
        if(blk_write_direct(pb,g_b)!=0) return -1;   /* [M27b] data: ordered (direct, pre-commit) */
        done+=chunk;
    }
    uint32_t newsize = (uint32_t)(offset+len); if(newsize<fsize) newsize=fsize;
    wr32(inode+4, newsize);
    /* i_blocks in 512-byte units (data blocks only; approximate) */
    uint32_t nblk = (newsize + g_blk_size -1)/g_blk_size;
    wr32(inode+28, nblk*g_sec_per_blk);
    if(inode_write(ino, inode)!=0) return -1;
    return (int)len;
}

/* ── Directory operations ────────────────────────────────────────────────── */
static int name_eq(const uint8_t* a, const char* b, size_t n){ for(size_t i=0;i<n;i++) if(a[i]!=(uint8_t)b[i]) return 0; return 1; }
static uint32_t round4(uint32_t x){ return (x+3)&~3u; }

/* Find `name` in directory inode; returns child inode number or 0. */
static uint32_t dir_lookup(uint32_t dir_ino, const char* name, size_t nlen){
    uint8_t inode[256];
    if(inode_read(dir_ino, inode)!=0) return 0;
    uint32_t dsize = inode_size_lo(inode);
    uint32_t nblocks = (dsize + g_blk_size -1)/g_blk_size;
    for(uint32_t fb=0; fb<nblocks; fb++){
        uint32_t pb = bmap_read(inode, fb); if(!pb) continue;
        if(blk_read(pb, g_b)!=0) return 0;
        uint32_t off=0;
        while(off + 8 <= g_blk_size){
            uint32_t e_ino = rd32(g_b+off);
            uint16_t rec   = rd16(g_b+off+4);
            uint8_t  nl    = g_b[off+6];
            if(rec<8) break;
            if(e_ino!=0 && nl==nlen && name_eq(g_b+off+8, name, nlen)) return e_ino;
            off += rec;
        }
    }
    return 0;
}

/* Add an entry (name->child, ftype) to directory dir_ino. */
static int dir_add(uint32_t dir_ino, const char* name, size_t nlen, uint32_t child, uint8_t ftype){
    uint8_t inode[256];
    if(inode_read(dir_ino, inode)!=0) return -1;
    uint32_t dsize = inode_size_lo(inode);
    uint32_t nblocks = (dsize + g_blk_size -1)/g_blk_size;
    uint32_t need = round4(8 + (uint32_t)nlen);
    for(uint32_t fb=0; fb<nblocks; fb++){
        uint32_t pb = bmap_read(inode, fb); if(!pb) continue;
        if(blk_read(pb, g_b)!=0) return -1;
        uint32_t off=0;
        while(off + 8 <= g_blk_size){
            uint32_t e_ino = rd32(g_b+off);
            uint16_t rec   = rd16(g_b+off+4);
            uint8_t  nl    = g_b[off+6];
            if(rec<8) break;
            uint32_t used = (e_ino==0) ? 0 : round4(8+nl);
            if(rec - used >= need){
                uint32_t new_off = off + used;
                if(used>0) wr16(g_b+off+4, (uint16_t)used);
                wr32(g_b+new_off+0, child);
                wr16(g_b+new_off+4, (uint16_t)(rec-used));
                g_b[new_off+6]=(uint8_t)nlen;
                g_b[new_off+7]=ftype;
                for(size_t i=0;i<nlen;i++) g_b[new_off+8+i]=(uint8_t)name[i];
                return blk_write(pb, g_b);
            }
            off += rec;
        }
    }
    /* Append a fresh directory block. */
    uint32_t fb = nblocks;
    uint32_t pb = bmap_alloc(inode, fb); if(!pb) return -1;
    mem_zero(g_b, g_blk_size);
    wr32(g_b+0, child);
    wr16(g_b+4, (uint16_t)g_blk_size);
    g_b[6]=(uint8_t)nlen; g_b[7]=ftype;
    for(size_t i=0;i<nlen;i++) g_b[8+i]=(uint8_t)name[i];
    if(blk_write(pb, g_b)!=0) return -1;
    uint32_t newsize = (fb+1)*g_blk_size;
    wr32(inode+4, newsize);
    wr32(inode+28, ((newsize+g_blk_size-1)/g_blk_size)*g_sec_per_blk);
    return inode_write(dir_ino, inode);
}

/* Remove entry `name` from directory; coalesces rec_len into the previous. */
static int dir_remove_entry(uint32_t dir_ino, const char* name, size_t nlen){
    uint8_t inode[256];
    if(inode_read(dir_ino, inode)!=0) return -1;
    uint32_t dsize = inode_size_lo(inode);
    uint32_t nblocks = (dsize + g_blk_size -1)/g_blk_size;
    for(uint32_t fb=0; fb<nblocks; fb++){
        uint32_t pb = bmap_read(inode, fb); if(!pb) continue;
        if(blk_read(pb, g_b)!=0) return -1;
        uint32_t off=0, prev=0xFFFFFFFF;
        while(off + 8 <= g_blk_size){
            uint32_t e_ino = rd32(g_b+off);
            uint16_t rec   = rd16(g_b+off+4);
            uint8_t  nl    = g_b[off+6];
            if(rec<8) break;
            if(e_ino!=0 && nl==nlen && name_eq(g_b+off+8, name, nlen)){
                if(prev!=0xFFFFFFFF){ uint16_t prec=rd16(g_b+prev+4); wr16(g_b+prev+4,(uint16_t)(prec+rec)); }
                else { wr32(g_b+off+0, 0); } /* first in block: just clear inode */
                return blk_write(pb, g_b);
            }
            prev=off; off+=rec;
        }
    }
    return -1;
}

/* ── Path resolution ─────────────────────────────────────────────────────── */
static uint32_t path_to_ino(const char* path, int* is_dir, uint32_t* size_out){
    if(!path) return 0;
    if(path[0]=='/') path++;
    uint32_t cur = EXT2_ROOT_INO;
    if(path[0]==0){ if(is_dir)*is_dir=1; if(size_out)*size_out=0; return cur; }
    char comp[128];
    uint8_t inode[256];
    while(*path){
        size_t k=0; while(*path && *path!='/' && k<sizeof(comp)-1) comp[k++]=*path++;
        comp[k]=0; while(*path=='/') path++;
        if(k==0) continue;
        uint32_t child = dir_lookup(cur, comp, k);
        if(!child) return 0;
        cur = child;
        if(*path){ /* must be a dir to continue */
            if(inode_read(cur, inode)!=0) return 0;
            if((rd16(inode+0)&S_IFMT)!=S_IFDIR) return 0;
        }
    }
    if(inode_read(cur, inode)!=0) return 0;
    if(is_dir)*is_dir = ((rd16(inode+0)&S_IFMT)==S_IFDIR);
    if(size_out)*size_out = inode_size_lo(inode);
    return cur;
}

/* Split path into parent inode + final component. */
static int split_parent(const char* path, uint32_t* parent_ino, char* comp, size_t csz){
    if(!path || path[0]!='/') return -1;
    const char* p=path; const char* ls=path;
    while(*p){ if(*p=='/') ls=p; p++; }
    const char* child=ls+1; if(*child==0) return -1;
    size_t k=0; while(child[k] && k<csz-1){ comp[k]=child[k]; k++; } comp[k]=0;
    char parent[256]; size_t plen=(size_t)(ls-path);
    if(plen==0){ parent[0]='/'; parent[1]=0; }
    else { if(plen>sizeof(parent)-1) plen=sizeof(parent)-1; for(size_t i=0;i<plen;i++) parent[i]=path[i]; parent[plen]=0; }
    int isd; uint32_t pino=path_to_ino(parent,&isd,NULL);
    if(!pino || !isd) return -1;
    *parent_ino=pino; return 0;
}

/* ── VFS inode pool ──────────────────────────────────────────────────────── */
typedef struct { uint32_t ino; uint32_t size; int is_dir; } e2_node_t;
#define E2_INODES 32
static vfs_inode_t g_vino[E2_INODES];
static e2_node_t   g_vnode[E2_INODES];
static int         g_vused;

static vfs_inode_t* vino_alloc(const char* path){
    for(int i=0;i<g_vused;i++){ const char* a=g_vino[i].path; size_t k=0; int eq=1; while(a[k]||path[k]){ if(a[k]!=path[k]){eq=0;break;} k++; } if(eq) return &g_vino[i]; }
    int idx=(g_vused<E2_INODES)?g_vused++:(g_vused-1);
    vfs_inode_t* ino=&g_vino[idx];
    size_t k=0; for(; path[k] && k<sizeof(ino->path)-1; k++) ino->path[k]=path[k]; ino->path[k]=0;
    ino->fs_data=&g_vnode[idx];
    return ino;
}

/* [M26] Read a symlink target. Fast symlinks (target <= 60 bytes) store the
 * target inline in the i_block area (offset 40); larger ones use data block 0. */
static int e2_readlink(const char* path, char* buf, size_t len){
    if(!g_mounted||!path||!buf||len==0) return -1;
    int isd; uint32_t ino=path_to_ino(path,&isd,NULL);
    if(!ino) return -1;
    uint8_t in[256]; if(inode_read(ino,in)!=0) return -1;
    if((rd16(in+0)&S_IFMT)!=S_IFLNK) return -1;          /* not a symlink */
    uint32_t sz=inode_size_lo(in); if(sz==0) return -1;
    uint32_t n = (sz < len-1) ? sz : (uint32_t)(len-1);
    if(sz<=60){ for(uint32_t i=0;i<n;i++) buf[i]=(char)in[40+i]; }
    else {
        uint32_t pb=bmap_read(in,0); if(!pb) return -1;
        if(blk_read(pb,g_b)!=0) return -1;
        for(uint32_t i=0;i<n;i++) buf[i]=(char)g_b[i];
    }
    buf[n]=0; return (int)n;
}

/* [M26] Create a fast symlink (target inline in i_block; supports up to 60-byte
 * targets, which covers virtually all real symlinks). */
static int e2_symlink(const char* target, const char* linkpath){
    if(!g_mounted||!target||!linkpath) return -1;
    if(path_to_ino(linkpath,NULL,NULL)) return -1;       /* exists */
    size_t tlen=0; while(target[tlen]) tlen++;
    if(tlen==0 || tlen>60) return -1;                    /* fast symlink only */
    uint32_t parent; char comp[128];
    if(split_parent(linkpath,&parent,comp,sizeof(comp))!=0) return -1;
    uint32_t nino=alloc_inode(0); if(!nino) return -1;
    uint8_t inode[256]; mem_zero(inode, g_inode_size);
    wr16(inode+0, (uint16_t)(S_IFLNK | 0777));           /* mode */
    wr32(inode+4, (uint32_t)tlen);                        /* i_size = target length */
    wr16(inode+26, 1);                                    /* links_count */
    for(size_t i=0;i<tlen;i++) inode[40+i]=(uint8_t)target[i];  /* inline target */
    if(inode_write(nino, inode)!=0) return -1;
    if(dir_add(parent, comp, e2_strlen(comp), nino, DT_LNK)!=0) return -1;
    return 0;
}

/* ── VFS ops ─────────────────────────────────────────────────────────────── */
static vfs_inode_t* e2_lookup(const char* path){
    if(!g_mounted) return NULL;
    int isd; uint32_t sz;
    uint32_t ino=path_to_ino(path,&isd,&sz);
    if(!ino) return NULL;
    vfs_inode_t* v=vino_alloc(path&&path[0]?path:"/"); if(!v) return NULL;
    v->type=isd?VFS_NODE_DIR:VFS_NODE_FILE; v->size=sz;
    // [M26] Populate POSIX metadata from the on-disk inode (standard ext2 layout:
    // i_mode@0, i_uid@2, i_atime@8, i_ctime@12, i_mtime@16, i_gid@24, i_links@26).
    uint8_t in[256];
    if(inode_read(ino, in)==0){
        uint16_t m=rd16(in+0);
        v->mode=m;
        if((m&S_IFMT)==0xA000) v->type=VFS_NODE_SYMLINK;   // S_IFLNK
        v->uid=rd16(in+2); v->gid=rd16(in+24);
        v->atime=rd32(in+8); v->ctime=rd32(in+12); v->mtime=rd32(in+16);
        v->nlink=rd16(in+26);
    }
    e2_node_t* nd=(e2_node_t*)v->fs_data; nd->ino=ino; nd->size=sz; nd->is_dir=isd;
    return v;
}

static int e2_readdir(const char* dir_path, vfs_iter_cb cb, void* user){
    if(!g_mounted || !cb) return -1;
    int isd; uint32_t dino=path_to_ino(dir_path,&isd,NULL);
    if(!dino || !isd) return -1;
    uint8_t inode[256]; if(inode_read(dino, inode)!=0) return -1;
    uint32_t dsize=inode_size_lo(inode);
    uint32_t nblocks=(dsize+g_blk_size-1)/g_blk_size;
    char base[256]; size_t bl=0; if(dir_path){ for(; dir_path[bl]&&bl<sizeof(base)-1; bl++) base[bl]=dir_path[bl]; } base[bl]=0;
    if(bl==1&&base[0]=='/'){ bl=0; base[0]=0; }
    for(uint32_t fb=0; fb<nblocks; fb++){
        uint32_t pb=bmap_read(inode,fb); if(!pb) continue;
        if(blk_read(pb, g_b)!=0) return -1;
        uint32_t off=0;
        while(off+8<=g_blk_size){
            uint32_t e_ino=rd32(g_b+off); uint16_t rec=rd16(g_b+off+4); uint8_t nl=g_b[off+6]; uint8_t ft=g_b[off+7];
            if(rec<8) break;
            if(e_ino!=0 && nl>0){
                /* skip "." and ".." */
                int dot = (nl==1 && g_b[off+8]=='.') || (nl==2 && g_b[off+8]=='.' && g_b[off+9]=='.');
                if(!dot){
                    vfs_inode_t child; size_t k=0;
                    for(size_t i=0;i<bl && k<sizeof(child.path)-1;i++) child.path[k++]=base[i];
                    if(k<sizeof(child.path)-1) child.path[k++]='/';
                    for(uint8_t i=0;i<nl && k<sizeof(child.path)-1;i++) child.path[k++]=(char)g_b[off+8+i];
                    child.path[k]=0;
                    child.type=(ft==DT_DIR)?VFS_NODE_DIR:VFS_NODE_FILE;
                    child.size=0; child.fs_data=NULL; child.ops=NULL;
                    cb(&child, user);
                }
            }
            off+=rec;
        }
    }
    return 0;
}

static int e2_read(vfs_inode_t* v, size_t off, void* buf, size_t len){
    if(!g_mounted||!v||v->type!=VFS_NODE_FILE) return -1;
    return file_read(((e2_node_t*)v->fs_data)->ino, off, buf, len);
}

/* [M26] chmod/chown/utimes: patch the on-disk inode metadata in place. The mode
 * change preserves the S_IFMT type bits (chmod sets only the low 12 perm bits).
 * e2fsck-clean: we only touch fields, never the block map. */
static int e2_setattr(const char* path, const vfs_attr_t* a, unsigned valid){
    if(!g_mounted || !path || !a) return -1;
    int isd; uint32_t ino=path_to_ino(path,&isd,NULL);
    if(!ino) return -1;
    uint8_t in[256]; if(inode_read(ino,in)!=0) return -1;
    if(valid&VFS_ATTR_MODE){ uint16_t old=rd16(in+0); wr16(in+0,(uint16_t)((old&S_IFMT)|(a->mode&0x0FFF))); }
    if(valid&VFS_ATTR_UID)   wr16(in+2,  (uint16_t)a->uid);
    if(valid&VFS_ATTR_GID)   wr16(in+24, (uint16_t)a->gid);
    if(valid&VFS_ATTR_ATIME) wr32(in+8,  (uint32_t)a->atime);
    if(valid&VFS_ATTR_MTIME) wr32(in+16, (uint32_t)a->mtime);
    return inode_write(ino,in);
}
static int e2_write(vfs_inode_t* v, size_t off, const void* data, size_t len){
    if(!g_mounted||!v||v->type!=VFS_NODE_FILE) return -1;
    e2_node_t* nd=(e2_node_t*)v->fs_data;
    int r=file_write(nd->ino, off, data, len);
    if(r>=0){ uint8_t in[256]; if(inode_read(nd->ino,in)==0){ nd->size=inode_size_lo(in); v->size=nd->size; } }
    return r;
}

/* Create a regular file (optionally with initial data). */
static int e2_create(const char* path, const void* initial, size_t size){
    if(!g_mounted||!path) return -1;
    if(path_to_ino(path,NULL,NULL)) return -1; /* exists */
    uint32_t parent; char comp[128];
    if(split_parent(path,&parent,comp,sizeof(comp))!=0) return -1;
    uint32_t nino=alloc_inode(0); if(!nino) return -1;
    uint8_t inode[256]; mem_zero(inode, g_inode_size);
    wr16(inode+0, S_IFREG | 0644); /* mode */
    wr16(inode+2, 0);              /* uid */
    wr32(inode+4, 0);              /* size */
    wr16(inode+26, 1);             /* links_count */
    if(g_use_extents){
        wr32(inode+32, EXT4_EXTENTS_FL);         /* i_flags */
        wr16(inode+40, EXT4_EXTENT_MAGIC);       /* eh_magic */
        wr16(inode+42, 0);                       /* eh_entries */
        wr16(inode+44, 4);                       /* eh_max (inline) */
        wr16(inode+46, 0);                       /* eh_depth */
    }
    if(inode_write(nino, inode)!=0) return -1;
    if(dir_add(parent, comp, e2_strlen(comp), nino, DT_REG)!=0) return -1;
    if(initial && size>0){ if(file_write(nino, 0, initial, size)<0) return -1; }
    return 0;
}

static int e2_mkdir(const char* path){
    if(!g_mounted||!path) return -1;
    if(path_to_ino(path,NULL,NULL)) return -1;
    uint32_t parent; char comp[128];
    if(split_parent(path,&parent,comp,sizeof(comp))!=0) return -1;
    uint32_t nino=alloc_inode(1); if(!nino) return -1;
    uint32_t db=alloc_block(); if(!db) return -1;
    /* build "." and ".." */
    mem_zero(g_b, g_blk_size);
    wr32(g_b+0, nino); wr16(g_b+4, round4(8+1)); g_b[6]=1; g_b[7]=DT_DIR; g_b[8]='.';
    uint32_t o2=round4(8+1);
    wr32(g_b+o2+0, parent); wr16(g_b+o2+4, (uint16_t)(g_blk_size-o2)); g_b[o2+6]=2; g_b[o2+7]=DT_DIR; g_b[o2+8]='.'; g_b[o2+9]='.';
    if(blk_write(db, g_b)!=0) return -1;
    uint8_t inode[256]; mem_zero(inode, g_inode_size);
    wr16(inode+0, S_IFDIR | 0755);
    wr32(inode+4, g_blk_size);
    wr16(inode+26, 2); /* . and parent link */
    wr32(inode+28, g_sec_per_blk);
    wr32(inode+40, db); /* i_block[0] */
    if(inode_write(nino, inode)!=0) return -1;
    if(dir_add(parent, comp, e2_strlen(comp), nino, DT_DIR)!=0) return -1;
    /* bump parent link count */
    uint8_t pin[256]; if(inode_read(parent,pin)==0){ wr16(pin+26, rd16(pin+26)+1); inode_write(parent,pin); }
    return 0;
}

static int e2_remove(const char* path){
    if(!g_mounted||!path) return -1;
    int isd; uint32_t sz; uint32_t ino=path_to_ino(path,&isd,&sz);
    if(!ino) return -1;
    uint32_t parent; char comp[128];
    if(split_parent(path,&parent,comp,sizeof(comp))!=0) return -1;
    /* free data blocks */
    uint8_t inode[256];
    if(inode_read(ino,inode)==0){
        if(rd32(inode+32)&EXT4_EXTENTS_FL){
            uint8_t* hdr=inode+40;
            if(rd16(hdr+0)==EXT4_EXTENT_MAGIC && rd16(hdr+6)==0){
                uint16_t entries=rd16(hdr+2); uint8_t* e=inode+40+12;
                for(uint16_t k=0;k<entries;k++,e+=12){ uint32_t lo=rd32(e+8); uint16_t ln=rd16(e+4); for(uint16_t b=0;b<ln;b++) free_block(lo+b); }
            }
        } else {
            uint32_t fsize=inode_size_lo(inode);
            uint32_t nblocks=(fsize+g_blk_size-1)/g_blk_size;
            for(uint32_t fb=0;fb<nblocks;fb++){ uint32_t pb=bmap_read(inode,fb); if(pb) free_block(pb); }
            uint32_t ind=rd32(inode+40+12*4); if(ind) free_block(ind);
        }
    }
    if(dir_remove_entry(parent, comp, e2_strlen(comp))!=0) return -1;
    free_inode(ino, isd);
    return 0;
}

static int e2_truncate(const char* path, size_t new_size){
    if(!g_mounted||!path) return -1;
    int isd; uint32_t sz; uint32_t ino=path_to_ino(path,&isd,&sz);
    if(!ino||isd) return -1;
    uint8_t inode[256]; if(inode_read(ino,inode)!=0) return -1;
    if(rd32(inode+32)&EXT4_EXTENTS_FL) return -1;
    uint32_t old=inode_size_lo(inode);
    if(new_size<old){
        uint32_t keep=(uint32_t)((new_size+g_blk_size-1)/g_blk_size);
        uint32_t had=(old+g_blk_size-1)/g_blk_size;
        for(uint32_t fb=keep;fb<had;fb++){ uint32_t pb=bmap_read(inode,fb); if(pb){ free_block(pb); /* clear pointer for direct */ if(fb<12) wr32(inode+40+fb*4,0); } }
        wr32(inode+4,(uint32_t)new_size);
        wr32(inode+28, keep*g_sec_per_blk);
        return inode_write(ino, inode);
    } else if(new_size>old){
        /* extend by writing zero at the new end via file_write of 0 bytes is no-op;
         * just bump size and let holes read as zero */
        wr32(inode+4,(uint32_t)new_size);
        return inode_write(ino, inode);
    }
    return 0;
}

static int e2_rename(const char* oldp, const char* newp){
    if(!g_mounted||!oldp||!newp) return -1;
    int isd; uint32_t ino=path_to_ino(oldp,&isd,NULL);
    if(!ino) return -1;
    if(path_to_ino(newp,NULL,NULL)) return -1;
    uint32_t np; char nc[128];
    if(split_parent(newp,&np,nc,sizeof(nc))!=0) return -1;
    uint32_t op; char oc[128];
    if(split_parent(oldp,&op,oc,sizeof(oc))!=0) return -1;
    if(dir_add(np, nc, e2_strlen(nc), ino, isd?DT_DIR:DT_REG)!=0) return -1;
    return dir_remove_entry(op, oc, e2_strlen(oc));
}

/* [M27b] Transaction wrappers: each mutating op runs as one journalled metadata
 * transaction (commit-then-checkpoint on success, atomic rollback on failure).
 * Read-only ops (lookup/readdir/read/readlink) are not wrapped. */
static int e2j_create(const char* p, const void* d, size_t s){ jt_begin(); int r=e2_create(p,d,s); return jt_finish(r, r==0); }
static int e2j_mkdir(const char* p){ jt_begin(); int r=e2_mkdir(p); return jt_finish(r, r==0); }
static int e2j_remove(const char* p){ jt_begin(); int r=e2_remove(p); return jt_finish(r, r==0); }
static int e2j_rename(const char* o,const char* n){ jt_begin(); int r=e2_rename(o,n); return jt_finish(r, r==0); }
static int e2j_truncate(const char* p, size_t s){ jt_begin(); int r=e2_truncate(p,s); return jt_finish(r, r==0); }
static int e2j_setattr(const char* p, const vfs_attr_t* a, unsigned v){ jt_begin(); int r=e2_setattr(p,a,v); return jt_finish(r, r==0); }
static int e2j_symlink(const char* t, const char* l){ jt_begin(); int r=e2_symlink(t,l); return jt_finish(r, r==0); }
static int e2j_write(vfs_inode_t* v, size_t off, const void* d, size_t l){ jt_begin(); int r=e2_write(v,off,d,l); return jt_finish(r, r>=0); }

static vfs_fs_ops_t ext2_ops = {
    .lookup=e2_lookup, .readdir=e2_readdir, .read=e2_read, .write=e2j_write,
    .create=e2j_create, .mkdir=e2j_mkdir, .remove=e2j_remove, .rename=e2j_rename,
    .truncate=e2j_truncate, .setattr=e2j_setattr,
    .readlink=e2_readlink, .symlink=e2j_symlink
};

/* ── Mount ───────────────────────────────────────────────────────────────── */
/* Read + validate the ext2 superblock and populate the global FS state for dev.
 * Returns 0 on a valid ext2 volume, -1 otherwise. Sets g_mounted=1 on success. */
static int ext2_read_super(block_dev_t* dev){
    if(!dev) return -1;
    g_dev=dev;
    /* Superblock is at byte offset 1024. Read enough to cover it. */
    uint8_t sbsec[1024];
    if(block_read(dev, 2, sbsec, 2)!=2) return -1; /* sectors 2..3 -> bytes 1024..2047 */
    uint8_t* sb=sbsec; /* sbsec already starts at byte 1024 */
    if(rd16(sb+56)!=EXT2_MAGIC) return -1;
    g_total_inodes      = rd32(sb+0);
    g_total_blocks      = rd32(sb+4);
    uint32_t log_bs     = rd32(sb+24);
    g_blk_size          = 1024u << log_bs;
    if(g_blk_size>MAX_BLK) return -1;
    g_blocks_per_group  = rd32(sb+32);
    g_inodes_per_group  = rd32(sb+40);
    g_first_data_block  = rd32(sb+20);
    uint32_t rev        = rd32(sb+76);
    g_inode_size        = (rev>=1) ? rd16(sb+88) : 128;
    if(g_inode_size==0||g_inode_size>256) g_inode_size=128;
    /* feature_incompat at 96; 64bit feature (0x80) implies 64-byte group desc */
    uint32_t feat_incompat = rd32(sb+96);
    g_desc_size = (feat_incompat & 0x80) ? rd16(sb+254) : 32;   /* INCOMPAT_64BIT */
    if(g_desc_size==0) g_desc_size=32;
    g_use_extents = (feat_incompat & 0x40) ? 1 : 0;             /* INCOMPAT_EXTENTS */
    g_sec_per_blk = g_blk_size/SECSZ;
    g_num_groups  = (g_total_blocks - g_first_data_block + g_blocks_per_group -1)/g_blocks_per_group;
    g_bgdt_block  = g_first_data_block + 1;
    g_vused=0;
    g_mounted=1;
    return 0;
}

/* ── [M27a] JBD2 journal recovery (replay) ──────────────────────────────────
 * Read-side crash recovery: if an ext3/ext4 volume carries a non-empty journal
 * (left dirty by a crash on another OS), replay the committed-but-not-checkpointed
 * transactions before mounting, so we never read stale in-place metadata. JBD2 is
 * BIG-ENDIAN on disk. We implement the standard 3-pass recovery (SCAN to find the
 * last committed transaction, REVOKE to build the revoke table, REPLAY to write
 * the journalled blocks honouring revokes), then mark the journal/superblock clean
 * so we don't recover twice. Checksums are not verified (we replay regardless),
 * which is safe for recovery and keeps us compatible with csum-enabled journals. */
#define JBD2_MAGIC            0xc03b3998u
#define JBD2_DESCRIPTOR_BLOCK 1
#define JBD2_COMMIT_BLOCK     2
#define JBD2_REVOKE_BLOCK     5
#define JBD2_FLAG_ESCAPE      0x1
#define JBD2_FLAG_SAME_UUID   0x2
#define JBD2_FLAG_LAST_TAG    0x8
#define JBD2_INCOMPAT_64BIT   0x2
#define JBD2_INCOMPAT_CSUM_V3 0x10

static uint8_t g_jdesc[MAX_BLK];     /* current descriptor/revoke block        */
static uint8_t g_jdata[MAX_BLK];     /* current journalled data block          */
static uint8_t g_jin[256];           /* journal inode                          */
static struct { uint64_t blk; uint32_t seq; } g_jrev[2048];
static int     g_njrev;

static uint32_t be32(const uint8_t* p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint16_t be16(const uint8_t* p){ return (uint16_t)((p[0]<<8)|p[1]); }

/* Read journal logical block `lb` (via the journal inode's block map) into buf. */
static int jread(uint32_t lb, uint8_t* buf){
    uint32_t pb = bmap_read(g_jin, lb);   /* note: clobbers g_xb */
    if(!pb) return -1;
    return blk_read(pb, buf);
}

static int jrev_test(uint64_t blk, uint32_t seq){          /* revoked at >= seq? */
    for(int i=0;i<g_njrev;i++) if(g_jrev[i].blk==blk && g_jrev[i].seq>=seq) return 1;
    return 0;
}

/* One recovery pass. phase: 0=SCAN, 1=REVOKE, 2=REPLAY. Returns the number of
 * committed transactions seen (the SCAN result is used to bound REVOKE/REPLAY). */
enum { JP_SCAN, JP_REVOKE, JP_REPLAY };
static uint32_t jbd2_pass(int phase, uint32_t first, uint32_t maxlen,
                          uint32_t incompat, uint32_t start, uint32_t start_seq,
                          uint32_t end_seq){
    uint32_t next = start, seq = start_seq, committed = 0;
    int tag3 = (incompat & JBD2_INCOMPAT_CSUM_V3) ? 1 : 0;
    int b64  = (incompat & JBD2_INCOMPAT_64BIT) ? 1 : 0;
    int tagsz = tag3 ? 16 : (b64 ? 12 : 8);
    for(;;){
        if(phase!=JP_SCAN && seq>=end_seq) break;          /* stop at uncommitted tail */
        if(jread(next, g_jdesc)!=0) break;
        if(be32(g_jdesc+0)!=JBD2_MAGIC) break;
        uint32_t btype=be32(g_jdesc+4), bseq=be32(g_jdesc+8);
        if(bseq!=seq) break;
        if(btype==JBD2_DESCRIPTOR_BLOCK){
            uint32_t next_data = (next+1>=maxlen)? first : next+1;
            uint32_t off=12;
            for(;;){
                if(off+tagsz > g_blk_size) break;
                const uint8_t* t=g_jdesc+off;
                uint32_t flags = tag3 ? be32(t+4) : be16(t+6);
                uint64_t lo = be32(t+0);
                uint64_t tgt = (b64 ? ((uint64_t)be32(t+ (tag3?8:8))<<32) : 0) | lo;
                off += tagsz;
                if(!(flags & JBD2_FLAG_SAME_UUID)) off += 16;   /* skip UUID */
                if(phase==JP_REPLAY){
                    if(jread(next_data, g_jdata)==0 && !jrev_test(tgt, seq)){
                        if(flags & JBD2_FLAG_ESCAPE){           /* first word was un-escaped */
                            g_jdata[0]=0xc0; g_jdata[1]=0x3b; g_jdata[2]=0x39; g_jdata[3]=0x98;
                        }
                        blk_write((uint32_t)tgt, g_jdata);
                    }
                }
                next_data = (next_data+1>=maxlen)? first : next_data+1;
                if(flags & JBD2_FLAG_LAST_TAG) break;
            }
            next = next_data;                                /* descriptor + its data blocks */
            continue;
        } else if(btype==JBD2_COMMIT_BLOCK){
            seq++; committed++;
            next = (next+1>=maxlen)? first : next+1;
            continue;
        } else if(btype==JBD2_REVOKE_BLOCK){
            if(phase==JP_REVOKE){
                uint32_t cnt=be32(g_jdesc+12);               /* bytes used incl 12-byte header? r_count */
                uint32_t roff=16, rsz=b64?8:4;               /* header is 16 bytes for v? actually 12 + r_count@12 */
                /* jbd2 revoke header: h(12) + r_count(be32)@12 -> records start @16 */
                while(roff+rsz<=cnt && roff+rsz<=g_blk_size && g_njrev<2048){
                    uint64_t rb = b64 ? (((uint64_t)be32(g_jdesc+roff)<<32)|be32(g_jdesc+roff+4))
                                      : be32(g_jdesc+roff);
                    g_jrev[g_njrev].blk=rb; g_jrev[g_njrev].seq=seq; g_njrev++;
                    roff += rsz;
                }
            }
            next = (next+1>=maxlen)? first : next+1;
            continue;
        } else break;
    }
    return committed;
}

/* Recover the journal of the currently-mounted volume (globals from read_super).
 * Returns 1 if a replay happened, 0 if nothing to do, -1 on error. */
static int jbd2_recover(void){
    uint8_t sbsec[1024];
    if(block_read(g_dev, 2, sbsec, 2)!=2) return -1;
    uint8_t* sb=sbsec;
    uint32_t feat_compat   = rd32(sb+92);
    uint32_t feat_incompat = rd32(sb+96);
    if(!(feat_compat & 0x4)) return 0;                       /* no HAS_JOURNAL */
    uint32_t jinum = rd32(sb+224);                           /* s_journal_inum */
    if(!jinum) return 0;
    if(inode_read(jinum, g_jin)!=0) return -1;
    if(jread(0, g_jdesc)!=0) return -1;                      /* journal superblock @ jblk 0 */
    if(be32(g_jdesc+0)!=JBD2_MAGIC) return -1;
    uint32_t jmaxlen  = be32(g_jdesc+16);
    uint32_t jfirst   = be32(g_jdesc+20);
    uint32_t jseq     = be32(g_jdesc+24);
    uint32_t jstart   = be32(g_jdesc+28);
    uint32_t jincompat= be32(g_jdesc+40);
    int needs = (feat_incompat & 0x4) ? 1 : 0;               /* EXT4 NEEDS_RECOVERY */
    if(jstart==0){                                           /* journal clean */
        if(needs){                                           /* clear stale flag */
            wr32(sb+96, feat_incompat & ~0x4u); block_write(g_dev, 2, sbsec, 2);
        }
        return 0;
    }
    g_njrev=0;
    uint32_t committed = jbd2_pass(JP_SCAN,   jfirst, jmaxlen, jincompat, jstart, jseq, 0);
    uint32_t end_seq   = jseq + committed;
    debugcon_writestring("[M27] journal recover: txns="); debugcon_print_hex(committed); debugcon_writestring("\n");
    jbd2_pass(JP_REVOKE, jfirst, jmaxlen, jincompat, jstart, jseq, end_seq);
    jbd2_pass(JP_REPLAY, jfirst, jmaxlen, jincompat, jstart, jseq, end_seq);
    /* Mark the journal empty (s_start=0, s_sequence=end_seq) and clear the fs
     * NEEDS_RECOVERY flag so we don't replay again and e2fsck sees it clean. */
    if(jread(0, g_jdesc)==0){
        g_jdesc[28]=0;g_jdesc[29]=0;g_jdesc[30]=0;g_jdesc[31]=0;            /* s_start = 0 (BE) */
        g_jdesc[24]=(uint8_t)(end_seq>>24);g_jdesc[25]=(uint8_t)(end_seq>>16);
        g_jdesc[26]=(uint8_t)(end_seq>>8);g_jdesc[27]=(uint8_t)end_seq;     /* s_sequence (BE) */
        uint32_t jpb=bmap_read(g_jin,0); if(jpb) blk_write(jpb, g_jdesc);
    }
    if(needs){ wr32(sb+96, feat_incompat & ~0x4u); block_write(g_dev, 2, sbsec, 2); }
    return committed ? 1 : 0;
}

/* ── [M27b] Write-side transaction layer ────────────────────────────────────*/
#define JT_MAX 64
static struct { uint32_t blk; uint8_t data[MAX_BLK]; } g_jt[JT_MAX];
static int      g_jt_n;
static int      g_jhas;              /* volume carries a usable journal          */
static uint32_t g_jfirst, g_jmaxlen, g_jseq;   /* journal geometry / next sequence */

static void wbe32(uint8_t* p, uint32_t v){ p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v; }
static void wbe16(uint8_t* p, uint16_t v){ p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v; }

/* Capture journal geometry at mount (after any recovery). Call with g_mounted. */
static void jt_init(void){
    g_jhas=0; g_jt_active=0; g_jt_n=0;
    uint8_t sbsec[1024];
    if(block_read(g_dev,2,sbsec,2)!=2) return;
    if(!(rd32(sbsec+92)&0x4)) return;                 /* no HAS_JOURNAL */
    uint32_t jinum=rd32(sbsec+224); if(!jinum) return;
    if(inode_read(jinum,g_jin)!=0) return;
    if(jread(0,g_jdesc)!=0 || be32(g_jdesc+0)!=JBD2_MAGIC) return;
    /* Only journal to a checksum-free, non-async journal we can format simply. */
    uint32_t jincompat=be32(g_jdesc+40);
    if(jincompat & ~JBD2_INCOMPAT_64BIT) return;      /* csum/async journal -> stay legacy */
    g_jmaxlen=be32(g_jdesc+16); g_jfirst=be32(g_jdesc+20); g_jseq=be32(g_jdesc+24);
    if(g_jfirst==0) g_jfirst=1;
    g_jhas=1;
}

static int jt_read_cached(uint32_t b, uint8_t* buf){
    for(int i=0;i<g_jt_n;i++) if(g_jt[i].blk==b){ mem_copy(buf,g_jt[i].data,g_blk_size); return 1; }
    return 0;
}

static int jt_buffer(uint32_t b, const uint8_t* buf){
    for(int i=0;i<g_jt_n;i++) if(g_jt[i].blk==b){ mem_copy(g_jt[i].data,buf,g_blk_size); return 0; }
    if(g_jt_n>=JT_MAX) return blk_write_direct(b,buf);     /* overflow: degrade (rare) */
    g_jt[g_jt_n].blk=b; mem_copy(g_jt[g_jt_n].data,buf,g_blk_size); g_jt_n++;
    return 0;
}

static void jt_begin(void){ if(g_jhas){ g_jt_n=0; g_jt_active=1; } }

/* Discard a transaction without writing anything: the buffered metadata never
 * reaches disk, so a failed operation leaves the fs exactly as before (atomic
 * rollback). Any direct data/zeroing writes landed on not-yet-referenced blocks
 * and are harmless. */
static void jt_abort(void){ g_jt_active=0; g_jt_n=0; }

/* Finish a transaction: commit on success (r ok), roll back otherwise. */
static int jt_finish(int r, int ok){ if(ok) jt_commit(); else jt_abort(); return r; }

/* Commit the buffered metadata atomically: write the journal transaction
 * (descriptor + data blocks + commit), publish it (s_start), checkpoint in place,
 * then retire it (s_start=0). A crash between publish and retire is replayed by
 * M27a on the next mount; a crash before publish leaves the fs untouched. */
static void jt_commit(void){
    if(!g_jt_active) return;
    g_jt_active=0;
    int n=g_jt_n; if(n<=0) return;
    if(!g_jhas){ for(int i=0;i<n;i++) blk_write_direct(g_jt[i].blk,g_jt[i].data); return; }
    uint32_t jp_desc=bmap_read(g_jin,g_jfirst);
    /* descriptor block: header + one tag per buffered block (first tag carries a
     * UUID, the rest set SAME_UUID; last sets LAST_TAG) */
    mem_zero(g_jdesc,g_blk_size);
    wbe32(g_jdesc+0,JBD2_MAGIC); wbe32(g_jdesc+4,JBD2_DESCRIPTOR_BLOCK); wbe32(g_jdesc+8,g_jseq);
    uint32_t off=12;
    for(int i=0;i<n;i++){
        uint16_t flags=0; if(i>0) flags|=JBD2_FLAG_SAME_UUID; if(i==n-1) flags|=JBD2_FLAG_LAST_TAG;
        wbe32(g_jdesc+off,g_jt[i].blk); wbe16(g_jdesc+off+4,0); wbe16(g_jdesc+off+6,flags); off+=8;
        if(i==0){ for(int u=0;u<16;u++) g_jdesc[off+u]=0; off+=16; }   /* UUID (zeros ok, no csum) */
    }
    if(jp_desc) blk_write_direct(jp_desc,g_jdesc);
    /* data blocks follow the descriptor */
    for(int i=0;i<n;i++){ uint32_t jp=bmap_read(g_jin,g_jfirst+1+i); if(jp) blk_write_direct(jp,g_jt[i].data); }
    /* [M27b test] crash BEFORE the commit block: txn never becomes valid -> the
     * next mount sees no committed transaction -> filesystem stays in the OLD state. */
    if(g_crash_mode==1){ g_blk_dead=1; debugcon_writestring("[M27B] CRASH before commit\n"); }
    /* commit block */
    mem_zero(g_jdesc,g_blk_size);
    wbe32(g_jdesc+0,JBD2_MAGIC); wbe32(g_jdesc+4,JBD2_COMMIT_BLOCK); wbe32(g_jdesc+8,g_jseq);
    uint32_t jp_commit=bmap_read(g_jin,g_jfirst+1+n); if(jp_commit) blk_write_direct(jp_commit,g_jdesc);
    /* PUBLISH: journal sb s_start = descriptor block, s_sequence = this txn */
    uint32_t jp_sb=bmap_read(g_jin,0);
    if(jread(0,g_jdesc)==0){ wbe32(g_jdesc+28,g_jfirst); wbe32(g_jdesc+24,g_jseq); if(jp_sb) blk_write_direct(jp_sb,g_jdesc); }
    /* [M27b test] crash AFTER publish, before checkpoint completes: the committed
     * txn is durable in the journal -> the next mount REPLAYS it -> NEW state. */
    if(g_crash_mode==2){ g_blk_dead=1; debugcon_writestring("[M27B] CRASH after publish\n"); }
    /* CHECKPOINT: write the buffered metadata to its final in-place locations */
    for(int i=0;i<n;i++) blk_write_direct(g_jt[i].blk,g_jt[i].data);
    /* RETIRE: journal sb s_start = 0, s_sequence = next */
    g_jseq++;
    if(jread(0,g_jdesc)==0){ wbe32(g_jdesc+28,0); wbe32(g_jdesc+24,g_jseq); if(jp_sb) blk_write_direct(jp_sb,g_jdesc); }
    g_jt_n=0;
}

int ext2_mount(const char* dev_name, const char* mount_point){
    if(ext2_read_super(block_find(dev_name))!=0) return -1;
    jbd2_recover();                                          /* [M27a] replay a dirty journal */
    if(ext2_read_super(block_find(dev_name))!=0) return -1;  /* re-read SB (recovery cleared flags) */
    jt_init();                                               /* [M27b] capture journal geometry */
    return vfs_mount(mount_point, &ext2_ops, "ext2");
}

/* [M23] Mount an ext2 volume as the persistent VFS root, but only if it is a
 * SecOS system disk — identified by the marker file "/.secosroot" at its root
 * (so a plain ext2 *data* disk is never silently grabbed as the root). Returns 0
 * if mounted as root, -1 otherwise (caller falls back to the ramfs root). */
int ext2_mount_root(const char* dev_name){
    if(ext2_read_super(block_find(dev_name))!=0) return -1;
    jbd2_recover();                              /* [M27a] replay a dirty journal first */
    if(ext2_read_super(block_find(dev_name))!=0) return -1;
    jt_init();                                   /* [M27b] capture journal geometry */
    g_vused=0;
    if(!e2_lookup("/.secosroot")) return -1;     /* ext2 but not a SecOS root */
    g_vused=0;
    return vfs_mount_root(&ext2_ops, "ext2");
}
