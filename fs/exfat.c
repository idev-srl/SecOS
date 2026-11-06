/*
 * SecOS Kernel - exFAT read-write filesystem
 *
 * Implemented from the public Microsoft "exFAT File System Specification"
 * (2019). Supports:
 *   - boot region validation (main + backup, boot checksum), geometry checks;
 *   - allocation bitmap, FAT chains and NoFatChain (contiguous) streams;
 *   - the up-case table (compressed form, table checksum) for case-insensitive
 *     lookups and the stream NameHash;
 *   - directory entry sets (File + Stream Extension + File Name entries) with
 *     SetChecksum verification; long UTF-16 names <-> UTF-8 paths;
 *   - create, write/extend (ValidDataLength-aware), truncate, unlink, mkdir,
 *     rmdir, rename (also across directories), timestamps from the RTC;
 *   - VolumeDirty set while modified, cleared by sync / clean unmount.
 *
 * Every on-disk value is untrusted: cluster numbers, chain lengths, entry
 * counts, name lengths and sizes are bounds-checked, FAT chains are walked
 * with a hard step bound (cycles are detected, not followed forever), and a
 * corrupt image is refused at mount or yields an error from the operation.
 *
 * Limitations: one mounted exFAT volume at a time; TexFAT (two FATs) and
 * volumes recovered from the backup boot region mount read-only; no locking
 * beyond the block layer (the kernel serializes FS calls today).
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "exfat.h"
#include "vfs.h"
#include "block.h"
#include "debugcon.h"
#include "heap.h"
#include "pagecache.h"
#include <stdint.h>
#include <stddef.h>

extern uint64_t kclock_real_sec(void);
extern void kclock_unix_to_civil(uint64_t t, int* y, int* mo, int* d, int* h, int* mi, int* s);
extern uint64_t kclock_civil_to_unix(int y, int mo, int d, int h, int mi, int s);

/* ── On-disk constants ───────────────────────────────────────────────────── */
#define EX_EOC          0xFFFFFFFFu
#define EX_MAX_SS       4096u
#define EX_MAX_DIR      (256u * 1024u * 1024u)   /* spec: directories <= 256 MiB */
#define EX_MAXSET       19u                      /* File + Stream + 17 names */

#define ET_EOD          0x00
#define ET_BITMAP       0x81
#define ET_UPCASE       0x82
#define ET_FILE         0x85
#define ET_STREAM       0xC0
#define ET_NAME         0xC1
#define ET_INUSE        0x80

#define AT_RO           0x0001
#define AT_DIR          0x0010
#define AT_ARCH         0x0020

#define SF_ALLOC        0x01
#define SF_NOFAT        0x02

#define VF_ACTIVEFAT    0x0001
#define VF_DIRTY        0x0002
#define VF_MEDIAFAIL    0x0004

#define EXFAT_MAGIC     0x2011BAB0u              /* Linux EXFAT_SUPER_MAGIC */

/* ── Little-endian helpers ───────────────────────────────────────────────── */
static uint16_t rd16(const uint8_t* p){ return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p){ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t* p){ return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static void wr16(uint8_t* p, uint16_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v){ for(int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void wr64(uint8_t* p, uint64_t v){ for(int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void mzero(void* d, size_t n){ uint8_t* p = (uint8_t*)d; while(n--) *p++ = 0; }
static void mcopy(void* d, const void* s, size_t n){ uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while(n--) *a++ = *b++; }
static size_t slen(const char* s){ size_t n = 0; while(s[n]) n++; return n; }
static int seq(const char* a, const char* b){ while(*a && *a == *b){ a++; b++; } return *a == *b; }

static void ex_log(const char* m){ debugcon_writestring("[EXFAT] "); debugcon_writestring(m); debugcon_writestring("\n"); }
static void ex_logx(const char* m, uint64_t v){ debugcon_writestring("[EXFAT] "); debugcon_writestring(m); debugcon_print_hex(v); debugcon_writestring("\n"); }

/* ── Stream: the data of a file or directory ─────────────────────────────── */
typedef struct {
    uint32_t first;      /* first cluster (0 = nothing allocated) */
    uint64_t size;       /* DataLength */
    uint64_t vdl;        /* ValidDataLength: bytes past it read as zero */
    int      nofat;      /* NoFatChain: clusters first..first+n-1, FAT unused */
    uint32_t cidx, cclus;/* cursor cache: cluster index -> cluster number */
    int      cvalid;
} exst_t;

/* ── Mounted volume state (single instance) ──────────────────────────────── */
static struct {
    block_dev_t* dev;
    uint32_t spf;          /* device sectors per fs sector */
    uint32_t ss_shift, ss; /* fs sector size */
    uint32_t spc_shift;    /* log2 fs sectors per cluster */
    uint32_t cb_shift, cb; /* cluster size in bytes */
    uint64_t vol_len;      /* volume length in fs sectors */
    uint32_t fat_off, fat_len, heap_off, ccount, root_clus;
    uint32_t active_fat;
    uint16_t flags;        /* VolumeFlags as found at mount */
    int      dirty_at_mount;
    int      dirty_set;    /* we set VolumeDirty and owe a clear */
    int      ro;
    int      mounted;
    uint32_t* bmp_clus;    /* allocation bitmap cluster list */
    uint32_t bmp_nclus;
    uint64_t bmp_len;
    uint64_t free;         /* free cluster count */
    uint32_t hint;         /* next-fit allocation hint */
    exst_t   root;
} V;

static char g_mp[256];           /* the VFS keeps our mount point pointer */
static char g_devname[32];
static uint16_t g_up[65536];     /* expanded up-case table */
static uint8_t  g_bounce[EX_MAX_SS];
static uint8_t  g_zero[EX_MAX_SS];

/* ── Sector I/O + small write-through metadata cache ─────────────────────── */
static int dev_rd(uint64_t fsec, void* buf, uint32_t n){
    if(fsec >= V.vol_len || n > V.vol_len - fsec) return -1;
    return block_read(V.dev, fsec * V.spf, buf, n * V.spf) == (int)(n * V.spf) ? 0 : -1;
}
static int dev_wr(uint64_t fsec, const void* buf, uint32_t n){
    if(V.ro) return -1;
    if(fsec >= V.vol_len || n > V.vol_len - fsec) return -1;
    return block_write(V.dev, fsec * V.spf, buf, n * V.spf) == (int)(n * V.spf) ? 0 : -1;
}

#define EX_CACHE 16
static uint8_t  g_cbuf[EX_CACHE][EX_MAX_SS];
static uint64_t g_clba[EX_CACHE];
static uint32_t g_cage[EX_CACHE];
static int      g_cval[EX_CACHE];
static uint32_t g_ctick;

static void cache_reset(void){ for(int i = 0; i < EX_CACHE; i++) g_cval[i] = 0; }
static void cache_inval(uint64_t lba, uint64_t n){
    for(int i = 0; i < EX_CACHE; i++) if(g_cval[i] && g_clba[i] >= lba && g_clba[i] - lba < n) g_cval[i] = 0;
}
static int cache_slot(uint64_t lba, int* hit){
    int v = 0;
    for(int i = 0; i < EX_CACHE; i++){
        if(g_cval[i] && g_clba[i] == lba){ g_cage[i] = ++g_ctick; *hit = 1; return i; }
    }
    for(int i = 0; i < EX_CACHE; i++){
        if(!g_cval[i]){ v = i; break; }
        if(g_cage[i] < g_cage[v]) v = i;
    }
    *hit = 0;
    return v;
}
/* Return the cached copy of fs sector `lba` (read on miss). At most the last
 * few returned pointers stay valid: callers copy out what they keep. */
static uint8_t* sec_get(uint64_t lba){
    if(lba >= V.vol_len) return NULL;
    int hit, i = cache_slot(lba, &hit);
    if(hit) return g_cbuf[i];
    g_cval[i] = 0;
    if(dev_rd(lba, g_cbuf[i], 1) != 0) return NULL;
    g_clba[i] = lba; g_cage[i] = ++g_ctick; g_cval[i] = 1;
    return g_cbuf[i];
}
/* Write back a sector previously returned by sec_get (write-through). */
static int sec_put(uint64_t lba, const uint8_t* p){
    int r = dev_wr(lba, p, 1);
    if(r != 0) cache_inval(lba, 1);   /* keep the cache equal to the disk */
    return r;
}

/* ── Cluster helpers ─────────────────────────────────────────────────────── */
static int cl_ok(uint32_t c){ return c >= 2 && (uint64_t)c < (uint64_t)V.ccount + 2; }
static uint64_t cl_sec(uint32_t c){ return (uint64_t)V.heap_off + ((uint64_t)(c - 2) << V.spc_shift); }

static int fat_get(uint32_t c, uint32_t* out){
    uint64_t off = (uint64_t)c * 4;
    uint64_t s = off >> V.ss_shift;
    if(s >= V.fat_len) return -1;
    uint8_t* p = sec_get((uint64_t)V.fat_off + (uint64_t)V.active_fat * V.fat_len + s);
    if(!p) return -1;
    *out = rd32(p + (off & (V.ss - 1)));
    return 0;
}
static int fat_set(uint32_t c, uint32_t v){
    uint64_t off = (uint64_t)c * 4;
    uint64_t s = off >> V.ss_shift;
    if(V.ro || s >= V.fat_len) return -1;
    uint64_t lba = (uint64_t)V.fat_off + s;      /* RW implies a single FAT */
    uint8_t* p = sec_get(lba);
    if(!p) return -1;
    wr32(p + (off & (V.ss - 1)), v);
    return sec_put(lba, p);
}
/* 1 = *n is the next cluster, 0 = end of chain, -1 = corrupt entry / I/O. */
static int fat_next(uint32_t c, uint32_t* n){
    uint32_t v;
    if(fat_get(c, &v) != 0) return -1;
    if(v == EX_EOC) return 0;
    if(!cl_ok(v)) return -1;        /* free, bad (0xFFFFFFF7) or out of range */
    *n = v;
    return 1;
}

/* ── Allocation bitmap ───────────────────────────────────────────────────── */
static uint8_t* bmp_byte(uint32_t c, uint64_t* lba, uint32_t* boff){
    uint64_t byte = (uint64_t)(c - 2) >> 3;
    if(byte >= V.bmp_len) return NULL;
    uint64_t ci = byte >> V.cb_shift;
    if(ci >= V.bmp_nclus) return NULL;
    uint32_t inb = (uint32_t)(byte & (V.cb - 1));
    *lba = cl_sec(V.bmp_clus[ci]) + (inb >> V.ss_shift);
    uint8_t* p = sec_get(*lba);
    *boff = inb & (V.ss - 1);
    return p;
}
static int bmp_get(uint32_t c){
    uint64_t lba; uint32_t o;
    if(!cl_ok(c)) return -1;
    uint8_t* p = bmp_byte(c, &lba, &o);
    if(!p) return -1;
    return (p[o] >> ((c - 2) & 7)) & 1;
}
static int bmp_put(uint32_t c, int used){
    uint64_t lba; uint32_t o;
    if(V.ro || !cl_ok(c)) return -1;
    uint8_t* p = bmp_byte(c, &lba, &o);
    if(!p) return -1;
    uint8_t m = (uint8_t)(1u << ((c - 2) & 7));
    int was = (p[o] & m) ? 1 : 0;
    if(was == used) return 0;
    if(used) p[o] |= m; else p[o] &= (uint8_t)~m;
    if(sec_put(lba, p) != 0) return -1;
    if(used){ if(V.free) V.free--; } else V.free++;
    return 0;
}
/* Allocate a free cluster: `pref` if free, else next-fit from the hint. */
static uint32_t cl_alloc(uint32_t pref){
    if(cl_ok(pref) && bmp_get(pref) == 0){
        if(bmp_put(pref, 1) != 0) return 0;
        V.hint = pref + 1;
        return pref;
    }
    uint32_t c = cl_ok(V.hint) ? V.hint : 2;
    uint64_t scanned = 0;
    while(scanned < (uint64_t)V.ccount + 8){
        uint64_t lba; uint32_t o;
        uint8_t* p = bmp_byte(c, &lba, &o);
        if(!p) return 0;
        uint32_t bit = (c - 2) & 7;
        uint32_t step = 1;
        if(bit == 0 && p[o] == 0xFF) step = 8;
        else if(!((p[o] >> bit) & 1)){
            if(bmp_put(c, 1) != 0) return 0;
            V.hint = c + 1;
            return c;
        }
        scanned += step;
        c += step;
        if(!cl_ok(c)) c = 2;
    }
    return 0;
}

/* ── Streams ─────────────────────────────────────────────────────────────── */
static uint32_t st_nclus(const exst_t* s){ return (uint32_t)((s->size + V.cb - 1) >> V.cb_shift); }

/* Cluster number holding cluster index `idx` of the stream. */
static int st_clus(exst_t* s, uint32_t idx, uint32_t* out){
    if(!cl_ok(s->first)) return -1;
    if(s->nofat){
        uint64_t c = (uint64_t)s->first + idx;
        if(c >= (uint64_t)V.ccount + 2) return -1;
        *out = (uint32_t)c;
        return 0;
    }
    uint32_t i = 0, c = s->first;
    if(s->cvalid && s->cidx <= idx){ i = s->cidx; c = s->cclus; }
    while(i < idx){
        uint32_t n;
        if(fat_next(c, &n) != 1) return -1;
        c = n; i++;
    }
    s->cidx = i; s->cclus = c; s->cvalid = 1;
    *out = c;
    return 0;
}

/* Check a stream before trusting it for I/O: sizes are sane, the clusters are
 * in range and a FAT chain is exactly as long as DataLength needs (a cycle
 * never reaches the end-of-chain mark, so it fails the bounded walk). */
static int st_validate(exst_t* s){
    if(s->vdl > s->size) return -1;
    if(s->size > ((uint64_t)V.ccount << V.cb_shift)) return -1;
    uint32_t n = st_nclus(s);
    if(n == 0) return 0;
    if(!cl_ok(s->first)) return -1;
    if(s->nofat) return ((uint64_t)s->first + n - 1 < (uint64_t)V.ccount + 2) ? 0 : -1;
    uint32_t c = s->first;
    for(uint32_t i = 1; i < n; i++){
        uint32_t nx;
        if(fat_next(c, &nx) != 1) return -1;   /* short chain / bad link */
        c = nx;
    }
    uint32_t nx;
    int r = fat_next(c, &nx);
    if(r == 0) return 0;
    if(r < 0) return -1;
    /* Longer than DataLength: tolerate a clean over-allocation, but a cycle
     * (or garbage) never reaches EOC within ccount steps. */
    c = nx;
    for(uint64_t i = n; i <= V.ccount; i++){
        r = fat_next(c, &nx);
        if(r == 0) return 0;
        if(r < 0) return -1;
        c = nx;
    }
    return -1;
}

/* Read [off, off+len) of a stream; bytes past ValidDataLength read as zero. */
static int64_t st_read(exst_t* s, uint64_t off, uint8_t* out, uint64_t len){
    if(off >= s->size) return 0;
    if(len > s->size - off) len = s->size - off;
    uint64_t done = 0;
    while(done < len){
        uint64_t pos = off + done;
        if(pos >= s->vdl){ mzero(out + done, (size_t)(len - done)); done = len; break; }
        uint64_t lim = len - done;
        if(lim > s->vdl - pos) lim = s->vdl - pos;
        uint32_t c;
        if(st_clus(s, (uint32_t)(pos >> V.cb_shift), &c) != 0) return -1;
        uint32_t inc = (uint32_t)(pos & (V.cb - 1));
        if(lim > V.cb - inc) lim = V.cb - inc;
        uint64_t lba = cl_sec(c) + (inc >> V.ss_shift);
        uint32_t ins = inc & (V.ss - 1);
        if(ins == 0 && lim >= V.ss){
            uint32_t ns = (uint32_t)(lim >> V.ss_shift);
            if(ns > 64) ns = 64;
            if(dev_rd(lba, out + done, ns) != 0) return -1;
            done += (uint64_t)ns << V.ss_shift;
        } else {
            uint32_t n = V.ss - ins;
            if(n > lim) n = (uint32_t)lim;
            if(dev_rd(lba, g_bounce, 1) != 0) return -1;
            mcopy(out + done, g_bounce + ins, n);
            done += n;
        }
    }
    return (int64_t)done;
}

/* Write [off, off+len) into already-allocated clusters (in == NULL: zeros). */
static int st_rw(exst_t* s, uint64_t off, const uint8_t* in, uint64_t len){
    uint64_t done = 0;
    while(done < len){
        uint64_t pos = off + done;
        uint32_t c;
        if(st_clus(s, (uint32_t)(pos >> V.cb_shift), &c) != 0) return -1;
        uint32_t inc = (uint32_t)(pos & (V.cb - 1));
        uint64_t lim = len - done;
        if(lim > V.cb - inc) lim = V.cb - inc;
        uint64_t lba = cl_sec(c) + (inc >> V.ss_shift);
        uint32_t ins = inc & (V.ss - 1);
        if(ins == 0 && lim >= V.ss){
            uint32_t ns = (uint32_t)(lim >> V.ss_shift);
            if(!in){
                if(ns > EX_MAX_SS / V.ss) ns = EX_MAX_SS / V.ss;
                if(ns == 0) ns = 1;
                if(dev_wr(lba, g_zero, ns) != 0) return -1;
            } else {
                if(ns > 64) ns = 64;
                if(dev_wr(lba, in + done, ns) != 0) return -1;
            }
            cache_inval(lba, ns);
            done += (uint64_t)ns << V.ss_shift;
        } else {
            uint32_t n = V.ss - ins;
            if(n > lim) n = (uint32_t)lim;
            if(dev_rd(lba, g_bounce, 1) != 0) return -1;
            if(in) mcopy(g_bounce + ins, in + done, n); else mzero(g_bounce + ins, n);
            if(dev_wr(lba, g_bounce, 1) != 0) return -1;
            cache_inval(lba, 1);
            done += n;
        }
    }
    return 0;
}

/* Release clusters [keep, have) of a stream. */
static int st_shrink(exst_t* s, uint32_t have, uint32_t keep){
    s->cvalid = 0;
    if(keep >= have) return 0;
    if(s->nofat){
        for(uint32_t i = keep; i < have; i++) bmp_put(s->first + i, 0);
    } else {
        uint32_t c;
        if(keep == 0) c = s->first;
        else {
            uint32_t lk;
            if(st_clus(s, keep - 1, &lk) != 0) return -1;
            if(fat_next(lk, &c) != 1) return -1;
            if(fat_set(lk, EX_EOC) != 0) return -1;
        }
        for(uint32_t i = keep; i < have; i++){
            if(!cl_ok(c)) break;
            bmp_put(c, 0);
            uint32_t n;
            if(fat_next(c, &n) != 1) break;
            c = n;
        }
    }
    if(keep == 0){ s->first = 0; s->nofat = 0; }
    s->cvalid = 0;
    return 0;
}

/* Grow a stream from `have` to `need` clusters. Stays NoFatChain while the
 * next cluster is free; otherwise the run is converted to a FAT chain. */
static int st_grow(exst_t* s, uint32_t have, uint32_t need){
    uint32_t orig = have, last = 0;
    s->cvalid = 0;
    if(have > 0 && st_clus(s, have - 1, &last) != 0) return -1;
    while(have < need){
        uint32_t c;
        if(have == 0){
            c = cl_alloc(V.hint);
            if(!c) goto fail;
            s->first = c; s->nofat = 1;
        } else {
            c = cl_alloc(last + 1);
            if(!c) goto fail;
            if(!(s->nofat && c == last + 1)){
                if(s->nofat){
                    for(uint32_t i = 0; i + 1 < have; i++)
                        if(fat_set(s->first + i, s->first + i + 1) != 0){ bmp_put(c, 0); goto fail; }
                    s->nofat = 0;
                }
                if(fat_set(last, c) != 0){ bmp_put(c, 0); goto fail; }
            }
            if(!s->nofat && fat_set(c, EX_EOC) != 0){ goto fail_linked; }
        }
        last = c; have++;
        continue;
fail_linked:
        have++;
        goto fail;
    }
    s->cvalid = 0;
    return 0;
fail:
    if(have > orig) st_shrink(s, have, orig);
    else if(orig == 0){ s->first = 0; s->nofat = 0; }
    s->cvalid = 0;
    return -1;
}

/* ── Volume flags ────────────────────────────────────────────────────────── */
static int vol_dirty(void){
    if(V.ro) return -1;
    if(V.dirty_set) return 0;
    uint8_t* p = sec_get(0);
    if(!p) return -1;
    wr16(p + 106, (uint16_t)(rd16(p + 106) | VF_DIRTY));
    if(sec_put(0, p) != 0) return -1;
    V.dirty_set = 1;
    return 0;
}
static int vol_clean(void){
    if(V.ro || !V.dirty_set) return 0;
    uint8_t* p = sec_get(0);
    if(!p) return -1;
    /* A volume that was already dirty when mounted stays dirty: it may hold
     * damage from the earlier crash that only a checker should clear. */
    if(!V.dirty_at_mount) wr16(p + 106, (uint16_t)(rd16(p + 106) & ~VF_DIRTY));
    uint64_t used = V.ccount - V.free;
    p[112] = (uint8_t)(V.ccount ? (used * 100) / V.ccount : 0);
    if(sec_put(0, p) != 0) return -1;
    V.dirty_set = 0;
    return 0;
}

/* ── Timestamps ──────────────────────────────────────────────────────────── */
static uint32_t ts_now(uint8_t* ms10){
    int y, mo, d, h, mi, sc;
    kclock_unix_to_civil(kclock_real_sec(), &y, &mo, &d, &h, &mi, &sc);
    if(y < 1980){ y = 1980; mo = 1; d = 1; h = mi = sc = 0; }
    if(y > 2107){ y = 2107; mo = 12; d = 31; h = 23; mi = 59; sc = 58; }
    if(ms10) *ms10 = (uint8_t)((sc & 1) ? 100 : 0);
    return ((uint32_t)(y - 1980) << 25) | ((uint32_t)mo << 21) | ((uint32_t)d << 16) |
           ((uint32_t)h << 11) | ((uint32_t)mi << 5) | (uint32_t)(sc / 2);
}
static uint32_t ts_from_unix(uint64_t t){
    int y, mo, d, h, mi, sc;
    kclock_unix_to_civil(t, &y, &mo, &d, &h, &mi, &sc);
    if(y < 1980) return (0u << 25) | (1u << 21) | (1u << 16);
    if(y > 2107) y = 2107;
    return ((uint32_t)(y - 1980) << 25) | ((uint32_t)mo << 21) | ((uint32_t)d << 16) |
           ((uint32_t)h << 11) | ((uint32_t)mi << 5) | (uint32_t)(sc / 2);
}
static uint64_t ts_to_unix(uint32_t ts, uint8_t ms10, uint8_t utc){
    int y = 1980 + (int)(ts >> 25), mo = (ts >> 21) & 15, d = (ts >> 16) & 31;
    int h = (ts >> 11) & 31, mi = (ts >> 5) & 63, sc = (int)(ts & 31) * 2 + (ms10 >= 100 ? 1 : 0);
    if(mo < 1 || mo > 12 || d < 1 || h > 23 || mi > 59 || sc > 59) return 0;
    int64_t t = (int64_t)kclock_civil_to_unix(y, mo, d, h, mi, sc);
    if(utc & 0x80){                              /* OffsetValid: local = UTC + off */
        int off = utc & 0x7F; if(off & 0x40) off -= 0x80;
        t -= (int64_t)off * 15 * 60;
    }
    return t < 0 ? 0 : (uint64_t)t;
}

/* ── Names: UTF-8 <-> UTF-16, up-case, hash ──────────────────────────────── */
static uint16_t up16(uint16_t c){ return g_up[c]; }

static uint16_t name_hash(const uint16_t* n, uint32_t len){
    uint16_t h = 0;
    for(uint32_t i = 0; i < len; i++){
        uint16_t u = up16(n[i]);
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (u & 0xFF));
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (u >> 8));
    }
    return h;
}

/* Decode one UTF-8 path component into UTF-16. -1 on invalid UTF-8 or > 255 units. */
static int utf8_to16(const char* s, size_t n, uint16_t* out, uint32_t* olen){
    uint32_t o = 0;
    size_t i = 0;
    while(i < n){
        uint32_t c = (uint8_t)s[i], need, min;
        if(c < 0x80){ need = 0; min = 0; }
        else if((c & 0xE0) == 0xC0){ c &= 0x1F; need = 1; min = 0x80; }
        else if((c & 0xF0) == 0xE0){ c &= 0x0F; need = 2; min = 0x800; }
        else if((c & 0xF8) == 0xF0){ c &= 0x07; need = 3; min = 0x10000; }
        else return -1;
        for(uint32_t k = 1; k <= need; k++){
            if(i + k >= n) return -1;
            uint8_t b = (uint8_t)s[i + k];
            if((b & 0xC0) != 0x80) return -1;
            c = (c << 6) | (b & 0x3F);
        }
        if(c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return -1;
        i += need + 1;
        if(c >= 0x10000){
            if(o + 2 > 255) return -1;
            c -= 0x10000;
            out[o++] = (uint16_t)(0xD800 | (c >> 10));
            out[o++] = (uint16_t)(0xDC00 | (c & 0x3FF));
        } else {
            if(o + 1 > 255) return -1;
            out[o++] = (uint16_t)c;
        }
    }
    *olen = o;
    return 0;
}

/* Encode UTF-16 to UTF-8 (lone surrogates -> U+FFFD). Returns length or -1 if
 * it does not fit in `cap` (NUL included). */
static int utf16_to8(const uint16_t* in, uint32_t n, char* out, size_t cap){
    size_t o = 0;
    for(uint32_t i = 0; i < n; i++){
        uint32_t c = in[i];
        if(c >= 0xD800 && c <= 0xDBFF && i + 1 < n && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF){
            c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            i++;
        } else if(c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
        uint8_t b[4]; size_t k;
        if(c < 0x80){ b[0] = (uint8_t)c; k = 1; }
        else if(c < 0x800){ b[0] = (uint8_t)(0xC0 | (c >> 6)); b[1] = (uint8_t)(0x80 | (c & 0x3F)); k = 2; }
        else if(c < 0x10000){ b[0] = (uint8_t)(0xE0 | (c >> 12)); b[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)); b[2] = (uint8_t)(0x80 | (c & 0x3F)); k = 3; }
        else { b[0] = (uint8_t)(0xF0 | (c >> 18)); b[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F)); b[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)); b[3] = (uint8_t)(0x80 | (c & 0x3F)); k = 4; }
        if(o + k + 1 > cap) return -1;
        for(size_t j = 0; j < k; j++) out[o++] = (char)b[j];
    }
    out[o] = 0;
    return (int)o;
}

/* Characters the spec forbids in a file name. */
static int name_valid(const uint16_t* n, uint32_t len){
    if(len == 0 || len > 255) return 0;
    if(len == 1 && n[0] == '.') return 0;
    if(len == 2 && n[0] == '.' && n[1] == '.') return 0;
    for(uint32_t i = 0; i < len; i++){
        uint16_t c = n[i];
        if(c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
           c == '>' || c == '?' || c == '\\' || c == '|') return 0;
    }
    return 1;
}

static int name_eq(const uint16_t* a, uint32_t al, const uint16_t* b, uint32_t bl){
    if(al != bl) return 0;
    for(uint32_t i = 0; i < al; i++) if(up16(a[i]) != up16(b[i])) return 0;
    return 1;
}

/* ── Directory entries and entry sets ────────────────────────────────────── */
typedef struct {
    uint32_t slot, cnt;
    uint8_t  raw[EX_MAXSET * 32];
    uint16_t name[255];
    uint32_t nlen;
} exset_t;

static int dent_loc(exst_t* d, uint32_t slot, uint64_t* lba, uint32_t* off){
    uint64_t b = (uint64_t)slot * 32;
    if(b + 32 > d->size) return -1;
    uint32_t c;
    if(st_clus(d, (uint32_t)(b >> V.cb_shift), &c) != 0) return -1;
    uint32_t in = (uint32_t)(b & (V.cb - 1));
    *lba = cl_sec(c) + (in >> V.ss_shift);
    *off = in & (V.ss - 1);
    return 0;
}
static int dent_rd(exst_t* d, uint32_t slot, uint8_t* e){
    uint64_t lba; uint32_t off;
    if(dent_loc(d, slot, &lba, &off) != 0) return -1;
    uint8_t* p = sec_get(lba);
    if(!p) return -1;
    mcopy(e, p + off, 32);
    return 0;
}
static int dent_wr(exst_t* d, uint32_t slot, const uint8_t* e){
    uint64_t lba; uint32_t off;
    if(V.ro || dent_loc(d, slot, &lba, &off) != 0) return -1;
    uint8_t* p = sec_get(lba);
    if(!p) return -1;
    mcopy(p + off, e, 32);
    return sec_put(lba, p);
}

static uint16_t set_csum(const uint8_t* raw, uint32_t cnt){
    uint16_t cs = 0;
    for(uint32_t i = 0; i < cnt * 32; i++){
        if(i == 2 || i == 3) continue;
        cs = (uint16_t)(((cs & 1) ? 0x8000 : 0) + (cs >> 1) + raw[i]);
    }
    return cs;
}

static uint32_t g_corrupt_logged;
static void log_corrupt(const char* what, uint32_t slot){
    if(g_corrupt_logged >= 32) return;      /* do not flood on a trashed image */
    g_corrupt_logged++;
    debugcon_writestring("[EXFAT] corrupt entry set ("); debugcon_writestring(what);
    debugcon_writestring(") at slot "); debugcon_print_hex(slot); debugcon_writestring(" - skipped\n");
}

/* Read + verify the entry set whose File entry is at `slot`.
 * 0 = valid, -1 = corrupt (skip it), -2 = I/O / out of range. */
static int set_read(exst_t* d, uint32_t slot, exset_t* s){
    uint8_t* r = s->raw;
    if(dent_rd(d, slot, r) != 0) return -2;
    if(r[0] != ET_FILE) return -1;
    uint32_t sc = r[1];
    if(sc < 2 || sc > EX_MAXSET - 1){ log_corrupt("SecondaryCount", slot); return -1; }
    if((uint64_t)(slot + 1 + sc) * 32 > d->size){ log_corrupt("set past dir end", slot); return -1; }
    for(uint32_t i = 1; i <= sc; i++) if(dent_rd(d, slot + i, r + 32 * i) != 0) return -2;
    if(r[32] != ET_STREAM){ log_corrupt("no stream entry", slot); return -1; }
    uint32_t nlen = r[32 + 3];
    uint32_t nn = (nlen + 14) / 15;
    if(nlen == 0 || nn > sc - 1){ log_corrupt("NameLength", slot); return -1; }
    for(uint32_t i = 0; i < nn; i++) if(r[64 + 32 * i] != ET_NAME){ log_corrupt("name entry", slot); return -1; }
    for(uint32_t i = 2 + nn; i <= sc; i++)
        if((r[32 * i] & 0xC0) != 0xC0){ log_corrupt("secondary type", slot); return -1; }
    if(set_csum(r, sc + 1) != rd16(r + 2)){ log_corrupt("SetChecksum", slot); return -1; }
    for(uint32_t i = 0; i < nlen; i++) s->name[i] = rd16(r + 64 + 32 * (i / 15) + 2 + 2 * (i % 15));
    for(uint32_t i = 0; i < nlen; i++)
        if(s->name[i] == 0 || s->name[i] == '/'){ log_corrupt("name char", slot); return -1; }
    s->nlen = nlen; s->slot = slot; s->cnt = sc + 1;
    return 0;
}

/* Iterate every valid entry set of a directory. cb returns nonzero to stop
 * (that value is returned). 0 = scanned to the end, -2 = I/O error. */
typedef int (*set_cb_t)(exset_t* s, void* u);
static int dir_scan(exst_t* d, set_cb_t cb, void* u){
    uint32_t nslots = (uint32_t)(d->size / 32);
    static exset_t sset;
    uint8_t e[32];
    for(uint32_t slot = 0; slot < nslots;){
        if(dent_rd(d, slot, e) != 0) return -2;
        if(e[0] == ET_EOD) break;
        if(e[0] == ET_FILE){
            int r = set_read(d, slot, &sset);
            if(r == -2) return -2;
            if(r == 0){
                int c = cb(&sset, u);
                if(c) return c;
                slot += sset.cnt;
                continue;
            }
        }
        slot++;
    }
    return 0;
}

/* ── Objects (a file or directory found by path) ─────────────────────────── */
typedef struct {
    int      is_root;
    exst_t   dir;        /* directory holding the entry set (copy) */
    uint32_t slot, cnt;  /* entry set location */
    uint16_t attr;
    exst_t   st;         /* the object's own data */
    int      bad;        /* stream fields failed validation */
    uint32_t tc, tm, ta;
    uint8_t  tc10, tm10, uc, um, ua;
    uint16_t hash;
    uint32_t nlen;
} exobj_t;

static void obj_from_set(exobj_t* o, const exst_t* dir, const exset_t* s){
    const uint8_t* r = s->raw;
    mzero(o, sizeof(*o));
    o->dir = *dir; o->dir.cvalid = 0;
    o->slot = s->slot; o->cnt = s->cnt;
    o->attr = rd16(r + 4);
    o->tc = rd32(r + 8); o->tm = rd32(r + 12); o->ta = rd32(r + 16);
    o->tc10 = r[20]; o->tm10 = r[21]; o->uc = r[22]; o->um = r[23]; o->ua = r[24];
    uint8_t fl = r[33];
    o->hash = rd16(r + 36); o->nlen = r[35];
    o->st.nofat = (fl & SF_NOFAT) ? 1 : 0;
    o->st.vdl = rd64(r + 40);
    o->st.first = rd32(r + 52);
    o->st.size = rd64(r + 56);
    /* Structural sanity of the stream (deeper chain check is lazy). */
    if(o->st.vdl > o->st.size) o->bad = 1;
    if(o->st.size > ((uint64_t)V.ccount << V.cb_shift)) o->bad = 1;
    if(o->st.size && !cl_ok(o->st.first)) o->bad = 1;
    if(o->st.size && !(fl & SF_ALLOC)) o->bad = 1;
    if(o->st.nofat && o->st.size &&
       (uint64_t)o->st.first + st_nclus(&o->st) - 1 >= (uint64_t)V.ccount + 2) o->bad = 1;
    if(o->attr & AT_DIR){
        if(o->st.size > EX_MAX_DIR || (o->st.size & (V.cb - 1))) o->bad = 1;
        o->st.vdl = o->st.size;       /* directories are always fully valid */
    }
    if(!o->st.size){ o->st.first = 0; o->st.nofat = 0; }
}

struct find_ctx { const uint16_t* name; uint32_t nlen; uint16_t hash; exset_t* out; };
static int find_cb(exset_t* s, void* u){
    struct find_ctx* f = (struct find_ctx*)u;
    if(rd16(s->raw + 36) != f->hash) return 0;
    if(!name_eq(s->name, s->nlen, f->name, f->nlen)) return 0;
    mcopy(f->out, s, sizeof(*s));
    return 1;
}
/* Find `name` in directory `d`. 0 found, -1 not found, -2 I/O. */
static int dir_find(exst_t* d, const uint16_t* name, uint32_t nlen, exset_t* out){
    struct find_ctx f = { name, nlen, name_hash(name, nlen), out };
    int r = dir_scan(d, find_cb, &f);
    if(r == 1) return 0;
    return r == -2 ? -2 : -1;
}

static void obj_root(exobj_t* o){
    mzero(o, sizeof(*o));
    o->is_root = 1; o->attr = AT_DIR; o->st = V.root;
}
static exst_t* obj_dirst(exobj_t* o){ return o->is_root ? &V.root : &o->st; }

/* Resolve a mount-relative path. 0 ok, -1 not found / not a dir, -2 I/O. */
static int ex_resolve(const char* path, exobj_t* o){
    static exset_t rs;
    obj_root(o);
    if(!path) return -1;
    const char* p = path;
    while(*p == '/') p++;
    while(*p){
        const char* c = p;
        while(*p && *p != '/') p++;
        size_t cl = (size_t)(p - c);
        while(*p == '/') p++;
        if(cl == 0 || (cl == 1 && c[0] == '.')) continue;
        if(!(o->attr & AT_DIR) || o->bad) return -1;
        uint16_t nm[255]; uint32_t nl;
        if(utf8_to16(c, cl, nm, &nl) != 0) return -1;
        exst_t dir = *obj_dirst(o);
        int r = dir_find(&dir, nm, nl, &rs);
        if(r != 0) return r;
        obj_from_set(o, &dir, &rs);
    }
    return 0;
}

/* Split "/a/b/name": resolve the parent directory, decode the final name. */
static int ex_resolve_parent(const char* path, exobj_t* parent, uint16_t* name, uint32_t* nlen){
    if(!path) return -1;
    size_t n = slen(path);
    while(n > 1 && path[n - 1] == '/') n--;
    size_t ls = n;
    while(ls > 0 && path[ls - 1] != '/') ls--;
    if(ls == n) return -1;                     /* empty final component */
    char pp[256];
    if(ls >= sizeof(pp)) return -1;
    mcopy(pp, path, ls); pp[ls] = 0;
    if(utf8_to16(path + ls, n - ls, name, nlen) != 0) return -1;
    if(ex_resolve(pp, parent) != 0) return -1;
    if(!(parent->attr & AT_DIR) || parent->bad) return -1;
    return 0;
}

/* Persist an object's stream (and optionally its modify time) into its set. */
static int obj_store(exobj_t* o, int touch){
    if(o->is_root){ V.root = o->st; return 0; }
    static exset_t ws;
    if(set_read(&o->dir, o->slot, &ws) != 0) return -1;
    uint8_t* r = ws.raw;
    r[33] = (uint8_t)(SF_ALLOC | (o->st.nofat ? SF_NOFAT : 0));
    wr64(r + 40, o->st.vdl);
    wr32(r + 52, o->st.first);
    wr64(r + 56, o->st.size);
    if(touch){
        uint8_t ms;
        uint32_t t = ts_now(&ms);
        wr32(r + 12, t); wr32(r + 16, t);
        r[21] = ms; r[23] = 0x80; r[24] = 0x80;
        if(!(o->attr & AT_DIR)) o->attr |= AT_ARCH;
        wr16(r + 4, o->attr);
        o->tm = o->ta = t; o->tm10 = ms; o->um = o->ua = 0x80;
    }
    wr16(r + 2, set_csum(r, ws.cnt));
    if(dent_wr(&o->dir, o->slot, r) != 0) return -1;
    return dent_wr(&o->dir, o->slot + 1, r + 32);
}

/* Build an entry set for `name` (attr + stream). Returns the entry count. */
static uint32_t set_build(uint8_t* raw, const uint16_t* name, uint32_t nlen, uint16_t attr, const exst_t* st){
    uint32_t nn = (nlen + 14) / 15, cnt = 2 + nn;
    mzero(raw, cnt * 32);
    uint8_t ms;
    uint32_t t = ts_now(&ms);
    raw[0] = ET_FILE; raw[1] = (uint8_t)(cnt - 1);
    wr16(raw + 4, attr);
    wr32(raw + 8, t); wr32(raw + 12, t); wr32(raw + 16, t);
    raw[20] = ms; raw[21] = ms; raw[22] = 0x80; raw[23] = 0x80; raw[24] = 0x80;
    uint8_t* s = raw + 32;
    s[0] = ET_STREAM;
    s[1] = (uint8_t)(SF_ALLOC | (st->nofat ? SF_NOFAT : 0));
    s[3] = (uint8_t)nlen;
    wr16(s + 4, name_hash(name, nlen));
    wr64(s + 8, st->vdl);
    wr32(s + 20, st->first);
    wr64(s + 24, st->size);
    for(uint32_t i = 0; i < nn; i++){
        uint8_t* e = raw + 64 + 32 * i;
        e[0] = ET_NAME;
        for(uint32_t k = 0; k < 15 && i * 15 + k < nlen; k++) wr16(e + 2 + 2 * k, name[i * 15 + k]);
    }
    wr16(raw + 2, set_csum(raw, cnt));
    return cnt;
}

/* Zero a freshly allocated directory cluster through the cache. */
static int cl_zero_meta(uint32_t c){
    uint64_t lba = cl_sec(c);
    uint32_t n = 1u << V.spc_shift;
    for(uint32_t i = 0; i < n; i++){
        if(dev_wr(lba + i, g_zero, 1) != 0) return -1;
    }
    cache_inval(lba, n);
    return 0;
}

/* Insert a prepared entry set into directory object `dobj`, growing it by
 * whole clusters if no run of `cnt` free slots exists. */
static int dir_insert(exobj_t* dobj, uint8_t* raw, uint32_t cnt, uint32_t* slot_out){
    exst_t* d = obj_dirst(dobj);
    uint32_t nslots = (uint32_t)(d->size / 32);
    uint32_t run = 0, start = 0;
    uint8_t e[32];
    int found = 0;
    for(uint32_t s = 0; s < nslots; s++){
        if(dent_rd(d, s, e) != 0) return -1;
        if(e[0] == ET_EOD){
            /* everything from here on is free (spec: EOD is followed by EOD) */
            if(run == 0) start = s;
            run = nslots - start;
            if(run >= cnt) found = 1;
            break;
        }
        if(!(e[0] & ET_INUSE)){
            if(run == 0) start = s;
            if(++run >= cnt){ found = 1; break; }
        } else run = 0;
    }
    if(!found){
        if(run == 0) start = nslots;
        uint32_t extra = (cnt - run) * 32;
        uint32_t add = (extra + V.cb - 1) >> V.cb_shift;
        if(d->size + ((uint64_t)add << V.cb_shift) > EX_MAX_DIR) return -1;
        uint32_t have = st_nclus(d);
        if(st_grow(d, have, have + add) != 0) return -1;
        for(uint32_t i = 0; i < add; i++){
            uint32_t c;
            if(st_clus(d, have + i, &c) != 0 || cl_zero_meta(c) != 0){ st_shrink(d, have + add, have); return -1; }
        }
        d->size += (uint64_t)add << V.cb_shift;
        d->vdl = d->size;
        if(!dobj->is_root && obj_store(dobj, 1) != 0) return -1;
    }
    for(uint32_t i = 0; i < cnt; i++) if(dent_wr(d, start + i, raw + 32 * i) != 0) return -1;
    *slot_out = start;
    return 0;
}

/* Mark every entry of a set not-in-use (spec: delete = clear InUse). */
static int set_delete(exst_t* d, uint32_t slot, uint32_t cnt){
    uint8_t e[32];
    for(uint32_t i = 0; i < cnt; i++){
        if(dent_rd(d, slot + i, e) != 0) return -1;
        e[0] &= (uint8_t)~ET_INUSE;
        if(dent_wr(d, slot + i, e) != 0) return -1;
    }
    return 0;
}

/* ── VFS inode pool ──────────────────────────────────────────────────────── */
typedef struct { exobj_t o; int chain_ok; int dead; } exnode_t;
#define EX_INODES 64
static vfs_inode_t g_ino[EX_INODES];
static exnode_t    g_nd[EX_INODES];
static int         g_ino_used, g_ino_next;

static vfs_inode_t* inode_get(const char* path){
    for(int i = 0; i < g_ino_used; i++) if(seq(g_ino[i].path, path)) return &g_ino[i];
    int idx;
    if(g_ino_used < EX_INODES) idx = g_ino_used++;
    else { idx = g_ino_next; g_ino_next = (g_ino_next + 1) % EX_INODES; }
    vfs_inode_t* ino = &g_ino[idx];
    /* The page cache is keyed by inode pointer: a slot handed to another file
     * (or reused after a remount) must not serve the previous file's pages. */
    pagecache_invalidate(ino, 0, 1ULL << 62);
    size_t k = 0;
    for(; path[k] && k < sizeof(ino->path) - 1; k++) ino->path[k] = path[k];
    ino->path[k] = 0;
    ino->fs_data = &g_nd[idx];
    return ino;
}
static int same_loc(const exobj_t* a, uint32_t dfirst, uint32_t slot){
    return !a->is_root && a->dir.first == dfirst && a->slot == slot;
}
/* An entry set moved or vanished: retarget / kill cached nodes. */
static void nodes_moved(uint32_t odfirst, uint32_t oslot, const exobj_t* now){
    for(int i = 0; i < g_ino_used; i++){
        exnode_t* n = &g_nd[i];
        if(n->dead || !same_loc(&n->o, odfirst, oslot)) continue;
        /* Open fds keep working on a renamed file; the old path no longer
         * names this slot either way. */
        if(now){ n->o.dir = now->dir; n->o.slot = now->slot; n->o.cnt = now->cnt; }
        else n->dead = 1;
        g_ino[i].path[0] = 0;
    }
}

static vfs_fs_ops_t exfat_ops;
static int ex_attached(void){ return V.mounted && vfs_mount_point_of(&exfat_ops) != NULL; }

static void ino_fill(vfs_inode_t* ino, const exobj_t* o){
    ino->type = (o->attr & AT_DIR) ? VFS_NODE_DIR : VFS_NODE_FILE;
    ino->size = (o->attr & AT_DIR) ? 0 : (size_t)o->st.size;
    ino->mode = 0;
    if(!(o->attr & AT_DIR) && (o->attr & AT_RO)) ino->mode = 0100444u;
    ino->uid = ino->gid = 0;
    ino->nlink = 1;
    if(o->is_root){ ino->atime = ino->mtime = ino->ctime = 0; return; }
    ino->mtime = ts_to_unix(o->tm, o->tm10, o->um);
    ino->atime = ts_to_unix(o->ta, 0, o->ua);
    ino->ctime = ts_to_unix(o->tc, o->tc10, o->uc);
    if(!ino->ctime) ino->ctime = ino->mtime;
}

/* ── VFS operations ──────────────────────────────────────────────────────── */
static vfs_inode_t* ex_lookup(const char* path){
    if(!V.mounted) return NULL;
    exobj_t o;
    if(ex_resolve(path, &o) != 0) return NULL;
    vfs_inode_t* ino = inode_get(path && path[0] ? path : "/");
    exnode_t* n = (exnode_t*)ino->fs_data;
    n->o = o; n->chain_ok = 0; n->dead = 0;
    ino_fill(ino, &o);
    return ino;
}

struct rd_ctx { const char* base; size_t bl; vfs_iter_cb cb; void* user; };
static int readdir_cb(exset_t* s, void* u){
    struct rd_ctx* c = (struct rd_ctx*)u;
    static vfs_inode_t child;
    size_t k = 0;
    for(; k < c->bl && k < sizeof(child.path) - 1; k++) child.path[k] = c->base[k];
    if(k >= sizeof(child.path) - 2) return 0;
    child.path[k++] = '/';
    if(utf16_to8(s->name, s->nlen, child.path + k, sizeof(child.path) - k) < 0) return 0;
    uint16_t attr = rd16(s->raw + 4);
    child.type = (attr & AT_DIR) ? VFS_NODE_DIR : VFS_NODE_FILE;
    child.size = (attr & AT_DIR) ? 0 : (size_t)rd64(s->raw + 56);
    child.fs_data = NULL; child.ops = NULL;
    child.mode = 0; child.uid = child.gid = 0; child.nlink = 1;
    child.mtime = ts_to_unix(rd32(s->raw + 12), s->raw[21], s->raw[23]);
    child.atime = child.ctime = child.mtime;
    c->cb(&child, c->user);
    return 0;
}
static int ex_readdir(const char* dir_path, vfs_iter_cb cb, void* user){
    if(!V.mounted || !cb) return -1;
    exobj_t o;
    if(ex_resolve(dir_path, &o) != 0) return -1;
    if(!(o.attr & AT_DIR) || o.bad) return -1;
    struct rd_ctx c;
    c.base = dir_path ? dir_path : "";
    c.bl = slen(c.base);
    if(c.bl == 1 && c.base[0] == '/') c.bl = 0;
    c.cb = cb; c.user = user;
    exst_t d = *obj_dirst(&o);
    if(!o.is_root && st_validate(&d) != 0) return -1;
    return dir_scan(&d, readdir_cb, &c) == -2 ? -1 : 0;
}

static int node_ready(exnode_t* n){
    if(n->dead || n->o.bad) return -1;
    if(!n->chain_ok){
        if(st_validate(&n->o.st) != 0){
            n->o.bad = 1;
            ex_logx("corrupt cluster chain, first=", n->o.st.first);
            return -1;
        }
        n->chain_ok = 1;
    }
    return 0;
}

static int ex_read(vfs_inode_t* ino, size_t off, void* buf, size_t len){
    if(!ex_attached() || !ino || ino->type != VFS_NODE_FILE || !ino->fs_data) return -1;
    exnode_t* n = (exnode_t*)ino->fs_data;
    if(node_ready(n) != 0) return -1;
    if(len > 0x7FFFFFFF) len = 0x7FFFFFFF;
    int64_t r = st_read(&n->o.st, off, (uint8_t*)buf, len);
    return r < 0 ? -1 : (int)r;
}

/* Write `len` bytes at `off` into file object `o`, growing it as needed. */
static int obj_write(exobj_t* o, uint64_t off, const uint8_t* in, uint64_t len){
    exst_t* s = &o->st;
    if(len == 0) return 0;
    uint64_t end = off + len;
    if(end < off || end > ((uint64_t)V.ccount << V.cb_shift)) return -1;
    if(vol_dirty() != 0) return -1;
    uint32_t have = st_nclus(s);
    uint32_t need = (uint32_t)((end + V.cb - 1) >> V.cb_shift);
    if(need > have && st_grow(s, have, need) != 0) return -1;
    if(off > s->vdl && st_rw(s, s->vdl, NULL, off - s->vdl) != 0) goto fail;
    if(st_rw(s, off, in, len) != 0) goto fail;
    if(end > s->size) s->size = end;
    if(end > s->vdl) s->vdl = end;
    if(obj_store(o, 1) != 0) return -1;
    return (int)len;
fail:
    if(need > have) st_shrink(s, need, have);
    return -1;
}

static int ex_write(vfs_inode_t* ino, size_t off, const void* data, size_t len){
    if(!ex_attached() || V.ro || !ino || ino->type != VFS_NODE_FILE || !ino->fs_data) return -1;
    exnode_t* n = (exnode_t*)ino->fs_data;
    if(node_ready(n) != 0) return -1;
    if(len > 0x7FFFFFFF) len = 0x7FFFFFFF;
    int r = obj_write(&n->o, off, (const uint8_t*)data, len);
    ino->size = (size_t)n->o.st.size;
    return r;
}

static int ex_new_entry(const char* path, uint16_t attr, exst_t* st, exobj_t* made){
    exobj_t parent, tmp;
    uint16_t nm[255]; uint32_t nl;
    if(ex_resolve(path, &tmp) == 0) return -1;            /* exists */
    if(ex_resolve_parent(path, &parent, nm, &nl) != 0) return -1;
    if(!name_valid(nm, nl)) return -1;
    if(!parent.is_root && st_validate(&parent.st) != 0) return -1;
    static uint8_t raw[EX_MAXSET * 32];
    uint32_t cnt = set_build(raw, nm, nl, attr, st);
    uint32_t slot;
    if(dir_insert(&parent, raw, cnt, &slot) != 0) return -1;
    if(made){
        static exset_t ms;
        exst_t* pd = obj_dirst(&parent);
        if(set_read(pd, slot, &ms) != 0) return -1;
        obj_from_set(made, pd, &ms);
    }
    return 0;
}

static int ex_create(const char* path, const void* initial, size_t size){
    if(!V.mounted || V.ro || !path) return -1;
    if(vol_dirty() != 0) return -1;
    exst_t st; mzero(&st, sizeof(st));
    exobj_t o;
    if(ex_new_entry(path, AT_ARCH, &st, &o) != 0) return -1;
    if(initial && size > 0 && obj_write(&o, 0, (const uint8_t*)initial, size) < 0) return -1;
    return 0;
}

static int ex_mkdir(const char* path){
    if(!V.mounted || V.ro || !path) return -1;
    if(vol_dirty() != 0) return -1;
    exst_t st; mzero(&st, sizeof(st));
    if(st_grow(&st, 0, 1) != 0) return -1;
    if(cl_zero_meta(st.first) != 0){ st_shrink(&st, 1, 0); return -1; }
    st.size = st.vdl = V.cb;
    if(ex_new_entry(path, AT_DIR, &st, NULL) != 0){ st_shrink(&st, 1, 0); return -1; }
    return 0;
}

static int any_cb(exset_t* s, void* u){ (void)s; (void)u; return 1; }

static int ex_remove(const char* path){
    if(!V.mounted || V.ro || !path) return -1;
    exobj_t o;
    if(ex_resolve(path, &o) != 0 || o.is_root) return -1;
    if(vol_dirty() != 0) return -1;
    int can_free = !o.bad && st_validate(&o.st) == 0;
    if((o.attr & AT_DIR) && can_free){
        exst_t d = o.st;
        int r = dir_scan(&d, any_cb, NULL);
        if(r != 0) return -1;                     /* not empty, or I/O error */
    }
    /* Unlink first, free after: a crash leaks clusters instead of leaving an
     * entry that points at free space. A corrupt stream is unlinked but its
     * clusters are left alone (fsck reclaims them) rather than trusted. */
    if(set_delete(&o.dir, o.slot, o.cnt) != 0) return -1;
    nodes_moved(o.dir.first, o.slot, NULL);
    if(can_free && o.st.size) st_shrink(&o.st, st_nclus(&o.st), 0);
    return 0;
}

static int ex_rename(const char* oldp, const char* newp){
    if(!V.mounted || V.ro || !oldp || !newp) return -1;
    exobj_t o, parent, t;
    uint16_t nm[255]; uint32_t nl;
    if(ex_resolve(oldp, &o) != 0 || o.is_root) return -1;
    if(ex_resolve_parent(newp, &parent, nm, &nl) != 0) return -1;
    if(!name_valid(nm, nl)) return -1;
    if(vol_dirty() != 0) return -1;
    static exset_t os;
    if(set_read(&o.dir, o.slot, &os) != 0) return -1;
    if(ex_resolve(newp, &t) == 0 && !t.is_root){
        if(!same_loc(&t, o.dir.first, o.slot)){
            /* replace an existing target (the VFS checked the type rules) */
            if(ex_remove(newp) != 0) return -1;
            if(ex_resolve_parent(newp, &parent, nm, &nl) != 0) return -1;
            if(set_read(&o.dir, o.slot, &os) != 0) return -1;
        }
    }
    /* New set = old File entry fields + old stream fields + the new name. */
    static uint8_t raw[EX_MAXSET * 32];
    exst_t st = o.st;
    uint32_t cnt = set_build(raw, nm, nl, o.attr, &st);
    mcopy(raw + 4, os.raw + 4, 28);                  /* attr + timestamps */
    raw[33] = os.raw[33];                            /* stream flags verbatim */
    wr64(raw + 40, rd64(os.raw + 40));
    wr32(raw + 52, rd32(os.raw + 52));
    wr64(raw + 56, rd64(os.raw + 56));
    wr16(raw + 2, set_csum(raw, cnt));
    uint32_t slot;
    if(dir_insert(&parent, raw, cnt, &slot) != 0) return -1;
    /* If the new set landed in the same directory, re-read the old location
     * (inserting may have grown the directory, never moved the old set). */
    exst_t* pd = obj_dirst(&parent);
    exst_t od = o.dir;
    if(!o.dir.first || (pd->first == o.dir.first)) od = *pd;
    if(set_delete(&od, o.slot, o.cnt) != 0) return -1;
    exobj_t now; mzero(&now, sizeof(now));
    now.dir = *pd; now.dir.cvalid = 0; now.slot = slot; now.cnt = cnt;
    nodes_moved(o.dir.first, o.slot, &now);
    return 0;
}

static int ex_truncate(const char* path, size_t new_size){
    if(!V.mounted || V.ro || !path) return -1;
    exobj_t o;
    if(ex_resolve(path, &o) != 0 || (o.attr & AT_DIR) || o.is_root || o.bad) return -1;
    if(st_validate(&o.st) != 0) return -1;
    if((uint64_t)new_size > ((uint64_t)V.ccount << V.cb_shift)) return -1;
    if(vol_dirty() != 0) return -1;
    exst_t* s = &o.st;
    uint32_t have = st_nclus(s);
    uint32_t need = (uint32_t)(((uint64_t)new_size + V.cb - 1) >> V.cb_shift);
    uint64_t old = s->size;
    if(need < have){ if(st_shrink(s, have, need) != 0) return -1; }
    else if(need > have){ if(st_grow(s, have, need) != 0) return -1; }
    if((uint64_t)new_size > old){
        /* Zero the grown range on disk and make it valid, instead of relying on
         * ValidDataLength alone: readers that ignore VDL (some FUSE drivers)
         * would otherwise see the stale contents of reused clusters. */
        if(st_rw(s, s->vdl, NULL, (uint64_t)new_size - s->vdl) != 0){
            if(need > have) st_shrink(s, need, have);
            return -1;
        }
        s->vdl = new_size;
    }
    s->size = new_size;
    if(s->vdl > new_size) s->vdl = new_size;
    if(obj_store(&o, 1) != 0) return -1;
    /* refresh cached nodes of this entry (open fds) */
    for(int i = 0; i < g_ino_used; i++){
        exnode_t* n = &g_nd[i];
        if(!n->dead && same_loc(&n->o, o.dir.first, o.slot)){ n->o.st = o.st; n->chain_ok = 0; g_ino[i].size = (size_t)o.st.size; }
    }
    return 0;
}

static int ex_setattr(const char* path, const vfs_attr_t* a, unsigned valid){
    if(!V.mounted || !path || !a) return -1;
    exobj_t o;
    if(ex_resolve(path, &o) != 0) return -1;
    if(o.is_root) return 0;
    if(!(valid & (VFS_ATTR_MODE | VFS_ATTR_MTIME | VFS_ATTR_ATIME))) return 0;  /* no owners on exFAT */
    if(V.ro || vol_dirty() != 0) return -1;
    static exset_t as;
    if(set_read(&o.dir, o.slot, &as) != 0) return -1;
    uint8_t* r = as.raw;
    if(valid & VFS_ATTR_MODE){
        uint16_t at = rd16(r + 4);
        if(a->mode & 0222) at &= (uint16_t)~AT_RO; else at |= AT_RO;
        wr16(r + 4, at);
    }
    if(valid & VFS_ATTR_MTIME){ wr32(r + 12, ts_from_unix(a->mtime)); r[21] = 0; r[23] = 0x80; }
    if(valid & VFS_ATTR_ATIME){ wr32(r + 16, ts_from_unix(a->atime)); r[24] = 0x80; }
    wr16(r + 2, set_csum(r, as.cnt));
    return dent_wr(&o.dir, o.slot, r);
}

static int ex_statfs(vfs_statfs_t* st){
    if(!V.mounted || !st) return -1;
    st->type = EXFAT_MAGIC;
    st->bsize = st->frsize = V.cb;
    st->blocks = V.ccount;
    st->bfree = st->bavail = V.free;
    st->files = 0; st->ffree = 0;
    st->namelen = 255;
    st->flags = V.ro ? 1 : 0;                    /* ST_RDONLY */
    return 0;
}

int exfat_sync(void){
    if(!V.mounted) return 0;
    return vol_clean();
}
static int ex_sync(void){ return exfat_sync(); }

static int ex_mntinfo(const char** src, const char** type){
    if(!V.mounted) return -1;
    if(src) *src = g_devname;
    if(type) *type = "exfat";
    return 0;
}

static vfs_fs_ops_t exfat_ops = {
    .lookup = ex_lookup, .readdir = ex_readdir, .read = ex_read, .write = ex_write,
    .create = ex_create, .mkdir = ex_mkdir, .remove = ex_remove, .rename = ex_rename,
    .truncate = ex_truncate, .setattr = ex_setattr, .sync = ex_sync,
    .statfs = ex_statfs, .mntinfo = ex_mntinfo
};

int exfat_is_mounted(void){ return ex_attached(); }

/* ── Mount ───────────────────────────────────────────────────────────────── */
/* exFAT boot checksum over sectors 0..10 (VolumeFlags + PercentInUse skipped). */
static uint32_t boot_csum_step(uint32_t cs, const uint8_t* p, uint32_t n, uint32_t base){
    for(uint32_t i = 0; i < n; i++){
        uint32_t k = base + i;
        if(k == 106 || k == 107 || k == 112) continue;
        cs = ((cs & 1) ? 0x80000000u : 0) + (cs >> 1) + p[i];
    }
    return cs;
}

typedef struct {
    uint32_t ss_shift, spc_shift, nfats;
    uint64_t vol_len;
    uint32_t fat_off, fat_len, heap_off, ccount, root;
    uint16_t flags;
} exgeo_t;

/* Validate the boot sector fields (sector already in `bs`). */
static int geo_parse(const uint8_t* bs, uint32_t dss, uint64_t dev_bytes, exgeo_t* g){
    static const char sig[8] = { 'E','X','F','A','T',' ',' ',' ' };
    if(bs[0] != 0xEB || bs[1] != 0x76 || bs[2] != 0x90) return -1;
    for(int i = 0; i < 8; i++) if(bs[3 + i] != (uint8_t)sig[i]) return -1;
    for(int i = 11; i < 64; i++) if(bs[i]) return -1;          /* MustBeZero */
    if(bs[510] != 0x55 || bs[511] != 0xAA) return -1;
    g->ss_shift = bs[108];
    g->spc_shift = bs[109];
    g->nfats = bs[110];
    if(g->ss_shift < 9 || g->ss_shift > 12) return -1;
    if((1u << g->ss_shift) < dss) return -1;                   /* fs sector >= device sector */
    if(g->spc_shift > 25 - g->ss_shift) return -1;
    if(g->nfats != 1 && g->nfats != 2) return -1;
    if(bs[105] != 1) return -1;                                /* revision 1.xx */
    g->vol_len = rd64(bs + 72);
    g->fat_off = rd32(bs + 80); g->fat_len = rd32(bs + 84);
    g->heap_off = rd32(bs + 88); g->ccount = rd32(bs + 92);
    g->root = rd32(bs + 96);
    g->flags = rd16(bs + 106);
    uint64_t ss = 1ull << g->ss_shift;
    if(g->vol_len < (1ull << 20) / ss) return -1;
    if(g->vol_len > dev_bytes / ss) return -1;                 /* larger than the device */
    if(g->fat_off < 24) return -1;
    if(g->ccount == 0 || g->ccount > 0xFFFFFFF5u) return -1;
    if((uint64_t)g->fat_len * ss < ((uint64_t)g->ccount + 2) * 4) return -1;
    if((uint64_t)g->fat_off + (uint64_t)g->fat_len * g->nfats > g->heap_off) return -1;
    if((uint64_t)g->heap_off + ((uint64_t)g->ccount << g->spc_shift) > g->vol_len) return -1;
    if(g->root < 2 || (uint64_t)g->root >= (uint64_t)g->ccount + 2) return -1;
    return 0;
}

/* Read + validate one boot region starting at fs sector `base` with the
 * given sector size. Returns 0 and the geometry, or -1. */
static int boot_region_ok(block_dev_t* dev, uint32_t ss_shift, uint32_t base, exgeo_t* g){
    static uint8_t buf[EX_MAX_SS];
    uint32_t dss = dev->sector_size;
    uint32_t ss = 1u << ss_shift;
    if(ss < dss || ss > EX_MAX_SS) return -1;
    uint32_t spf = ss / dss;
    uint64_t dev_bytes = dev->sector_count * (uint64_t)dss;
    if((uint64_t)(base + 12) * ss > dev_bytes) return -1;
    if(block_read(dev, (uint64_t)base * spf, buf, spf) != (int)spf) return -1;
    if(geo_parse(buf, dss, dev_bytes, g) != 0 || g->ss_shift != ss_shift) return -1;
    uint32_t cs = boot_csum_step(0, buf, ss, 0);
    for(uint32_t s = 1; s <= 10; s++){
        if(block_read(dev, (uint64_t)(base + s) * spf, buf, spf) != (int)spf) return -1;
        cs = boot_csum_step(cs, buf, ss, s * ss);
    }
    if(block_read(dev, (uint64_t)(base + 11) * spf, buf, spf) != (int)spf) return -1;
    for(uint32_t i = 0; i < ss; i += 4) if(rd32(buf + i) != cs) return -1;
    return 0;
}

int exfat_probe(const char* dev_name){
    block_dev_t* dev = block_find(dev_name);
    static uint8_t b[EX_MAX_SS];
    if(!dev || dev->sector_size < 512 || dev->sector_size > EX_MAX_SS) return 0;
    if(block_read(dev, 0, b, 1) != 1) return 0;
    return b[3] == 'E' && b[4] == 'X' && b[5] == 'F' && b[6] == 'A' && b[7] == 'T';
}

/* Read a FAT-chained metadata stream (bitmap / up-case) into memory. */
static int meta_clusters(uint32_t first, uint64_t len, uint32_t* out, uint32_t n){
    uint32_t c = first;
    for(uint32_t i = 0; i < n; i++){
        out[i] = c;
        if(i + 1 < n){
            uint32_t nx;
            if(fat_next(c, &nx) != 1) return -1;
            c = nx;
        }
    }
    (void)len;
    return 0;
}

struct root_scan { uint32_t bmp_first; uint64_t bmp_len; int bmp_found;
                   uint32_t up_first; uint64_t up_len; uint32_t up_csum; int up_found; };

static int load_upcase(struct root_scan* rs){
    for(uint32_t i = 0; i < 65536; i++) g_up[i] = (uint16_t)i;
    if(rs->up_len < 2 || rs->up_len > 262144 || (rs->up_len & 1)) return -1;
    if(!cl_ok(rs->up_first)) return -1;
    uint8_t* t = (uint8_t*)kmalloc((size_t)rs->up_len);
    if(!t) return -1;
    exst_t s; mzero(&s, sizeof(s));
    s.first = rs->up_first; s.size = s.vdl = rs->up_len; s.nofat = 0;
    if(st_validate(&s) != 0 || st_read(&s, 0, t, rs->up_len) != (int64_t)rs->up_len){ kfree(t); return -1; }
    uint32_t cs = 0;
    for(uint64_t i = 0; i < rs->up_len; i++) cs = ((cs & 1) ? 0x80000000u : 0) + (cs >> 1) + t[i];
    if(cs != rs->up_csum){ kfree(t); ex_log("up-case table checksum mismatch"); return -1; }
    uint32_t o = 0;
    for(uint64_t i = 0; i + 1 < rs->up_len && o < 65536; i += 2){
        uint16_t v = rd16(t + i);
        if(v == 0xFFFF && i + 3 < rs->up_len){
            uint32_t run = rd16(t + i + 2);
            o += run;                                 /* identity range */
            i += 2;
            continue;
        }
        g_up[o++] = v;
    }
    kfree(t);
    return 0;
}

int exfat_mount(const char* dev_name, const char* mount_point){
    block_dev_t* dev = block_find(dev_name);
    if(!dev || !mount_point || mount_point[0] != '/') return -1;
    if(dev->sector_size < 512 || dev->sector_size > EX_MAX_SS || (dev->sector_size & (dev->sector_size - 1))) return -1;
    if(ex_attached()) return -1;                      /* single instance busy */
    if(!exfat_probe(dev_name)) return -1;

    /* Boot region: main, else the backup (then read-only). */
    exgeo_t g;
    int backup = 0;
    static uint8_t b0[EX_MAX_SS];
    if(block_read(dev, 0, b0, 1) != 1) return -1;
    uint32_t sh = b0[108];
    if(!(sh >= 9 && sh <= 12 && boot_region_ok(dev, sh, 0, &g) == 0)){
        int ok = 0;
        for(uint32_t s = 9; s <= 12 && !ok; s++) if(boot_region_ok(dev, s, 12, &g) == 0) ok = 1;
        if(!ok){ ex_log("boot region invalid (main + backup checksum/geometry) - refused"); return -1; }
        backup = 1;
        ex_log("main boot region invalid; using backup (read-only)");
    }

    /* Commit geometry (nothing of a previous mount is attached any more). */
    if(V.bmp_clus){ kfree(V.bmp_clus); V.bmp_clus = NULL; }
    mzero(&V, sizeof(V));
    V.dev = dev;
    V.ss_shift = g.ss_shift; V.ss = 1u << g.ss_shift;
    V.spf = V.ss / dev->sector_size;
    V.spc_shift = g.spc_shift;
    V.cb_shift = g.ss_shift + g.spc_shift; V.cb = 1u << V.cb_shift;
    V.vol_len = g.vol_len;
    V.fat_off = g.fat_off; V.fat_len = g.fat_len; V.heap_off = g.heap_off;
    V.ccount = g.ccount; V.root_clus = g.root;
    V.flags = g.flags;
    V.active_fat = (g.nfats == 2 && (g.flags & VF_ACTIVEFAT)) ? 1 : 0;
    V.dirty_at_mount = (g.flags & VF_DIRTY) ? 1 : 0;
    V.ro = backup || !dev->write || g.nfats != 1 || (g.flags & VF_MEDIAFAIL);
    V.hint = 2;
    cache_reset();
    g_corrupt_logged = 0;
    for(uint32_t i = 0; i < EX_MAX_SS; i++) g_zero[i] = 0;

    /* Root directory: FAT chain, bounded walk (a cycle is refused). */
    uint64_t maxn = EX_MAX_DIR >> V.cb_shift;
    if(maxn == 0) maxn = 1;
    if(maxn > V.ccount) maxn = V.ccount;
    uint32_t c = V.root_clus, n = 1;
    for(;;){
        uint32_t nx;
        int r = fat_next(c, &nx);
        if(r == 0) break;
        if(r < 0 || n >= maxn){ ex_log("root directory chain corrupt or cyclic - refused"); return -1; }
        c = nx; n++;
    }
    V.root.first = V.root_clus; V.root.nofat = 0;
    V.root.size = V.root.vdl = (uint64_t)n << V.cb_shift;

    /* Critical primary entries in the root: allocation bitmap + up-case. */
    struct root_scan rs; mzero(&rs, sizeof(rs));
    uint8_t e[32];
    uint32_t nslots = (uint32_t)(V.root.size / 32);
    for(uint32_t s = 0; s < nslots; s++){
        if(dent_rd(&V.root, s, e) != 0) return -1;
        if(e[0] == ET_EOD) break;
        if(e[0] == ET_BITMAP && (e[1] & 1) == V.active_fat && !rs.bmp_found){
            rs.bmp_first = rd32(e + 20); rs.bmp_len = rd64(e + 24); rs.bmp_found = 1;
        } else if(e[0] == ET_UPCASE && !rs.up_found){
            rs.up_csum = rd32(e + 4); rs.up_first = rd32(e + 20); rs.up_len = rd64(e + 24); rs.up_found = 1;
        }
    }
    if(!rs.bmp_found || !rs.up_found){ ex_log("missing bitmap / up-case entry - refused"); return -1; }
    if(load_upcase(&rs) != 0){ ex_log("up-case table invalid - refused"); return -1; }

    uint64_t need_bmp = ((uint64_t)V.ccount + 7) / 8;
    if(!cl_ok(rs.bmp_first) || rs.bmp_len < need_bmp || rs.bmp_len > ((uint64_t)V.ccount << V.cb_shift)){
        ex_log("allocation bitmap entry invalid - refused"); return -1;
    }
    V.bmp_len = need_bmp;
    V.bmp_nclus = (uint32_t)((need_bmp + V.cb - 1) >> V.cb_shift);
    V.bmp_clus = (uint32_t*)kmalloc((size_t)V.bmp_nclus * 4);
    if(!V.bmp_clus) return -1;
    if(meta_clusters(rs.bmp_first, need_bmp, V.bmp_clus, V.bmp_nclus) != 0){
        ex_log("allocation bitmap chain corrupt - refused");
        kfree(V.bmp_clus); V.bmp_clus = NULL; return -1;
    }

    /* Free-cluster count from the bitmap. */
    uint64_t freec = 0;
    for(uint32_t ci = 0; ci < V.bmp_nclus; ci++){
        for(uint32_t sct = 0; sct < (1u << V.spc_shift); sct++){
            uint64_t byte0 = ((uint64_t)ci << V.cb_shift) + ((uint64_t)sct << V.ss_shift);
            if(byte0 >= need_bmp) break;
            uint8_t* p = sec_get(cl_sec(V.bmp_clus[ci]) + sct);
            if(!p){ kfree(V.bmp_clus); V.bmp_clus = NULL; return -1; }
            for(uint32_t i = 0; i < V.ss && byte0 + i < need_bmp; i++){
                uint8_t b = p[i];
                for(int k = 0; k < 8; k++){
                    uint64_t cl = (byte0 + i) * 8 + (uint64_t)k;
                    if(cl >= V.ccount) break;
                    if(!((b >> k) & 1)) freec++;
                }
            }
        }
    }
    V.free = freec;

    size_t k = 0;
    for(; mount_point[k] && k < sizeof(g_mp) - 1; k++) g_mp[k] = mount_point[k];
    g_mp[k] = 0;
    for(k = 0; dev_name[k] && k < sizeof(g_devname) - 1; k++) g_devname[k] = dev_name[k];
    g_devname[k] = 0;
    g_ino_used = 0; g_ino_next = 0;
    V.mounted = 1;
    if(vfs_mount(g_mp, &exfat_ops, "exfat") != 0){ V.mounted = 0; return -1; }
    debugcon_writestring("[EXFAT] mounted "); debugcon_writestring(g_devname);
    debugcon_writestring(" at "); debugcon_writestring(g_mp);
    debugcon_writestring(V.ro ? " (ro)" : " (rw)");
    debugcon_writestring(" clusters="); debugcon_print_hex(V.ccount);
    debugcon_writestring(" csize="); debugcon_print_hex(V.cb);
    debugcon_writestring(" free="); debugcon_print_hex(V.free);
    if(V.dirty_at_mount) debugcon_writestring(" [was dirty]");
    debugcon_writestring("\n");
    return 0;
}
