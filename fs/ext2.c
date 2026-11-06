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
#include "heap.h"
#include "pagecache.h"
#include "mntopts.h"
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
static uint8_t g_eb[MAX_BLK];       /* extent leaf block (write/free/truncate/count) */
#define EXT4_INDEX_FL    0x00001000u  /* i_flags: htree-indexed directory */

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
static int g_ro;                     /* [M46] mounted read-only: nothing is written */
static int blk_write_direct(uint32_t b, const uint8_t* buf){
    if(g_blk_dead) return 0;         /* write lost to the (simulated) crash */
    if(g_ro) return -1;              /* never touch a read-only volume */
    return block_write(g_dev, (uint64_t)b*g_sec_per_blk, buf, g_sec_per_blk)==(int)g_sec_per_blk ? 0 : -1;
}
static int blk_write(uint32_t b, const uint8_t* buf){
    if(g_jt_active) return jt_buffer(b, buf);            /* metadata -> transaction */
    return blk_write_direct(b, buf);
}

static void mem_zero(uint8_t* p, size_t n){ while(n--) *p++=0; }
static void mem_copy(uint8_t* d, const uint8_t* s, size_t n){ while(n--) *d++=*s++; }
static size_t e2_strlen(const char* s){ size_t n=0; while(s[n]) n++; return n; }

/* Real (RTC-anchored) time for inode stamps: atime/ctime/mtime at 8/12/16. */
extern uint64_t kclock_real_sec(void);
static uint32_t e2_now(void){ return (uint32_t)kclock_real_sec(); }
static void e2_stamp(uint8_t* in, int a, int c, int m){
    uint32_t t=e2_now();
    if(a) wr32(in+8,t);
    if(c) wr32(in+12,t);
    if(m) wr32(in+16,t);
}
static uint8_t g_tb[MAX_BLK];       /* truncate/free: indirect block copy */
static int e2_adjust_links(uint32_t ino, int delta);   /* fwd: link-count += delta, stamp m/ctime */

/* ── [M33] ext4 metadata_csum (crc32c, Castagnoli) ────────────────────────────
 * When RO_COMPAT_METADATA_CSUM is set, every metadata block carries a crc32c.
 * Each mutating write must refresh the relevant checksum or the volume fails
 * e2fsck / a real ext4 driver. Formulas verified byte-for-byte against a real
 * `mkfs.ext4` image (superblock, group desc, inode, dir block, bitmaps). */
static int      g_csum;          /* 1 if the volume has metadata_csum */
static uint32_t g_csum_seed;     /* s_checksum_seed or crc32c(~0, uuid) */
static uint8_t  g_csb[MAX_BLK];  /* scratch for bitmap reads inside gd_write */

static uint32_t e2_crc32c(uint32_t crc, const uint8_t* p, uint32_t n){
    while(n--){ crc ^= *p++; for(int k=0;k<8;k++) crc = (crc>>1) ^ (0x82F63B78u & (uint32_t)(-(int32_t)(crc&1))); }
    return crc;
}
/* per-inode seed = crc32c(crc32c(s_csum_seed, le32(ino)), le32(gen)) */
static uint32_t e2_ino_seed(uint32_t ino, uint32_t gen){
    uint8_t b[4]; wr32(b,ino); uint32_t c=e2_crc32c(g_csum_seed,b,4); wr32(b,gen); return e2_crc32c(c,b,4);
}
/* Superblock checksum (seed is ~0, NOT s_csum_seed): crc32c(~0, sb[0..0x3FC]). */
static void sb_finalize_csum(uint8_t* sb){
    if(g_csum) wr32(sb+0x3FC, e2_crc32c(0xFFFFFFFFu, sb, 0x3FC));
}
/* Group descriptor: bitmap csums (stored in the gd) first, then bg_checksum
 * (low 16 bits) which covers them. Reads both bitmaps for the group. */
static void gd_finalize_csum(uint32_t group, uint8_t* gd){
    uint32_t bbmp = rd32(gd+0), ibmp = rd32(gd+4);
    uint16_t flags = rd16(gd+0x12);
    /* A bitmap still flagged UNINIT is not on disk (mke2fs never wrote it):
     * leave it and its flag alone. The allocator initialises the bitmap and
     * clears the flag itself the first time it uses the group. */
    if(bbmp && !(flags & 0x2) && blk_read(bbmp, g_csb)==0){
        uint32_t c = e2_crc32c(g_csum_seed, g_csb, g_blocks_per_group/8);
        wr16(gd+0x18, (uint16_t)c);
        if(g_desc_size>=0x3A) wr16(gd+0x38, (uint16_t)(c>>16));
    }
    if(ibmp && !(flags & 0x1) && blk_read(ibmp, g_csb)==0){
        uint32_t c = e2_crc32c(g_csum_seed, g_csb, g_inodes_per_group/8);
        wr16(gd+0x1A, (uint16_t)c);
        if(g_desc_size>=0x3C) wr16(gd+0x3A, (uint16_t)(c>>16));
        /* bg_itable_unused = count of never-used inodes at the tail of the table.
         * Recompute it EXACTLY from the bitmap (highest set bit) so e2fsck agrees. */
        int32_t hibit=-1;
        for(int32_t b=(int32_t)g_inodes_per_group-1; b>=0; b--){ if(g_csb[b>>3]&(1<<(b&7))){ hibit=b; break; } }
        uint32_t unused = g_inodes_per_group - (uint32_t)(hibit+1);
        wr16(gd+0x1C, (uint16_t)unused);
        if(g_desc_size>=0x3E) wr16(gd+0x3C, (uint16_t)(unused>>16));
    }
    wr16(gd+0x12, flags);
    uint8_t le_group[4]; wr32(le_group, group);
    uint8_t z2[2]={0,0};
    uint32_t c = e2_crc32c(g_csum_seed, le_group, 4);
    c = e2_crc32c(c, gd, 0x1E);
    c = e2_crc32c(c, z2, 2);
    c = e2_crc32c(c, gd+0x20, g_desc_size-0x20);
    wr16(gd+0x1E, (uint16_t)c);
}
/* Inode: crc32c over the inode with the two checksum fields zeroed; lo at 0x7C,
 * hi at 0x82 (only when i_extra_isize covers it). */
static void inode_finalize_csum(uint32_t ino, uint8_t* raw){
    uint32_t gen = rd32(raw+0x64);
    uint32_t seed = e2_ino_seed(ino, gen);
    int has_hi = (g_inode_size>128) && (rd16(raw+0x80) >= 4);
    uint8_t z2[2]={0,0};
    uint32_t c = e2_crc32c(seed, raw, 0x7C);
    c = e2_crc32c(c, z2, 2);                          /* i_checksum_lo zeroed */
    if(has_hi){
        c = e2_crc32c(c, raw+0x7E, 0x82-0x7E);        /* l_i_reserved + i_extra_isize */
        c = e2_crc32c(c, z2, 2);                      /* i_checksum_hi zeroed */
        c = e2_crc32c(c, raw+0x84, g_inode_size-0x84);
    } else {
        c = e2_crc32c(c, raw+0x7E, g_inode_size-0x7E);
    }
    wr16(raw+0x7C, (uint16_t)c);
    if(has_hi) wr16(raw+0x82, (uint16_t)(c>>16));
}
/* Directory block: write the 12-byte dirent tail at blocksize-12 and its csum.
 * The entries region is [0, blocksize-12); the tail holds crc32c over it. */
static void dir_finalize_csum(uint32_t dir_ino, uint32_t gen, uint8_t* buf){
    uint8_t* t = buf + g_blk_size - 12;
    wr32(t+0, 0); wr16(t+4, 12); t[6]=0; t[7]=0xDE;
    uint32_t seed = e2_ino_seed(dir_ino, gen);
    wr32(t+8, e2_crc32c(seed, buf, g_blk_size-12));
}
/* Usable bytes for real dir entries (csum volumes reserve the 12-byte tail). */
static uint32_t dir_entry_limit(void){ return g_csum ? g_blk_size-12 : g_blk_size; }

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
    if(g_csum) gd_finalize_csum(group, g_mb+within);   /* [M33] bitmap + bg_checksum */
    return blk_write(blk, g_mb);
}
static uint32_t gd_inode_table(uint32_t group){ uint8_t gd[64]; return gd_read(group,gd)==0?rd32(gd+8):0; }
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
    if(g_csum) inode_finalize_csum(ino, g_mb+within);   /* [M33] i_checksum_lo/hi */
    return blk_write(blk, g_mb);
}

/* ── Superblock free-count maintenance (best effort) ─────────────────────── */
static void sb_adjust_free_blocks(int delta){
    if(blk_read(g_first_data_block, g_mb)!=0) return; /* SB lives in block holding offset 1024 */
    uint8_t* sb = (g_blk_size==1024) ? g_mb : g_mb+1024;
    uint32_t fb = rd32(sb+12); fb=(uint32_t)((int)fb+delta); wr32(sb+12, fb);
    sb_finalize_csum(sb);                                 /* [M33] s_checksum */
    blk_write(g_first_data_block, g_mb);
}
static void sb_adjust_free_inodes(int delta){
    if(blk_read(g_first_data_block, g_mb)!=0) return;
    uint8_t* sb = (g_blk_size==1024) ? g_mb : g_mb+1024;
    uint32_t fi = rd32(sb+16); fi=(uint32_t)((int)fi+delta); wr32(sb+16, fi);
    sb_finalize_csum(sb);                                 /* [M33] s_checksum */
    blk_write(g_first_data_block, g_mb);
}

/* ── Bitmap-based allocation ─────────────────────────────────────────────── */
/* Uninitialised groups (metadata_csum / uninit_bg): mke2fs does not write the
 * block bitmap of a group flagged EXT4_BG_BLOCK_UNINIT, nor the inode bitmap of
 * one flagged EXT4_BG_INODE_UNINIT, so the on-disk block is garbage (zeros on a
 * fresh image). The first allocation in such a group builds the bitmap the way
 * the ext4 driver does (ext4_init_block_bitmap) and clears the flag. */
#define EXT4_BG_INODE_UNINIT 0x1
#define EXT4_BG_BLOCK_UNINIT 0x2
static uint32_t g_gdt_blocks, g_resv_gdt;    /* GDT + reserved GDT blocks per backup */
static uint32_t g_feat_compat, g_feat_ro;    /* superblock feature words */
static uint32_t g_backup_bgs[2];             /* sparse_super2 backup groups */

static int e2_is_power(uint32_t g, uint32_t base){
    uint32_t n=base; while(n<g) n*=base; return n==g;
}
/* Does group g hold a superblock (primary or backup) + GDT copy? */
static int e2_group_has_super(uint32_t g){
    if(g==0) return 1;
    if(g_feat_compat & 0x200u) return g==g_backup_bgs[0] || g==g_backup_bgs[1];   /* sparse_super2 */
    if(!(g_feat_ro & 0x1u)) return 1;                                            /* no sparse_super */
    return g==1 || e2_is_power(g,3) || e2_is_power(g,5) || e2_is_power(g,7);
}
static void e2_bmp_mark(uint8_t* bmp, uint32_t gstart, uint32_t b){
    if(b>=gstart && b<gstart+g_blocks_per_group){ uint32_t i=b-gstart; bmp[i>>3] |= (uint8_t)(1u<<(i&7)); }
}
/* Build the block bitmap of an uninitialised group: superblock + GDT backup,
 * every group's bitmaps and inode table that live in this group (flex_bg packs
 * them into the first group of a flex group), and the padding past the end of
 * the filesystem in the last group. */
static void e2_init_block_bitmap(uint32_t g, uint8_t* bmp){
    mem_zero(bmp, g_blk_size);
    uint32_t gstart = g_first_data_block + g*g_blocks_per_group;
    if(e2_group_has_super(g)){
        uint32_t n = 1 + g_gdt_blocks + g_resv_gdt;
        for(uint32_t i=0;i<n;i++) e2_bmp_mark(bmp, gstart, gstart+i);
    }
    uint32_t itb = (g_inodes_per_group*g_inode_size + g_blk_size-1)/g_blk_size;
    for(uint32_t h=0; h<g_num_groups; h++){
        uint8_t gd[64]; if(gd_read(h,gd)!=0) continue;
        e2_bmp_mark(bmp, gstart, rd32(gd+0));
        e2_bmp_mark(bmp, gstart, rd32(gd+4));
        uint32_t it=rd32(gd+8);
        if(it+itb>gstart && it<gstart+g_blocks_per_group) for(uint32_t k=0;k<itb;k++) e2_bmp_mark(bmp, gstart, it+k);
    }
    uint32_t in_group = (g==g_num_groups-1) ? g_total_blocks - gstart : g_blocks_per_group;
    for(uint32_t i=in_group; i<g_blk_size*8; i++) bmp[i>>3] |= (uint8_t)(1u<<(i&7));
}

static uint32_t alloc_block(void){
    for(uint32_t g=0; g<g_num_groups; g++){
        uint8_t gd[64]; if(gd_read(g,gd)!=0) continue;
        if(rd16(gd+12)==0 && (g_desc_size<64 || rd16(gd+0x2C)==0)) continue;   /* group full */
        uint32_t bmp = rd32(gd+0);
        if(!bmp) continue;
        int uninit = (rd16(gd+0x12) & EXT4_BG_BLOCK_UNINIT) ? 1 : 0;
        if(uninit) e2_init_block_bitmap(g, g_xb);
        else if(blk_read(bmp, g_xb)!=0) continue;
        uint32_t nbits = g_blocks_per_group;
        for(uint32_t i=0;i<nbits;i++){
            if(g_xb[i>>3]==0xFF){ i|=7; continue; }          /* 8 used blocks at once */
            if(!(g_xb[i>>3] & (1u<<(i&7)))){
                g_xb[i>>3] |= (1u<<(i&7));
                if(blk_write(bmp, g_xb)!=0) return 0;
                /* update group free count (and mark the bitmap initialised) */
                if(gd_read(g,gd)==0){
                    uint16_t fc=rd16(gd+12); if(fc) wr16(gd+12,fc-1);
                    if(uninit) wr16(gd+0x12, (uint16_t)(rd16(gd+0x12) & ~EXT4_BG_BLOCK_UNINIT));
                    gd_write(g,gd);
                }
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
    uint8_t gd[64]; if(gd_read(g,gd)!=0) return;
    if(rd16(gd+0x12) & EXT4_BG_BLOCK_UNINIT) return;      /* nothing allocated there */
    uint32_t bmp = rd32(gd+0); if(!bmp) return;
    if(blk_read(bmp, g_xb)!=0) return;
    if(g_xb[i>>3] & (1u<<(i&7))){
        g_xb[i>>3] &= ~(1u<<(i&7));
        blk_write(bmp, g_xb);
        if(gd_read(g,gd)==0){ uint16_t fc=rd16(gd+12); wr16(gd+12,fc+1); gd_write(g,gd); }
        sb_adjust_free_blocks(+1);
    }
}
static uint32_t alloc_inode(int is_dir){
    for(uint32_t g=0; g<g_num_groups; g++){
        uint8_t gd[64]; if(gd_read(g,gd)!=0) continue;
        if(rd16(gd+14)==0 && (g_desc_size<64 || rd16(gd+0x2E)==0)) continue;   /* no free inode */
        uint32_t bmp = rd32(gd+4);
        if(!bmp) continue;
        int uninit = (rd16(gd+0x12) & EXT4_BG_INODE_UNINIT) ? 1 : 0;
        if(uninit){                                       /* never used: all free */
            mem_zero(g_xb, g_blk_size);
            for(uint32_t i=g_inodes_per_group; i<g_blk_size*8; i++) g_xb[i>>3] |= (uint8_t)(1u<<(i&7));
        } else if(blk_read(bmp, g_xb)!=0) continue;
        uint32_t nbits = g_inodes_per_group;
        for(uint32_t i=0;i<nbits;i++){
            if(g_xb[i>>3]==0xFF){ i|=7; continue; }
            if(!(g_xb[i>>3] & (1u<<(i&7)))){
                g_xb[i>>3] |= (1u<<(i&7));
                if(blk_write(bmp, g_xb)!=0) return 0;
                if(gd_read(g,gd)==0){
                    uint16_t fc=rd16(gd+14); if(fc) wr16(gd+14,fc-1);
                    if(is_dir){ uint16_t ud=rd16(gd+16); wr16(gd+16,ud+1); }
                    if(uninit) wr16(gd+0x12, (uint16_t)(rd16(gd+0x12) & ~EXT4_BG_INODE_UNINIT));
                    gd_write(g,gd);
                }
                sb_adjust_free_inodes(-1);
                return g*g_inodes_per_group + i + 1;
            }
        }
    }
    return 0;
}
static void vslot_drop_cache(uint32_t ino);
static void free_inode(uint32_t ino, int is_dir){
    if(ino==0) return;
    vslot_drop_cache(ino);   /* the number (and its VFS slot) may go to a new file */
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

/* Blocks an inode really maps (i_blocks / sec_per_blk): extent lengths for a
 * depth-0 tree, mapped direct + single-indirect pointers (+ the indirect block
 * itself) for ext2, so sparse files and indirect blocks count correctly. Falls
 * back to size-derived for trees SecOS doesn't write (deeper extents, dind). */
static uint32_t inode_data_blocks(const uint8_t* inode, uint32_t fsize){
    if(rd32(inode+32) & EXT4_EXTENTS_FL){
        const uint8_t* hdr=inode+40;
        if(rd16(hdr+0)!=EXT4_EXTENT_MAGIC || rd16(hdr+6)>1) return (fsize+g_blk_size-1)/g_blk_size;
        uint32_t n=0; const uint8_t* e=hdr+12;
        if(rd16(hdr+6)==1){                       /* depth 1: the leaves + their extents */
            for(uint16_t k=0;k<rd16(hdr+2);k++,e+=12){
                n++;
                if(blk_read(rd32(e+4), g_eb)!=0) continue;
                const uint8_t* le=g_eb+12;
                for(uint16_t j=0;j<rd16(g_eb+2);j++,le+=12){ uint16_t l=rd16(le+4); n += (l>32768) ? (uint32_t)(l-32768) : l; }
            }
            return n;
        }
        for(uint16_t k=0;k<rd16(hdr+2);k++,e+=12){ uint16_t l=rd16(e+4); n += (l>32768) ? (uint32_t)(l-32768) : l; }
        return n;
    }
    if(rd32(inode+40+13*4) || rd32(inode+40+14*4)) return (fsize+g_blk_size-1)/g_blk_size + 1;
    uint32_t n=0;
    for(uint32_t i=0;i<12;i++) if(rd32(inode+40+i*4)) n++;
    uint32_t ind=rd32(inode+40+12*4);
    if(ind){
        n++;
        if(blk_read(ind, g_xb)==0) for(uint32_t i=0;i<g_blk_size/4;i++) if(rd32(g_xb+i*4)) n++;
    }
    return n;
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

/* ── Extent write path (inline root + one level of leaf blocks) ─────────────
 * A new file starts with the 4-entry inline extent tree. When it needs a fifth
 * extent (fragmented free space, a file crossing a group's metadata), the
 * entries move into a leaf block (340 extents at 4 KiB) and the root becomes a
 * depth-1 index; up to 4 leaves are added the same way. Deeper trees (written
 * only by other drivers for huge, very fragmented files) are read but not
 * extended. Leaf blocks are metadata: they go through the journal and carry the
 * metadata_csum extent tail. */
/* (g_eb, the extent leaf buffer, is declared with the scratch buffers) */

static uint16_t ext_leaf_max(void){ return (uint16_t)((g_blk_size-12)/12); }
static void ext_block_finalize_csum(uint32_t ino, uint32_t gen, uint8_t* blk){
    if(!g_csum) return;
    uint32_t toff = 12u + 12u*rd16(blk+4);
    if(toff+4 > g_blk_size) return;
    wr32(blk+toff, e2_crc32c(e2_ino_seed(ino,gen), blk, toff));
}
/* Append fb->pb to a depth-0 node (the inline root or a leaf block), merging
 * with the last extent when contiguous. 0 = done, 1 = node full. */
static int ext_node_append(uint8_t* hdr, uint32_t fb, uint32_t pb){
    uint16_t entries=rd16(hdr+2), maxent=rd16(hdr+4);
    uint8_t* base=hdr+12;
    if(entries>0){
        uint8_t* last=base+(entries-1)*12;
        uint32_t lfirst=rd32(last+0); uint16_t llen=rd16(last+4); uint32_t llo=rd32(last+8);
        if(fb==lfirst+llen && pb==llo+llen && llen<32768){ wr16(last+4,(uint16_t)(llen+1)); return 0; }
    }
    if(entries>=maxent) return 1;
    uint8_t* e=base+entries*12;
    wr32(e+0,fb); wr16(e+4,1); wr16(e+6,0); wr32(e+8,pb);
    wr16(hdr+2,(uint16_t)(entries+1));
    return 0;
}
static void ext_leaf_init(uint8_t* blk){
    mem_zero(blk, g_blk_size);
    wr16(blk+0, EXT4_EXTENT_MAGIC); wr16(blk+2, 0); wr16(blk+4, ext_leaf_max()); wr16(blk+6, 0);
}
/* Map file block fb -> physical pb in the inode's extent tree (in memory; the
 * caller writes the inode). Leaf blocks are written here. 0 ok, -1 failure. */
static int extent_append(uint32_t ino, uint8_t* inode, uint32_t fb, uint32_t pb){
    uint8_t* hdr=inode+40;
    if(rd16(hdr+0)!=EXT4_EXTENT_MAGIC) return -1;
    uint32_t gen=rd32(inode+0x64);
    uint16_t depth=rd16(hdr+6);
    if(depth==0){
        if(ext_node_append(hdr,fb,pb)==0) return 0;
        uint32_t leaf=alloc_block(); if(!leaf) return -1;  /* grow: inline entries -> a leaf */
        ext_leaf_init(g_eb);
        uint16_t n=rd16(hdr+2);
        mem_copy(g_eb+12, hdr+12, (size_t)n*12); wr16(g_eb+2, n);
        ext_node_append(g_eb,fb,pb);
        ext_block_finalize_csum(ino,gen,g_eb);
        if(blk_write(leaf,g_eb)!=0) return -1;
        mem_zero(hdr+12, 48);
        wr16(hdr+2,1); wr16(hdr+4,4); wr16(hdr+6,1);
        wr32(hdr+12+0, n ? rd32(g_eb+12) : fb);           /* ei_block: first logical block */
        wr32(hdr+12+4, leaf); wr16(hdr+12+8, 0);          /* ei_leaf_lo / ei_leaf_hi */
        return 0;
    }
    if(depth!=1) return -1;
    uint16_t entries=rd16(hdr+2);
    if(entries==0) return -1;
    uint8_t* idx=hdr+12+(entries-1)*12;
    uint32_t leaf=rd32(idx+4);
    if(blk_read(leaf,g_eb)!=0 || rd16(g_eb+0)!=EXT4_EXTENT_MAGIC || rd16(g_eb+6)!=0) return -1;
    if(ext_node_append(g_eb,fb,pb)==0){
        ext_block_finalize_csum(ino,gen,g_eb);
        return blk_write(leaf,g_eb);
    }
    if(entries>=rd16(hdr+4)) return -1;                   /* 4 full leaves: depth 2 unsupported */
    uint32_t nl=alloc_block(); if(!nl) return -1;
    ext_leaf_init(g_eb); ext_node_append(g_eb,fb,pb);
    ext_block_finalize_csum(ino,gen,g_eb);
    if(blk_write(nl,g_eb)!=0) return -1;
    uint8_t* ni=hdr+12+entries*12;
    wr32(ni+0,fb); wr32(ni+4,nl); wr16(ni+8,0); wr16(ni+10,0);
    wr16(hdr+2,(uint16_t)(entries+1));
    return 0;
}

/* Map file block fb to a physical block for WRITE, allocating as needed.
 * Handles both extent (ext4) and indirect (ext2) inodes. Returns 0 on fail. */
static uint32_t block_for_write(uint32_t ino, uint8_t* inode, uint32_t fb){
    if(rd32(inode+32) & EXT4_EXTENTS_FL){
        uint32_t pb = extent_bmap(inode+40, fb, 1);
        if(pb) return pb;
        pb = alloc_block(); if(!pb) return 0;
        if(extent_append(ino, inode, fb, pb)!=0){ free_block(pb); return 0; }
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
        uint32_t pb=block_for_write(ino, inode, fb);
        if(!pb) return -1;
        uint32_t chunk=g_blk_size-bo; if(chunk>len-done) chunk=(uint32_t)(len-done);
        if(bo!=0 || chunk!=g_blk_size){ if(blk_read(pb,g_b)!=0) return -1; }
        for(uint32_t i=0;i<chunk;i++) g_b[bo+i]=in[done+i];
        if(blk_write_direct(pb,g_b)!=0) return -1;   /* [M27b] data: ordered (direct, pre-commit) */
        done+=chunk;
    }
    uint32_t newsize = (uint32_t)(offset+len); if(newsize<fsize) newsize=fsize;
    wr32(inode+4, newsize);
    /* i_blocks in 512-byte units: the blocks really mapped (holes excluded) */
    wr32(inode+28, inode_data_blocks(inode, newsize)*g_sec_per_blk);
    e2_stamp(inode,0,1,1);
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
    uint32_t gen = rd32(inode+0x64);                 /* [M33] dir csum seed */
    uint32_t lim = dir_entry_limit();
    uint32_t dsize = inode_size_lo(inode);
    uint32_t nblocks = (dsize + g_blk_size -1)/g_blk_size;
    uint32_t need = round4(8 + (uint32_t)nlen);
    /* An htree (dir_index) directory, made by Linux or e2fsck -D: its index
     * lives in the free space of ".." and in empty-looking blocks, so a plain
     * insert would overwrite it. Drop the index flag first: by design every
     * htree directory is also a valid linear directory. */
    if(rd32(inode+32) & EXT4_INDEX_FL){
        wr32(inode+32, rd32(inode+32) & ~EXT4_INDEX_FL);
        if(inode_write(dir_ino, inode)!=0) return -1;
        debugcon_writestring("[EXT2] htree index dropped on a directory (now linear)\n");
    }
    for(uint32_t fb=0; fb<nblocks; fb++){
        uint32_t pb = bmap_read(inode, fb); if(!pb) continue;
        if(blk_read(pb, g_b)!=0) return -1;
        uint32_t off=0;
        while(off + 8 <= lim){
            uint32_t e_ino = rd32(g_b+off);
            uint16_t rec   = rd16(g_b+off+4);
            uint8_t  nl    = g_b[off+6];
            if(rec<8) break;
            if(off + rec > lim) rec = (uint16_t)(lim - off);   /* never into the csum tail */
            uint32_t used = (e_ino==0) ? 0 : round4(8+nl);
            if(rec >= used && rec - used >= need){
                uint32_t new_off = off + used;
                if(used>0) wr16(g_b+off+4, (uint16_t)used);
                wr32(g_b+new_off+0, child);
                wr16(g_b+new_off+4, (uint16_t)(rec-used));
                g_b[new_off+6]=(uint8_t)nlen;
                g_b[new_off+7]=ftype;
                for(size_t i=0;i<nlen;i++) g_b[new_off+8+i]=(uint8_t)name[i];
                if(g_csum) dir_finalize_csum(dir_ino, gen, g_b);
                return blk_write(pb, g_b);
            }
            off += rec;
        }
    }
    /* Append a fresh directory block. */
    uint32_t fb = nblocks;
    uint32_t pb = block_for_write(dir_ino, inode, fb); if(!pb) return -1;  /* extent or indirect dir */
    mem_zero(g_b, g_blk_size);
    wr32(g_b+0, child);
    wr16(g_b+4, (uint16_t)lim);                      /* [M33] leave room for the tail */
    g_b[6]=(uint8_t)nlen; g_b[7]=ftype;
    for(size_t i=0;i<nlen;i++) g_b[8+i]=(uint8_t)name[i];
    if(g_csum) dir_finalize_csum(dir_ino, gen, g_b);
    if(blk_write(pb, g_b)!=0) return -1;
    uint32_t newsize = (fb+1)*g_blk_size;
    wr32(inode+4, newsize);
    wr32(inode+28, inode_data_blocks(inode, newsize)*g_sec_per_blk);
    e2_stamp(inode,0,1,1);
    return inode_write(dir_ino, inode);
}

/* Remove entry `name` from directory; coalesces rec_len into the previous. */
static int dir_remove_entry(uint32_t dir_ino, const char* name, size_t nlen){
    uint8_t inode[256];
    if(inode_read(dir_ino, inode)!=0) return -1;
    uint32_t gen = rd32(inode+0x64);
    uint32_t lim = dir_entry_limit();
    uint32_t dsize = inode_size_lo(inode);
    uint32_t nblocks = (dsize + g_blk_size -1)/g_blk_size;
    for(uint32_t fb=0; fb<nblocks; fb++){
        uint32_t pb = bmap_read(inode, fb); if(!pb) continue;
        if(blk_read(pb, g_b)!=0) return -1;
        uint32_t off=0, prev=0xFFFFFFFF;
        while(off + 8 <= lim){
            uint32_t e_ino = rd32(g_b+off);
            uint16_t rec   = rd16(g_b+off+4);
            uint8_t  nl    = g_b[off+6];
            if(rec<8) break;
            if(e_ino!=0 && nl==nlen && name_eq(g_b+off+8, name, nlen)){
                if(prev!=0xFFFFFFFF){ uint16_t prec=rd16(g_b+prev+4); wr16(g_b+prev+4,(uint16_t)(prec+rec)); }
                else { wr32(g_b+off+0, 0); } /* first in block: just clear inode */
                if(g_csum) dir_finalize_csum(dir_ino, gen, g_b);
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
// One VFS inode per on-disk inode, never reused while mounted: an open fd keeps
// its vfs_inode_t* (the VFS has no inode refcount), so recycling a slot made
// that fd read and write whatever file the slot was handed to next. (The old
// fixed 32-slot pool reused its last slot for every further file: on a
// persistent root a service's log fd ended up writing into /etc/passwd.)
// Slots come in kmalloc'd chunks; a remount reuses the chunks.
typedef struct { uint32_t ino; uint32_t size; int is_dir; } e2_node_t;
typedef struct { vfs_inode_t v; e2_node_t n; } e2_vslot_t;
#define E2_VCHUNK      64
#define E2_VCHUNKS_MAX 512                 /* 32768 distinct inodes per mount */
static e2_vslot_t* g_vchunk[E2_VCHUNKS_MAX];
static int         g_vused;

static e2_vslot_t* vslot(int i){ return &g_vchunk[i/E2_VCHUNK][i%E2_VCHUNK]; }

static e2_vslot_t* vslot_find(uint32_t ino){
    for(int i=0;i<g_vused;i++){ e2_vslot_t* s=vslot(i); if(s->n.ino==ino) return s; }
    return NULL;
}

/* An on-disk inode was freed: a later file that gets the same number shares
 * its VFS slot, so the old file's cached pages must go now (kernel-internal
 * vfs_remove callers don't invalidate before removing). */
static void vslot_drop_cache(uint32_t ino){
    e2_vslot_t* s=vslot_find(ino);
    if(s) pagecache_invalidate(&s->v, 0, 1ULL<<62);
}

static vfs_inode_t* vino_alloc(const char* path){
    int idx=g_vused;
    if(idx/E2_VCHUNK >= E2_VCHUNKS_MAX){ debugcon_writestring("[EXT2] VFS inode pool full\n"); return NULL; }
    if(!g_vchunk[idx/E2_VCHUNK]){
        g_vchunk[idx/E2_VCHUNK]=(e2_vslot_t*)kmalloc(sizeof(e2_vslot_t)*E2_VCHUNK);
        if(!g_vchunk[idx/E2_VCHUNK]) return NULL;
    }
    e2_vslot_t* s=vslot(idx);
    mem_zero((uint8_t*)s, sizeof(*s));
    g_vused++;
    vfs_inode_t* ino=&s->v;
    size_t k=0; for(; path[k] && k<sizeof(ino->path)-1; k++) ino->path[k]=path[k]; ino->path[k]=0;
    ino->fs_data=&s->n;
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
    e2_stamp(inode,1,1,1);
    if(inode_write(nino, inode)!=0) return -1;
    if(dir_add(parent, comp, e2_strlen(comp), nino, DT_LNK)!=0) return -1;
    return e2_adjust_links(parent, 0);                    /* parent mtime/ctime */
}

/* ── VFS ops ─────────────────────────────────────────────────────────────── */
static vfs_inode_t* e2_lookup(const char* path){
    if(!g_mounted) return NULL;
    int isd; uint32_t sz;
    uint32_t ino=path_to_ino(path,&isd,&sz);
    if(!ino) return NULL;
    // One VFS inode per on-disk inode: hard links (several paths, one inode)
    // must share the slot, because the page cache is keyed by the VFS inode —
    // a write through one name must be seen by a read through another.
    e2_vslot_t* slot=vslot_find(ino);
    vfs_inode_t* v=slot?&slot->v:NULL;
    if(v){ const char* np=path&&path[0]?path:"/"; size_t k=0; for(; np[k] && k<sizeof(v->path)-1; k++) v->path[k]=np[k]; v->path[k]=0; }
    else v=vino_alloc(path&&path[0]?path:"/");
    if(!v) return NULL;
    v->type=isd?VFS_NODE_DIR:VFS_NODE_FILE; v->size=sz; v->ino=ino;
    // [M26] Populate POSIX metadata from the on-disk inode (standard ext2 layout:
    // i_mode@0, i_uid@2, i_atime@8, i_ctime@12, i_mtime@16, i_gid@24, i_links@26).
    uint8_t in[256];
    if(inode_read(ino, in)==0){
        uint16_t m=rd16(in+0);
        v->mode=m;
        if((m&S_IFMT)==0xA000) v->type=VFS_NODE_SYMLINK;   // S_IFLNK
        // [M43] 32-bit ids: osd2 l_i_uid_high@120 / l_i_gid_high@122 (Linux layout).
        v->uid=(uint32_t)rd16(in+2) | ((uint32_t)rd16(in+120)<<16);
        v->gid=(uint32_t)rd16(in+24) | ((uint32_t)rd16(in+122)<<16);
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
                    child.type=(ft==DT_DIR)?VFS_NODE_DIR:(ft==DT_LNK)?VFS_NODE_SYMLINK:VFS_NODE_FILE;
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
    if(valid&VFS_ATTR_UID){  wr16(in+2,  (uint16_t)a->uid); wr16(in+120, (uint16_t)(a->uid>>16)); }
    if(valid&VFS_ATTR_GID){  wr16(in+24, (uint16_t)a->gid); wr16(in+122, (uint16_t)(a->gid>>16)); }
    if(valid&VFS_ATTR_ATIME) wr32(in+8,  (uint32_t)a->atime);
    if(valid&VFS_ATTR_MTIME) wr32(in+16, (uint32_t)a->mtime);
    e2_stamp(in,0,1,0);                                   /* ctime: attributes changed */
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
    e2_stamp(inode,1,1,1);
    if(inode_write(nino, inode)!=0) return -1;
    if(dir_add(parent, comp, e2_strlen(comp), nino, DT_REG)!=0) return -1;
    if(e2_adjust_links(parent, 0)!=0) return -1;          /* parent mtime/ctime */
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
    /* build "." and ".." (leave room for the dirent tail on csum volumes) */
    uint32_t lim=dir_entry_limit();
    mem_zero(g_b, g_blk_size);
    wr32(g_b+0, nino); wr16(g_b+4, round4(8+1)); g_b[6]=1; g_b[7]=DT_DIR; g_b[8]='.';
    uint32_t o2=round4(8+1);
    wr32(g_b+o2+0, parent); wr16(g_b+o2+4, (uint16_t)(lim-o2)); g_b[o2+6]=2; g_b[o2+7]=DT_DIR; g_b[o2+8]='.'; g_b[o2+9]='.';
    if(g_csum) dir_finalize_csum(nino, 0, g_b);    /* new inode -> generation 0 */
    if(blk_write(db, g_b)!=0) return -1;
    uint8_t inode[256]; mem_zero(inode, g_inode_size);
    wr16(inode+0, S_IFDIR | 0755);
    wr32(inode+4, g_blk_size);
    wr16(inode+26, 2); /* . and parent link */
    wr32(inode+28, g_sec_per_blk);
    wr32(inode+40, db); /* i_block[0] */
    e2_stamp(inode,1,1,1);
    if(inode_write(nino, inode)!=0) return -1;
    if(dir_add(parent, comp, e2_strlen(comp), nino, DT_DIR)!=0) return -1;
    /* bump parent link count (the new ".."), stamp its mtime/ctime */
    return e2_adjust_links(parent, +1);
}

/* Unmap every file block >= keep from a depth-0 extent node (inline root or a
 * leaf block), freeing the data blocks. Returns the entries left. */
static uint16_t ext_node_trim(uint8_t* hdr, uint32_t keep){
    uint16_t entries=rd16(hdr+2);
    uint8_t* base=hdr+12;
    for(int k=(int)entries-1;k>=0;k--){
        uint8_t* e=base+k*12;
        uint32_t first=rd32(e+0); uint16_t rl=rd16(e+4); uint32_t start=rd32(e+8);
        int uninit = rl>32768; uint16_t len = uninit ? (uint16_t)(rl-32768) : rl;
        if(first>=keep){
            for(uint16_t b=0;b<len;b++) free_block(start+b);
            for(int j=k;j<(int)entries-1;j++) mem_copy(base+j*12, base+(j+1)*12, 12);
            entries--;
        } else if(first+len>keep){
            uint16_t nl=(uint16_t)(keep-first);
            for(uint16_t b=nl;b<len;b++) free_block(start+b);
            wr16(e+4,(uint16_t)(uninit ? nl+32768 : nl));
        }
    }
    wr16(hdr+2,entries);
    return entries;
}

/* Unmap every file block >= keep from an inode's extent tree (depth 0 or 1;
 * the caller writes the inode). Emptied leaves are freed and a tree left with
 * no leaf falls back to an empty inline root. 0 ok, -1 unsupported tree. */
static int ext_trim(uint32_t ino, uint8_t* inode, uint32_t keep){
    uint8_t* hdr=inode+40;
    if(rd16(hdr+0)!=EXT4_EXTENT_MAGIC) return -1;
    uint16_t depth=rd16(hdr+6);
    if(depth==0){ ext_node_trim(hdr, keep); return 0; }
    if(depth!=1) return -1;
    uint32_t gen=rd32(inode+0x64);
    uint16_t entries=rd16(hdr+2);
    uint8_t* base=hdr+12;
    for(int k=(int)entries-1;k>=0;k--){
        uint8_t* idx=base+k*12;
        uint32_t leaf=rd32(idx+4);
        if(blk_read(leaf,g_eb)!=0 || rd16(g_eb+0)!=EXT4_EXTENT_MAGIC) return -1;
        if(ext_node_trim(g_eb, keep)==0){
            free_block(leaf);
            for(int j=k;j<(int)entries-1;j++) mem_copy(base+j*12, base+(j+1)*12, 12);
            entries--;
        } else {
            ext_block_finalize_csum(ino,gen,g_eb);
            if(blk_write(leaf,g_eb)!=0) return -1;
        }
    }
    wr16(hdr+2,entries);
    if(entries==0){ mem_zero(hdr+12,48); wr16(hdr+4,4); wr16(hdr+6,0); }
    return 0;
}

/* Free every data block (and the ext2 indirect block) an inode maps. Extent
 * trees of depth 0 and 1 and direct + single-indirect maps (what SecOS itself
 * writes). */
static void e2_free_inode_blocks(uint32_t ino, uint8_t* inode){
    if(rd32(inode+32)&EXT4_EXTENTS_FL){
        if(ext_trim(ino, inode, 0)!=0)
            debugcon_writestring("[EXT2] WARN: deep extent tree not freed (blocks leak until e2fsck)\n");
    } else if((rd16(inode+0)&S_IFMT)!=S_IFLNK || inode_size_lo(inode)>60){
        for(uint32_t fb=0;fb<12;fb++){ uint32_t pb=rd32(inode+40+fb*4); if(pb){ free_block(pb); wr32(inode+40+fb*4,0); } }
        uint32_t ind=rd32(inode+40+12*4);
        if(ind){
            if(blk_read(ind, g_tb)==0){
                for(uint32_t i=0;i<g_blk_size/4;i++){ uint32_t pb=rd32(g_tb+i*4); if(pb) free_block(pb); }
            }
            free_block(ind); wr32(inode+40+12*4,0);
        }
    }
}

/* Drop one link of `ino` (its directory entry is already gone). The inode and
 * its blocks are released when the last link goes; a directory always goes. */
static int e2_drop_link(uint32_t ino, int isd){
    uint8_t in[256]; if(inode_read(ino,in)!=0) return -1;
    uint16_t links=rd16(in+26);
    if(!isd && links>1){
        wr16(in+26,(uint16_t)(links-1));
        e2_stamp(in,0,1,0);
        return inode_write(ino,in);
    }
    e2_free_inode_blocks(ino, in);
    wr16(in+26,0);
    wr32(in+20,e2_now());                                 /* i_dtime */
    wr32(in+28,0);                                        /* i_blocks */
    if(inode_write(ino,in)!=0) return -1;
    free_inode(ino, isd);
    return 0;
}

static int e2_adjust_links(uint32_t ino, int delta){
    uint8_t in[256]; if(inode_read(ino,in)!=0) return -1;
    int l=(int)rd16(in+26)+delta; if(l<0) l=0;
    wr16(in+26,(uint16_t)l);
    e2_stamp(in,0,1,1);
    return inode_write(ino,in);
}

static int e2_remove(const char* path){
    if(!g_mounted||!path) return -1;
    int isd; uint32_t sz; uint32_t ino=path_to_ino(path,&isd,&sz);
    if(!ino || ino==EXT2_ROOT_INO) return -1;
    uint32_t parent; char comp[128];
    if(split_parent(path,&parent,comp,sizeof(comp))!=0) return -1;
    if(dir_remove_entry(parent, comp, e2_strlen(comp))!=0) return -1;
    /* a directory's ".." was one link of its parent */
    if(e2_adjust_links(parent, isd ? -1 : 0)!=0) return -1;
    return e2_drop_link(ino, isd);
}

/* Shrink or grow (sparse) a regular file. Freed blocks are unmapped from the
 * depth-0 extent tree or the direct/single-indirect map; the tail of the last
 * kept block is zeroed so a later grow reads zeros (POSIX). */
static int e2_truncate(const char* path, size_t new_size){
    if(!g_mounted||!path) return -1;
    int isd; uint32_t sz; uint32_t ino=path_to_ino(path,&isd,&sz);
    if(!ino||isd) return -1;
    if(new_size>0xFFFFFFFFu) return -1;
    uint8_t inode[256]; if(inode_read(ino,inode)!=0) return -1;
    if((rd16(inode+0)&S_IFMT)==S_IFLNK) return -1;
    int ext = (rd32(inode+32)&EXT4_EXTENTS_FL) ? 1 : 0;
    uint32_t old=inode_size_lo(inode);
    if(new_size<old){
        uint32_t keep=(uint32_t)((new_size+g_blk_size-1)/g_blk_size);
        if(ext){
            if(ext_trim(ino, inode, keep)!=0) return -1;   /* depth > 1: unsupported */
        } else {
            uint32_t ppb=g_blk_size/4;
            if(rd32(inode+40+13*4)) return -1;            /* double indirect: unsupported */
            for(uint32_t fb=keep; fb<12; fb++){ uint32_t pb=rd32(inode+40+fb*4); if(pb){ free_block(pb); wr32(inode+40+fb*4,0); } }
            uint32_t ind=rd32(inode+40+12*4);
            if(ind && blk_read(ind, g_tb)==0){
                uint32_t from = keep>12 ? keep-12 : 0;
                for(uint32_t i=from;i<ppb;i++){ uint32_t pb=rd32(g_tb+i*4); if(pb){ free_block(pb); wr32(g_tb+i*4,0); } }
                if(keep<=12){ free_block(ind); wr32(inode+40+12*4,0); }
                else if(blk_write(ind, g_tb)!=0) return -1;
            }
        }
        if(new_size % g_blk_size){                         /* zero the kept block's tail */
            uint32_t pb=bmap_read(inode,(uint32_t)(new_size/g_blk_size));
            if(pb && blk_read(pb,g_b)==0){
                for(uint32_t i=(uint32_t)(new_size%g_blk_size);i<g_blk_size;i++) g_b[i]=0;
                if(blk_write_direct(pb,g_b)!=0) return -1;
            }
        }
    }
    wr32(inode+4,(uint32_t)new_size);
    wr32(inode+28, inode_data_blocks(inode, (uint32_t)new_size)*g_sec_per_blk);
    e2_stamp(inode,0,1,1);
    return inode_write(ino, inode);
}

/* Directory-entry file type for an on-disk inode mode. */
static uint8_t e2_ftype(uint16_t mode){
    switch(mode&S_IFMT){
        case S_IFDIR: return DT_DIR;
        case S_IFLNK: return DT_LNK;
        case 0x2000:  return 3;     /* chr  */
        case 0x6000:  return 4;     /* blk  */
        case 0x1000:  return 5;     /* fifo */
        case 0xC000:  return 6;     /* sock */
        default:      return DT_REG;
    }
}

/* Point the existing entry `name` of dir_ino at `child` (type ft) in place. */
static int dir_set_entry(uint32_t dir_ino, const char* name, size_t nlen, uint32_t child, uint8_t ft){
    uint8_t inode[256];
    if(inode_read(dir_ino, inode)!=0) return -1;
    uint32_t gen = rd32(inode+0x64);
    uint32_t lim = dir_entry_limit();
    uint32_t nblocks = (inode_size_lo(inode) + g_blk_size -1)/g_blk_size;
    for(uint32_t fb=0; fb<nblocks; fb++){
        uint32_t pb = bmap_read(inode, fb); if(!pb) continue;
        if(blk_read(pb, g_b)!=0) return -1;
        uint32_t off=0;
        while(off + 8 <= lim){
            uint32_t e_ino = rd32(g_b+off); uint16_t rec = rd16(g_b+off+4); uint8_t nl = g_b[off+6];
            if(rec<8) break;
            if(e_ino!=0 && nl==nlen && name_eq(g_b+off+8, name, nlen)){
                wr32(g_b+off, child); g_b[off+7]=ft;
                if(g_csum) dir_finalize_csum(dir_ino, gen, g_b);
                return blk_write(pb, g_b);
            }
            off += rec;
        }
    }
    return -1;
}

/* rename(2) inside one volume, as a single metadata transaction (e2j_rename):
 * an existing target entry is re-pointed in place (never a moment with neither
 * name), the old entry removed, a moved directory's ".." re-parented, link
 * counts fixed, and the replaced inode loses its link. The VFS has already
 * checked the POSIX type rules (dir over non-dir, non-empty target, ...). */
static int e2_rename(const char* oldp, const char* newp){
    if(!g_mounted||!oldp||!newp) return -1;
    int isd; uint32_t ino=path_to_ino(oldp,&isd,NULL);
    if(!ino || ino==EXT2_ROOT_INO) return -1;
    uint32_t np; char nc[128];
    if(split_parent(newp,&np,nc,sizeof(nc))!=0) return -1;
    uint32_t op; char oc[128];
    if(split_parent(oldp,&op,oc,sizeof(oc))!=0) return -1;
    uint8_t in[256]; if(inode_read(ino,in)!=0) return -1;
    uint8_t ft = e2_ftype(rd16(in+0));
    int tdir=0; uint32_t tino=path_to_ino(newp,&tdir,NULL);
    if(tino==ino) return 0;                                /* same inode: nothing to do */
    if(tino){ if(dir_set_entry(np, nc, e2_strlen(nc), ino, ft)!=0) return -1; }
    else    { if(dir_add(np, nc, e2_strlen(nc), ino, ft)!=0) return -1; }
    if(dir_remove_entry(op, oc, e2_strlen(oc))!=0) return -1;
    int dnp = 0, dop = 0;                                  /* parent link deltas */
    if(isd && op!=np){
        if(dir_set_entry(ino, "..", 2, np, DT_DIR)!=0) return -1;
        dop--; dnp++;
    }
    if(tino && tdir) dnp--;                                /* replaced dir's ".." */
    if(e2_adjust_links(op, dop)!=0) return -1;
    if(np!=op && e2_adjust_links(np, dnp)!=0) return -1;
    if(np==op && dnp && e2_adjust_links(np, dnp)!=0) return -1;
    if(inode_read(ino,in)==0){ e2_stamp(in,0,1,0); inode_write(ino,in); }
    if(tino) return e2_drop_link(tino, tdir);
    return 0;
}

/* link(2): one more name for an existing non-directory inode. */
static int e2_link(const char* oldp, const char* newp){
    if(!g_mounted||!oldp||!newp) return -1;
    int isd; uint32_t ino=path_to_ino(oldp,&isd,NULL);
    if(!ino || isd) return -1;
    if(path_to_ino(newp,NULL,NULL)) return -1;            /* exists */
    uint32_t np; char nc[128];
    if(split_parent(newp,&np,nc,sizeof(nc))!=0) return -1;
    uint8_t in[256]; if(inode_read(ino,in)!=0) return -1;
    uint16_t links=rd16(in+26); if(links>=65000) return -1;
    wr16(in+26,(uint16_t)(links+1));
    e2_stamp(in,0,1,0);
    if(inode_write(ino,in)!=0) return -1;
    if(dir_add(np, nc, e2_strlen(nc), ino, e2_ftype(rd16(in+0)))!=0) return -1;
    return e2_adjust_links(np, 0);                         /* parent mtime/ctime */
}

/* statfs(2): block/inode totals from the superblock, free counts summed over
 * the group descriptors (the per-group counts are what allocation keeps exact;
 * the superblock totals can lag, as on Linux). */
static int e2_statfs(vfs_statfs_t* st){
    if(!g_mounted || !st) return -1;
    if(blk_read(g_first_data_block, g_mb)!=0) return -1;
    const uint8_t* sb = (g_blk_size==1024) ? g_mb : g_mb+1024;
    uint64_t blocks = rd32(sb+4);
    uint64_t rblocks = rd32(sb+8);
    if(g_desc_size>=64){ blocks |= (uint64_t)rd32(sb+0x150)<<32; rblocks |= (uint64_t)rd32(sb+0x154)<<32; }
    uint64_t inodes = rd32(sb+0);
    uint64_t bfree=0, ifree=0;
    for(uint32_t g=0; g<g_num_groups; g++){
        uint8_t gd[64]; if(gd_read(g,gd)!=0) return -1;
        bfree += rd16(gd+12); ifree += rd16(gd+14);
        if(g_desc_size>=64){ bfree += (uint64_t)rd16(gd+0x2C)<<16; ifree += (uint64_t)rd16(gd+0x2E)<<16; }
    }
    st->type = EXT2_MAGIC;
    st->bsize = st->frsize = g_blk_size;
    st->blocks = blocks - g_first_data_block;
    st->bfree = bfree;
    st->bavail = bfree > rblocks ? bfree - rblocks : 0;
    st->files = inodes;
    st->ffree = ifree;
    st->namelen = 255;
    return 0;
}

/* [M27b] Transaction wrappers: each mutating op runs as one journalled metadata
 * transaction (commit-then-checkpoint on success, atomic rollback on failure).
 * Read-only ops (lookup/readdir/read/readlink) are not wrapped. */
/* [M46] On a read-only mount every change is refused up front with -EROFS. */
#define E2_RO_CHECK() do { if(g_ro) return -30; } while(0)
static int e2j_create(const char* p, const void* d, size_t s){ E2_RO_CHECK(); jt_begin(); int r=e2_create(p,d,s); return jt_finish(r, r==0); }
static int e2j_mkdir(const char* p){ E2_RO_CHECK(); jt_begin(); int r=e2_mkdir(p); return jt_finish(r, r==0); }
static int e2j_remove(const char* p){ E2_RO_CHECK(); jt_begin(); int r=e2_remove(p); return jt_finish(r, r==0); }
static int e2j_rename(const char* o,const char* n){ E2_RO_CHECK(); jt_begin(); int r=e2_rename(o,n); return jt_finish(r, r==0); }
static int e2j_truncate(const char* p, size_t s){ E2_RO_CHECK(); jt_begin(); int r=e2_truncate(p,s); return jt_finish(r, r==0); }
static int e2j_setattr(const char* p, const vfs_attr_t* a, unsigned v){ E2_RO_CHECK(); jt_begin(); int r=e2_setattr(p,a,v); return jt_finish(r, r==0); }
static int e2j_symlink(const char* t, const char* l){ E2_RO_CHECK(); jt_begin(); int r=e2_symlink(t,l); return jt_finish(r, r==0); }
static int e2j_link(const char* o, const char* n){ E2_RO_CHECK(); jt_begin(); int r=e2_link(o,n); return jt_finish(r, r==0); }
static int e2j_write(vfs_inode_t* v, size_t off, const void* d, size_t l){ E2_RO_CHECK(); jt_begin(); int r=e2_write(v,off,d,l); return jt_finish(r, r>=0); }

/* Orderly shutdown / sync. Every mutating op already runs as one committed and
 * checkpointed transaction (or writes directly on a journal-less volume), so
 * only a transaction left open by an interrupted op needs committing here. */
static int g_jdirty;                 /* (defined with the journal code) */
static void e2_sb_set_recover(int on);
static int e2_sync(void){
    if(g_ro) return 0;                   /* [M46] nothing was changed */
    if(g_jt_active) jt_commit();
    /* Every transaction is checkpointed and retired: the journal is empty, so
     * the volume is clean until the next transaction sets the flag again. */
    if(g_jdirty){ e2_sb_set_recover(0); g_jdirty=0; }
    return 0;
}

static int g_jhas;                   /* (tentative; defined with the journal code) */
static int e2_mntinfo(const char** src, const char** type){
    if(!g_mounted) return -1;
    if(src) *src = (g_dev && g_dev->name) ? g_dev->name : "ext2";
    if(type) *type = g_use_extents ? "ext4" : (g_jhas ? "ext3" : "ext2");
    return 0;
}

static int e2_remount(int ro);
static vfs_fs_ops_t ext2_ops = {
    .lookup=e2_lookup, .readdir=e2_readdir, .read=e2_read, .write=e2j_write,
    .create=e2j_create, .mkdir=e2j_mkdir, .remove=e2j_remove, .rename=e2j_rename,
    .truncate=e2j_truncate, .setattr=e2j_setattr,
    .readlink=e2_readlink, .symlink=e2j_symlink, .sync=e2_sync,
    .link=e2j_link, .statfs=e2_statfs, .mntinfo=e2_mntinfo, .remount=e2_remount
};

/* ── Mount ───────────────────────────────────────────────────────────────── */
/* Why the last ext2_read_super() refused a volume (NULL = accepted or not ext). */
static const char* g_reject;
const char* ext2_last_error(void){ return g_reject ? g_reject : "unknown error"; }

/* Feature bits this driver reads AND writes correctly. Anything else in
 * INCOMPAT or RO_COMPAT makes the volume unusable here: we refuse it instead of
 * mounting it and corrupting it. COMPAT features are safe by definition. */
#define E2_INCOMPAT_OK  (0x0002u /*FILETYPE*/ | 0x0004u /*RECOVER*/ | 0x0040u /*EXTENTS*/ | \
                         0x0080u /*64BIT*/ | 0x0200u /*FLEX_BG*/ | 0x2000u /*CSUM_SEED*/ | \
                         0x4000u /*LARGEDIR*/)
#define E2_ROCOMPAT_OK  (0x0001u /*SPARSE_SUPER*/ | 0x0002u /*LARGE_FILE*/ | 0x0004u /*BTREE_DIR*/ | \
                         0x0008u /*HUGE_FILE*/ | 0x0020u /*DIR_NLINK*/ | 0x0040u /*EXTRA_ISIZE*/ | \
                         0x0400u /*METADATA_CSUM*/)

/* Sanity-check a superblock before anything is written to the volume. Returns
 * NULL when usable, else a short reason (logged by the caller). */
static const char* e2_check_super(const uint8_t* sb){
    uint32_t log_bs = rd32(sb+24);
    if(log_bs>2) return "block size above 4 KiB";
    uint32_t bs = 1024u<<log_bs;
    uint32_t bpg = rd32(sb+32), ipg = rd32(sb+40);
    if(bpg==0 || bpg>8*bs) return "bad blocks-per-group";
    if(ipg==0 || ipg>8*bs) return "bad inodes-per-group";
    if(rd32(sb+4)==0 || rd32(sb+0)==0) return "zero block/inode count";
    if(rd32(sb+20)!=(bs==1024?1u:0u)) return "bad first data block";
    uint32_t rev = rd32(sb+76);
    if(rev>=1){
        uint16_t isz = rd16(sb+88);
        if(isz<128 || isz>256 || (isz&(isz-1))) return "unsupported inode size";
    }
    uint32_t inc = rd32(sb+96), roc = rd32(sb+100);
    if(rev>=1 && (inc & ~E2_INCOMPAT_OK)) return "unsupported INCOMPAT feature";
    if(rev>=1 && (roc & ~E2_ROCOMPAT_OK)){
        if((roc & 0x10u) && !(roc & 0x400u)) return "uninit_bg (crc16 group checksums) not supported";
        if(roc & ~(E2_ROCOMPAT_OK|0x10u)) return "unsupported RO_COMPAT feature";
    }
    if((inc & 0x80u) && rd32(sb+0x150)!=0) return "more than 2^32 blocks";
    if(inc & 0x80u){ uint16_t ds=rd16(sb+254); if(ds!=0 && ds!=32 && ds!=64) return "unsupported group descriptor size"; }
    if(roc & 0x400u){
        if(sb[0x175]!=1) return "unknown metadata checksum type";
        if(rd32(sb+0x3FC)!=e2_crc32c(0xFFFFFFFFu, sb, 0x3FC)) return "superblock checksum mismatch";
    }
    if(rd16(sb+58) & 0x2u) return "filesystem marked with errors (run e2fsck)";
    return NULL;
}

/* Read + validate the ext2 superblock and populate the global FS state for dev.
 * Returns 0 on a usable volume, -1 when there is no ext2/3/4 superblock, -3 when
 * it is an ext volume this driver must not touch (g_reject says why). Sets
 * g_mounted=1 on success. Nothing is written. */
static int ext2_read_super(block_dev_t* dev){
    g_reject=NULL;
    if(!dev) return -1;
    /* Superblock is at byte offset 1024. Read enough to cover it. */
    uint8_t sbsec[1024];
    if(block_read(dev, 2, sbsec, 2)!=2) return -1; /* sectors 2..3 -> bytes 1024..2047 */
    uint8_t* sb=sbsec; /* sbsec already starts at byte 1024 */
    if(rd16(sb+56)!=EXT2_MAGIC) return -1;
    if((g_reject=e2_check_super(sb))!=NULL) return -3;
    g_dev=dev;
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
    /* [M33] metadata_csum: RO_COMPAT bit 0x400. Seed is s_checksum_seed when the
     * INCOMPAT_CSUM_SEED feature (0x2000) is present, else crc32c(~0, uuid). */
    uint32_t feat_ro = rd32(sb+100);
    g_csum = (feat_ro & 0x400) ? 1 : 0;
    g_csum_seed = 0;
    if(g_csum){
        if(feat_incompat & 0x2000) g_csum_seed = rd32(sb+0x270);
        else g_csum_seed = e2_crc32c(0xFFFFFFFFu, sb+0x68, 16);
    }
    g_sec_per_blk = g_blk_size/SECSZ;
    g_num_groups  = (g_total_blocks - g_first_data_block + g_blocks_per_group -1)/g_blocks_per_group;
    g_bgdt_block  = g_first_data_block + 1;
    g_feat_compat = rd32(sb+92);
    g_feat_ro     = feat_ro;
    g_gdt_blocks  = (g_num_groups*g_desc_size + g_blk_size-1)/g_blk_size;
    g_resv_gdt    = (g_feat_compat & 0x10u) ? rd16(sb+0xCE) : 0;     /* resize_inode */
    g_backup_bgs[0] = rd32(sb+0x24C); g_backup_bgs[1] = rd32(sb+0x250);
    if((uint64_t)g_total_blocks*g_sec_per_blk > dev->sector_count){ g_reject="filesystem larger than the device"; return -3; }
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
static void jsb_finalize_csum(uint8_t* jsb);   /* fwd ([M27b] writer) */
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
            wr32(sb+96, feat_incompat & ~0x4u); sb_finalize_csum(sb); block_write(g_dev, 2, sbsec, 2);
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
        jsb_finalize_csum(g_jdesc);
        uint32_t jpb=bmap_read(g_jin,0); if(jpb) blk_write(jpb, g_jdesc);
    }
    /* Clear NEEDS_RECOVERY on the superblock AS REPLAYED: the transaction may
     * have carried the superblock itself (free counts), so the copy read
     * before the replay is stale and must not be written back. */
    if(block_read(g_dev, 2, sbsec, 2)==2 && (rd32(sb+96) & 0x4u)){
        wr32(sb+96, rd32(sb+96) & ~0x4u); sb_finalize_csum(sb); block_write(g_dev, 2, sbsec, 2);
    }
    return committed ? 1 : 0;
}

/* ── [M27b] Write-side transaction layer ────────────────────────────────────*/
#define JT_MAX 64
static struct { uint32_t blk; uint8_t data[MAX_BLK]; } g_jt[JT_MAX];
static int      g_jt_n;
static int      g_jt_cap;            /* tags that fit one descriptor block (<= JT_MAX) */
static int      g_jhas;              /* volume carries a usable journal          */
static uint32_t g_jfirst, g_jmaxlen, g_jseq;   /* journal geometry / next sequence */
static int      g_jcsum3;            /* journal uses checksum v3 (metadata_csum)  */
static int      g_jb64;              /* journal tags carry 64-bit block numbers   */
static uint32_t g_jcsum_seed;        /* crc32c(~0, journal s_uuid)                */
static uint8_t  g_juuid[16];

static void wbe32(uint8_t* p, uint32_t v){ p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v; }
static void wbe16(uint8_t* p, uint16_t v){ p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v; }

/* Journal superblock checksum (v2/v3): crc32c(~0) over the 1024-byte
 * journal_superblock_t with s_checksum (0xFC) zeroed. */
static void jsb_finalize_csum(uint8_t* jsb){
    if(!(be32(jsb+40) & (JBD2_INCOMPAT_CSUM_V3|0x8u))) return;
    wbe32(jsb+0xFC, 0);
    wbe32(jsb+0xFC, e2_crc32c(0xFFFFFFFFu, jsb, 1024));
}

/* Set or clear EXT4_FEATURE_INCOMPAT_RECOVER in the primary superblock (direct
 * read-modify-write of the 1 KiB at byte 1024, checksum refreshed). Linux and
 * e2fsck replay a journal only when this flag says so; it is set before the
 * first transaction after a mount or sync, and cleared by sync. */
static void e2_sb_set_recover(int on){
    uint8_t sbsec[1024];
    if(g_blk_dead || block_read(g_dev, 2, sbsec, 2)!=2) return;
    uint32_t inc=rd32(sbsec+96);
    uint32_t want = on ? (inc|0x4u) : (inc&~0x4u);
    if(want==inc) return;
    wr32(sbsec+96, want);
    sb_finalize_csum(sbsec);
    block_write(g_dev, 2, sbsec, 2);
}

/* Capture journal geometry at mount (after any recovery). Call with g_mounted. */
static void jt_init(void){
    g_jhas=0; g_jt_active=0; g_jt_n=0; g_jdirty=0;
    uint8_t sbsec[1024];
    if(block_read(g_dev,2,sbsec,2)!=2) return;
    if(!(rd32(sbsec+92)&0x4)) return;                 /* no HAS_JOURNAL */
    uint32_t jinum=rd32(sbsec+224); if(!jinum) return;
    if(inode_read(jinum,g_jin)!=0) return;
    if(jread(0,g_jdesc)!=0 || be32(g_jdesc+0)!=JBD2_MAGIC) return;
    /* Journal formats written here: plain, 64-bit tags and checksum v3 (what
     * mkfs.ext4 makes for a metadata_csum volume). Not v1/v2 checksums, async
     * commit or fast commits: such a volume stays on direct writes. */
    uint32_t jcompat=be32(g_jdesc+36), jincompat=be32(g_jdesc+40);
    if(jcompat & 0x1u) return;                        /* COMPAT_CHECKSUM (v1) */
    if(jincompat & ~(JBD2_INCOMPAT_64BIT|JBD2_INCOMPAT_CSUM_V3|0x1u /*REVOKE*/)) return;
    g_jcsum3 = (jincompat & JBD2_INCOMPAT_CSUM_V3) ? 1 : 0;
    g_jb64   = (jincompat & JBD2_INCOMPAT_64BIT) ? 1 : 0;
    mem_copy(g_juuid, g_jdesc+0x30, 16);
    g_jcsum_seed = e2_crc32c(0xFFFFFFFFu, g_juuid, 16);
    g_jmaxlen=be32(g_jdesc+16); g_jfirst=be32(g_jdesc+20); g_jseq=be32(g_jdesc+24);
    if(g_jfirst==0) g_jfirst=1;
    uint32_t tagsz = g_jcsum3 ? 16 : (g_jb64 ? 12 : 8);
    uint32_t room = g_blk_size - 12 - 16 /* first tag's UUID */ - (g_jcsum3 ? 4 : 0);
    g_jt_cap = (int)(room / tagsz); if(g_jt_cap>JT_MAX) g_jt_cap=JT_MAX;
    if(g_jmaxlen < g_jfirst + (uint32_t)g_jt_cap + 2) return;   /* journal too small */
    g_jhas=1;
}

/* Journal state for the boot report: 0 none, 1 journalled, -1 present but in a
 * format this driver does not write (direct writes, no crash safety). */
int ext2_journal_state(void){
    if(g_jhas) return 1;
    uint8_t sbsec[1024];
    if(!g_mounted || block_read(g_dev,2,sbsec,2)!=2) return 0;
    return (rd32(sbsec+92)&0x4) ? -1 : 0;
}

static int jt_read_cached(uint32_t b, uint8_t* buf){
    for(int i=0;i<g_jt_n;i++) if(g_jt[i].blk==b){ mem_copy(buf,g_jt[i].data,g_blk_size); return 1; }
    return 0;
}

static int jt_buffer(uint32_t b, const uint8_t* buf){
    for(int i=0;i<g_jt_n;i++) if(g_jt[i].blk==b){ mem_copy(g_jt[i].data,buf,g_blk_size); return 0; }
    if(g_jt_n>=g_jt_cap){                                  /* overflow: degrade (rare) */
        debugcon_writestring("[EXT2] WARN: transaction full, metadata block written unjournalled\n");
        return blk_write_direct(b,buf);
    }
    g_jt[g_jt_n].blk=b; mem_copy(g_jt[g_jt_n].data,buf,g_blk_size); g_jt_n++;
    return 0;
}

static void jt_begin(void){
    if(!g_jhas) return;
    if(!g_jdirty){ e2_sb_set_recover(1); g_jdirty=1; }   /* the journal may now hold data */
    g_jt_n=0; g_jt_active=1;
}

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
 * M27a (or by e2fsck / Linux) on the next mount; a crash before publish leaves
 * the fs untouched. Checksum-v3 journals get the tag, descriptor-tail,
 * commit-block and journal-superblock checksums JBD2 defines; a block whose
 * first word is the JBD2 magic is stored escaped (first word zeroed). */
static void jt_commit(void){
    if(!g_jt_active) return;
    g_jt_active=0;
    int n=g_jt_n; if(n<=0) return;
    if(!g_jhas){ for(int i=0;i<n;i++) blk_write_direct(g_jt[i].blk,g_jt[i].data); return; }
    uint32_t tagsz = g_jcsum3 ? 16 : (g_jb64 ? 12 : 8);
    uint32_t jp_desc=bmap_read(g_jin,g_jfirst);
    uint8_t seqbe[4]; wbe32(seqbe, g_jseq);
    /* descriptor block: header + one tag per buffered block (the first tag
     * carries the UUID, the rest set SAME_UUID; the last sets LAST_TAG) */
    mem_zero(g_jdesc,g_blk_size);
    wbe32(g_jdesc+0,JBD2_MAGIC); wbe32(g_jdesc+4,JBD2_DESCRIPTOR_BLOCK); wbe32(g_jdesc+8,g_jseq);
    uint32_t off=12;
    for(int i=0;i<n;i++){
        uint32_t flags=0; if(i>0) flags|=JBD2_FLAG_SAME_UUID; if(i==n-1) flags|=JBD2_FLAG_LAST_TAG;
        const uint8_t* d=g_jt[i].data;
        int esc = (be32(d)==JBD2_MAGIC);
        if(esc) flags|=JBD2_FLAG_ESCAPE;
        uint8_t* t=g_jdesc+off;
        wbe32(t+0,g_jt[i].blk);
        if(g_jcsum3){
            wbe32(t+4,flags); wbe32(t+8,0);
            /* tag checksum: crc32c(seed, be32 sequence), then the block as stored */
            uint32_t c=e2_crc32c(g_jcsum_seed, seqbe, 4);
            if(esc){ uint8_t z[4]={0,0,0,0}; c=e2_crc32c(c,z,4); c=e2_crc32c(c,d+4,g_blk_size-4); }
            else c=e2_crc32c(c,d,g_blk_size);
            wbe32(t+12,c);
        } else {
            wbe16(t+4,0); wbe16(t+6,(uint16_t)flags);
            if(g_jb64) wbe32(t+8,0);
        }
        off+=tagsz;
        if(i==0){ mem_copy(g_jdesc+off,g_juuid,16); off+=16; }
    }
    if(g_jcsum3) wbe32(g_jdesc+g_blk_size-4, e2_crc32c(g_jcsum_seed,g_jdesc,g_blk_size));
    if(jp_desc) blk_write_direct(jp_desc,g_jdesc);
    /* data blocks follow the descriptor (escaped ones with a zero first word) */
    for(int i=0;i<n;i++){
        uint32_t jp=bmap_read(g_jin,g_jfirst+1+i); if(!jp) continue;
        if(be32(g_jt[i].data)==JBD2_MAGIC){
            mem_copy(g_jdata,g_jt[i].data,g_blk_size); wbe32(g_jdata,0); blk_write_direct(jp,g_jdata);
        } else blk_write_direct(jp,g_jt[i].data);
    }
    /* [M27b test] crash BEFORE the commit block: txn never becomes valid -> the
     * next mount sees no committed transaction -> filesystem stays in the OLD state. */
    if(g_crash_mode==1){ g_blk_dead=1; debugcon_writestring("[M27B] CRASH before commit\n"); }
    /* commit block (h_commit_sec at 48, h_chksum[0] at 16 for v2/v3) */
    mem_zero(g_jdesc,g_blk_size);
    wbe32(g_jdesc+0,JBD2_MAGIC); wbe32(g_jdesc+4,JBD2_COMMIT_BLOCK); wbe32(g_jdesc+8,g_jseq);
    { uint64_t now=e2_now(); wbe32(g_jdesc+48,(uint32_t)(now>>32)); wbe32(g_jdesc+52,(uint32_t)now); }
    if(g_jcsum3) wbe32(g_jdesc+16, e2_crc32c(g_jcsum_seed,g_jdesc,g_blk_size));
    uint32_t jp_commit=bmap_read(g_jin,g_jfirst+1+n); if(jp_commit) blk_write_direct(jp_commit,g_jdesc);
    /* PUBLISH: journal sb s_start = descriptor block, s_sequence = this txn */
    uint32_t jp_sb=bmap_read(g_jin,0);
    if(jread(0,g_jdesc)==0){ wbe32(g_jdesc+28,g_jfirst); wbe32(g_jdesc+24,g_jseq); jsb_finalize_csum(g_jdesc); if(jp_sb) blk_write_direct(jp_sb,g_jdesc); }
    /* [M27b test] crash AFTER publish, before checkpoint completes: the committed
     * txn is durable in the journal -> the next mount REPLAYS it -> NEW state. */
    if(g_crash_mode==2){ g_blk_dead=1; debugcon_writestring("[M27B] CRASH after publish\n"); }
    /* CHECKPOINT: write the buffered metadata to its final in-place locations */
    for(int i=0;i<n;i++) blk_write_direct(g_jt[i].blk,g_jt[i].data);
    /* RETIRE: journal sb s_start = 0, s_sequence = next */
    g_jseq++;
    if(jread(0,g_jdesc)==0){ wbe32(g_jdesc+28,0); wbe32(g_jdesc+24,g_jseq); jsb_finalize_csum(g_jdesc); if(jp_sb) blk_write_direct(jp_sb,g_jdesc); }
    g_jt_n=0;
}

/* One ext2/ext4 volume at a time: the driver keeps its state in globals, so a
 * second mount while the first is still in the VFS would retarget the live one
 * at another device. */
static int e2_busy(void){ return vfs_mount_point_of(&ext2_ops)!=NULL; }

/* [M46] Options of the next ext2_mount (mount(2) data): "ro" / "rw" /
 * "defaults", and "noload" / "norecovery" (ro only: do not replay the journal).
 * Like Linux, a plain read-only mount still replays a dirty journal (the only
 * write it makes): the committed transactions are part of the filesystem, and
 * reading it without them would show a stale, possibly inconsistent tree.
 * The FAT-style owner options (uid=, umask=, ...) are accepted and ignored:
 * ext2/ext4 keep real owners. Returns 0, or -1 for a malformed list. */
static int g_e2opt_ro, g_e2opt_noload;
static int g_noload_dirty;           /* mounted noload with a journal left unreplayed */
static int e2_opt_tok(const char** pp, const char* k){
    const char* p = *pp;
    while(*k){ if(*p != *k) return 0; p++; k++; }
    if(*p && *p != ',') return 0;
    *pp = p; return 1;
}
int ext2_set_mount_options(const char* opts){
    char rest[128]; size_t n = 0;
    int ro = 0, noload = 0;
    const char* p = opts ? opts : "";
    while(*p){
        if(e2_opt_tok(&p, "noload") || e2_opt_tok(&p, "norecovery")) noload = 1;
        else {                              /* anything else: the shared parser */
            if(n) rest[n++] = ',';
            while(*p && *p != ','){ if(n >= sizeof(rest) - 2) return -1; rest[n++] = *p++; }
        }
        if(*p == ',') p++;
    }
    rest[n] = 0;
    mntopts_t o;
    if(mntopts_parse(rest, &o) != 0) return -1;
    ro = o.ro;
    if(noload && !ro) return -1;            /* noload needs ro (Linux: same) */
    g_e2opt_ro = ro; g_e2opt_noload = noload;
    return 0;
}

/* Validate, replay the journal, re-validate and capture the journal geometry.
 * Returns 0, -1 (no ext superblock) or -3 (refused; g_reject says why). */
static int e2_open_volume(const char* dev_name){
    block_dev_t* dev=block_find(dev_name);
    g_ro = 0;                            /* reads/replay below must not be refused */
    int r=ext2_read_super(dev);
    if(r!=0) return r;
    g_noload_dirty = 0;
    if(g_e2opt_noload){                  /* [M46] ro,noload: leave the journal alone */
        uint8_t sbsec[1024];
        if(block_read(g_dev, 2, sbsec, 2)==2 && (rd32(sbsec+96) & 0x4u)){
            g_noload_dirty = 1;
            debugcon_writestring("[EXT2] noload: journal NOT replayed (needs recovery; read-only view may be stale)\n");
        }
    } else if(jbd2_recover()<0){                         /* [M27a] */
        g_reject="journal recovery failed (corrupt journal)"; g_mounted=0; return -3;
    }
    r=ext2_read_super(dev);                              /* re-read SB (recovery cleared flags) */
    if(r!=0) return r;
    jt_init();                                           /* [M27b] capture journal geometry */
    return 0;
}

int ext2_mount(const char* dev_name, const char* mount_point){
    if(e2_busy()){ debugcon_writestring("[EXT2] an ext2/ext4 volume is already mounted\n"); return -1; }
    int r=e2_open_volume(dev_name);
    if(r==-3){ debugcon_writestring("[EXT2] refusing "); debugcon_writestring(dev_name);
               debugcon_writestring(": "); debugcon_writestring(g_reject); debugcon_writestring("\n"); }
    int want_ro = g_e2opt_ro;
    g_e2opt_ro = 0; g_e2opt_noload = 0;  /* options are for this mount only */
    if(r!=0) return -1;
    /* [M46] -o ro, or a device without a write path: read-only from here on. */
    g_ro = (want_ro || !g_dev || !g_dev->write) ? 1 : 0;
    ext2_ops.flags = g_ro ? VFS_FS_RDONLY : 0;
    if(g_ro) debugcon_writestring("[EXT2] mounted read-only\n");
    return vfs_mount(mount_point, &ext2_ops, "ext2");
}

/* [M23] Mount an ext2/ext4 volume as the persistent VFS root, but only if it is
 * a SecOS system disk — identified by the marker file "/.secosroot" at its root
 * (so a plain ext2 *data* disk is never silently grabbed as the root).
 * Returns 0 when mounted as root, -1 when the device holds no ext2/3/4
 * filesystem, -2 for an ext volume without the marker (a data disk), and -3 for
 * an ext volume this driver refuses (unsupported features, bad checksums,
 * errors, corrupt journal: ext2_last_error() says why). Nothing is written
 * unless the volume passes validation; nothing is ever formatted.
 * [M46] The marker is looked up BEFORE the journal is replayed, so probing a
 * data disk at boot writes nothing to it (its journal stays for the mount
 * that actually uses it, e.g. a later `mount -o ro,noload`). The marker is
 * made with the disk and never rewritten, so the unreplayed tree has it. */
int ext2_mount_root(const char* dev_name){
    if(e2_busy()) return -1;
    g_ro=0; g_e2opt_noload=0;
    int r=ext2_read_super(block_find(dev_name));
    if(r!=0) return r;
    g_vused=0;
    if(!e2_lookup("/.secosroot")) return -2;     /* ext2 but not a SecOS root */
    g_vused=0;
    r=e2_open_volume(dev_name);                  /* now replay + re-validate */
    if(r!=0) return r;
    g_vused=0;
    return vfs_mount_root(&ext2_ops, "ext2")==0 ? 0 : -3;
}

/* Boot report: does the mounted volume carry metadata checksums? */
int ext2_has_csum(void){ return g_mounted && g_csum; }

/* [M46] mount -o remount,ro|rw. Every metadata transaction is committed and
 * checkpointed when its operation returns, so going read-only only has to
 * retire the journal (e2_sync). Going read-write needs a writable device and a
 * replayed journal (a noload mount must be unmounted and mounted again). */
static int e2_remount(int ro){
    if(!g_mounted) return -22;
    if(ro){
        if(!g_ro) e2_sync();
        g_ro = 1;
    } else {
        if(!g_dev || !g_dev->write || g_noload_dirty) return -30;
        g_ro = 0;
    }
    ext2_ops.flags = g_ro ? VFS_FS_RDONLY : 0;
    debugcon_writestring(g_ro ? "[EXT2] remounted read-only\n" : "[EXT2] remounted read-write\n");
    return 0;
}
