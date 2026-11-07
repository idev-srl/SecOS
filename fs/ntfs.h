/*
 * SecOS Kernel - NTFS read-only filesystem
 * Written from public format documentation (see fs/ntfs.c, docs/devlog/M42-ntfs.md).
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define NTFS_MAX_VOLUMES 4

// Mount the NTFS volume on block device `dev_name` at `mount_point`,
// read-only. Returns 0 on success, -1 if the device does not hold a valid NTFS
// volume (every on-disk value is validated; a corrupt image is refused, never
// trusted) or if all NTFS_MAX_VOLUMES instances are in use. A volume whose
// dirty flag is set or that Windows left hibernated (Fast Startup) is still
// mounted read-only, with a warning on debugcon.
int ntfs_mount(const char* dev_name, const char* mount_point);

// Same, with an explicit access mode: want_rw != 0 is refused with -30
// (EROFS) because this driver has no write support.
int ntfs_mount_opts(const char* dev_name, const char* mount_point, int want_rw);
// Owner/mode options (uid=,gid=,umask=,dmask=,fmask=,ro; "rw" -> EROFS;
// fs/mntopts.h) for the next mount. 0, or -1 if malformed.
int ntfs_set_mount_options(const char* opts);

// Probe only: 1 if `dev_name` carries an NTFS boot sector signature, else 0.
int ntfs_probe(const char* dev_name);

// Number of NTFS instances currently attached to the VFS.
int ntfs_mounted_count(void);

// [M51 C4] The K30 klock of the NTFS instance behind `ops` (NULL if `ops` is
// not an NTFS instance): one lock per volume, e.g. to drain an unmount.
struct klock;
struct vfs_fs_ops;
struct klock* ntfs_fs_lock(const struct vfs_fs_ops* ops);
