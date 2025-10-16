// SecOS VFS layer (minimal) - abstraction to mount different filesystems
#pragma once
#include <stddef.h>
#include <stdint.h>

// Basic inode type enumeration
typedef enum { VFS_NODE_FILE=1, VFS_NODE_DIR=2 } vfs_node_type_t;

// Forward decl filesystem ops
struct vfs_fs_ops;

// Generic VFS inode representation
typedef struct vfs_inode {
    char path[256];            // absolute canonical path (no trailing '/') except root
    vfs_node_type_t type;      // file or directory
    size_t size;               // file size (0 for dir)
    void* fs_data;             // pointer to underlying FS specific entry/object
    const struct vfs_fs_ops* ops; // back-pointer to filesystem operations
} vfs_inode_t;

// Directory iteration callback
typedef void (*vfs_iter_cb)(const vfs_inode_t* child, void* user);

// Filesystem operations interface (per mounted FS root)
typedef struct vfs_fs_ops {
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
// Lookup inode by absolute path
vfs_inode_t* vfs_lookup(const char* path);
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
// Rename
int vfs_rename(const char* oldp, const char* newp);
// Truncate
int vfs_truncate(const char* path, size_t new_size);
