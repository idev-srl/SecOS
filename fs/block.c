/*
 * SecOS Kernel - Block Device Registry
 * Lightweight registry for fixed small set of block devices.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "block.h"
#include "spinlock.h"

// [M29] One global lock serializes all block-device I/O: the underlying drivers
// (virtio-blk / AHCI / NVMe) each drive a single polled queue/port, so two CPUs
// must not enter a device read/write concurrently. irqsave keeps the polled
// hardware op atomic against this CPU's ISRs too.
static spinlock_t block_io_lock = SPINLOCK_INIT;

// Base disks (vda/sda../nvme0n1/usb0) + their MBR/GPT partition sub-devices
// (usb0p1, sda1, ...) all live here, so this must be comfortably > a few disks.
#define BLOCK_MAX_DEVS 24
static block_dev_t* g_devs[BLOCK_MAX_DEVS];
static int str_eq(const char* a,const char* b){ while(*a && *b){ if(*a!=*b) return 0; a++; b++; } return *a==0 && *b==0; }

int block_register(block_dev_t* dev){ if(!dev||!dev->name||!dev->read||dev->sector_size==0) return -1; for(int i=0;i<BLOCK_MAX_DEVS;i++){ if(g_devs[i]==dev) return 0; if(!g_devs[i]){ g_devs[i]=dev; return 0; } } return -1; }
block_dev_t* block_find(const char* name){ if(!name) return NULL; for(int i=0;i<BLOCK_MAX_DEVS;i++){ if(g_devs[i] && str_eq(g_devs[i]->name,name)) return g_devs[i]; } return NULL; }
int block_count(void){ int n=0; for(int i=0;i<BLOCK_MAX_DEVS;i++) if(g_devs[i]) n++; return n; }
block_dev_t* block_get(int i){ int n=0; for(int k=0;k<BLOCK_MAX_DEVS;k++){ if(g_devs[k]){ if(n==i) return g_devs[k]; n++; } } return NULL; }

int block_read(block_dev_t* dev, uint64_t lba, void* buf, uint32_t count){
    if(!dev||!dev->read||!buf) return -1;
    uint64_t fl = spin_lock_irqsave(&block_io_lock);
    int r = dev->read(dev,lba,buf,count);
    spin_unlock_irqrestore(&block_io_lock, fl);
    return r;
}
int block_write(block_dev_t* dev, uint64_t lba, const void* buf, uint32_t count){
    if(!dev||!dev->write||!buf) return -1;
    uint64_t fl = spin_lock_irqsave(&block_io_lock);
    int r = dev->write(dev,lba,buf,count);
    spin_unlock_irqrestore(&block_io_lock, fl);
    return r;
}
