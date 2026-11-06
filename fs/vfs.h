/*
 * SecOS Kernel - VFS Interface (Minimal)
 * Abstracts mounting of different filesystems (currently single root).
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
// SecOS VFS layer (minimal) - abstraction to mount different filesystems
#pragma once
#include <stddef.h>
#include <stdint.h>

// Basic inode type enumeration
typedef enum { VFS_NODE_FILE=1, VFS_NODE_DIR=2, VFS_NODE_SYMLINK=3 } vfs_node_type_t; // [M26] symlink

// Forward decl filesystem ops
struct vfs_fs_ops;

// Generic VFS inode representation
typedef struct vfs_inode {
    char path[256];            // absolute canonical path (no trailing '/') except root
    vfs_node_type_t type;      // file, directory or symlink
    size_t size;               // file size (0 for dir)
    void* fs_data;             // pointer to underlying FS specific entry/object
    const struct vfs_fs_ops* ops; // back-pointer to filesystem operations
    // [M26] POSIX metadata. Stored & exposed (stat/chmod/chown/utimes) but NOT
    // used for multi-user access enforcement — the signature is the trust
    // boundary in SecOS, so a signed process has ambient access regardless.
    uint32_t mode;             // full POSIX mode: S_IFMT type bits | permission bits
    uint32_t uid, gid;         // owner / group
    uint64_t atime, mtime, ctime; // unix seconds
    uint32_t nlink;            // hard-link count
} vfs_inode_t;

// [M26] Attribute-change request for setattr (chmod/chown/utimes). Only the
// fields whose VFS_ATTR_* bit is set in `valid` are applied.
#define VFS_ATTR_MODE  0x01
#define VFS_ATTR_UID   0x02
#define VFS_ATTR_GID   0x04
#define VFS_ATTR_ATIME 0x08
#define VFS_ATTR_MTIME 0x10
typedef struct vfs_attr {
    uint32_t mode;
    uint32_t uid, gid;
    uint64_t atime, mtime;
} vfs_attr_t;

// statfs(2) result (Linux struct statfs fields, in units of frsize/bsize).
typedef struct vfs_statfs {
    uint64_t type;     // f_type magic (EXT2_SUPER_MAGIC 0xEF53, MSDOS 0x4d44, ...)
    uint64_t bsize;    // optimal transfer block size
    uint64_t blocks;   // total data blocks (frsize units)
    uint64_t bfree;    // free blocks
    uint64_t bavail;   // free blocks available to unprivileged users
    uint64_t files;    // total inodes
    uint64_t ffree;    // free inodes
    uint64_t namelen;  // maximum filename length
    uint64_t frsize;   // fragment size (== bsize here)
    uint64_t flags;    // ST_* mount flags
} vfs_statfs_t;

// Directory iteration callback
typedef void (*vfs_iter_cb)(const vfs_inode_t* child, void* user);

// [M23] Per-FS behaviour flags (default 0). Virtual filesystems whose contents
// are generated/dynamic (devfs, procfs, sysfs) set VFS_FS_NOCACHE so reads go
// straight to ->read() instead of through the unified page cache.
#define VFS_FS_NOCACHE 0x1
// Read-only filesystem (e.g. NTFS): open() for writing / O_TRUNC / O_CREAT
// fails with -EROFS and /proc/mounts shows "ro".
#define VFS_FS_RDONLY  0x2

// Filesystem operations interface (per mounted FS root)
typedef struct vfs_fs_ops {
    // [M23] behaviour flags (VFS_FS_NOCACHE, ...). 0 for normal disk/ram FSes.
    unsigned flags;
    // Lookup path (absolute, no trailing slash except root). Returns NULL if not found.
    vfs_inode_t* (*lookup)(const char* path);
    // Iterate direct children of a directory path.
    int (*readdir)(const char* dir_path, vfs_iter_cb cb, void* user);
    // Read file contents into buffer, returns bytes read or -1.
    int (*read)(vfs_inode_t* inode, size_t offset, void* buf, size_t len);
    // Write file (grow if needed) returns bytes written or -1.
    int (*write)(vfs_inode_t* inode, size_t offset, const void* buf, size_t len);
    // Create file (mutable) under parent dir; fail if exists.
    int (*create)(const char* path, const void* initial, size_t size);
    // Make directory
    int (*mkdir)(const char* path);
    // Remove file or empty directory
    int (*remove)(const char* path);
    // Rename (same FS)
    int (*rename)(const char* old_path, const char* new_path);
    // Truncate file
    int (*truncate)(const char* path, size_t new_size);
    // [M26] Change attributes (mode/uid/gid/times); `valid` = VFS_ATTR_* bitmask.
    // NULL = no on-disk metadata: vfs_setattr updates the in-memory inode. Returns 0/-1.
    int (*setattr)(const char* path, const vfs_attr_t* a, unsigned valid);
    // [M26] Read a symlink's target into buf (NUL-terminated). Returns length or -1.
    int (*readlink)(const char* path, char* buf, size_t len);
    // [M26] Create a symlink at linkpath pointing to target. Returns 0/-1.
    int (*symlink)(const char* target, const char* linkpath);
    // Flush anything this FS still holds in memory to its device (orderly
    // shutdown, sync). NULL = write-through, nothing to flush. Returns 0/-1.
    int (*sync)(void);
    // Hard link: give the existing non-directory at old_path the extra name
    // new_path (same FS). NULL = no hard links (link() fails with EPERM).
    int (*link)(const char* old_path, const char* new_path);
    // Fill filesystem usage for statfs(). NULL = the VFS reports zero counts
    // with the f_type magic derived from the mount's fs name.
    int (*statfs)(vfs_statfs_t* st);
    // Mount-table info for /proc/mounts: backing device name + filesystem type
    // ("ext4", "vfat", ...). NULL = virtual FS (the mount's fs name is used).
    int (*mntinfo)(const char** source, const char** fstype);
} vfs_fs_ops_t;

// Mount record (single root for now)
typedef struct vfs_mount {
    const char* mount_point;      // e.g. "/"
    const vfs_fs_ops_t* ops;      // operations
    const char* fs_name;          // human label
} vfs_mount_t;

// Initialize VFS (empty, no mounts)
void vfs_init(void);
// Mount a filesystem at a given mount point (currently only root supported). Returns 0 on success.
int vfs_mount_root(const vfs_fs_ops_t* ops, const char* fs_name);
// Replace current root FS (unmount old conceptually). Returns 0 success.
int vfs_replace_root(const vfs_fs_ops_t* ops, const char* fs_name);
// Mount a filesystem at an absolute mount point (e.g. "/mnt"). Returns 0 success.
int vfs_mount(const char* mount_point, const vfs_fs_ops_t* ops, const char* fs_name);
// Unmount the filesystem at the given mount point. Returns 0 success.
int vfs_unmount(const char* mount_point);
// Sync every mounted filesystem (ops->sync). Returns how many were synced.
int vfs_sync_all(void);
// Unmount every mount except the root "/" (shutdown). Returns how many.
int vfs_unmount_all(void);
// Lookup inode by absolute path
vfs_inode_t* vfs_lookup(const char* path);
// Lookup following symlinks in every component, the final one included
// (bounded hop count). Used by stat/open/chdir.
vfs_inode_t* vfs_lookup_follow(const char* path);
// Lookup with symlinks followed in intermediate components; the final one only
// when `follow`. On failure returns NULL and *err = -ENOENT/-ENOTDIR/-ELOOP/...
vfs_inode_t* vfs_lookup_ex(const char* path, int follow, int* err);
// Canonicalize an absolute path (".", "..", symlinks resolved). 0 or -errno.
int vfs_walk(const char* path, char* out, size_t outsz, int follow_final);
// rmdir: remove an empty directory (-ENOTDIR/-ENOTEMPTY/-EBUSY). 0 or -errno.
int vfs_rmdir(const char* path);
// Hard link old -> new (same FS; -EXDEV/-EEXIST/-EPERM). 0 or -errno.
int vfs_link(const char* oldp, const char* newp);
// statfs() of the filesystem holding `path` / behind `ops`. 0 or -errno.
int vfs_statfs(const char* path, vfs_statfs_t* st);
int vfs_statfs_ops(const vfs_fs_ops_t* ops, vfs_statfs_t* st);
// Mount point of the filesystem behind `ops` (NULL if not mounted).
const char* vfs_mount_point_of(const vfs_fs_ops_t* ops);
// Flush one filesystem (fsync of a file on it). 0/-1.
int vfs_sync_ops(const vfs_fs_ops_t* ops);
// Iterate directory children
int vfs_readdir(const char* path, vfs_iter_cb cb, void* user);
// Read whole file convenience (returns size or -1 if too small)
int vfs_read_all(const char* path, void* buf, size_t bufsize);
// Create file
int vfs_create(const char* path, const void* data, size_t size);
// Write file
int vfs_write(const char* path, size_t offset, const void* data, size_t len);
// Mkdir
int vfs_mkdir(const char* path);
// Remove
int vfs_remove(const char* path);
// Rename (atomic within one FS, replaces an existing target; -EXDEV across
// mounts). 0 or -errno.
int vfs_rename(const char* oldp, const char* newp);
// Truncate
int vfs_truncate(const char* path, size_t new_size);
// [M26] Change attributes (chmod/chown/utimes); valid = VFS_ATTR_* bitmask.
int vfs_setattr(const char* path, const vfs_attr_t* a, unsigned valid);
// [M26] Read a symlink target (NUL-terminated). Returns length or -1.
int vfs_readlink(const char* path, char* buf, size_t len);
// [M26] Create a symlink at linkpath pointing to target. Returns 0/-1.
int vfs_symlink(const char* target, const char* linkpath);
// [M23] Mount-table introspection (for /proc/mounts, df, etc.).
int vfs_mount_count(void);
int vfs_mount_info(int i, const char** mount_point, const char** fs_name);
const vfs_fs_ops_t* vfs_mount_ops_at(int i);
