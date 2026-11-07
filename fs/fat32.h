/*
 * SecOS Kernel - FAT32 Interface (Stub)
 * Partial BPB parsing and mount entry point (diagnostic only).
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "block.h"
#include "vfs.h"

// FAT32 BPB essential fields
typedef struct fat32_bpb {
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint32_t sectors_per_fat;
    uint32_t root_cluster;
    uint32_t total_sectors_32;
} fat32_bpb_t;

// Mount FAT32 read-only from block device name -> returns 0 success
int fat32_mount(const char* dev_name, const char* mount_point);

// [M51 C4] Locking hooks for the VFS (see fs/fat32.c "Locking"): the driver's
// K30 klock (to drain an unmount), and an atomic O_APPEND write that resolves
// the offset (returned in *at) to the current end of file under that lock.
struct klock* fat32_fs_lock(void);
int fat32_write_append(vfs_inode_t* ino, const void* data, size_t len, size_t* at);
