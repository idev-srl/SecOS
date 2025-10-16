#include "block.h"
#include <stdint.h>

// Simple zero-filled block device to allow ext2 mount attempt (will fail magic unless we inject fake superblock later)
static uint8_t dummy_storage[4096]; // 8 sectors of 512

static int ext2ram_read(block_dev_t* dev, uint64_t lba, void* buf, uint32_t count){ if(lba+count > 8) return -1; for(uint32_t s=0;s<count;s++){ for(int i=0;i<512;i++){ ((uint8_t*)buf)[s*512+i] = dummy_storage[(lba+s)*512 + i]; } } return (int)count; }

static block_dev_t ext2ram_dev = { .name="ext2ram", .sector_size=512, .sector_count=8, .read=ext2ram_read };

int ext2ramdev_register(void){ return block_register(&ext2ram_dev); }
