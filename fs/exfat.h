/*
 * SecOS Kernel - exFAT read-write filesystem
 * Written from the public Microsoft exFAT specification (2019).
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

// Mount the exFAT volume on block device `dev_name` at `mount_point`.
// Returns 0 on success, -1 if the device does not hold a valid exFAT volume
// (every on-disk value is validated; a corrupt image is refused, never trusted)
// or if the single exFAT instance is already mounted elsewhere.
int exfat_mount(const char* dev_name, const char* mount_point);

// Owner/mode options (uid=,gid=,umask=,dmask=,fmask=,ro,rw; fs/mntopts.h) for
// the next exfat_mount(). 0, or -1 if malformed. "" restores the defaults.
int exfat_set_mount_options(const char* opts);

// Probe only: 1 if `dev_name` carries an exFAT boot sector signature, else 0.
int exfat_probe(const char* dev_name);

// 1 while the exFAT instance is attached to the VFS.
int exfat_is_mounted(void);

// Flush and mark the volume clean (clears VolumeDirty). Same as ->sync.
int exfat_sync(void);
