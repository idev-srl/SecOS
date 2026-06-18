/*
 * nvme.h — [M22] NVMe block driver.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Drives an NVMe controller (PCI class 01/08/02) — the storage controller on
 * NVMe-only laptops and the VMware "NVMe" option. Brings up the admin queue,
 * IDENTIFYs the first namespace and one I/O queue pair, and reads/writes with
 * polled DMA. Registers each namespace as block device "nvme0n1".., under which
 * FAT32/ext2/ext4 mount unchanged.
 */
#ifndef NVME_H
#define NVME_H

// Probe the NVMe controller, bring up the first namespace, and register it as a
// block device. Returns 0 on success, -1 if no NVMe controller/namespace.
int nvme_init(void);

#endif // NVME_H
