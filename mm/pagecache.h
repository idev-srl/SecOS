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
 */
#ifndef PAGECACHE_H
#define PAGECACHE_H
#include <stdint.h>

struct vfs_inode;

// Physical frame of the cached page holding file bytes [off&~0xFFF, +4096),
// populated from the VFS on miss. Returns 0 on failure.
uint64_t pagecache_get_phys(struct vfs_inode* inode, uint64_t off);

// Read up to 'len' bytes from 'inode' at 'offset' into 'kbuf' through the cache.
// Returns the number of bytes read (clamped to file size), or <0 on error.
int pagecache_read(struct vfs_inode* inode, uint64_t offset, void* kbuf, uint64_t len);

// Drop cached pages of 'inode' overlapping [offset, offset+len) (after a write),
// so a subsequent read re-populates from the (now updated) file.
void pagecache_invalidate(struct vfs_inode* inode, uint64_t offset, uint64_t len);

#endif // PAGECACHE_H
