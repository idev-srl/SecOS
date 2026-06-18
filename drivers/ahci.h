/*
 * ahci.h — [M21] AHCI (SATA) block driver.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Drives a SATA disk behind an AHCI HBA (PCI class 01/06/01) — the storage
 * controller VMware (SATA option) and most physical PCs expose. Registers the
 * first usable SATA port as block device "sda", under which FAT32/ext2/ext4
 * mount unchanged. Polled DMA, single command slot.
 */
#ifndef AHCI_H
#define AHCI_H

// Probe the AHCI controller, bring up the first SATA disk, and register it as
// block device "sda". Returns 0 on success, -1 if no AHCI/SATA disk is present.
int ahci_init(void);

#endif // AHCI_H
