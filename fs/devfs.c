/*
 * devfs.c — [M23] /dev virtual filesystem.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Exposes devices as files under /dev so source-built (and signed) Linux-style
 * programs can open() them. Trust is rooted in the mandatory signature: only
 * signed code runs, so a signed program gets ambient access to /dev (the Driver
 * Space capability model is orthogonal — a /dev/<blk> read still goes through the
 * kernel block driver). Char devices (null/zero/full/random/urandom/console/tty)
 * are synthesized; block devices (sda/nvme0n1/usb0/vda...) are byte-addressed
 * over the registered block_dev_t. A pure namespace: no create/mkdir/remove.
 */
#include "vfs.h"
#include "block.h"
#include "terminal.h"
#include "keyboard.h"
#include <stddef.h>
#include <stdint.h>

extern uint64_t timer_get_ticks(void);

enum dev_kind { DEV_NULL, DEV_ZERO, DEV_FULL, DEV_RANDOM, DEV_CONSOLE, DEV_BLOCK };

struct dev_node {
    char name[16];
    enum dev_kind kind;
    block_dev_t* blk;     // for DEV_BLOCK
};

#define DEVFS_MAX 16
static struct dev_node g_nodes[DEVFS_MAX];
static int g_nnodes;
static vfs_inode_t g_inodes[DEVFS_MAX + 1];   // +1 for the root dir
static int g_built;

static void sset(char* d, const char* s) { int i=0; while(s[i] && i<15){ d[i]=s[i]; i++; } d[i]=0; }
static int seq(const char* a, const char* b){ int i=0; while(a[i]&&b[i]){ if(a[i]!=b[i]) return 0; i++; } return a[i]==b[i]; }

static void add(const char* name, enum dev_kind k, block_dev_t* b) {
    if (g_nnodes >= DEVFS_MAX) return;
    sset(g_nodes[g_nnodes].name, name);
    g_nodes[g_nnodes].kind = k;
    g_nodes[g_nnodes].blk = b;
    g_nnodes++;
}

// Build the device table from the synthetic char devices + every registered
// block device. Called lazily on first VFS access (so all drivers are up).
static void devfs_build(void) {
    if (g_built) return;
    g_built = 1;
    g_nnodes = 0;
    add("null", DEV_NULL, NULL);
    add("zero", DEV_ZERO, NULL);
    add("full", DEV_FULL, NULL);
    add("random", DEV_RANDOM, NULL);
    add("urandom", DEV_RANDOM, NULL);
    add("console", DEV_CONSOLE, NULL);
    add("tty", DEV_CONSOLE, NULL);
    int n = block_count();
    for (int i = 0; i < n; i++) {
        block_dev_t* b = block_get(i);
        if (b && b->name) add(b->name, DEV_BLOCK, b);
    }
}

static uint64_t dev_size(const struct dev_node* d) {
    if (d->kind == DEV_BLOCK && d->blk) return d->blk->sector_count * (uint64_t)d->blk->sector_size;
    return 0;   // char devices report size 0
}

// Fill an inode for node index i (or the root when i<0).
static vfs_inode_t* fill_inode(int i) {
    vfs_inode_t* ino = &g_inodes[i < 0 ? DEVFS_MAX : i];
    if (i < 0) {
        ino->path[0]='/'; ino->path[1]=0; ino->type=VFS_NODE_DIR; ino->size=0; ino->fs_data=NULL;
        return ino;
    }
    ino->path[0]='/'; int k=1; const char* s=g_nodes[i].name;
    for (int j=0; s[j] && k<255; j++) ino->path[k++]=s[j]; ino->path[k]=0;
    ino->type = VFS_NODE_FILE;
    ino->size = dev_size(&g_nodes[i]);
    ino->fs_data = &g_nodes[i];
    return ino;
}

static int find_node(const char* rel) {
    // rel is "/name"; skip the leading slash.
    if (!rel || rel[0] != '/') return -2;
    if (rel[1] == 0) return -1;             // root dir
    const char* name = rel + 1;
    for (int i=0;i<g_nnodes;i++) if (seq(g_nodes[i].name, name)) return i;
    return -2;                              // not found
}

static vfs_inode_t* devfs_lookup(const char* rel) {
    devfs_build();
    int i = find_node(rel);
    if (i == -2) return NULL;
    return fill_inode(i);
}

static int devfs_readdir(const char* dir, vfs_iter_cb cb, void* user) {
    devfs_build();
    if (!cb) return -1;
    if (!(dir && dir[0]=='/' && dir[1]==0)) return -1;   // only the root dir lists
    for (int i=0;i<g_nnodes;i++) cb(fill_inode(i), user);
    return 0;
}

// xorshift64* PRNG for /dev/random — NOT cryptographically secure (a real CSPRNG
// would seed from the in-kernel crypto). Seeded from the tick counter.
static uint64_t g_rng;
static uint64_t rng_next(void) {
    if (!g_rng) g_rng = timer_get_ticks() | 1;
    uint64_t x = g_rng; x ^= x >> 12; x ^= x << 25; x ^= x >> 27; g_rng = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static int blk_byte_io(block_dev_t* b, size_t off, void* buf, size_t len, int write);

static int devfs_read(vfs_inode_t* inode, size_t off, void* buf, size_t len) {
    if (!inode || !inode->fs_data) return -1;
    struct dev_node* d = (struct dev_node*)inode->fs_data;
    uint8_t* out = (uint8_t*)buf;
    switch (d->kind) {
        case DEV_NULL: return 0;                                   // EOF
        case DEV_ZERO: for (size_t i=0;i<len;i++) out[i]=0; return (int)len;
        case DEV_FULL: for (size_t i=0;i<len;i++) out[i]=0; return (int)len;
        case DEV_RANDOM: {
            for (size_t i=0;i<len;i++) out[i] = (uint8_t)rng_next();
            return (int)len;
        }
        case DEV_CONSOLE: {                                        // read keyboard
            size_t i=0; while (i<len && keyboard_has_char()) out[i++]=(uint8_t)keyboard_getchar();
            return (int)i;
        }
        case DEV_BLOCK: return blk_byte_io(d->blk, off, buf, len, 0);
    }
    return -1;
}

static int devfs_write(vfs_inode_t* inode, size_t off, const void* buf, size_t len) {
    if (!inode || !inode->fs_data) return -1;
    struct dev_node* d = (struct dev_node*)inode->fs_data;
    const uint8_t* in = (const uint8_t*)buf;
    switch (d->kind) {
        case DEV_NULL: case DEV_ZERO: return (int)len;            // sink
        case DEV_FULL: return -1;                                 // ENOSPC
        case DEV_RANDOM: return (int)len;                         // reseed ignored
        case DEV_CONSOLE: for (size_t i=0;i<len;i++) terminal_putchar((char)in[i]); return (int)len;
        case DEV_BLOCK: return blk_byte_io(d->blk, off, (void*)(uintptr_t)buf, len, 1);
    }
    return -1;
}

// Byte-addressed I/O over a sector device, via a one-sector bounce buffer for the
// unaligned head/tail and whole-sector middle.
static int blk_byte_io(block_dev_t* b, size_t off, void* buf, size_t len, int write) {
    if (!b) return -1;
    uint32_t ss = b->sector_size;
    uint64_t total = b->sector_count * (uint64_t)ss;
    if (off >= total) return 0;
    if (off + len > total) len = (size_t)(total - off);
    static uint8_t sec[4096];
    if (ss > sizeof(sec)) return -1;
    uint8_t* p = (uint8_t*)buf;
    size_t done = 0;
    while (done < len) {
        uint64_t abs = off + done;
        uint64_t lba = abs / ss;
        uint32_t soff = (uint32_t)(abs % ss);
        uint32_t chunk = ss - soff; if (chunk > len - done) chunk = (uint32_t)(len - done);
        if (soff == 0 && chunk == ss) {
            // Whole-sector fast path.
            if (write) { if (!b->write || b->write(b, lba, p+done, 1) != 1) return -1; }
            else       { if (b->read(b, lba, p+done, 1) != 1) return -1; }
        } else {
            if (b->read(b, lba, sec, 1) != 1) return -1;
            if (write) {
                for (uint32_t i=0;i<chunk;i++) sec[soff+i] = p[done+i];
                if (!b->write || b->write(b, lba, sec, 1) != 1) return -1;
            } else {
                for (uint32_t i=0;i<chunk;i++) p[done+i] = sec[soff+i];
            }
        }
        done += chunk;
    }
    return (int)done;
}

static int devfs_ro_create(const char* p, const void* d, size_t s){ (void)p;(void)d;(void)s; return -1; }
static int devfs_ro_mkdir(const char* p){ (void)p; return -1; }
static int devfs_ro_remove(const char* p){ (void)p; return -1; }
static int devfs_ro_rename(const char* a, const char* b){ (void)a;(void)b; return -1; }
static int devfs_ro_trunc(const char* p, size_t s){ (void)p;(void)s; return -1; }

static vfs_fs_ops_t devfs_ops = {
    .flags = VFS_FS_NOCACHE,
    .lookup = devfs_lookup,
    .readdir = devfs_readdir,
    .read = devfs_read,
    .write = devfs_write,
    .create = devfs_ro_create,
    .mkdir = devfs_ro_mkdir,
    .remove = devfs_ro_remove,
    .rename = devfs_ro_rename,
    .truncate = devfs_ro_trunc,
};

int devfs_mount(const char* mount_point) {
    extern int vfs_mount(const char*, const vfs_fs_ops_t*, const char*);
    return vfs_mount(mount_point, &devfs_ops, "devfs");
}
