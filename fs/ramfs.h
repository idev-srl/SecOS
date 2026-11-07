/*
 * SecOS Kernel - RAMFS Interface
 * Simple in-memory filesystem with fixed entry table and hierarchical paths.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef RAMFS_H
#define RAMFS_H
#include <stdint.h>
#include <stddef.h>
#include "klock.h"

// Simple in-memory filesystem with fixed table.
// Limits: RAMFS_MAX_FILES entries, name < RAMFS_NAME_MAX chars, content in kernel memory.

#define RAMFS_MAX_FILES 256 /* [M31] /bin coreutils + FHS skeleton; BusyBox adds ~125 applet stubs */
#define RAMFS_NAME_MAX  96  // full path length, subdirectories included

// flags: bit0 immutable entry, bit1 directory
typedef struct {
    char     name[RAMFS_NAME_MAX]; // full path (e.g. "dir/sub/file") or simple root name
    uint8_t* data; // file data (NULL for directory)
    size_t   size; // file size (0 for directory)
    unsigned flags; // bit0 immutable, bit1 directory
} ramfs_entry_t;

// [M51 C2] The instance lock (LK_K30_FS): guards the entry table and the
// ramfs VFS inode cache (fs/ramfs_vfs.c). Every function below takes it; the
// *_locked variants expect the caller to hold it. Never held across a user
// copy, a wait, or a call into another filesystem.
klock_t* ramfs_fs_lock(void);

int ramfs_init(void); // initialize table and register static files
// The pointer-returning lookups hand out a stable slot; its contents are only
// stable while ramfs_fs_lock() is held (see ramfs.c).
const ramfs_entry_t* ramfs_find(const char* name);
size_t ramfs_list(const ramfs_entry_t** out_array, size_t max);
int ramfs_add(const char* name, const void* data, size_t size); // add mutable file
int ramfs_add_static(const char* name, const void* data, size_t size); // add immutable file (init phase)
int ramfs_write(const char* name, size_t offset, const void* src, size_t len); // returns bytes written or -1
int ramfs_truncate(const char* name, size_t new_size); // -1 on error
int ramfs_remove(const char* name); // -1 on error (not found / immutable)
// Directory API
int ramfs_mkdir(const char* path); // create empty mutable directory
int ramfs_rmdir(const char* path); // remove empty directory (must not be immutable)
size_t ramfs_list_path(const char* path, const ramfs_entry_t** out_array, size_t max); // list direct children
int ramfs_is_dir(const char* path); // 1 if directory, 0 if file, -1 if missing
int ramfs_rename(const char* old_path, const char* new_path); // rename file or directory (updates descendants if directory)

// The same operations for a caller that holds ramfs_fs_lock().
const ramfs_entry_t* ramfs_find_locked(const char* name);
size_t ramfs_list_locked(const ramfs_entry_t** out_array, size_t max);
const ramfs_entry_t* ramfs_next_child_locked(const char* path, size_t* cursor);
int ramfs_add_locked(const char* name, const void* data, size_t size);
int ramfs_write_locked(const char* name, size_t offset, const void* src, size_t len);
int ramfs_write_entry_locked(ramfs_entry_t* e, size_t offset, const void* src, size_t len);
int ramfs_truncate_locked(const char* name, size_t new_size);
int ramfs_remove_locked(const char* name);
int ramfs_mkdir_locked(const char* path);
int ramfs_rmdir_locked(const char* path);
size_t ramfs_list_path_locked(const char* path, const ramfs_entry_t** out_array, size_t max);
int ramfs_is_dir_locked(const char* path);
int ramfs_rename_locked(const char* old_path, const char* new_path);

#endif
