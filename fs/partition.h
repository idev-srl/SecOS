/*
 * SecOS Kernel - MBR/GPT partition table parsing
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Real media (USB sticks, SATA/NVMe disks) is partitioned (MBR or GPT); the QEMU
 * test images put a raw filesystem at LBA 0, which hid this. This layer reads the
 * partition table of a block device and registers each partition as a sub-device
 * named "<parent>pN" (e.g. usb0p1) whose read/write are offset to the partition.
 * The FS code (fat32/ext2) then mounts a partition unchanged — it sees the
 * filesystem at the sub-device's LBA 0.
 */
#pragma once
#include "block.h"

/* Probe one block device for an MBR/GPT table; register its partitions as
 * sub-devices. Returns the number of partitions registered (0 if none/unpartitioned). */
int block_probe_partitions(block_dev_t* parent);

/* Probe every currently-registered base block device for partitions. Call once
 * after all storage drivers have registered their disks (and before mounting). */
void block_scan_partitions(void);
