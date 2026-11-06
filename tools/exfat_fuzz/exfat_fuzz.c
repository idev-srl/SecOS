/*
 * tools/exfat_fuzz/exfat_fuzz.c - host fuzz harness for fs/exfat.c
 *
 * Builds the kernel exFAT driver unchanged against stub block/VFS layers and
 * throws randomly corrupted images at it: mount, walk every directory, read
 * every file, then create/write/mkdir/rename/truncate/unlink/sync and mount
 * again. Built with ASan + UBSan, so any out-of-bounds access, overflow or
 * hang (SIGALRM) on a malformed image is a failure with a reproducible seed.
 *
 *   make -C tools/exfat_fuzz            # build
 *   tools/exfat_fuzz/exfat_fuzz BASE.img ITERATIONS [SEED]
 *   tools/exfat_fuzz/exfat_fuzz BASE.img 0 OUT.img   (clean workload, no mutation)
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
#include "exfat.h"

int g_fuzz_verbose = 0;

/* ---- stubs for the kernel services exfat.c uses ---- */
static uint8_t* g_img;
static size_t   g_img_len;
static int r_dev(block_dev_t* d, uint64_t lba, void* buf, uint32_t n){
    (void)d;
    if(lba * 512 + (uint64_t)n * 512 > g_img_len) return -1;
    memcpy(buf, g_img + lba * 512, (size_t)n * 512);
    return (int)n;
}
static int w_dev(block_dev_t* d, uint64_t lba, const void* buf, uint32_t n){
    (void)d;
    if(lba * 512 + (uint64_t)n * 512 > g_img_len) return -1;
    memcpy(g_img + lba * 512, buf, (size_t)n * 512);
    return (int)n;
}
static block_dev_t g_dev = { .name = "fz", .sector_size = 512, .read = r_dev, .write = w_dev };
block_dev_t* block_find(const char* n){ return (n && !strcmp(n, "fz")) ? &g_dev : NULL; }
int block_read(block_dev_t* d, uint64_t lba, void* b, uint32_t c){ return d->read(d, lba, b, c); }
int block_write(block_dev_t* d, uint64_t lba, const void* b, uint32_t c){ return d->write ? d->write(d, lba, b, c) : -1; }

static const vfs_fs_ops_t* g_ops;
int vfs_mount(const char* mp, const vfs_fs_ops_t* ops, const char* name){ (void)mp; (void)name; g_ops = ops; return 0; }
const char* vfs_mount_point_of(const vfs_fs_ops_t* ops){ return (g_ops && ops == g_ops) ? "/mnt" : NULL; }

void pagecache_invalidate(struct vfs_inode* i, uint64_t off, uint64_t len){ (void)i; (void)off; (void)len; }
uint64_t kclock_real_sec(void){ return 1762419600ull; }
void kclock_unix_to_civil(uint64_t t, int* y, int* mo, int* d, int* h, int* mi, int* s){
    (void)t; *y = 2025; *mo = 11; *d = 6; *h = 9; *mi = 0; *s = 0;
}
uint64_t kclock_civil_to_unix(int y, int mo, int d, int h, int mi, int s){
    return (uint64_t)(y - 1970) * 31556952ull + (uint64_t)mo * 2629746ull + (uint64_t)d * 86400ull +
           (uint64_t)h * 3600ull + (uint64_t)mi * 60ull + (uint64_t)s;
}

/* ---- tree walk ---- */
#define MAXP 512
static char g_paths[MAXP][256];
static int  g_isdir[MAXP];
static int  g_np;
static void collect(const vfs_inode_t* c, void* u){
    (void)u;
    if(g_np >= MAXP) return;
    snprintf(g_paths[g_np], 256, "%s", c->path);
    g_isdir[g_np] = c->type == VFS_NODE_DIR;
    g_np++;
}
static uint8_t g_rbuf[65536];
static void exercise(void){
    const vfs_fs_ops_t* o = g_ops;
    g_np = 0;
    o->readdir("/", collect, NULL);
    for(int i = 0; i < g_np && i < MAXP; i++){
        vfs_inode_t* ino = o->lookup(g_paths[i]);
        if(!ino) continue;
        if(g_isdir[i]){ if(g_np < MAXP) o->readdir(g_paths[i], collect, NULL); continue; }
        for(size_t off = 0; off < ino->size && off < 4 * sizeof(g_rbuf); off += sizeof(g_rbuf))
            if(o->read(ino, off, g_rbuf, sizeof(g_rbuf)) < 0) break;
    }
    static uint8_t w[20000];
    for(size_t i = 0; i < sizeof(w); i++) w[i] = (uint8_t)i;
    o->create("/fz_new.txt", w, sizeof(w));
    o->mkdir("/fz_dir");
    o->create("/fz_dir/a", "a", 1);
    vfs_inode_t* ino = o->lookup("/fz_new.txt");
    if(ino) o->write(ino, 50000, w, sizeof(w));
    o->rename("/fz_new.txt", "/fz_dir/b");
    o->truncate("/fz_dir/b", 3);
    o->truncate("/fz_dir/b", 9000);
    for(int i = 0; i < g_np && i < 8; i++){
        if(!g_isdir[i]){
            vfs_inode_t* f = o->lookup(g_paths[i]);
            if(f){ o->write(f, 7, "zz", 2); o->truncate(g_paths[i], 1); }
            o->remove(g_paths[i]);
        }
    }
    o->remove("/fz_dir/a");
    o->remove("/fz_dir");
    o->sync();
}

static uint64_t g_rng;
static uint64_t rnd(void){ g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }
static uint64_t g_seed;
static void on_alarm(int s){ (void)s; fprintf(stderr, "HANG on seed %llu\n", (unsigned long long)g_seed); _exit(3); }

static uint32_t rd32(const uint8_t* p){ return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32(uint8_t* p, uint32_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void fix_boot_csum(uint8_t* img, uint32_t ss){
    uint32_t cs = 0;
    for(uint32_t i = 0; i < 11 * ss; i++){
        if(i == 106 || i == 107 || i == 112) continue;
        cs = ((cs & 1) ? 0x80000000u : 0) + (cs >> 1) + img[i];
    }
    for(uint32_t i = 0; i < ss; i += 4) wr32(img + 11 * ss + i, cs);
}

int main(int argc, char** argv){
    if(argc < 3){ fprintf(stderr, "usage: %s BASE.img ITERATIONS [SEED]\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if(!f){ perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); size_t len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* base = malloc(len);
    if(fread(base, 1, len, f) != len){ perror("read"); return 2; }
    fclose(f);
    g_img = malloc(len); g_img_len = len;
    g_dev.sector_count = len / 512;
    long iters = atol(argv[2]);
    if(iters == 0 && argc > 3){
        /* Clean mode: run the workload on the unmodified image (twice, with a
         * remount in between) and write the result out for fsck/deepcheck. */
        memcpy(g_img, base, len);
        for(int pass = 0; pass < 2; pass++){
            g_ops = NULL;
            if(exfat_mount("fz", "/mnt") != 0){ fprintf(stderr, "mount failed\n"); return 1; }
            exercise();
            g_ops = NULL;
        }
        FILE* o = fopen(argv[3], "wb");
        if(!o || fwrite(g_img, 1, len, o) != len){ perror(argv[3]); return 1; }
        fclose(o);
        free(base); free(g_img);
        printf("exfat_fuzz: clean workload written to %s\n", argv[3]);
        return 0;
    }
    uint64_t seed0 = argc > 3 ? strtoull(argv[3], 0, 0) : 1;
    if(getenv("FUZZ_VERBOSE")) g_fuzz_verbose = 1;
    signal(SIGALRM, on_alarm);

    uint32_t ss = 1u << base[108], cb = ss << base[109];
    uint32_t fat_off = rd32(base + 80), heap = rd32(base + 88), root = rd32(base + 96);
    uint64_t fat_b = (uint64_t)fat_off * ss, heap_b = (uint64_t)heap * ss;
    uint64_t root_b = heap_b + (uint64_t)(root - 2) * cb;
    long mounted = 0;
    for(long it = 0; it < iters; it++){
        g_seed = seed0 + (uint64_t)it;
        g_rng = g_seed * 0x9E3779B97F4A7C15ull + 1;
        memcpy(g_img, base, len);
        int nflip = 1 + (int)(rnd() % 24);
        int region = (int)(rnd() % 6);
        for(int k = 0; k < nflip; k++){
            uint64_t off;
            switch(region){
            case 0: off = 64 + rnd() % 64; break;                         /* boot fields */
            case 1: off = fat_b + rnd() % (4 * ss); break;                /* FAT head */
            case 2: off = root_b + rnd() % cb; break;                     /* root dir */
            case 3: off = heap_b + rnd() % (32ull * cb); break;           /* bitmap/upcase/dirs */
            case 4: off = root_b + 32 * (rnd() % (cb / 32)) + (rnd() % 4 == 0 ? 0 : 1 + rnd() % 31); break;
            default: off = rnd() % (heap_b + 64ull * cb); break;
            }
            if(off >= len) continue;
            uint8_t v = (uint8_t)rnd();
            if(rnd() % 3 == 0) v = (rnd() & 1) ? 0xFF : 0x00;
            g_img[off] = v;
        }
        if(region == 0 && (rnd() % 4)) fix_boot_csum(g_img, ss);     /* get past the checksum */
        if(region == 0) memcpy(g_img + 12 * ss, g_img, 12 * ss);      /* same in the backup */
        alarm(20);
        for(int pass = 0; pass < 2; pass++){
            g_ops = NULL;
            if(exfat_mount("fz", "/mnt") != 0) break;
            mounted++;
            exercise();
            g_ops = NULL;                                             /* "unmount" */
        }
        alarm(0);
        if((it + 1) % 1000 == 0) fprintf(stderr, "  %ld iterations, %ld mounts\n", it + 1, mounted);
    }
    free(base); free(g_img);
    printf("exfat_fuzz: %ld images, %ld successful mounts, no crash/hang/UB\n", iters, mounted);
    return 0;
}
