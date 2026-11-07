/*
 * SecOS Kernel - EXT2 Interface (Stub)
 * Minimal superblock structure parsing for early diagnostics and experimentation.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
#include "block.h"
#include "vfs.h"

// Minimal ext2 superblock (partial fields used)
typedef struct ext2_superblock {
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t reserved_blocks_count;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size; // 0 => 1024, else 1024<<log_block_size
    uint32_t log_frag_size;
    uint32_t blocks_per_group;
    uint32_t frags_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mount_count;
    uint16_t max_mount_count;
    uint16_t magic; // 0xEF53
} ext2_superblock_t;

// Mount ext2/ext4 (no-journal) read-write from block device name at the given
// VFS mount point; returns 0 on success.
int ext2_mount(const char* dev_name, const char* mount_point);
// [M46] mount(2) data for the next ext2_mount: ro / rw / defaults, noload /
// norecovery (ro only). 0, or -1 for a malformed list.
int ext2_set_mount_options(const char* opts);

// [M51 C3] The driver's one lock (LK_K30_FS, docs/devlog/M51-ext4.md). Every
// ext2 op takes it itself; the VFS may take it only to drain the driver (lock
// + unlock, e.g. on unmount) and must never call an ext2 op while holding it.
klock_t* ext2_fs_lock(void);
// [M51 C3] Atomic O_APPEND: write at the end of file as it is under the lock
// (same as ops->write(v, VFS_OFF_APPEND, ...)). *off (optional) gets the
// offset the data landed at. Returns bytes written or a negative value.
int ext2_write_append(vfs_inode_t* v, const void* data, size_t len, uint64_t* off);
// The ops table, for in-kernel tests that drive the driver directly.
const vfs_fs_ops_t* ext2_vfs_ops(void);
