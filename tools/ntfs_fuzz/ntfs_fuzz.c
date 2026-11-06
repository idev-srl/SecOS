/*
 * tools/ntfs_fuzz/ntfs_fuzz.c - host harness for fs/ntfs.c
 *
 * Builds the kernel NTFS driver unchanged against stub block/VFS layers.
 *
 *   make -C tools/ntfs_fuzz
 *   tools/ntfs_fuzz/ntfs_fuzz IMG list              walk + hash every file
 *   tools/ntfs_fuzz/ntfs_fuzz IMG ITERATIONS [SEED] fuzz
 *
 * `list` mounts the image and prints one line per object:
 *   F <size> <fnv1a64> <path>     regular file (content hash)
 *   E <errno> <size> <path>       file whose read fails (e.g. 13 = EFS)
 *   D - - <path>                  directory
 *   L - <target> <path>           symlink
 * (fields separated by tabs)
 * tools/ntfs-host.sh compares that with the same walk over ntfs-3g.
 *
 * Fuzz mode throws corrupted images at it (boot sector, MFT records,
 * attribute headers and run lists, INDX blocks, random bytes): mount, walk
 * every directory, read every file, readlink, statfs, try every write op.
 * Built with ASan + UBSan; any memory error, UB or hang (SIGALRM) fails with a
 * reproducible seed.
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vfs.h"
#include "block.h"
#include "ntfs.h"

int g_fuzz_verbose = 0;

/* ---- stubs for the kernel services ntfs.c uses ---- */
static uint8_t* g_img;
static size_t   g_img_len;
static int r_dev(block_dev_t* d, uint64_t lba, void* buf, uint32_t n){
    (void)d;
    if(lba * 512 + (uint64_t)n * 512 > g_img_len) return -1;
    memcpy(buf, g_img + lba * 512, (size_t)n * 512);
    return (int)n;
}
static block_dev_t g_dev = { .name = "fz", .sector_size = 512, .read = r_dev, .write = NULL };
block_dev_t* block_find(const char* n){ return (n && !strcmp(n, "fz")) ? &g_dev : NULL; }
int block_read(block_dev_t* d, uint64_t lba, void* b, uint32_t c){ return d->read(d, lba, b, c); }

static const vfs_fs_ops_t* g_ops;
int vfs_mount(const char* mp, const vfs_fs_ops_t* ops, const char* name){ (void)mp; (void)name; g_ops = ops; return 0; }
const char* vfs_mount_point_of(const vfs_fs_ops_t* ops){ return (g_ops && ops == g_ops) ? "/mnt" : NULL; }

/* ---- tree walk ---- */
#define MAXP 200000
static char** g_paths;
static int*   g_type;
static int    g_np, g_maxp;
static void collect(const vfs_inode_t* c, void* u){
    (void)u;
    if(g_np >= g_maxp) return;
    g_paths[g_np] = strdup(c->path);
    g_type[g_np] = c->type;
    g_np++;
}
static void walk_free(void){ for(int i = 0; i < g_np; i++) free(g_paths[i]); g_np = 0; }

static uint64_t fnv(uint64_t h, const uint8_t* p, size_t n){
    for(size_t i = 0; i < n; i++){ h ^= p[i]; h *= 0x100000001B3ULL; }
    return h;
}
static uint8_t g_rbuf[1 << 20];

static int cmp_str(const void* a, const void* b){ return strcmp(*(char* const*)a, *(char* const*)b); }

static int do_list(void){
    const vfs_fs_ops_t* o = g_ops;
    g_maxp = MAXP; g_np = 0;
    if(o->readdir("/", collect, NULL) != 0){ printf("READDIR-FAIL /\n"); return 1; }
    int rc = 0;
    for(int i = 0; i < g_np; i++){
        if(g_type[i] == VFS_NODE_DIR){
            if(o->readdir(g_paths[i], collect, NULL) != 0){ printf("READDIR-FAIL %s\n", g_paths[i]); rc = 1; }
        }
    }
    qsort(g_paths, (size_t)g_np, sizeof(char*), cmp_str);
    for(int i = 0; i < g_np; i++){
        vfs_inode_t* ino = o->lookup(g_paths[i]);
        if(!ino){ printf("LOOKUP-FAIL %s\n", g_paths[i]); rc = 1; continue; }
        if(ino->type == VFS_NODE_DIR){ printf("D\t-\t-\t%s\n", g_paths[i]); continue; }
        if(ino->type == VFS_NODE_SYMLINK){
            char t[300];
            int n = o->readlink(g_paths[i], t, sizeof(t));
            printf("L\t-\t%s\t%s\n", n >= 0 ? t : "?", g_paths[i]);
            continue;
        }
        uint64_t h = 0xCBF29CE484222325ULL;
        int err = 0;
        for(size_t off = 0; off < ino->size;){
            size_t want = ino->size - off > sizeof(g_rbuf) ? sizeof(g_rbuf) : ino->size - off;
            int r = o->read(ino, off, g_rbuf, want);
            if(r < 0){ err = -r; break; }
            if(r == 0 || (size_t)r > want){ err = 999; break; }
            h = fnv(h, g_rbuf, (size_t)r);
            off += (size_t)r;
        }
        if(err) printf("E\t%d\t%zu\t%s\n", err, ino->size, g_paths[i]);
        else printf("F\t%zu\t%016llx\t%s\n", ino->size, (unsigned long long)h, g_paths[i]);
    }
    vfs_statfs_t st;
    if(o->statfs(&st) == 0)
        printf("# statfs type=%llx bsize=%llu blocks=%llu bfree=%llu ro=%llu\n", (unsigned long long)st.type,
               (unsigned long long)st.bsize, (unsigned long long)st.blocks, (unsigned long long)st.bfree,
               (unsigned long long)st.flags);
    /* write ops must all be refused with EROFS */
    vfs_inode_t* any = o->lookup("/");
    int ro = o->create("/x_new", "a", 1) == -30 && o->mkdir("/x_dir") == -30 && o->remove("/x") == -30 &&
             o->rename("/a", "/b") == -30 && o->truncate("/a", 0) == -30 && o->symlink("a", "/b") == -30 &&
             o->link("/a", "/b") == -30 && any && o->write(any, 0, "x", 1) == -30;
    printf("# writes-refused-erofs=%d\n", ro);
    walk_free();
    return rc;
}

static void exercise(void){
    const vfs_fs_ops_t* o = g_ops;
    g_maxp = 3000; g_np = 0;
    o->readdir("/", collect, NULL);
    for(int i = 0; i < g_np; i++){
        vfs_inode_t* ino = o->lookup(g_paths[i]);
        if(!ino) continue;
        if(ino->type == VFS_NODE_DIR){ o->readdir(g_paths[i], collect, NULL); continue; }
        if(ino->type == VFS_NODE_SYMLINK){ char t[300]; o->readlink(g_paths[i], t, sizeof(t)); continue; }
        size_t lim = ino->size < 300000 ? ino->size : 300000;
        for(size_t off = 0; off < lim; off += 65536){
            int r = o->read(ino, off, g_rbuf, 65536);
            if(r > 65536){ fprintf(stderr, "read overrun\n"); abort(); }
            if(r <= 0) break;
        }
        if(ino->size > 1) o->read(ino, ino->size / 2 + 1, g_rbuf, 4097);
        o->read(ino, ino->size, g_rbuf, 10);
    }
    o->lookup("/no/such/path");
    o->lookup("/../..");
    vfs_statfs_t st; o->statfs(&st);
    o->create("/fz", "a", 1); o->mkdir("/fzd");
    walk_free();
}

/* Pre-scan helpers (raw, unfixed records: good enough to find targets). */
static uint32_t rd32h(const uint8_t* p){ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16h(const uint8_t* p){ return (uint16_t)(p[0] | (p[1] << 8)); }
/* 1 if the record has an attribute list, is an extension record, holds an
 * index allocation / reparse point, or compressed/sparse data. Compressed
 * $DATA runs are added to the cluster list `cl`. */
static int interesting(const uint8_t* r, size_t rs, uint64_t cb, uint64_t* cl, size_t* ncl, size_t maxcl){
    int hit = rd32h(r + 0x20) != 0;                      /* base reference: extension */
    uint32_t pos = rd16h(r + 0x14), used = rd32h(r + 0x18);
    if(used > rs) used = (uint32_t)rs;
    for(int k = 0; k < 64 && pos + 16 <= used; k++){
        uint32_t t = rd32h(r + pos), len = rd32h(r + pos + 4);
        if(t == 0xFFFFFFFF || len < 0x18 || pos + len > used) break;
        uint16_t fl = rd16h(r + pos + 0x0C);
        if(t == 0x20 || t == 0xA0 || t == 0xC0) hit = 1;
        if(t == 0x80 && r[pos + 8] && (fl & 0x80FF)){
            hit = 1;
            uint32_t p = pos + rd16h(r + pos + 0x20);
            int64_t lcn = 0;
            while(p < pos + len && r[p] && *ncl < maxcl){
                uint32_t lb = r[p] & 15, ob = r[p] >> 4;
                if(!lb || lb > 8 || ob > 8 || p + 1 + lb + ob > pos + len) break;
                uint64_t ln = 0; for(uint32_t i = 0; i < lb; i++) ln |= (uint64_t)r[p + 1 + i] << (8 * i);
                if(ob){
                    uint64_t d = 0; for(uint32_t i = 0; i < ob; i++) d |= (uint64_t)r[p + 1 + lb + i] << (8 * i);
                    if(ob < 8 && (r[p + lb + ob] & 0x80)) d |= ~0ULL << (8 * ob);
                    lcn += (int64_t)d;
                    for(uint64_t c = 0; c < ln && *ncl < maxcl; c++) cl[(*ncl)++] = ((uint64_t)lcn + c) * cb;
                }
                p += 1 + lb + ob;
            }
        }
        pos += len;
    }
    return hit;
}

static uint64_t g_rng;
static uint64_t rnd(void){ g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }
static uint64_t g_seed;
static void on_alarm(int s){ (void)s; fprintf(stderr, "HANG on seed %llu\n", (unsigned long long)g_seed); _exit(3); }

int main(int argc, char** argv){
    if(argc < 3){ fprintf(stderr, "usage: %s IMG list | %s IMG ITERATIONS [SEED]\n", argv[0], argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if(!f){ perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); size_t len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* base = malloc(len);
    if(!base || fread(base, 1, len, f) != len){ perror("read"); return 2; }
    fclose(f);
    g_img = malloc(len); g_img_len = len;
    g_dev.sector_count = len / 512;
    g_paths = calloc(MAXP, sizeof(char*)); g_type = calloc(MAXP, sizeof(int));
    if(getenv("FUZZ_VERBOSE")) g_fuzz_verbose = 1;
    if(!strcmp(argv[2], "list")){
        memcpy(g_img, base, len);
        if(ntfs_mount("fz", "/mnt") != 0){ fprintf(stderr, "mount failed\n"); return 1; }
        int rc = do_list();
        g_ops = NULL;
        return rc;
    }
    long iters = atol(argv[2]);
    uint64_t seed0 = argc > 3 ? strtoull(argv[3], 0, 0) : 1;
    signal(SIGALRM, on_alarm);

    /* Locate FILE records and INDX blocks of the base image (512-aligned). */
    size_t nfile = 0, nindx = 0;
    size_t* files = malloc(sizeof(size_t) * (len / 512 + 1));
    size_t* indx = malloc(sizeof(size_t) * (len / 512 + 1));
    for(size_t o = 0; o + 1024 <= len; o += 512){
        if(!memcmp(base + o, "FILE", 4)) files[nfile++] = o;
        else if(!memcmp(base + o, "INDX", 4)) indx[nindx++] = o;
    }
    if(!nfile){ fprintf(stderr, "no FILE records found\n"); return 2; }
    uint64_t cb = (uint64_t)rd16h(base + 0x0B) * (base[0x0D] <= 0x80 ? base[0x0D] : 1u);
    if(!cb) cb = 4096;
    size_t nint = 0, ncl = 0;
    size_t* hot = malloc(sizeof(size_t) * nfile);
    uint64_t* cl = malloc(sizeof(uint64_t) * 65536);
    for(size_t i = 0; i < nfile; i++)
        if(interesting(base + files[i], 1024, cb, cl, &ncl, 65536)) hot[nint++] = files[i];
    if(!nint) hot[nint++] = files[0];
    fprintf(stderr, "base: %zu FILE records (%zu interesting), %zu INDX blocks, %zu compressed clusters\n",
            nfile, nint, nindx, ncl);
    long mounted = 0;
    for(long it = 0; it < iters; it++){
        g_seed = seed0 + (uint64_t)it;
        g_rng = g_seed * 0x9E3779B97F4A7C15ull + 1;
        memcpy(g_img, base, len);
        int nflip = 1 + (int)(rnd() % 16);
        int region = (int)(rnd() % 11);
        size_t pick = files[rnd() % nfile];
        if(region >= 8 && region <= 9) pick = hot[rnd() % nint];
        if(region == 2 && nindx) pick = indx[rnd() % nindx];
        for(int k = 0; k < nflip; k++){
            uint64_t off;
            switch(region){
            case 0: off = rnd() % 0x60; break;                              /* boot sector fields */
            case 1: off = pick + rnd() % 1024; break;                       /* one MFT record */
            case 2: off = pick + rnd() % 4096; break;                       /* one INDX block */
            case 3: off = pick + 0x38 + rnd() % 0x180; break;               /* attribute headers / runs */
            case 4: off = files[rnd() % (nfile < 40 ? nfile : 40)] + rnd() % 1024; break; /* metafiles */
            case 5: off = files[rnd() % nfile] + 0x38 + rnd() % 0x100; break;            /* many records */
            case 6: off = (nindx ? indx[rnd() % nindx] : pick) + 0x18 + rnd() % 0x200; break; /* index headers */
            case 8: off = pick + rnd() % 1024; break;                       /* attr list / extension / index / reparse / compressed */
            case 9: off = pick + 0x38 + rnd() % 0x200; break;               /* ... their attribute headers + runs */
            case 10: off = ncl ? cl[rnd() % ncl] + rnd() % cb : rnd() % len; break;  /* LZNT1 data */
            default: off = rnd() % len; break;
            }
            if(off >= len) continue;
            uint8_t v = (uint8_t)rnd();
            int m = (int)(rnd() % 4);
            if(m == 0) v = (rnd() & 1) ? 0xFF : 0x00;
            else if(m == 1) v = (uint8_t)(g_img[off] ^ (1u << (rnd() % 8)));
            else if(m == 2) v = (uint8_t)(g_img[off] + 1);
            g_img[off] = v;
        }
        alarm(20);
        for(int pass = 0; pass < 2; pass++){
            g_ops = NULL;
            if(ntfs_mount("fz", "/mnt") != 0) break;
            mounted++;
            exercise();
            g_ops = NULL;
        }
        alarm(0);
        if((it + 1) % 1000 == 0) fprintf(stderr, "  %ld iterations, %ld mounts\n", it + 1, mounted);
    }
    free(base); free(g_img); free(files); free(indx); free(hot); free(cl);
    printf("ntfs_fuzz: %ld images, %ld successful mounts, no crash/hang/UB\n", iters, mounted);
    return 0;
}
