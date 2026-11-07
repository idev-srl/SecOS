/*
 * pagecache.h — [M20] Unified file page cache.
 * Copyright (c) 2025 iDev srl
 * SPDX-License-Identifier: MIT
 *
 * A small cache of file pages keyed by (inode, page-aligned offset). Both file
 * read() and file-backed mmap source their bytes here, so the two are coherent.
 * Pages are populated from the VFS on miss and zero-padded past EOF. Cache frames
 * are kernel-only (file-backed mmap is MAP_PRIVATE: a fault copies the cache page
 * into a private frame), so eviction simply frees the frame.
 *
 * [M50 B2] The cache lock (LK_S40_PAGECACHE) covers the index only: misses are
 * filled with no lock held, so every entry point may be called with FS / block
 * klocks held (they are outer) and the filesystem may be entered from a fill.
 * Callers get bytes copied out, never a cache frame: a frame can be evicted by
 * another CPU the moment the lock drops. Every write / truncate / rename /
 * unlink must call pagecache_invalidate AFTER the change: that also cancels a
 * fill that read the old bytes concurrently (it is not published).
 */
#ifndef PAGECACHE_H
#define PAGECACHE_H
#include <stdint.h>

struct vfs_inode;
struct vfs_fs_ops;

// Copy the 4096-byte page holding file bytes [off&~0xFFF, +4096) (zero-padded
// past EOF) into 'dst' (kernel memory), populating the cache on a miss.
// Returns 0, or -1 when the filesystem read failed / no memory.
int pagecache_copy_page(struct vfs_inode* inode, uint64_t off, void* dst);

// Read up to 'len' bytes from 'inode' at 'offset' into 'kbuf' through the cache.
// Returns the number of bytes read (clamped to file size), or <0 on error.
int pagecache_read(struct vfs_inode* inode, uint64_t offset, void* kbuf, uint64_t len);

// Drop cached pages of 'inode' overlapping [offset, offset+len) (after a write),
// so a subsequent read re-populates from the (now updated) file. A fill of an
// overlapping page that is in flight on another CPU is not published.
void pagecache_invalidate(struct vfs_inode* inode, uint64_t offset, uint64_t len);

// Drop every cached page of the filesystem behind 'ops' (on mount/unmount:
// inode slots of the previous mount are reused for other files).
void pagecache_invalidate_fs(const struct vfs_fs_ops* ops);

// Counters (since boot) and the current slot census.
typedef struct pagecache_stats {
    uint64_t hits, misses, published, discarded;   // discarded = fill invalidated
    uint64_t waits, wait_timeouts, direct;         // direct = read uncached
    uint64_t evictions, invalidations, doomed;     // doomed = fills marked stale
    uint32_t valid, filling;
} pagecache_stats_t;
void pagecache_get_stats(pagecache_stats_t* out);

#ifdef SECOS_PAGECACHE_TEST
// Test hooks (mm/pagecache_test.c): called between the filesystem read of a
// fill and its publication; the negative control ignores the stale bit.
extern void (*pagecache_test_fill_hook)(struct vfs_inode* inode, uint64_t off);
extern int pagecache_test_nostale;
#endif

#endif // PAGECACHE_H
