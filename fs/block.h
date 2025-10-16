#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct block_dev {
    const char* name;
    uint32_t sector_size; // bytes per sector (typically 512)
    uint64_t sector_count; // total sectors
    // Read sectors: returns number of sectors read or -1
    int (*read)(struct block_dev* dev, uint64_t lba, void* buf, uint32_t count);
} block_dev_t;

// Register a block device (returns 0 OK, -1 fail). Max small fixed table.
int block_register(block_dev_t* dev);
// Find device by name
block_dev_t* block_find(const char* name);
