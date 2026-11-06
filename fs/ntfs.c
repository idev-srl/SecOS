/*
 * SecOS Kernel - NTFS read-only filesystem
 *
 * Written from public format documentation only (no GPL code was used):
 *   - the Linux-NTFS project "NTFS Documentation" (Richard Russon, Yuval Fledel)
 *     and the flatcap.org NTFS pages: boot sector, MFT records + update
 *     sequence arrays, attribute headers, run lists, $ATTRIBUTE_LIST,
 *     $FILE_NAME namespaces, $INDEX_ROOT / $INDEX_ALLOCATION / $BITMAP,
 *     $UpCase, $Volume flags, reparse points;
 *   - Microsoft [MS-XCA] "Xpress Compression Algorithm", section 2.5 (LZNT1);
 *   - Microsoft [MS-FSCC] "File System Control Codes", sections 2.1.2
 *     (reparse tags, symbolic link / mount point reparse data buffers).
 *
 * Supports: boot sector (+ the backup copy at the end of the volume), $MFT
 * bootstrap with $MFTMirr cross-check (a damaged one of records 0..3 is
 * served from the mirror), fixups, resident and non-resident attributes,
 * sparse runs, $ATTRIBUTE_LIST (fragmented files and the fragmented $MFT
 * itself), Win32/POSIX names (DOS 8.3 duplicates hidden), UTF-16 <-> UTF-8,
 * case-insensitive lookups through $UpCase, directory B+ trees with INDX
 * fixups and the $BITMAP, LZNT1-compressed files, symlinks and junctions
 * (reparse points, WSL and Interix symlinks) as VFS symlinks, NT times.
 *
 * Read-only: every mutating operation returns -EROFS. Encrypted (EFS) files
 * return -EACCES from read; other reparse-backed files (WOF, dedup, cloud
 * placeholders...) return -EIO, as do corrupt objects.
 *
 * Every on-disk value is untrusted: record and attribute sizes/offsets, run
 * lists (no overflow, no LCN outside the volume), attribute-list entries,
 * index headers and entries, B+ tree depth, and loops (a block revisited
 * during a directory walk) are all checked; a bad object yields an error,
 * never a fault or a hang.
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
#include "mntopts.h"
#include <stdint.h>
#include <stddef.h>

/* ── errno values returned to the VFS / syscall layer ────────────────────── */
#define NT_ENOENT  2
#define NT_EIO     5
#define NT_EACCES  13
#define NT_EROFS   30

/* ── On-disk constants ───────────────────────────────────────────────────── */
#define AT_STD_INFO     0x10u
#define AT_ATTR_LIST    0x20u
#define AT_FILE_NAME    0x30u
#define AT_VOLUME_INFO  0x70u
#define AT_DATA         0x80u
#define AT_INDEX_ROOT   0x90u
#define AT_INDEX_ALLOC  0xA0u
#define AT_BITMAP       0xB0u
#define AT_REPARSE      0xC0u
#define AT_END          0xFFFFFFFFu

#define AF_COMPRESS_MASK 0x00FFu
#define AF_COMPRESS_LZNT1 0x0001u
#define AF_ENCRYPTED    0x4000u
#define AF_SPARSE       0x8000u

#define FA_READONLY     0x0001u
#define FA_SYSTEM       0x0004u
#define FA_REPARSE      0x0400u
#define FA_ENCRYPTED    0x4000u
#define FA_DIR_INDEX    0x10000000u   /* $FILE_NAME: "has a $I30 index" */

#define MR_IN_USE       0x0001u
#define MR_IS_DIR       0x0002u

#define IE_SUBNODE      0x0001u
#define IE_LAST         0x0002u

#define NS_POSIX        0
#define NS_WIN32        1
#define NS_DOS          2
#define NS_WIN32_DOS    3

#define RT_MOUNT_POINT  0xA0000003u
#define RT_SYMLINK      0xA000000Cu
#define RT_LX_SYMLINK   0xA000001Du

#define VI_DIRTY        0x0001u

#define REC_ROOT        5u
#define REC_VOLUME      3u
#define REC_BITMAP      6u
#define REC_UPCASE      10u
#define REC_FIRST_USER  24u           /* records below are metafiles / reserved */

#define NTFS_MAGIC      0x5346544Eu   /* Linux NTFS_SB_MAGIC */

/* ── Limits (corruption guards) ──────────────────────────────────────────── */
#define NT_MAX_DEPTH    16            /* B+ tree levels */
#define NT_MAX_RUNS     (1u << 18)    /* runs per attribute */
#define NT_MAX_ALIST    (256u * 1024u)/* $ATTRIBUTE_LIST bytes */
#define NT_MAX_EXTENTS  64u           /* extents of one attribute in one record */
#define NT_MAX_BMP      (4u << 20)    /* directory $BITMAP bytes */
#define NT_MAX_REPARSE  16384u
#define NT_SEARCH_BUDGET 4096u        /* index blocks one lookup may read */
#define NT_LINK_MAX     255u

#define NT_RCACHE       32            /* MFT records cached per volume */
#define NT_ICACHE       16            /* index blocks cached per volume */

#define NT_VCHUNK       64
#define NT_VCHUNKS_MAX  1024          /* 65536 distinct files per mount */
#define NT_HASH         1024

/* ── Little-endian helpers ───────────────────────────────────────────────── */
static uint16_t rd16(const uint8_t* p){ return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p){ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t* p){ return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static void mzero(void* d, size_t n){ uint8_t* p = (uint8_t*)d; while(n--) *p++ = 0; }
static void mcopy(void* d, const void* s, size_t n){ uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while(n--) *a++ = *b++; }
static int  mcmp(const void* a, const void* b, size_t n){ const uint8_t* x = a; const uint8_t* y = b; for(size_t i = 0; i < n; i++) if(x[i] != y[i]) return 1; return 0; }
static size_t slen(const char* s){ size_t n = 0; while(s[n]) n++; return n; }
static int is_pow2(uint64_t x){ return x && !(x & (x - 1)); }
static uint32_t log2u(uint64_t x){ uint32_t s = 0; while(x > 1){ x >>= 1; s++; } return s; }

static void nt_log(const char* m){ debugcon_writestring("[NTFS] "); debugcon_writestring(m); debugcon_writestring("\n"); }
static void nt_logx(const char* m, uint64_t v){ debugcon_writestring("[NTFS] "); debugcon_writestring(m); debugcon_print_hex(v); debugcon_writestring("\n"); }

/* ── Attribute (decoded) ─────────────────────────────────────────────────── */
typedef struct { uint64_t vcn, len; int64_t lcn; } nt_run_t;   /* lcn -1 = sparse */

typedef struct {
    int       present, resident;
    uint16_t  flags;                 /* attribute flags (compressed/encrypted/sparse) */
    uint8_t   cu_shift;              /* log2 clusters per compression unit */
    uint64_t  data_size, init_size, alloc_size;
    uint8_t*  rdata; uint32_t rlen;  /* resident value (kmalloc'd copy) */
    nt_run_t* runs; uint32_t nruns, cap;
    uint64_t  mapped;                /* VCNs covered by runs */
    uint32_t  cur;                   /* run cursor */
} nt_attr_t;

static void attr_free(nt_attr_t* a){
    if(a->rdata) kfree(a->rdata);
    if(a->runs) kfree(a->runs);
    mzero(a, sizeof(*a));
}

/* ── Volume / node state ─────────────────────────────────────────────────── */
struct nt_vol;
enum { NK_FILE = 1, NK_DIR = 2, NK_LINK = 3 };

typedef struct nt_node {
    struct nt_vol* v;
    uint32_t gen;
    uint64_t rec; uint16_t seq;
    uint8_t  kind, bad, enc, unsup, data_ok;
    uint32_t fattr, nlink;
    uint64_t t_mod, t_acc, t_chg;
    nt_attr_t data;                  /* unnamed $DATA (runs loaded on first read) */
    uint8_t* iroot; uint32_t iroot_len;
    nt_attr_t ia;                    /* $INDEX_ALLOCATION:$I30 */
    uint8_t* ibmp; uint64_t ibmp_len;/* $BITMAP:$I30 */
    char*    link;                   /* symlink target (UTF-8) */
    int32_t  hnext;
} nt_node_t;
typedef struct { vfs_inode_t vi; nt_node_t n; } nt_slot_t;

typedef struct nt_vol {
    int       idx, mounted;
    uint32_t  gen;
    block_dev_t* dev;
    uint32_t  dss;                   /* device sector size */
    uint32_t  bps;                   /* NTFS bytes per sector */
    uint32_t  cb, cb_shift;          /* cluster size */
    uint64_t  nclus, vol_bytes;
    uint32_t  rec_size, idx_size, idx_shift, ivcn_shift;
    uint64_t  mft_lcn, mirr_lcn;
    nt_attr_t mft;                   /* $MFT unnamed $DATA */
    uint64_t  mft_nrec;
    uint8_t*  mirr[4]; int mirr_use[4];
    uint16_t* up;                    /* $UpCase, 65536 entries */
    uint8_t   ver_major, ver_minor;
    uint16_t  vflags;
    int       hibernated, from_backup;
    uint64_t  free_clus; int free_known;
    uint32_t  corrupt_logged;
    char      mp[256], devname[32];
    /* caches */
    uint8_t*  rc_buf; uint64_t rc_num[NT_RCACHE]; uint32_t rc_age[NT_RCACHE]; uint8_t rc_val[NT_RCACHE];
    uint8_t*  ic_buf; uint64_t ic_rec[NT_ICACHE], ic_vcn[NT_ICACHE]; uint32_t ic_age[NT_ICACHE]; uint8_t ic_val[NT_ICACHE];
    uint32_t  tick;
    uint8_t*  lk_buf[NT_MAX_DEPTH];  /* lookup descent buffers */
    uint8_t*  cu_in; uint8_t* cu_out;/* compression unit buffers (64 KiB each) */
    nt_node_t* cu_node; uint64_t cu_idx; uint32_t cu_gen;
    /* inode pool */
    nt_slot_t* chunk[NT_VCHUNKS_MAX];
    int32_t   nslots;
    int32_t   hash[NT_HASH];
    nt_node_t* root;
    vfs_fs_ops_t ops;
    mntopts_t opt;                   /* owner/masks (NTFS ACLs are not mapped) */
} nt_vol_t;

static nt_vol_t g_vol[NTFS_MAX_VOLUMES];
static uint32_t g_gen_ctr;
static uint8_t  g_bounce[4096];

static int vol_attached(nt_vol_t* v){ return v->mounted && vfs_mount_point_of(&v->ops) != NULL; }

static void log_corrupt(nt_vol_t* v, const char* what, uint64_t val){
    if(v->corrupt_logged >= 32) return;
    v->corrupt_logged++;
    debugcon_writestring("[NTFS] corrupt: "); debugcon_writestring(what);
    debugcon_writestring(" "); debugcon_print_hex(val); debugcon_writestring("\n");
}

/* ── Device I/O ──────────────────────────────────────────────────────────── */
/* Read `len` bytes at byte offset `off` of the volume. Aligned spans go
 * straight to the caller's buffer; ragged edges through a bounce sector. */
static int dev_read(nt_vol_t* v, uint64_t off, void* buf, uint64_t len){
    if(off > v->vol_bytes || len > v->vol_bytes - off) return -1;
    uint8_t* out = (uint8_t*)buf;
    uint32_t ss = v->dss;
    while(len){
        uint64_t lba = off / ss;
        uint32_t in = (uint32_t)(off % ss);
        if(in == 0 && len >= ss){
            uint64_t n = len / ss;
            if(n > 128) n = 128;
            if(block_read(v->dev, lba, out, (uint32_t)n) != (int)n) return -1;
            out += n * ss; off += n * ss; len -= n * ss;
        } else {
            if(block_read(v->dev, lba, g_bounce, 1) != 1) return -1;
            uint64_t n = ss - in;
            if(n > len) n = len;
            mcopy(out, g_bounce + in, (size_t)n);
            out += n; off += n; len -= n;
        }
    }
    return 0;
}

/* ── Fixups (update sequence array) ──────────────────────────────────────── */
static int fixup(uint8_t* b, uint32_t size, const char* magic){
    if(b[0] != (uint8_t)magic[0] || b[1] != (uint8_t)magic[1] || b[2] != (uint8_t)magic[2] || b[3] != (uint8_t)magic[3]) return -1;
    if(size < 512 || (size & 511)) return -1;
    uint32_t uo = rd16(b + 4), uc = rd16(b + 6);
    if(uc != size / 512 + 1) return -1;
    if((uo & 1) || uo < 0x28 || uo + 2 * uc > 512) return -1;
    uint16_t usn = rd16(b + uo);
    for(uint32_t i = 1; i < uc; i++){
        uint32_t p = i * 512 - 2;
        if(rd16(b + p) != usn) return -1;           /* torn / corrupt sector */
        b[p] = b[uo + 2 * i]; b[p + 1] = b[uo + 2 * i + 1];
    }
    return 0;
}

/* Validate a fixed-up MFT record header. */
static int rec_check(nt_vol_t* v, const uint8_t* b, uint64_t recno){
    uint32_t uo = rd16(b + 4), uc = rd16(b + 6);
    uint32_t ao = rd16(b + 0x14), used = rd32(b + 0x18), alloc = rd32(b + 0x1C);
    if(alloc != v->rec_size || used > v->rec_size || used < 0x30) return -1;
    if(ao < uo + 2 * uc || (ao & 7) || ao + 8 > used) return -1;
    if(uo >= 0x30 && rd32(b + 0x2C) != (uint32_t)recno) return -1;   /* NTFS 3.1 self number */
    return 0;
}
/* Read + fixup + check a raw record copy (no cache, no mirror). */
static int rec_load_raw(nt_vol_t* v, uint8_t* b, uint64_t recno){
    if(fixup(b, v->rec_size, "FILE") != 0) return -1;
    return rec_check(v, b, recno);
}

/* ── Attribute iteration within one MFT record ───────────────────────────── */
typedef struct { const uint8_t* rec; uint32_t used, pos; int err; } ait_t;

static void ait_init(ait_t* it, const uint8_t* rec){
    it->rec = rec; it->used = rd32(rec + 0x18); it->pos = rd16(rec + 0x14); it->err = 0;
}
/* Next attribute header (validated) or NULL at the end / on corruption (err). */
static const uint8_t* ait_next(ait_t* it, uint32_t* alen){
    if(it->err) return NULL;
    if(it->pos + 4 > it->used){ it->err = 1; return NULL; }
    const uint8_t* a = it->rec + it->pos;
    uint32_t type = rd32(a);
    if(type == AT_END) return NULL;
    if(it->pos + 16 > it->used){ it->err = 1; return NULL; }
    uint32_t len = rd32(a + 4);
    if(len < 0x18 || (len & 7) || len > it->used - it->pos){ it->err = 1; return NULL; }
    uint32_t nonres = a[8], nl = a[9], no = rd16(a + 0x0A);
    if(nonres > 1 || (nl && (no < 0x10 || no + 2 * nl > len))){ it->err = 1; return NULL; }
    if(!nonres){
        uint32_t vl = rd32(a + 0x10), vo = rd16(a + 0x14);
        if(vo > len || vl > len - vo){ it->err = 1; return NULL; }
    } else {
        if(len < 0x40){ it->err = 1; return NULL; }
        uint32_t mpo = rd16(a + 0x20);
        if(mpo < 0x40 || mpo > len){ it->err = 1; return NULL; }
        if(rd64(a + 0x10) == 0){           /* sizes are valid on the first extent */
            uint64_t al = rd64(a + 0x28), ds = rd64(a + 0x30), is = rd64(a + 0x38);
            if(al > (1ULL << 60) || ds > al || is > ds){ it->err = 1; return NULL; }
        }
    }
    it->pos += len;
    *alen = len;
    return a;
}
static int attr_name_eq(const uint8_t* a, const uint16_t* name, uint32_t nlen){
    if(a[9] != nlen) return 0;
    const uint8_t* n = a + rd16(a + 0x0A);
    for(uint32_t i = 0; i < nlen; i++) if(rd16(n + 2 * i) != name[i]) return 0;
    return 1;
}

/* ── Run lists ───────────────────────────────────────────────────────────── */
static int runs_push(nt_attr_t* a, uint64_t vcn, uint64_t len, int64_t lcn){
    if(a->nruns && lcn < 0 && a->runs[a->nruns - 1].lcn < 0){ a->runs[a->nruns - 1].len += len; return 0; }
    if(a->nruns == a->cap){
        if(a->cap >= NT_MAX_RUNS) return -1;
        uint32_t nc = a->cap ? a->cap * 2 : 16;
        nt_run_t* nr = (nt_run_t*)kmalloc(sizeof(nt_run_t) * nc);
        if(!nr) return -1;
        if(a->runs){ mcopy(nr, a->runs, sizeof(nt_run_t) * a->nruns); kfree(a->runs); }
        a->runs = nr; a->cap = nc;
    }
    a->runs[a->nruns].vcn = vcn; a->runs[a->nruns].len = len; a->runs[a->nruns].lcn = lcn;
    a->nruns++;
    return 0;
}

/* Decode the mapping pairs of one non-resident extent; it must start exactly
 * where the previous one ended (a->mapped) and end at its HighestVcn + 1. */
static int runs_decode(nt_vol_t* v, const uint8_t* ah, uint32_t alen, nt_attr_t* a){
    uint64_t lowest = rd64(ah + 0x10), highest = rd64(ah + 0x18);
    uint64_t vmax = (1ULL << 62) >> v->cb_shift;
    if(lowest != a->mapped) return -1;
    if(highest + 1 < lowest || highest + 1 > vmax) return -1;     /* also catches -1 on empty */
    uint32_t p = rd16(ah + 0x20);
    uint64_t vcn = lowest;
    int64_t lcn = 0;
    while(p < alen){
        uint8_t h = ah[p];
        if(h == 0) break;
        uint32_t lb = h & 0x0F, ob = h >> 4;
        if(lb == 0 || lb > 8 || ob > 8 || p + 1 + lb + ob > alen) return -1;
        uint64_t len = 0;
        for(uint32_t i = 0; i < lb; i++) len |= (uint64_t)ah[p + 1 + i] << (8 * i);
        if((ah[p + lb] & 0x80) || len == 0) return -1;            /* negative / empty run */
        int64_t rl = -1;
        if(ob){
            uint64_t d = 0;
            for(uint32_t i = 0; i < ob; i++) d |= (uint64_t)ah[p + 1 + lb + i] << (8 * i);
            if(ob < 8 && (ah[p + lb + ob] & 0x80)) d |= ~0ULL << (8 * ob);
            int64_t delta = (int64_t)d;
            if(delta > (int64_t)v->nclus || delta < -(int64_t)v->nclus) return -1;
            lcn += delta;
            if(lcn < 0 || (uint64_t)lcn >= v->nclus || len > v->nclus - (uint64_t)lcn) return -1;
            rl = lcn;
        }
        if(len > vmax - vcn) return -1;
        if(runs_push(a, vcn, len, rl) != 0) return -1;
        vcn += len;
        p += 1 + lb + ob;
    }
    if(vcn != highest + 1) return -1;
    a->mapped = vcn;
    return 0;
}

static nt_run_t* run_find(nt_attr_t* a, uint64_t vcn){
    if(!a->nruns) return NULL;
    uint32_t c = a->cur < a->nruns ? a->cur : 0;
    for(int k = 0; k < 2 && c < a->nruns; k++, c++){
        nt_run_t* r = &a->runs[c];
        if(vcn >= r->vcn && vcn - r->vcn < r->len){ a->cur = c; return r; }
    }
    uint32_t lo = 0, hi = a->nruns;
    while(lo < hi){
        uint32_t mid = lo + (hi - lo) / 2;
        nt_run_t* r = &a->runs[mid];
        if(vcn < r->vcn) hi = mid;
        else if(vcn - r->vcn >= r->len) lo = mid + 1;
        else { a->cur = mid; return r; }
    }
    return NULL;
}

/* Read [off, off+len) of a non-compressed attribute value. Bytes past the
 * initialized size and sparse runs read as zero; an unmapped VCN is -EIO. */
static int attr_read_plain(nt_vol_t* v, nt_attr_t* a, uint64_t off, uint8_t* buf, uint64_t len){
    if(a->resident){
        if(off > a->rlen || len > a->rlen - off) return -NT_EIO;
        mcopy(buf, a->rdata + off, (size_t)len);
        return 0;
    }
    if(off > a->data_size || len > a->data_size - off) return -NT_EIO;
    while(len){
        if(off >= a->init_size){ mzero(buf, (size_t)len); return 0; }
        uint64_t vcn = off >> v->cb_shift;
        uint64_t in = off & (v->cb - 1);
        nt_run_t* r = run_find(a, vcn);
        if(!r) return -NT_EIO;
        uint64_t left = r->len - (vcn - r->vcn);                  /* clusters to run end */
        uint64_t n = len;
        if(left < ((len + in + v->cb - 1) >> v->cb_shift)) n = (left << v->cb_shift) - in;
        if(n > a->init_size - off) n = a->init_size - off;
        if(r->lcn < 0) mzero(buf, (size_t)n);
        else {
            uint64_t pos = (((uint64_t)r->lcn + (vcn - r->vcn)) << v->cb_shift) + in;
            if(dev_read(v, pos, buf, n) != 0) return -NT_EIO;
        }
        buf += n; off += n; len -= n;
    }
    return 0;
}

/* ── MFT record access ───────────────────────────────────────────────────── */
static int mft_read(nt_vol_t* v, uint64_t recno, uint8_t* out){
    if(recno >= v->mft_nrec) return -1;
    if(recno < 4 && v->mirr_use[recno]){ mcopy(out, v->mirr[recno], v->rec_size); return 0; }
    for(int i = 0; i < NT_RCACHE; i++){
        if(v->rc_val[i] && v->rc_num[i] == recno){
            v->rc_age[i] = ++v->tick;
            mcopy(out, v->rc_buf + (size_t)i * v->rec_size, v->rec_size);
            return 0;
        }
    }
    if(attr_read_plain(v, &v->mft, recno * v->rec_size, out, v->rec_size) != 0) return -1;
    if(rec_load_raw(v, out, recno) != 0) return -1;
    int s = 0;
    for(int i = 0; i < NT_RCACHE; i++){
        if(!v->rc_val[i]){ s = i; break; }
        if(v->rc_age[i] < v->rc_age[s]) s = i;
    }
    mcopy(v->rc_buf + (size_t)s * v->rec_size, out, v->rec_size);
    v->rc_num[s] = recno; v->rc_age[s] = ++v->tick; v->rc_val[s] = 1;
    return 0;
}

/* Take the resident value / first-extent sizes of attribute header `ah`. */
static int attr_take_first(nt_attr_t* out, const uint8_t* ah, uint32_t rmax){
    out->present = 1;
    out->flags = rd16(ah + 0x0C);
    if(!ah[8]){
        uint32_t vl = rd32(ah + 0x10), vo = rd16(ah + 0x14);
        if(vl > rmax) return -1;
        out->resident = 1;
        out->rdata = (uint8_t*)kmalloc(vl ? vl : 1);
        if(!out->rdata) return -1;
        mcopy(out->rdata, ah + vo, vl);
        out->rlen = vl;
        out->data_size = out->init_size = out->alloc_size = vl;
        return 0;
    }
    out->resident = 0;
    out->alloc_size = rd64(ah + 0x28);
    out->data_size = rd64(ah + 0x30);
    out->init_size = rd64(ah + 0x38);
    out->cu_shift = ah[0x22];
    return 0;
}

/* Find attribute (type, name) of base record `recno` (its fixed-up copy in
 * `base`), following $ATTRIBUTE_LIST into extension records. With want_runs
 * the whole run list is decoded (all extents, contiguity checked); without,
 * only the sizes/flags of the first extent are taken. 0, -2 absent, -1 bad. */
static int attr_find(nt_vol_t* v, uint64_t recno, const uint8_t* base, uint32_t type,
                     const uint16_t* name, uint32_t nlen, nt_attr_t* out, int want_runs, uint32_t rmax){
    mzero(out, sizeof(*out));
    ait_t it; uint32_t alen; const uint8_t* a;
    const uint8_t* al = NULL; uint32_t al_len = 0;
    ait_init(&it, base);
    while((a = ait_next(&it, &alen)) != NULL) if(rd32(a) == AT_ATTR_LIST){ al = a; al_len = alen; break; }
    if(it.err) return -1;

    if(!al){
        /* All extents live in the base record: collect them in VCN order. */
        uint32_t pos[NT_MAX_EXTENTS], plen[NT_MAX_EXTENTS], n = 0;
        ait_init(&it, base);
        while((a = ait_next(&it, &alen)) != NULL){
            if(rd32(a) != type || !attr_name_eq(a, name, nlen)) continue;
            if(n >= NT_MAX_EXTENTS) return -1;
            pos[n] = (uint32_t)(a - base); plen[n] = alen; n++;
        }
        if(it.err) return -1;
        if(!n) return -2;
        for(uint32_t i = 1; i < n; i++){                            /* insertion sort */
            for(uint32_t j = i; j > 0 && rd64(base + pos[j] + 0x10) < rd64(base + pos[j - 1] + 0x10); j--){
                uint32_t t = pos[j]; pos[j] = pos[j - 1]; pos[j - 1] = t;
                t = plen[j]; plen[j] = plen[j - 1]; plen[j - 1] = t;
            }
        }
        const uint8_t* f = base + pos[0];
        if(!f[8] && n > 1) return -1;                                /* resident: one only */
        if(f[8] && rd64(f + 0x10) != 0) return -1;
        if(attr_take_first(out, f, rmax) != 0){ attr_free(out); return -1; }
        if(out->resident || !want_runs) return 0;
        for(uint32_t i = 0; i < n; i++){
            const uint8_t* e = base + pos[i];
            if(!e[8] || runs_decode(v, e, plen[i], out) != 0){ attr_free(out); return -1; }
        }
        return 0;
    }

    /* $ATTRIBUTE_LIST: its value (resident or not) lists every extent. */
    uint8_t* list = NULL; uint32_t llen = 0;
    if(!al[8]){
        llen = rd32(al + 0x10);
        if(llen > NT_MAX_ALIST) return -1;
        list = (uint8_t*)kmalloc(llen ? llen : 1);
        if(!list) return -1;
        mcopy(list, al + rd16(al + 0x14), llen);
    } else {
        nt_attr_t la; mzero(&la, sizeof(la));
        if(rd64(al + 0x10) != 0 || attr_take_first(&la, al, 0) != 0 || la.data_size > NT_MAX_ALIST ||
           (la.flags & (AF_COMPRESS_MASK | AF_ENCRYPTED)) || runs_decode(v, al, al_len, &la) != 0){ attr_free(&la); return -1; }
        llen = (uint32_t)la.data_size;
        list = (uint8_t*)kmalloc(llen ? llen : 1);
        if(!list || attr_read_plain(v, &la, 0, list, llen) != 0){ attr_free(&la); if(list) kfree(list); return -1; }
        attr_free(&la);
    }
    uint8_t* ext = (uint8_t*)kmalloc(v->rec_size);
    if(!ext){ kfree(list); return -1; }
    int rc = -2, first = 1;
    uint32_t p = 0;
    while(p + 0x1A <= llen){
        const uint8_t* e = list + p;
        uint32_t el = rd16(e + 4), enl = e[6], eno = e[7];
        if(el < 0x1A || el > llen - p || (enl && eno + 2 * enl > el)){ rc = -1; break; }
        p += el;
        if(rd32(e) != type || enl != nlen) continue;
        int match = 1;
        for(uint32_t i = 0; i < nlen; i++) if(rd16(e + eno + 2 * i) != name[i]) { match = 0; break; }
        if(!match) continue;
        uint64_t elow = rd64(e + 8), ref = rd64(e + 0x10);
        uint64_t erec = ref & 0xFFFFFFFFFFFFULL; uint16_t eseq = (uint16_t)(ref >> 48), einst = rd16(e + 0x18);
        const uint8_t* r = base;
        if(erec != recno){
            if(mft_read(v, erec, ext) != 0){ rc = -1; break; }
            if(!(rd16(ext + 0x16) & MR_IN_USE) || (rd64(ext + 0x20) & 0xFFFFFFFFFFFFULL) != recno ||
               (eseq && rd16(ext + 0x10) != eseq)){ rc = -1; break; }
            r = ext;
        }
        ait_t i2; const uint8_t* x; uint32_t xl; const uint8_t* hit = NULL; uint32_t hl = 0;
        ait_init(&i2, r);
        while((x = ait_next(&i2, &xl)) != NULL){
            if(rd32(x) == type && rd16(x + 0x0E) == einst && attr_name_eq(x, name, nlen)){ hit = x; hl = xl; break; }
        }
        if(i2.err || !hit){ rc = -1; break; }
        if(first){
            if((hit[8] && rd64(hit + 0x10) != 0) || attr_take_first(out, hit, rmax) != 0){ rc = -1; break; }
            first = 0; rc = 0;
            if(out->resident || !want_runs) break;
        } else if(!hit[8]){ rc = -1; break; }                       /* resident extent after the first */
        if(rd64(hit + 0x10) != elow || runs_decode(v, hit, hl, out) != 0){ rc = -1; break; }
    }
    kfree(ext); kfree(list);
    if(rc != 0) attr_free(out);
    return rc;
}

/* ── LZNT1 ([MS-XCA] 2.5) ────────────────────────────────────────────────── */
/* Decompress one compression unit. Each chunk decodes to at most 4 KiB; a
 * short chunk is zero-padded to 4 KiB, a zero header ends the unit. */
static int lznt1_decompress(const uint8_t* src, uint32_t slen, uint8_t* dst, uint32_t dlen){
    uint32_t sp = 0, dp = 0;
    while(dp < dlen && sp + 2 <= slen){
        uint32_t h = rd16(src + sp);
        sp += 2;
        if(h == 0) break;
        uint32_t csz = (h & 0x0FFF) + 1;
        if(csz > slen - sp) return -1;
        uint32_t cend = sp + csz, cstart = dp;
        uint32_t cmax = dlen - dp < 4096 ? dlen : dp + 4096;
        if(!(h & 0x8000)){
            if(csz > cmax - dp) return -1;
            mcopy(dst + dp, src + sp, csz);
            dp += csz;
        } else {
            while(sp < cend){
                uint8_t tag = src[sp++];
                for(int bit = 0; bit < 8 && sp < cend; bit++){
                    if(!(tag & (1u << bit))){
                        if(dp >= cmax) return -1;
                        dst[dp++] = src[sp++];
                        continue;
                    }
                    if(sp + 2 > cend) return -1;
                    uint32_t t = rd16(src + sp);
                    sp += 2;
                    uint32_t pos = dp - cstart;
                    if(pos == 0) return -1;
                    uint32_t db = 4;
                    while((1u << db) < pos) db++;
                    uint32_t lbits = 16 - db;
                    uint32_t len = (t & ((1u << lbits) - 1)) + 3;
                    uint32_t disp = (t >> lbits) + 1;
                    if(disp > pos || len > cmax - dp) return -1;
                    for(uint32_t k = 0; k < len; k++){ dst[dp] = dst[dp - disp]; dp++; }
                }
            }
        }
        sp = cend;
        while(dp < cmax) dst[dp++] = 0;                              /* short chunk */
    }
    while(dp < dlen) dst[dp++] = 0;
    return 0;
}

/* Load compression unit `cu` of node n's data into v->cu_out. */
static int cu_load(nt_vol_t* v, nt_node_t* n, uint64_t cu){
    if(v->cu_node == n && v->cu_idx == cu && v->cu_gen == v->gen) return 0;
    v->cu_node = NULL;
    nt_attr_t* a = &n->data;
    uint32_t ncl = 1u << a->cu_shift, cub = ncl << v->cb_shift;
    uint64_t vcn0 = cu << a->cu_shift;
    uint32_t alloc = 0, seen_sparse = 0;
    for(uint32_t k = 0; k < ncl; k++){
        nt_run_t* r = run_find(a, vcn0 + k);
        if(!r) return -NT_EIO;
        if(r->lcn < 0) seen_sparse = 1;
        else if(seen_sparse) return -NT_EIO;                         /* data after the hole */
        else alloc++;
    }
    if(alloc == 0){ mzero(v->cu_out, cub); }
    else {
        uint8_t* dst = alloc == ncl ? v->cu_out : v->cu_in;
        for(uint32_t k = 0; k < alloc;){
            nt_run_t* r = run_find(a, vcn0 + k);
            if(!r || r->lcn < 0) return -NT_EIO;
            uint64_t left = r->len - (vcn0 + k - r->vcn);
            uint32_t take = (uint32_t)(left < alloc - k ? left : alloc - k);
            uint64_t pos = ((uint64_t)r->lcn + (vcn0 + k - r->vcn)) << v->cb_shift;
            if(dev_read(v, pos, dst + ((size_t)k << v->cb_shift), (uint64_t)take << v->cb_shift) != 0) return -NT_EIO;
            k += take;
        }
        if(alloc != ncl && lznt1_decompress(v->cu_in, alloc << v->cb_shift, v->cu_out, cub) != 0){
            log_corrupt(v, "LZNT1 stream, record", n->rec);
            return -NT_EIO;
        }
    }
    v->cu_node = n; v->cu_idx = cu; v->cu_gen = v->gen;
    return 0;
}

static int read_compressed(nt_vol_t* v, nt_node_t* n, uint64_t off, uint8_t* buf, uint64_t len){
    nt_attr_t* a = &n->data;
    if((a->flags & AF_COMPRESS_MASK) != AF_COMPRESS_LZNT1 || a->cu_shift == 0 || a->cu_shift > 4 || v->cb > 4096)
        return -NT_EIO;
    if(!v->cu_in){
        v->cu_in = (uint8_t*)kmalloc(65536); v->cu_out = (uint8_t*)kmalloc(65536);
        if(!v->cu_in || !v->cu_out) return -NT_EIO;
    }
    uint32_t cub = (1u << a->cu_shift) << v->cb_shift;
    while(len){
        if(off >= a->init_size){ mzero(buf, (size_t)len); return 0; }
        uint64_t cu = off / cub; uint32_t in = (uint32_t)(off % cub);
        uint64_t nb = cub - in;
        if(nb > len) nb = len;
        if(nb > a->init_size - off) nb = a->init_size - off;
        int r = cu_load(v, n, cu);
        if(r) return r;
        mcopy(buf, v->cu_out + in, (size_t)nb);
        buf += nb; off += nb; len -= nb;
    }
    return 0;
}

/* ── Names ───────────────────────────────────────────────────────────────── */
/* UTF-8 -> UTF-16. A 3-byte encoding of a lone surrogate (as produced by
 * utf16_to8 for unpaired surrogates on disk) is accepted so names round-trip. */
static int utf8_to16(const char* s, size_t n, uint16_t* out, uint32_t* olen){
    uint32_t o = 0;
    for(size_t i = 0; i < n;){
        uint32_t c = (uint8_t)s[i], need, cp;
        if(c < 0x80){ cp = c; need = 0; }
        else if((c & 0xE0) == 0xC0){ cp = c & 0x1F; need = 1; }
        else if((c & 0xF0) == 0xE0){ cp = c & 0x0F; need = 2; }
        else if((c & 0xF8) == 0xF0){ cp = c & 0x07; need = 3; }
        else return -1;
        if(i + need >= n && need) return -1;
        for(uint32_t k = 1; k <= need; k++){
            uint32_t cc = (uint8_t)s[i + k];
            if((cc & 0xC0) != 0x80) return -1;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) || (need == 3 && (cp < 0x10000 || cp > 0x10FFFF))) return -1;
        i += need + 1;
        if(cp >= 0x10000){
            if(o + 2 > 255) return -1;
            cp -= 0x10000;
            out[o++] = (uint16_t)(0xD800 + (cp >> 10));
            out[o++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            if(o + 1 > 255) return -1;
            out[o++] = (uint16_t)cp;
        }
    }
    *olen = o;
    return 0;
}
/* UTF-16 (raw little-endian bytes) -> UTF-8. Returns length or -1. */
static int utf16_to8(const uint8_t* in, uint32_t n, char* out, size_t cap){
    size_t o = 0;
    for(uint32_t i = 0; i < n; i++){
        uint32_t c = rd16(in + 2 * i);
        if(c >= 0xD800 && c < 0xDC00 && i + 1 < n){
            uint32_t d = rd16(in + 2 * i + 2);
            if(d >= 0xDC00 && d < 0xE000){ c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i++; }
        }
        if(c < 0x80){ if(o + 1 >= cap) return -1; out[o++] = (char)c; }
        else if(c < 0x800){ if(o + 2 >= cap) return -1; out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else if(c < 0x10000){ if(o + 3 >= cap) return -1; out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else { if(o + 4 >= cap) return -1; out[o++] = (char)(0xF0 | (c >> 18)); out[o++] = (char)(0x80 | ((c >> 12) & 0x3F)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = 0;
    return (int)o;
}
/* Case-insensitive collation (upcased UTF-16 code units, then length). */
static int name_cmp_ci(nt_vol_t* v, const uint16_t* a, uint32_t al, const uint8_t* b, uint32_t bl){
    uint32_t n = al < bl ? al : bl;
    for(uint32_t i = 0; i < n; i++){
        uint16_t x = v->up[a[i]], y = v->up[rd16(b + 2 * i)];
        if(x != y) return x < y ? -1 : 1;
    }
    return al == bl ? 0 : (al < bl ? -1 : 1);
}
static int name_eq_cs(const uint16_t* a, uint32_t al, const uint8_t* b, uint32_t bl){
    if(al != bl) return 0;
    for(uint32_t i = 0; i < al; i++) if(a[i] != rd16(b + 2 * i)) return 0;
    return 1;
}

/* NT time (100 ns since 1601) -> Unix seconds. */
static uint64_t nt2unix(uint64_t t){
    uint64_t s = t / 10000000ULL;
    return s > 11644473600ULL ? s - 11644473600ULL : 0;
}

/* ── Index structures ────────────────────────────────────────────────────── */
typedef struct {
    uint64_t ref, child;
    uint32_t elen, klen, flags;
    const uint8_t* key;                /* $FILE_NAME value (valid unless LAST) */
} nt_ie_t;

/* Entry range of the INDEX_HEADER at buf+hoff (buffer of blen bytes). */
static int ih_range(const uint8_t* buf, uint32_t blen, uint32_t hoff, uint32_t* es, uint32_t* ee){
    if(hoff > blen || blen - hoff < 16) return -1;
    uint32_t eo = rd32(buf + hoff), il = rd32(buf + hoff + 4);
    if(eo < 16 || (eo & 7) || eo > il || il > blen - hoff) return -1;
    *es = hoff + eo; *ee = hoff + il;
    return 0;
}
static int ie_parse(const uint8_t* buf, uint32_t p, uint32_t ee, nt_ie_t* e){
    if(p > ee || ee - p < 16) return -1;
    const uint8_t* x = buf + p;
    e->ref = rd64(x); e->elen = rd16(x + 8); e->klen = rd16(x + 10); e->flags = rd16(x + 12);
    if(e->elen < 16 || (e->elen & 7) || e->elen > ee - p) return -1;
    uint32_t tail = 0;
    if(e->flags & IE_SUBNODE){
        if(e->elen < 24) return -1;
        e->child = rd64(x + e->elen - 8);
        tail = 8;
    }
    e->key = NULL;
    if(!(e->flags & IE_LAST)){
        if(e->klen < 0x42 || 16 + e->klen > e->elen - tail) return -1;
        e->key = x + 16;
        uint32_t nl = e->key[0x40];
        if(nl == 0 || 0x42 + 2 * nl > e->klen) return -1;
    }
    return 0;
}

/* Read index block `vcn` of directory d (bitmap-checked, fixed up, verified). */
static int ib_read(nt_vol_t* v, nt_node_t* d, uint64_t vcn, uint8_t* out){
    if(!d->ia.present || d->ia.resident) return -1;
    if(vcn > ((1ULL << 56) >> v->ivcn_shift)) return -1;
    uint64_t off = vcn << v->ivcn_shift;
    if(off & (v->idx_size - 1)) return -1;
    if(off >= d->ia.data_size || v->idx_size > d->ia.data_size - off) return -1;
    uint64_t blk = off >> v->idx_shift;
    if(blk / 8 >= d->ibmp_len || !((d->ibmp[blk / 8] >> (blk % 8)) & 1)) return -1;   /* not in use */
    for(int i = 0; i < NT_ICACHE; i++){
        if(v->ic_val[i] && v->ic_rec[i] == d->rec && v->ic_vcn[i] == vcn){
            v->ic_age[i] = ++v->tick;
            mcopy(out, v->ic_buf + (size_t)i * v->idx_size, v->idx_size);
            return 0;
        }
    }
    if(attr_read_plain(v, &d->ia, off, out, v->idx_size) != 0) return -1;
    if(fixup(out, v->idx_size, "INDX") != 0 || rd64(out + 0x10) != vcn) return -1;
    uint32_t es, ee;
    if(ih_range(out, v->idx_size, 0x18, &es, &ee) != 0) return -1;
    int s = 0;
    for(int i = 0; i < NT_ICACHE; i++){
        if(!v->ic_val[i]){ s = i; break; }
        if(v->ic_age[i] < v->ic_age[s]) s = i;
    }
    mcopy(v->ic_buf + (size_t)s * v->idx_size, out, v->idx_size);
    v->ic_rec[s] = d->rec; v->ic_vcn[s] = vcn; v->ic_age[s] = ++v->tick; v->ic_val[s] = 1;
    return 0;
}

/* ── Directory lookup (B+ tree descent) ──────────────────────────────────── */
typedef struct {
    nt_vol_t* v; nt_node_t* d;
    const uint16_t* name; uint32_t nlen;
    uint64_t ref; int score;           /* best match so far */
    uint32_t budget;
} nt_search_t;

static void search_take(nt_search_t* s, const nt_ie_t* e){
    uint32_t ns = e->key[0x41], nl = e->key[0x40];
    if(ns == NS_DOS) return;                                        /* 8.3 alias */
    if((rd64(e->key) & 0xFFFFFFFFFFFFULL) != s->d->rec) return;     /* wrong parent */
    int sc = name_eq_cs(s->name, s->nlen, e->key + 0x42, nl) ? 2 : 1;
    if(sc > s->score){ s->score = sc; s->ref = e->ref; }
}
static int ix_search(nt_search_t* s, const uint8_t* buf, uint32_t blen, uint32_t hoff, int depth){
    uint32_t p, ee;
    if(ih_range(buf, blen, hoff, &p, &ee) != 0) return -1;
    for(;;){
        nt_ie_t e;
        if(ie_parse(buf, p, ee, &e) != 0) return -1;
        int c = (e.flags & IE_LAST) ? -1 : name_cmp_ci(s->v, s->name, s->nlen, e.key + 0x42, e.key[0x40]);
        if(c <= 0 && (e.flags & IE_SUBNODE)){
            if(depth >= NT_MAX_DEPTH || s->budget == 0) return -1;
            s->budget--;
            nt_vol_t* v = s->v;
            if(!v->lk_buf[depth] && !(v->lk_buf[depth] = (uint8_t*)kmalloc(v->idx_size))) return -1;
            uint8_t* nb = v->lk_buf[depth];
            if(ib_read(v, s->d, e.child, nb) != 0) return -1;
            if(ix_search(s, nb, v->idx_size, 0x18, depth + 1) != 0) return -1;
            if(s->score == 2) return 0;
        }
        if(c == 0) search_take(s, &e);
        if(c < 0 || (e.flags & IE_LAST) || s->score == 2) return 0;
        p += e.elen;
    }
}

/* ── Inode pool ──────────────────────────────────────────────────────────── */
/* One VFS inode per MFT base record, never reused while mounted: an open fd
 * keeps its vfs_inode_t* (the VFS has no inode refcount). Slots come in
 * kmalloc'd chunks (as in fs/ext2.c and fs/exfat.c); a remount of the same
 * instance reuses them, and a node from an older mount is rejected by its
 * generation stamp. */
static nt_slot_t* vslot(nt_vol_t* v, int i){ return &v->chunk[i / NT_VCHUNK][i % NT_VCHUNK]; }

static void node_free_bufs(nt_node_t* n){
    attr_free(&n->data); attr_free(&n->ia);
    if(n->iroot) kfree(n->iroot);
    if(n->ibmp) kfree(n->ibmp);
    if(n->link) kfree(n->link);
    n->iroot = NULL; n->ibmp = NULL; n->link = NULL;
}

static int read_value(nt_vol_t* v, nt_attr_t* a, uint8_t** out, uint64_t max, uint64_t* olen){
    if(a->data_size > max) return -1;
    if(a->flags & (AF_COMPRESS_MASK | AF_ENCRYPTED)) return -1;
    uint8_t* b = (uint8_t*)kmalloc(a->data_size ? (size_t)a->data_size : 1);
    if(!b) return -1;
    if(attr_read_plain(v, a, 0, b, a->data_size) != 0){ kfree(b); return -1; }
    *out = b; *olen = a->data_size;
    return 0;
}

static const uint16_t I30[4] = { '$', 'I', '3', '0' };

/* Directory: $INDEX_ROOT (resident, $FILE_NAME collation), $INDEX_ALLOCATION
 * and its $BITMAP. */
static int dir_load(nt_vol_t* v, nt_node_t* n, const uint8_t* rec){
    nt_attr_t ir;
    if(attr_find(v, n->rec, rec, AT_INDEX_ROOT, I30, 4, &ir, 0, v->rec_size) != 0) return -1;
    if(!ir.resident || ir.rlen < 0x20 || rd32(ir.rdata) != AT_FILE_NAME || rd32(ir.rdata + 4) != 1 ||
       rd32(ir.rdata + 8) != v->idx_size){ attr_free(&ir); return -1; }
    uint32_t es, ee;
    if(ih_range(ir.rdata, ir.rlen, 0x10, &es, &ee) != 0){ attr_free(&ir); return -1; }
    n->iroot = ir.rdata; n->iroot_len = ir.rlen;
    ir.rdata = NULL; attr_free(&ir);
    int r = attr_find(v, n->rec, rec, AT_INDEX_ALLOC, I30, 4, &n->ia, 1, 0);
    if(r == -2) return 0;                                           /* small directory */
    if(r != 0 || n->ia.resident || (n->ia.flags & (AF_COMPRESS_MASK | AF_ENCRYPTED))) return -1;
    nt_attr_t bm;
    if(attr_find(v, n->rec, rec, AT_BITMAP, I30, 4, &bm, 1, NT_MAX_BMP) != 0) return -1;
    r = read_value(v, &bm, &n->ibmp, NT_MAX_BMP, &n->ibmp_len);
    attr_free(&bm);
    return r;
}

/* Symlink targets. Relative Windows symlinks keep their path ('\' -> '/');
 * an absolute "X:\path" target (or a junction) is taken to live on this
 * volume and is mapped under the mount point. */
static char* link_from16(nt_vol_t* v, const uint8_t* s16, uint32_t nchars, int absolute){
    char tmp[NT_LINK_MAX + 1];
    if(nchars > NT_LINK_MAX || utf16_to8(s16, nchars, tmp, sizeof(tmp)) < 0) return NULL;
    char* t = tmp;
    if(t[0] == '\\' && t[1] == '?' && t[2] == '?' && t[3] == '\\') t += 4;      /* \??\ prefix */
    for(char* q = t; *q; q++) if(*q == '\\') *q = '/';
    char out[NT_LINK_MAX + 1];
    size_t o = 0;
    if(absolute && ((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= 'a' && t[0] <= 'z')) && t[1] == ':'){
        t += 2;
        for(size_t k = 0; v->mp[k] && !(v->mp[k] == '/' && !v->mp[k + 1]); k++){ if(o >= NT_LINK_MAX) return NULL; out[o++] = v->mp[k]; }
        if(t[0] != '/'){ if(o >= NT_LINK_MAX) return NULL; out[o++] = '/'; }
    }
    for(; *t; t++){ if(o >= NT_LINK_MAX) return NULL; out[o++] = *t; }
    if(o == 0) return NULL;
    out[o] = 0;
    char* r = (char*)kmalloc(o + 1);
    if(r) mcopy(r, out, o + 1);
    return r;
}
static void reparse_load(nt_vol_t* v, nt_node_t* n, const uint8_t* rec){
    nt_attr_t ra; uint8_t* d = NULL; uint64_t dl = 0;
    n->unsup = 1;                                                    /* unless understood */
    if(attr_find(v, n->rec, rec, AT_REPARSE, NULL, 0, &ra, 1, NT_MAX_REPARSE) != 0) return;
    int r = read_value(v, &ra, &d, NT_MAX_REPARSE, &dl);
    attr_free(&ra);
    if(r != 0) return;
    if(dl >= 8){
        uint32_t tag = rd32(d), rl = rd16(d + 4);
        const uint8_t* b = d + 8;
        if(rl <= dl - 8){
            if(tag == RT_SYMLINK || tag == RT_MOUNT_POINT){
                uint32_t hdr = tag == RT_SYMLINK ? 12 : 8;
                if(rl >= hdr){
                    uint32_t so = rd16(b), sl = rd16(b + 2), po = rd16(b + 4), pl = rd16(b + 6);
                    int rel = tag == RT_SYMLINK && (rd32(b + 8) & 1);
                    const uint8_t* pb = b + hdr; uint32_t pbl = rl - hdr;
                    uint32_t off = pl ? po : so, len = pl ? pl : sl;
                    if(!(off & 1) && !(len & 1) && off <= pbl && len <= pbl - off){
                        n->link = link_from16(v, pb + off, len / 2, !rel);
                        if(n->link){ n->kind = NK_LINK; n->unsup = 0; }
                    }
                }
            } else if(tag == RT_LX_SYMLINK && rl > 4 && rl - 4 <= NT_LINK_MAX){
                n->link = (char*)kmalloc(rl - 4 + 1);
                if(n->link){
                    mcopy(n->link, b + 4, rl - 4); n->link[rl - 4] = 0;
                    if(slen(n->link) == rl - 4){ n->kind = NK_LINK; n->unsup = 0; }
                    else { kfree(n->link); n->link = NULL; }
                }
            }
        }
    }
    kfree(d);
    if(n->kind == NK_DIR) n->unsup = 0;            /* other tagged dirs list normally */
}

/* Load metadata of base record n->rec (sequence n->seq; 0 = any). */
static int node_load(nt_vol_t* v, nt_node_t* n){
    uint8_t* rec = (uint8_t*)kmalloc(v->rec_size);
    if(!rec) return -1;
    int ok = -1;
    do {
        if(mft_read(v, n->rec, rec) != 0) break;
        uint16_t fl = rd16(rec + 0x16);
        if(!(fl & MR_IN_USE)) break;
        if(n->seq && rd16(rec + 0x10) != n->seq) break;                 /* stale reference */
        if(rd64(rec + 0x20) & 0xFFFFFFFFFFFFULL) break;                 /* not a base record */
        n->seq = rd16(rec + 0x10);
        n->kind = (fl & MR_IS_DIR) ? NK_DIR : NK_FILE;
        nt_attr_t si;
        if(attr_find(v, n->rec, rec, AT_STD_INFO, NULL, 0, &si, 0, 1024) != 0) break;
        if(!si.resident || si.rlen < 0x24){ attr_free(&si); break; }
        n->t_mod = nt2unix(rd64(si.rdata + 0x08));
        n->t_chg = nt2unix(rd64(si.rdata + 0x10));
        n->t_acc = nt2unix(rd64(si.rdata + 0x18));
        n->fattr = rd32(si.rdata + 0x20);
        attr_free(&si);
        /* Hard links: the non-DOS names in the base record. */
        ait_t it; const uint8_t* a; uint32_t al;
        n->nlink = 0;
        ait_init(&it, rec);
        while((a = ait_next(&it, &al)) != NULL){
            if(rd32(a) != AT_FILE_NAME || a[8] || rd32(a + 0x10) < 0x42) continue;
            if((a + rd16(a + 0x14))[0x41] != NS_DOS) n->nlink++;
        }
        if(it.err) break;
        if(!n->nlink) n->nlink = 1;
        if(n->fattr & FA_REPARSE) reparse_load(v, n, rec);
        if(n->kind == NK_DIR){
            if(n->nlink) n->nlink = 1;
            if(dir_load(v, n, rec) != 0){ log_corrupt(v, "directory index, record", n->rec); n->bad = 1; }
        } else if(n->kind == NK_FILE){
            int r = attr_find(v, n->rec, rec, AT_DATA, NULL, 0, &n->data, 0, v->rec_size);
            if(r == -1){ log_corrupt(v, "$DATA, record", n->rec); n->bad = 1; }
            if(r == -2) mzero(&n->data, sizeof(n->data));          /* no data: empty */
            if((n->fattr & FA_ENCRYPTED) || (n->data.flags & AF_ENCRYPTED)) n->enc = 1;
            if(n->data.resident) n->data_ok = 1;
            /* Interix symlink (as written by older ntfs-3g): system file
             * holding "IntxLNK\1" + a UTF-16 target. */
            if(!n->bad && !n->enc && (n->fattr & FA_SYSTEM) && !(n->fattr & FA_REPARSE) &&
               n->data.data_size > 8 && n->data.data_size <= 8 + 2 * NT_LINK_MAX && !(n->data.data_size & 1) &&
               !(n->data.flags & AF_COMPRESS_MASK)){
                uint8_t buf[8 + 2 * NT_LINK_MAX];
                nt_attr_t d2; int rr = -1;
                if(n->data.resident){ rr = 0; mcopy(buf, n->data.rdata, (size_t)n->data.data_size); }
                else if(attr_find(v, n->rec, rec, AT_DATA, NULL, 0, &d2, 1, 0) == 0){
                    if(d2.data_size == n->data.data_size) rr = attr_read_plain(v, &d2, 0, buf, d2.data_size);
                    attr_free(&d2);
                }
                if(rr == 0 && !mcmp(buf, "IntxLNK\1", 8)){
                    n->link = link_from16(v, buf + 8, (uint32_t)(n->data.data_size - 8) / 2, 0);
                    if(n->link) n->kind = NK_LINK;
                }
            }
        }
        ok = 0;
    } while(0);
    kfree(rec);
    return ok;
}

/* Get (or load) the node for MFT record `recno` with sequence `seq`. */
static nt_node_t* node_get(nt_vol_t* v, uint64_t recno, uint16_t seq, const char* path){
    int32_t h = (int32_t)(recno % NT_HASH);
    nt_slot_t* s = NULL;
    for(int32_t i = v->hash[h]; i >= 0; i = vslot(v, i)->n.hnext){
        nt_slot_t* c = vslot(v, i);
        if(c->n.rec == recno && c->n.gen == v->gen){ s = c; break; }
    }
    if(s){
        if(seq && s->n.seq != seq) return NULL;                     /* stale index entry */
    } else {
        int idx = v->nslots;
        if(idx / NT_VCHUNK >= NT_VCHUNKS_MAX){ nt_log("VFS inode pool full"); return NULL; }
        if(!v->chunk[idx / NT_VCHUNK]){
            v->chunk[idx / NT_VCHUNK] = (nt_slot_t*)kmalloc(sizeof(nt_slot_t) * NT_VCHUNK);
            if(!v->chunk[idx / NT_VCHUNK]) return NULL;
            mzero(v->chunk[idx / NT_VCHUNK], sizeof(nt_slot_t) * NT_VCHUNK);
        }
        s = vslot(v, idx);
        node_free_bufs(&s->n);                                       /* previous mount's */
        mzero(s, sizeof(*s));
        s->n.v = v; s->n.gen = v->gen; s->n.rec = recno; s->n.seq = seq;
        s->vi.fs_data = &s->n;
        s->vi.ops = &v->ops;
        if(node_load(v, &s->n) != 0){ node_free_bufs(&s->n); s->n.gen = 0; return NULL; }
        v->nslots++;
        s->n.hnext = v->hash[h];
        v->hash[h] = idx;
    }
    if(path){
        size_t k = 0;
        for(; path[k] && k < sizeof(s->vi.path) - 1; k++) s->vi.path[k] = path[k];
        s->vi.path[k] = 0;
    }
    return &s->n;
}
static vfs_inode_t* node_vi(nt_node_t* n){ return &((nt_slot_t*)((uint8_t*)n - offsetof(nt_slot_t, n)))->vi; }

/* Look `name` up in directory d. 0 + *ref, -2 absent, -1 corrupt. */
static int dir_lookup(nt_vol_t* v, nt_node_t* d, const uint16_t* name, uint32_t nlen, uint64_t* ref){
    if(d->kind != NK_DIR || d->bad || !d->iroot) return -1;
    nt_search_t s;
    s.v = v; s.d = d; s.name = name; s.nlen = nlen; s.ref = 0; s.score = 0; s.budget = NT_SEARCH_BUDGET;
    if(ix_search(&s, d->iroot, d->iroot_len, 0x10, 0) != 0){ log_corrupt(v, "index tree, directory", d->rec); return -1; }
    if(!s.score) return -2;
    *ref = s.ref;
    return 0;
}

/* Resolve a mount-relative path. 0 + *out, or -errno. */
static int nt_resolve(nt_vol_t* v, const char* path, nt_node_t** out){
    nt_node_t* cur = v->root;
    char sub[256];
    size_t sl = 0;
    if(!path) path = "";
    const char* p = path;
    for(;;){
        while(*p == '/') p++;
        if(!*p) break;
        const char* e = p;
        while(*e && *e != '/') e++;
        if(cur->kind != NK_DIR) return -20;                          /* ENOTDIR */
        size_t n = (size_t)(e - p);
        if((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return -NT_ENOENT;
        uint16_t nm[256]; uint32_t nl;
        if(utf8_to16(p, n, nm, &nl) != 0 || nl == 0) return -NT_ENOENT;
        uint64_t ref;
        int r = dir_lookup(v, cur, nm, nl, &ref);
        if(r == -2) return -NT_ENOENT;
        if(r != 0) return -NT_EIO;
        uint64_t rec = ref & 0xFFFFFFFFFFFFULL;
        if(rec < REC_FIRST_USER) return -NT_ENOENT;                 /* metafiles are hidden */
        if(sl + 1 + n >= sizeof(sub)) return -NT_ENOENT;
        sub[sl++] = '/'; mcopy(sub + sl, p, n); sl += n; sub[sl] = 0;
        nt_node_t* nx = node_get(v, rec, (uint16_t)(ref >> 48), sub);
        if(!nx) return -NT_EIO;
        cur = nx;
        p = e;
    }
    *out = cur;
    return 0;
}

/* ── VFS operations ──────────────────────────────────────────────────────── */
static void vi_fill(nt_node_t* n){
    vfs_inode_t* vi = node_vi(n);
    const mntopts_t* o = &n->v->opt;
    if(n->kind == NK_DIR){ vi->type = VFS_NODE_DIR; vi->size = 0; vi->mode = mntopts_mode(o, 1, 0); }
    else if(n->kind == NK_LINK){ vi->type = VFS_NODE_SYMLINK; vi->size = slen(n->link); vi->mode = 0120777; }
    else {
        vi->type = VFS_NODE_FILE; vi->size = (size_t)n->data.data_size;
        vi->mode = mntopts_mode(o, 0, (n->fattr & FA_READONLY) != 0);
    }
    vi->uid = o->uid; vi->gid = o->gid;
    vi->nlink = n->nlink ? n->nlink : 1;
    vi->mtime = n->t_mod; vi->atime = n->t_acc; vi->ctime = n->t_chg;
}

static vfs_inode_t* nt_lookup(nt_vol_t* v, const char* path){
    if(!vol_attached(v)) return NULL;
    nt_node_t* n;
    if(nt_resolve(v, path, &n) != 0) return NULL;
    if(n == v->root){
        vfs_inode_t* vi = node_vi(n);
        vi->path[0] = '/'; vi->path[1] = 0;
    }
    vi_fill(n);
    return node_vi(n);
}

typedef struct {
    nt_vol_t* v; nt_node_t* d;
    vfs_iter_cb cb; void* user;
    vfs_inode_t* child; size_t bl;
    uint8_t* lv[NT_MAX_DEPTH];
    uint8_t* seen; uint64_t nblk;
    int err;
} nt_walk_t;

static void walk_emit(nt_walk_t* w, const nt_ie_t* e){
    const uint8_t* k = e->key;
    uint32_t ns = k[0x41], nl = k[0x40];
    uint64_t rec = e->ref & 0xFFFFFFFFFFFFULL;
    if(ns == NS_DOS || rec < REC_FIRST_USER) return;                /* 8.3 alias / metafile */
    if((rd64(k) & 0xFFFFFFFFFFFFULL) != w->d->rec){ log_corrupt(w->v, "index entry parent, directory", w->d->rec); return; }
    for(uint32_t i = 0; i < nl; i++){ uint16_t c = rd16(k + 0x42 + 2 * i); if(c == 0 || c == '/') return; }
    vfs_inode_t* c = w->child;
    size_t b = w->bl;
    if(b + 2 >= sizeof(c->path)) return;
    c->path[b++] = '/';
    int r = utf16_to8(k + 0x42, nl, c->path + b, sizeof(c->path) - b);
    if(r <= 0) return;
    if((r == 1 && c->path[b] == '.') || (r == 2 && c->path[b] == '.' && c->path[b + 1] == '.')) return;
    uint32_t fa = rd32(k + 0x38), tag = rd32(k + 0x3C);
    c->type = (fa & FA_DIR_INDEX) ? VFS_NODE_DIR : VFS_NODE_FILE;
    if((fa & FA_REPARSE) && (tag == RT_SYMLINK || tag == RT_MOUNT_POINT || tag == RT_LX_SYMLINK)) c->type = VFS_NODE_SYMLINK;
    c->size = c->type == VFS_NODE_FILE ? (size_t)rd64(k + 0x30) : 0;
    c->mode = c->type == VFS_NODE_SYMLINK ? 0120777 : mntopts_mode(&w->v->opt, c->type == VFS_NODE_DIR, (fa & FA_READONLY) != 0);
    c->fs_data = NULL; c->ops = NULL;
    c->uid = w->v->opt.uid; c->gid = w->v->opt.gid; c->nlink = 1;
    c->mtime = nt2unix(rd64(k + 0x10)); c->ctime = nt2unix(rd64(k + 0x18)); c->atime = nt2unix(rd64(k + 0x20));
    w->cb(c, w->user);
}
static int ix_walk(nt_walk_t* w, const uint8_t* buf, uint32_t blen, uint32_t hoff, int depth){
    uint32_t p, ee;
    if(ih_range(buf, blen, hoff, &p, &ee) != 0) return -1;
    nt_vol_t* v = w->v;
    for(;;){
        nt_ie_t e;
        if(ie_parse(buf, p, ee, &e) != 0) return -1;
        if(e.flags & IE_SUBNODE){
            if(depth >= NT_MAX_DEPTH) return -1;
            if(e.child > ((1ULL << 56) >> v->ivcn_shift)) return -1;
            uint64_t off = e.child << v->ivcn_shift;
            uint64_t blk = off >> v->idx_shift;
            if(blk >= w->nblk || ((w->seen[blk / 8] >> (blk % 8)) & 1)) return -1;   /* loop */
            w->seen[blk / 8] |= (uint8_t)(1u << (blk % 8));
            if(!w->lv[depth] && !(w->lv[depth] = (uint8_t*)kmalloc(v->idx_size))) return -1;
            if(ib_read(v, w->d, e.child, w->lv[depth]) != 0) return -1;
            if(ix_walk(w, w->lv[depth], v->idx_size, 0x18, depth + 1) != 0) return -1;
        }
        if(e.flags & IE_LAST) return 0;
        walk_emit(w, &e);
        p += e.elen;
    }
}

static int nt_readdir(nt_vol_t* v, const char* path, vfs_iter_cb cb, void* user){
    if(!vol_attached(v) || !cb) return -1;
    nt_node_t* d;
    if(nt_resolve(v, path, &d) != 0) return -1;
    if(d->kind != NK_DIR || d->bad || !d->iroot) return -1;
    nt_walk_t w;
    mzero(&w, sizeof(w));
    w.v = v; w.d = d; w.cb = cb; w.user = user;
    w.child = (vfs_inode_t*)kmalloc(sizeof(vfs_inode_t));
    if(!w.child) return -1;
    mzero(w.child, sizeof(vfs_inode_t));
    const char* base = path ? path : "";
    size_t bl = slen(base);
    while(bl && base[bl - 1] == '/') bl--;
    if(bl >= sizeof(w.child->path) - 2){ kfree(w.child); return -1; }
    mcopy(w.child->path, base, bl);
    w.bl = bl;
    if(d->ia.present){
        w.nblk = d->ia.data_size >> v->idx_shift;
        if(w.nblk > (1ULL << 22)){ kfree(w.child); return -1; }
        w.seen = (uint8_t*)kmalloc((size_t)(w.nblk / 8 + 1));
        if(!w.seen){ kfree(w.child); return -1; }
        mzero(w.seen, (size_t)(w.nblk / 8 + 1));
    }
    int r = ix_walk(&w, d->iroot, d->iroot_len, 0x10, 0);
    if(r != 0) log_corrupt(v, "index tree walk, directory", d->rec);
    for(int i = 0; i < NT_MAX_DEPTH; i++) if(w.lv[i]) kfree(w.lv[i]);
    if(w.seen) kfree(w.seen);
    kfree(w.child);
    return r == 0 ? 0 : -1;
}

static int node_ok(nt_node_t* n){
    if(!n || !n->v || n->gen != n->v->gen || !vol_attached(n->v)) return 0;
    return 1;
}

static int node_read(nt_node_t* n, size_t off, void* buf, size_t len){
    nt_vol_t* v = n->v;
    if(n->kind != NK_FILE) return -1;
    if(n->bad) return -NT_EIO;
    if(n->enc) return -NT_EACCES;
    if(n->unsup) return -NT_EIO;
    if(!n->data.present) return 0;
    if(!n->data_ok){
        uint8_t* rec = (uint8_t*)kmalloc(v->rec_size);
        if(!rec) return -NT_EIO;
        nt_attr_t a;
        int r = mft_read(v, n->rec, rec) == 0 ? attr_find(v, n->rec, rec, AT_DATA, NULL, 0, &a, 1, v->rec_size) : -1;
        kfree(rec);
        if(r != 0 || a.data_size != n->data.data_size){
            if(r == 0) attr_free(&a);
            n->bad = 1; log_corrupt(v, "$DATA run list, record", n->rec);
            return -NT_EIO;
        }
        attr_free(&n->data);
        n->data = a;
        n->data_ok = 1;
    }
    nt_attr_t* a = &n->data;
    if(off >= a->data_size) return 0;
    if(len > 0x7FFFF000) len = 0x7FFFF000;
    if(len > a->data_size - off) len = (size_t)(a->data_size - off);
    int r = (!a->resident && (a->flags & AF_COMPRESS_MASK))
          ? read_compressed(v, n, off, (uint8_t*)buf, len)
          : attr_read_plain(v, a, off, (uint8_t*)buf, len);
    if(r != 0){ if(r == -NT_EIO) log_corrupt(v, "data read, record", n->rec); return r; }
    return (int)len;
}
static int nt_read(vfs_inode_t* ino, size_t off, void* buf, size_t len){
    if(!ino || !ino->fs_data) return -1;
    nt_node_t* n = (nt_node_t*)ino->fs_data;
    if(!node_ok(n)) return -1;
    return node_read(n, off, buf, len);
}

static int nt_readlink(nt_vol_t* v, const char* path, char* buf, size_t len){
    if(!vol_attached(v) || !buf || !len) return -1;
    nt_node_t* n;
    if(nt_resolve(v, path, &n) != 0 || n->kind != NK_LINK || !n->link) return -1;
    size_t k = slen(n->link);
    if(k >= len) k = len - 1;
    mcopy(buf, n->link, k);
    buf[k] = 0;
    return (int)k;
}

/* Free clusters, counted once from $Bitmap. */
static void count_free(nt_vol_t* v){
    v->free_known = 1; v->free_clus = 0;
    uint8_t* rec = (uint8_t*)kmalloc(v->rec_size);
    if(!rec) return;
    nt_attr_t a;
    int r = mft_read(v, REC_BITMAP, rec) == 0 ? attr_find(v, REC_BITMAP, rec, AT_DATA, NULL, 0, &a, 1, v->rec_size) : -1;
    kfree(rec);
    if(r != 0) return;
    uint8_t* b = (uint8_t*)kmalloc(4096);
    if(b && a.data_size >= (v->nclus + 7) / 8 && !(a.flags & (AF_COMPRESS_MASK | AF_ENCRYPTED))){
        uint64_t used = 0;
        for(uint64_t off = 0; off < (v->nclus + 7) / 8; off += 4096){
            uint64_t n = (v->nclus + 7) / 8 - off;
            if(n > 4096) n = 4096;
            if(attr_read_plain(v, &a, off, b, n) != 0){ used = v->nclus; break; }
            for(uint64_t i = 0; i < n; i++){
                for(int k = 0; k < 8; k++){
                    if((off + i) * 8 + (uint64_t)k >= v->nclus) break;
                    if((b[i] >> k) & 1) used++;
                }
            }
        }
        v->free_clus = used <= v->nclus ? v->nclus - used : 0;
    }
    if(b) kfree(b);
    attr_free(&a);
}

static int nt_statfs(nt_vol_t* v, vfs_statfs_t* st){
    if(!vol_attached(v) || !st) return -1;
    if(!v->free_known) count_free(v);
    st->type = NTFS_MAGIC;
    st->bsize = st->frsize = v->cb;
    st->blocks = v->nclus;
    st->bfree = st->bavail = v->free_clus;
    st->files = v->mft_nrec; st->ffree = 0;
    st->namelen = 255;
    st->flags = 1;                                                   /* ST_RDONLY */
    return 0;
}
static int nt_mntinfo(nt_vol_t* v, const char** src, const char** type){
    if(!v->mounted) return -1;
    if(src) *src = v->devname;
    if(type) *type = "ntfs";
    return 0;
}

/* Every mutating operation: read-only filesystem. */
static int nt_ro_write(vfs_inode_t* i, size_t o, const void* b, size_t l){ (void)i; (void)o; (void)b; (void)l; return -NT_EROFS; }
static int nt_ro_create(const char* p, const void* d, size_t s){ (void)p; (void)d; (void)s; return -NT_EROFS; }
static int nt_ro_path(const char* p){ (void)p; return -NT_EROFS; }
static int nt_ro_path2(const char* a, const char* b){ (void)a; (void)b; return -NT_EROFS; }
static int nt_ro_trunc(const char* p, size_t s){ (void)p; (void)s; return -NT_EROFS; }
static int nt_ro_setattr(const char* p, const vfs_attr_t* a, unsigned v){ (void)p; (void)a; (void)v; return -NT_EROFS; }
static int nt_sync(void){ return 0; }

/* Per-instance entry points (the VFS ops carry no context pointer). */
#define NT_WRAP(i) \
    static vfs_inode_t* nt_lookup##i(const char* p){ return nt_lookup(&g_vol[i], p); } \
    static int nt_readdir##i(const char* p, vfs_iter_cb cb, void* u){ return nt_readdir(&g_vol[i], p, cb, u); } \
    static int nt_readlink##i(const char* p, char* b, size_t l){ return nt_readlink(&g_vol[i], p, b, l); } \
    static int nt_statfs##i(vfs_statfs_t* s){ return nt_statfs(&g_vol[i], s); } \
    static int nt_mntinfo##i(const char** s, const char** t){ return nt_mntinfo(&g_vol[i], s, t); }
NT_WRAP(0)
NT_WRAP(1)
NT_WRAP(2)
NT_WRAP(3)

static void ops_init(nt_vol_t* v){
    static vfs_inode_t* (*const lk[NTFS_MAX_VOLUMES])(const char*) = { nt_lookup0, nt_lookup1, nt_lookup2, nt_lookup3 };
    static int (*const rd[NTFS_MAX_VOLUMES])(const char*, vfs_iter_cb, void*) = { nt_readdir0, nt_readdir1, nt_readdir2, nt_readdir3 };
    static int (*const rl[NTFS_MAX_VOLUMES])(const char*, char*, size_t) = { nt_readlink0, nt_readlink1, nt_readlink2, nt_readlink3 };
    static int (*const sf[NTFS_MAX_VOLUMES])(vfs_statfs_t*) = { nt_statfs0, nt_statfs1, nt_statfs2, nt_statfs3 };
    static int (*const mi[NTFS_MAX_VOLUMES])(const char**, const char**) = { nt_mntinfo0, nt_mntinfo1, nt_mntinfo2, nt_mntinfo3 };
    vfs_fs_ops_t* o = &v->ops;
    mzero(o, sizeof(*o));
    o->flags = VFS_FS_NOCACHE | VFS_FS_RDONLY;   /* read errors must reach the caller */
    o->lookup = lk[v->idx]; o->readdir = rd[v->idx]; o->read = nt_read; o->write = nt_ro_write;
    o->create = nt_ro_create; o->mkdir = nt_ro_path; o->remove = nt_ro_path; o->rename = nt_ro_path2;
    o->truncate = nt_ro_trunc; o->setattr = nt_ro_setattr; o->readlink = rl[v->idx];
    o->symlink = nt_ro_path2; o->link = nt_ro_path2; o->sync = nt_sync;
    o->statfs = sf[v->idx]; o->mntinfo = mi[v->idx];
}

int ntfs_mounted_count(void){
    int n = 0;
    for(int i = 0; i < NTFS_MAX_VOLUMES; i++) if(vol_attached(&g_vol[i])) n++;
    return n;
}

/* ── Mount ───────────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t bps, cb, rec_size, idx_size;
    uint64_t total_sec, mft_lcn, mirr_lcn;
} nt_geo_t;

static int boot_parse(const uint8_t* b, uint32_t dss, uint64_t dev_bytes, nt_geo_t* g){
    if(mcmp(b + 3, "NTFS    ", 8) || b[510] != 0x55 || b[511] != 0xAA) return -1;
    uint32_t bps = rd16(b + 0x0B);
    if(bps < 512 || bps > 4096 || !is_pow2(bps) || bps < dss) return -1;
    uint32_t spc = b[0x0D];
    uint64_t cb;
    if(spc == 0) return -1;
    if(spc <= 0x80){ if(!is_pow2(spc)) return -1; cb = (uint64_t)bps * spc; }
    else { uint32_t sh = 256 - spc; if(sh > 12) return -1; cb = (uint64_t)bps << sh; }
    if(cb > (2u << 20)) return -1;
    /* Fields that must be zero on NTFS (FAT BPB leftovers). */
    if(rd16(b + 0x0E) || b[0x10] || rd16(b + 0x11) || rd16(b + 0x13) || rd16(b + 0x16) || rd32(b + 0x20)) return -1;
    uint64_t ts = rd64(b + 0x28);
    if(ts == 0 || ts > dev_bytes / bps) return -1;
    uint64_t nclus = ts * bps / cb;
    if(nclus < 16) return -1;
    int8_t cpr = (int8_t)b[0x40], cpi = (int8_t)b[0x44];
    uint64_t rs = cpr > 0 ? (uint64_t)cpr * cb : (cpr < 0 && cpr >= -31 ? 1ULL << (-cpr) : 0);
    uint64_t is = cpi > 0 ? (uint64_t)cpi * cb : (cpi < 0 && cpi >= -31 ? 1ULL << (-cpi) : 0);
    if(rs < 1024 || rs > 4096 || !is_pow2(rs)) return -1;
    if(is < 512 || is > 65536 || !is_pow2(is)) return -1;
    uint64_t ml = rd64(b + 0x30), mm = rd64(b + 0x38);
    if(ml == 0 || mm == 0 || ml >= nclus || mm >= nclus) return -1;
    g->bps = bps; g->cb = (uint32_t)cb; g->rec_size = (uint32_t)rs; g->idx_size = (uint32_t)is;
    g->total_sec = ts; g->mft_lcn = ml; g->mirr_lcn = mm;
    return 0;
}

int ntfs_probe(const char* dev_name){
    block_dev_t* dev = block_find(dev_name);
    if(!dev || dev->sector_size < 512 || dev->sector_size > 4096) return 0;
    if(block_read(dev, 0, g_bounce, 1) != 1) return 0;
    return !mcmp(g_bounce + 3, "NTFS    ", 8);
}

/* Records 0..3 of the $MFT vs $MFTMirr: a damaged primary record is served
 * from the mirror; two valid copies that differ are reported. */
static int mirror_check(nt_vol_t* v){
    uint8_t* p = (uint8_t*)kmalloc(v->rec_size);
    if(!p) return -1;
    for(uint64_t i = 1; i < 4; i++){
        int pok = mft_read(v, i, p) == 0;
        int mok = v->mirr[i] != NULL;
        if(!pok && mok){ v->mirr_use[i] = 1; nt_logx("$MFT record damaged, using $MFTMirr copy: ", i); }
        else if(!pok){ kfree(p); return -1; }
        else if(mok){
            uint32_t ua = rd32(p + 0x18), ub = rd32(v->mirr[i] + 0x18), ao = rd16(p + 0x14);
            if(ua != ub || ao != rd16(v->mirr[i] + 0x14) || mcmp(p + ao, v->mirr[i] + ao, ua - ao))
                nt_logx("$MFT / $MFTMirr mismatch (using $MFT) on record ", i);
        }
    }
    kfree(p);
    return 0;
}

/* $MFT's own $DATA from record 0: first the extents in the base record
 * (enough to reach any extension records), then the full attribute list. */
static int mft_bootstrap(nt_vol_t* v, const uint8_t* r0){
    attr_free(&v->mft);
    ait_t it; const uint8_t* a; uint32_t al;
    nt_attr_t t; mzero(&t, sizeof(t));
    int have_list = 0, first = 1;
    /* Collect base-record $DATA extents in order (they are stored sorted). */
    ait_init(&it, r0);
    while((a = ait_next(&it, &al)) != NULL){
        if(rd32(a) == AT_ATTR_LIST) have_list = 1;
        if(rd32(a) != AT_DATA || a[9] != 0) continue;
        if(!a[8]) { attr_free(&t); return -1; }
        if(first){
            if(rd64(a + 0x10) != 0 || attr_take_first(&t, a, 0) != 0){ attr_free(&t); return -1; }
            first = 0;
        }
        if(rd64(a + 0x10) != t.mapped) continue;          /* out-of-order extent: the list has it */
        if(runs_decode(v, a, al, &t) != 0){ attr_free(&t); return -1; }
    }
    if(it.err || first){ attr_free(&t); return -1; }
    if(t.flags & (AF_COMPRESS_MASK | AF_ENCRYPTED | AF_SPARSE)){ attr_free(&t); return -1; }
    for(uint32_t i = 0; i < t.nruns; i++) if(t.runs[i].lcn < 0){ attr_free(&t); return -1; }
    v->mft = t;
    uint64_t mapped_bytes = v->mft.mapped << v->cb_shift;
    v->mft_nrec = (mapped_bytes < v->mft.data_size ? mapped_bytes : v->mft.data_size) / v->rec_size;
    if(have_list){
        nt_attr_t full;
        if(attr_find(v, 0, r0, AT_DATA, NULL, 0, &full, 1, 0) != 0) return -1;
        for(uint32_t i = 0; i < full.nruns; i++) if(full.runs[i].lcn < 0){ attr_free(&full); return -1; }
        attr_free(&v->mft);
        v->mft = full;
        mapped_bytes = v->mft.mapped << v->cb_shift;
        v->mft_nrec = (mapped_bytes < v->mft.data_size ? mapped_bytes : v->mft.data_size) / v->rec_size;
    }
    if(v->mft_nrec < REC_FIRST_USER || v->mft.init_size < (uint64_t)REC_FIRST_USER * v->rec_size) return -1;
    if(v->mft_nrec > 0xFFFFFFFFULL) v->mft_nrec = 0xFFFFFFFFULL;
    return 0;
}

static void vol_release(nt_vol_t* v){
    attr_free(&v->mft);
    for(int i = 0; i < 4; i++){ if(v->mirr[i]) kfree(v->mirr[i]); v->mirr[i] = NULL; v->mirr_use[i] = 0; }
    if(v->rc_buf) kfree(v->rc_buf);
    if(v->ic_buf) kfree(v->ic_buf);
    v->rc_buf = v->ic_buf = NULL;
    for(int i = 0; i < NT_MAX_DEPTH; i++){ if(v->lk_buf[i]) kfree(v->lk_buf[i]); v->lk_buf[i] = NULL; }
    if(v->cu_in) kfree(v->cu_in);
    if(v->cu_out) kfree(v->cu_out);
    v->cu_in = v->cu_out = NULL; v->cu_node = NULL;
    for(int i = 0; i < v->nslots; i++) node_free_bufs(&vslot(v, i)->n);
    v->nslots = 0;
    v->root = NULL;
    v->mounted = 0;
}

static int load_upcase(nt_vol_t* v){
    uint8_t* rec = (uint8_t*)kmalloc(v->rec_size);
    if(!rec) return -1;
    nt_attr_t a;
    int r = mft_read(v, REC_UPCASE, rec) == 0 ? attr_find(v, REC_UPCASE, rec, AT_DATA, NULL, 0, &a, 1, 0) : -1;
    kfree(rec);
    if(r != 0) return -1;
    if(a.data_size != 131072 || a.resident || (a.flags & (AF_COMPRESS_MASK | AF_ENCRYPTED))){ attr_free(&a); return -1; }
    if(!v->up && !(v->up = (uint16_t*)kmalloc(131072))){ attr_free(&a); return -1; }
    r = attr_read_plain(v, &a, 0, (uint8_t*)v->up, 131072);
    attr_free(&a);
    if(r != 0) return -1;
    for(uint32_t i = 0; i < 65536; i++){                          /* stored little-endian */
        uint8_t* b = (uint8_t*)&v->up[i];
        v->up[i] = (uint16_t)(b[0] | (b[1] << 8));
    }
    /* Sanity: ASCII must map as in every Windows table. */
    for(uint32_t c = 0; c < 128; c++){
        uint16_t want = (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32) : (uint16_t)c;
        if(v->up[c] != want) return -1;
    }
    return 0;
}

static void check_volume(nt_vol_t* v){
    uint8_t* rec = (uint8_t*)kmalloc(v->rec_size);
    if(!rec) return;
    nt_attr_t a;
    if(mft_read(v, REC_VOLUME, rec) == 0 && attr_find(v, REC_VOLUME, rec, AT_VOLUME_INFO, NULL, 0, &a, 0, 1024) == 0){
        if(a.resident && a.rlen >= 12){
            v->ver_major = a.rdata[8]; v->ver_minor = a.rdata[9]; v->vflags = rd16(a.rdata + 10);
        }
        attr_free(&a);
    } else nt_log("$Volume information missing");
    kfree(rec);
}

static void check_hiber(nt_vol_t* v){
    static const char* nm = "hiberfil.sys";
    uint16_t n16[16]; uint32_t nl;
    if(utf8_to16(nm, slen(nm), n16, &nl) != 0) return;
    uint64_t ref;
    if(dir_lookup(v, v->root, n16, nl, &ref) != 0) return;
    nt_node_t* n = node_get(v, ref & 0xFFFFFFFFFFFFULL, (uint16_t)(ref >> 48), "/hiberfil.sys");
    if(!n || n->kind != NK_FILE || n->data.data_size < 4) return;
    uint8_t sig[4];
    if(node_read(n, 0, sig, 4) != 4) return;
    for(int i = 0; i < 4; i++) if(sig[i] >= 'a' && sig[i] <= 'z') sig[i] = (uint8_t)(sig[i] - 32);
    if(!mcmp(sig, "HIBR", 4)) v->hibernated = 1;
}

static int mount_inner(nt_vol_t* v, block_dev_t* dev, const char* dev_name, const char* mount_point){
    uint32_t dss = dev->sector_size;
    uint64_t dev_bytes = dev->sector_count * dss;
    static uint8_t b0[4096];
    nt_geo_t g;
    int backup = 0;
    if(block_read(dev, 0, b0, 1) != 1) return -1;
    if(boot_parse(b0, dss, dev_bytes, &g) != 0){
        /* The backup boot sector is the last sector of the volume. */
        int ok = 0;
        for(uint32_t bps = dss; bps <= 4096 && !ok && dev_bytes >= bps; bps <<= 1){
            uint64_t lba = (dev_bytes - bps) / dss;
            if(block_read(dev, lba, b0, 1) == 1 && boot_parse(b0, dss, dev_bytes, &g) == 0 && g.bps == bps) ok = 1;
        }
        if(!ok){
            if(!mcmp(b0 + 3, "NTFS    ", 8) || ntfs_probe(dev_name)) nt_log("boot sector invalid (main + backup) - refused");
            return -1;
        }
        backup = 1;
        nt_log("main boot sector invalid; using the backup copy");
    }
    v->dev = dev; v->dss = dss; v->bps = g.bps;
    v->cb = g.cb; v->cb_shift = log2u(g.cb);
    v->vol_bytes = g.total_sec * g.bps;
    v->nclus = v->vol_bytes >> v->cb_shift;
    v->rec_size = g.rec_size; v->idx_size = g.idx_size; v->idx_shift = log2u(g.idx_size);
    v->ivcn_shift = g.idx_size >= g.cb ? v->cb_shift : 9;
    v->mft_lcn = g.mft_lcn; v->mirr_lcn = g.mirr_lcn;
    v->from_backup = backup;
    v->corrupt_logged = 0;
    for(int i = 0; i < NT_RCACHE; i++) v->rc_val[i] = 0;
    for(int i = 0; i < NT_ICACHE; i++) v->ic_val[i] = 0;
    for(int i = 0; i < NT_HASH; i++) v->hash[i] = -1;
    v->rc_buf = (uint8_t*)kmalloc((size_t)NT_RCACHE * v->rec_size);
    v->ic_buf = (uint8_t*)kmalloc((size_t)NT_ICACHE * v->idx_size);
    if(!v->rc_buf || !v->ic_buf) return -1;

    /* $MFTMirr records 0..3 and $MFT record 0. */
    for(uint32_t i = 0; i < 4; i++){
        uint8_t* m = (uint8_t*)kmalloc(v->rec_size);
        if(!m) return -1;
        if(dev_read(v, (v->mirr_lcn << v->cb_shift) + (uint64_t)i * v->rec_size, m, v->rec_size) != 0 ||
           rec_load_raw(v, m, i) != 0 || !(rd16(m + 0x16) & MR_IN_USE)){ kfree(m); m = NULL; }
        v->mirr[i] = m;
    }
    uint8_t* r0 = (uint8_t*)kmalloc(v->rec_size);
    if(!r0) return -1;
    int ok0 = dev_read(v, v->mft_lcn << v->cb_shift, r0, v->rec_size) == 0 && rec_load_raw(v, r0, 0) == 0 &&
              (rd16(r0 + 0x16) & MR_IN_USE) && mft_bootstrap(v, r0) == 0;
    if(!ok0 && v->mirr[0]){
        nt_log("$MFT record 0 damaged, using the $MFTMirr copy");
        mcopy(r0, v->mirr[0], v->rec_size);
        ok0 = mft_bootstrap(v, r0) == 0;
        v->mirr_use[0] = 1;
    }
    kfree(r0);
    if(!ok0){ nt_log("$MFT unusable (record 0 and mirror) - refused"); return -1; }
    if(mirror_check(v) != 0){ nt_log("$MFT records 1..3 unreadable - refused"); return -1; }

    check_volume(v);
    if(load_upcase(v) != 0){ nt_log("$UpCase invalid - refused"); return -1; }

    size_t k = 0;
    for(; mount_point[k] && k < sizeof(v->mp) - 1; k++) v->mp[k] = mount_point[k];
    v->mp[k] = 0;
    for(k = 0; dev_name[k] && k < sizeof(v->devname) - 1; k++) v->devname[k] = dev_name[k];
    v->devname[k] = 0;

    v->gen = ++g_gen_ctr;
    if(!v->gen) v->gen = ++g_gen_ctr;
    v->root = node_get(v, REC_ROOT, 0, "/");
    if(!v->root || v->root->kind != NK_DIR || v->root->bad){ nt_log("root directory invalid - refused"); return -1; }
    check_hiber(v);
    return 0;
}

/* Owner/mode options for the next mount (fs/mntopts.h); an explicit "rw"
 * makes it fail with EROFS, as a read-write request does. */
static mntopts_t g_nopt_new = { 0, 0, 022, 0133, 0, 0 };
int ntfs_set_mount_options(const char* opts){
    mntopts_t o;
    if(mntopts_parse(opts, &o) != 0) return -1;
    g_nopt_new = o;
    return 0;
}

int ntfs_mount_opts(const char* dev_name, const char* mount_point, int want_rw){
    mntopts_t opt = g_nopt_new; mntopts_default(&g_nopt_new);
    if(opt.rw) want_rw = 1;
    if(want_rw){ nt_log("read-write mount refused (driver is read-only)"); return -NT_EROFS; }
    if(!dev_name || !mount_point || mount_point[0] != '/') return -1;
    block_dev_t* dev = block_find(dev_name);
    if(!dev) return -1;
    uint32_t dss = dev->sector_size;
    if(dss < 512 || dss > 4096 || !is_pow2(dss)) return -1;
    /* No signature probe here: a volume whose main boot sector is damaged is
     * still mounted from the backup copy. Other filesystems are refused by
     * boot_parse (OEM id + BPB checks) on both copies. */
    nt_vol_t* v = NULL;
    for(int i = 0; i < NTFS_MAX_VOLUMES && !v; i++) if(!vol_attached(&g_vol[i])) v = &g_vol[i];
    if(!v){ nt_log("all NTFS instances busy"); return -1; }
    v->idx = (int)(v - g_vol);
    vol_release(v);
    ops_init(v);
    v->opt = opt;
    if(mount_inner(v, dev, dev_name, mount_point) != 0){ vol_release(v); return -1; }
    v->mounted = 1;
    if(vfs_mount(v->mp, &v->ops, "ntfs") != 0){ vol_release(v); return -1; }
    debugcon_writestring("[NTFS] mounted "); debugcon_writestring(v->devname);
    debugcon_writestring(" at "); debugcon_writestring(v->mp);
    debugcon_writestring(" (ro) v"); debugcon_print_hex(v->ver_major); debugcon_writestring(".");
    debugcon_print_hex(v->ver_minor);
    debugcon_writestring(" clusters="); debugcon_print_hex(v->nclus);
    debugcon_writestring(" csize="); debugcon_print_hex(v->cb);
    debugcon_writestring(" mftrec="); debugcon_print_hex(v->rec_size);
    debugcon_writestring("\n");
    if(v->vflags & VI_DIRTY) nt_log("WARNING: volume is marked dirty (not cleanly unmounted) - read-only");
    if(v->hibernated) nt_log("WARNING: Windows is hibernated on this volume (Fast Startup) - read-only");
    return 0;
}

int ntfs_mount(const char* dev_name, const char* mount_point){
    return ntfs_mount_opts(dev_name, mount_point, 0);
}
