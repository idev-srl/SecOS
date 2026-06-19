/*
 * pagecache.c — [M20] Unified file page cache.
 * Copyright (c) 2025 iDev srl
 * SPDX-License-Identifier: MIT
 */
#include "pagecache.h"
#include "pmm.h"
#include "vmm.h"        // phys_to_virt
#include "vfs.h"        // vfs_inode_t + ops->read
#include "spinlock.h"   // SMP serialization of the cache state

#define ADDRESS_MASK 0x000FFFFFFFFFF000ULL
#define PC_ENTRIES   128

typedef struct {
    vfs_inode_t* inode;
    uint64_t     off;     // page-aligned file offset
    uint64_t     phys;    // cached frame (0 = none)
    uint8_t      valid;
} pc_entry_t;

static pc_entry_t pc[PC_ENTRIES];
static uint32_t   pc_clock = 0;   // FIFO eviction hand
static spinlock_t pc_lock = SPINLOCK_INIT;   // guards pc[] + pc_clock

// Fill frame 'phys' with file bytes [off, off+4096), zero-padded past EOF.
static void pc_fill(vfs_inode_t* ino, uint64_t off, uint64_t phys) {
    uint8_t* page = (uint8_t*)phys_to_virt(phys);
    for (int i = 0; i < 4096; i++) page[i] = 0;
    if (ino->ops && ino->ops->read && off < ino->size) {
        uint64_t avail = ino->size - off;
        uint64_t n = avail < 4096 ? avail : 4096;
        if (n) ino->ops->read(ino, (size_t)off, page, (size_t)n);
    }
}

// Core lookup/fill; caller must hold pc_lock.
static uint64_t pagecache_get_phys_nolock(vfs_inode_t* inode, uint64_t off) {
    if (!inode) return 0;
    off &= ~0xFFFULL;
    for (int i = 0; i < PC_ENTRIES; i++)
        if (pc[i].valid && pc[i].inode == inode && pc[i].off == off) return pc[i].phys;

    // Miss: take a free slot, else FIFO-evict (cache frames are kernel-only).
    pc_entry_t* e = 0;
    for (int i = 0; i < PC_ENTRIES; i++) if (!pc[i].valid) { e = &pc[i]; break; }
    if (!e) {
        e = &pc[pc_clock++ % PC_ENTRIES];
        if (e->phys) pmm_free_frame((void*)e->phys);
        e->valid = 0; e->phys = 0;
    }
    void* f = pmm_alloc_frame();
    if (!f) return 0;
    uint64_t phys = (uint64_t)f & ADDRESS_MASK;
    pc_fill(inode, off, phys);
    e->inode = inode; e->off = off; e->phys = phys; e->valid = 1;
    return phys;
}

uint64_t pagecache_get_phys(vfs_inode_t* inode, uint64_t off) {
    uint64_t fl = spin_lock_irqsave(&pc_lock);
    uint64_t phys = pagecache_get_phys_nolock(inode, off);
    spin_unlock_irqrestore(&pc_lock, fl);
    return phys;
}

int pagecache_read(vfs_inode_t* inode, uint64_t offset, void* kbuf, uint64_t len) {
    if (!inode) return -1;
    if (offset >= inode->size) return 0;
    if (offset + len > inode->size) len = inode->size - offset;
    uint8_t* dst = (uint8_t*)kbuf;
    uint64_t done = 0;
    uint64_t fl = spin_lock_irqsave(&pc_lock);
    while (done < len) {
        uint64_t off    = offset + done;
        uint64_t pgoff  = off & ~0xFFFULL;
        uint64_t in_pg  = off - pgoff;
        uint64_t phys   = pagecache_get_phys_nolock(inode, pgoff);
        if (!phys) break;
        const uint8_t* src = (const uint8_t*)phys_to_virt(phys);
        uint64_t chunk = 4096 - in_pg;
        if (chunk > len - done) chunk = len - done;
        for (uint64_t i = 0; i < chunk; i++) dst[done + i] = src[in_pg + i];
        done += chunk;
    }
    spin_unlock_irqrestore(&pc_lock, fl);
    return (int)done;
}

void pagecache_invalidate(vfs_inode_t* inode, uint64_t offset, uint64_t len) {
    if (!inode) return;
    uint64_t end = offset + len;
    uint64_t fl = spin_lock_irqsave(&pc_lock);
    for (int i = 0; i < PC_ENTRIES; i++) {
        if (pc[i].valid && pc[i].inode == inode &&
            pc[i].off < end && pc[i].off + 4096 > offset) {
            if (pc[i].phys) pmm_free_frame((void*)pc[i].phys);
            pc[i].valid = 0; pc[i].phys = 0;
        }
    }
    spin_unlock_irqrestore(&pc_lock, fl);
}
