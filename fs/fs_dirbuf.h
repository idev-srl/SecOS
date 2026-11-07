/*
 * fs_dirbuf.h — collect a directory listing under a filesystem lock, replay
 * it to the caller's callback after the lock is dropped (M51 C4).
 *
 * A vfs_fs_ops->readdir callback is arbitrary caller code: the kernel shell's
 * `ls` pages its output and waits for a key (cpu_wait_irq), test code calls
 * vfs_lookup() on every child (re-entering the same filesystem), getdents
 * fills a kernel buffer. None of that may run while the driver's K30 klock is
 * held (no klock across cpu_wait_irq; a re-entry would take the lock twice).
 * So a driver's readdir wrapper runs its _locked walk with fs_dirbuf_cb as the
 * callback, unlocks, then calls fs_dirbuf_replay(): "collect first, then act"
 * (docs/BKL_REMOVAL_PLAN.md §8). The records keep every field a driver sets
 * on a transient child (path, type, size, mode, owner, times, nlink, ino);
 * fs_data and ops are always NULL for readdir children.
 *
 * Header-only (static inline) so the host fuzz harnesses, which compile the
 * drivers unchanged, need nothing beyond their kmalloc/kfree stubs.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FS_DIRBUF_H
#define FS_DIRBUF_H
#include <stdint.h>
#include <stddef.h>
#include "vfs.h"
#include "heap.h"

typedef struct fs_dirbuf {
    uint8_t* buf;
    size_t   len, cap;
    int      err;          /* out of memory: the listing is incomplete */
} fs_dirbuf_t;

typedef struct fs_dirrec {
    uint32_t reclen;       /* header + path + NUL, rounded up to 8 */
    uint32_t type, mode, uid, gid, nlink;
    uint64_t size, atime, mtime, ctime, ino;
    /* char path[] follows */
} fs_dirrec_t;

static inline void fs_dirbuf_init(fs_dirbuf_t* d) { d->buf = 0; d->len = 0; d->cap = 0; d->err = 0; }

static inline void fs_dirbuf_free(fs_dirbuf_t* d) {
    if (d->buf) kfree(d->buf);
    d->buf = 0; d->len = d->cap = 0;
}

/* vfs_iter_cb: append one child (called with the FS lock held). */
static inline void fs_dirbuf_cb(const vfs_inode_t* c, void* u) {
    fs_dirbuf_t* d = (fs_dirbuf_t*)u;
    if (d->err) return;
    size_t pl = 0;
    while (pl < sizeof(c->path) - 1 && c->path[pl]) pl++;
    size_t need = (sizeof(fs_dirrec_t) + pl + 1 + 7) & ~(size_t)7;
    if (d->len + need > d->cap) {
        size_t nc = d->cap ? d->cap * 2 : 4096;
        while (nc < d->len + need) nc *= 2;
        uint8_t* nb = (uint8_t*)kmalloc(nc);
        if (!nb) { d->err = 1; return; }
        for (size_t i = 0; i < d->len; i++) nb[i] = d->buf[i];
        if (d->buf) kfree(d->buf);
        d->buf = nb; d->cap = nc;
    }
    fs_dirrec_t* r = (fs_dirrec_t*)(d->buf + d->len);
    r->reclen = (uint32_t)need;
    r->type = (uint32_t)c->type; r->mode = c->mode; r->uid = c->uid; r->gid = c->gid;
    r->nlink = c->nlink;
    r->size = (uint64_t)c->size; r->atime = c->atime; r->mtime = c->mtime; r->ctime = c->ctime;
    r->ino = c->ino;
    char* p = (char*)(r + 1);
    for (size_t i = 0; i < pl; i++) p[i] = c->path[i];
    p[pl] = 0;
    d->len += need;
}

/* Hand every collected child to `cb` (no lock held). 0, or -1 if the listing
 * was cut short by an allocation failure (nothing is replayed then). */
static inline int fs_dirbuf_replay(const fs_dirbuf_t* d, vfs_iter_cb cb, void* user) {
    if (d->err) return -1;
    vfs_inode_t* c = (vfs_inode_t*)kmalloc(sizeof(vfs_inode_t));
    if (!c) return d->len ? -1 : 0;
    for (size_t off = 0; off < d->len;) {
        const fs_dirrec_t* r = (const fs_dirrec_t*)(d->buf + off);
        uint8_t* z = (uint8_t*)c;
        for (size_t i = 0; i < sizeof(*c); i++) z[i] = 0;
        const char* p = (const char*)(r + 1);
        size_t k = 0;
        for (; p[k] && k < sizeof(c->path) - 1; k++) c->path[k] = p[k];
        c->path[k] = 0;
        c->type = (vfs_node_type_t)r->type; c->mode = r->mode; c->uid = r->uid; c->gid = r->gid;
        c->nlink = r->nlink;
        c->size = (size_t)r->size; c->atime = r->atime; c->mtime = r->mtime; c->ctime = r->ctime;
        c->ino = r->ino;
        cb(c, user);
        off += r->reclen;
    }
    kfree(c);
    return 0;
}

/* Set a cached inode's path only when it really changes: procfs and the VFS
 * copy ino->path without the driver lock, so a lookup must not rewrite the
 * same string under them (a rename, a real change, still does). */
static inline void fs_set_path(char* dst, size_t cap, const char* src) {
    size_t k = 0;
    while (k < cap - 1 && src[k] && dst[k] == src[k]) k++;
    if ((k == cap - 1 || !src[k]) && dst[k] == 0) return;     /* unchanged */
    for (k = 0; src[k] && k < cap - 1; k++) dst[k] = src[k];
    dst[k] = 0;
}

/* The VFS-side contract for an atomic O_APPEND write (C1) is
 * ops->write(ino, VFS_OFF_APPEND, ...): the driver resolves the offset to the
 * file's current size under its own lock. vfs.h defines the value once C1 is
 * merged; until then the drivers use this one (never a valid file offset). */
#ifndef VFS_OFF_APPEND
#define VFS_OFF_APPEND ((size_t)-1)
#endif

#endif /* FS_DIRBUF_H */
