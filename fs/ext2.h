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

// Mount ext2 read-only from block device name; returns 0 on success
int ext2_mount(const char* dev_name);
