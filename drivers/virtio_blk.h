/*
 * SecOS Kernel - virtio-blk driver (legacy/transitional PCI, polling)
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>

/* Probe the PCI bus for a transitional virtio-blk device, initialise the
 * device + a single virtqueue, and register it as block device "vda".
 * Returns 0 on success, -1 if no device / init failure. */
int virtio_blk_init(void);
