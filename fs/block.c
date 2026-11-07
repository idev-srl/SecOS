/*
 * SecOS Kernel - Block Device Registry
 * Lightweight registry for fixed small set of block devices.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M50 B3] One klock per base disk (block.h) replaces the M29 global irqsave
 * block_io_lock: two CPUs can drive two different disks at once, and a waiter
 * spins with interrupts on instead of off.
 */
#include "block.h"
#include "spinlock.h"

// Base disks (vda/sda../nvme0n1/usb0) + their MBR/GPT partition sub-devices
// (usb0p1, sda1, ...) all live here, so this must be comfortably > a few disks.
#define BLOCK_MAX_DEVS 24
static block_dev_t* g_devs[BLOCK_MAX_DEVS];
// Guards g_devs[]: USB enumeration can register a disk while another CPU looks
// one up. A leaf: nothing is called under it.
static spinlock_t g_devs_lock = SPINLOCK_INIT_CLASS(LK_S64_LEAF, "block-reg");
static int str_eq(const char* a,const char* b){ while(*a && *b){ if(*a!=*b) return 0; a++; b++; } return *a==0 && *b==0; }

block_dev_t* block_base(block_dev_t* dev){
    for(int i = 0; dev && dev->parent && i < 4; i++) dev = dev->parent;
    return dev;
}

// The base disk's lock, initialized on first use (drivers declare their
// block_dev_t statically and need not know about it).
static klock_t* blk_lock_of(block_dev_t* dev){
    block_dev_t* b = block_base(dev);
    if(__atomic_load_n(&b->lock_state, __ATOMIC_ACQUIRE) != 2){
        uint8_t z = 0;
        if(__atomic_compare_exchange_n(&b->lock_state, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)){
            klock_init(&b->lock, LK_K40_BLKDEV, b->name ? b->name : "blkdev", 0);
            __atomic_store_n(&b->lock_state, 2, __ATOMIC_RELEASE);
        } else {
            while(__atomic_load_n(&b->lock_state, __ATOMIC_ACQUIRE) != 2) __asm__ volatile("pause");
        }
    }
    return &b->lock;
}

int block_register(block_dev_t* dev){
    if(!dev||!dev->name||!dev->read||dev->sector_size==0) return -1;
    (void)blk_lock_of(dev);
    int r = -1;
    uint64_t fl = spin_lock_irqsave(&g_devs_lock);
    for(int i=0;i<BLOCK_MAX_DEVS;i++){
        if(g_devs[i]==dev){ r = 0; break; }
        if(!g_devs[i]){ g_devs[i]=dev; r = 0; break; }
    }
    spin_unlock_irqrestore(&g_devs_lock, fl);
    return r;
}
block_dev_t* block_find(const char* name){
    if(!name) return NULL;
    block_dev_t* r = NULL;
    uint64_t fl = spin_lock_irqsave(&g_devs_lock);
    for(int i=0;i<BLOCK_MAX_DEVS;i++){ if(g_devs[i] && str_eq(g_devs[i]->name,name)){ r = g_devs[i]; break; } }
    spin_unlock_irqrestore(&g_devs_lock, fl);
    return r;
}
int block_count(void){
    int n=0;
    uint64_t fl = spin_lock_irqsave(&g_devs_lock);
    for(int i=0;i<BLOCK_MAX_DEVS;i++) if(g_devs[i]) n++;
    spin_unlock_irqrestore(&g_devs_lock, fl);
    return n;
}
block_dev_t* block_get(int i){
    block_dev_t* r = NULL; int n=0;
    uint64_t fl = spin_lock_irqsave(&g_devs_lock);
    for(int k=0;k<BLOCK_MAX_DEVS;k++){ if(g_devs[k]){ if(n==i){ r = g_devs[k]; break; } n++; } }
    spin_unlock_irqrestore(&g_devs_lock, fl);
    return r;
}

void block_lock(block_dev_t* dev){ if(dev) klock_lock(blk_lock_of(dev)); }
void block_unlock(block_dev_t* dev){ if(dev) klock_unlock(blk_lock_of(dev)); }
int  block_lock_held(block_dev_t* dev){ return dev ? klock_held(blk_lock_of(dev)) : 0; }

int block_read_locked(block_dev_t* dev, uint64_t lba, void* buf, uint32_t count){
    if(!dev||!dev->read||!buf) return -1;
    KLOCK_ASSERT_HELD(blk_lock_of(dev));
    return dev->read(dev,lba,buf,count);
}
int block_write_locked(block_dev_t* dev, uint64_t lba, const void* buf, uint32_t count){
    if(!dev||!dev->write||!buf) return -1;
    KLOCK_ASSERT_HELD(blk_lock_of(dev));
    return dev->write(dev,lba,buf,count);
}

int block_read(block_dev_t* dev, uint64_t lba, void* buf, uint32_t count){
    if(!dev||!dev->read||!buf) return -1;
    klock_t* l = blk_lock_of(dev);
    klock_lock(l);
    int r = dev->read(dev,lba,buf,count);
    klock_unlock(l);
    return r;
}
int block_write(block_dev_t* dev, uint64_t lba, const void* buf, uint32_t count){
    if(!dev||!dev->write||!buf) return -1;
    klock_t* l = blk_lock_of(dev);
    klock_lock(l);
    int r = dev->write(dev,lba,buf,count);
    klock_unlock(l);
    return r;
}
