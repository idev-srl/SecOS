/*
 * SecOS Kernel - Block Device Interface
 * Defines a minimal block device abstraction (read-only operations for now).
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M50 B3] Locking: every BASE disk (a device with parent == NULL) carries one
 * klock (level LK_K40_BLKDEV, kernel/lock_order.h) that serializes its driver;
 * a partition has no lock of its own and uses its parent's. block_read /
 * block_write take it around the driver call, so callers must never call
 * dev->read / dev->write directly. A caller that needs several I/Os to be
 * atomic (read-modify-write of a partial sector) takes block_lock() and uses
 * the *_locked variants. Drivers poll their hardware inside the lock and never
 * depend on an interrupt or the scheduler tick to make progress; the lock is a
 * klock, so its holder is never preempted and other CPUs wait with IF on.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "klock.h"

typedef struct block_dev {
    const char* name;
    uint32_t sector_size; // bytes per sector (typically 512)
    uint64_t sector_count; // total sectors
    // Read sectors: returns number of sectors read or -1
    int (*read)(struct block_dev* dev, uint64_t lba, void* buf, uint32_t count);
    // Write sectors: returns number of sectors written or -1 (NULL if read-only)
    int (*write)(struct block_dev* dev, uint64_t lba, const void* buf, uint32_t count);
    // [M50 B3] Partition: the disk it lives on (NULL = this is a base disk).
    struct block_dev* parent;
    // [M50 B3] Base disk only: serializes the driver. Initialized lazily by the
    // block layer (lock_state 0 -> 1 initializing -> 2 ready), so a driver may
    // leave both zero in its static initializer.
    klock_t lock;
    volatile uint8_t lock_state;
} block_dev_t;

// Register a block device (returns 0 OK, -1 fail). Max small fixed table.
int block_register(block_dev_t* dev);
// Find device by name
block_dev_t* block_find(const char* name);
// Enumerate registered devices: count, and the i-th device (NULL if out of range).
int block_count(void);
block_dev_t* block_get(int i);
// Convenience wrappers: returns sectors transferred or -1 (write -1 if read-only).
// They take the base disk's lock.
int block_read(block_dev_t* dev, uint64_t lba, void* buf, uint32_t count);
int block_write(block_dev_t* dev, uint64_t lba, const void* buf, uint32_t count);

// [M50 B3] The base disk under `dev` (dev itself for a base disk).
block_dev_t* block_base(block_dev_t* dev);
// Hold / release the base disk's lock across several I/Os.
void block_lock(block_dev_t* dev);
void block_unlock(block_dev_t* dev);
// I/O with the base disk's lock already held by the caller (block_lock).
int block_read_locked(block_dev_t* dev, uint64_t lba, void* buf, uint32_t count);
int block_write_locked(block_dev_t* dev, uint64_t lba, const void* buf, uint32_t count);
// Does the calling task hold the lock of `dev`'s base disk? (for asserts)
int block_lock_held(block_dev_t* dev);
