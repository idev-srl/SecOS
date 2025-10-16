#include "vfs.h"
#include <stddef.h>

static vfs_mount_t g_root_mount = {0};

void vfs_init(void){ g_root_mount.mount_point = NULL; g_root_mount.ops = NULL; g_root_mount.fs_name = NULL; }

int vfs_mount_root(const vfs_fs_ops_t* ops, const char* fs_name){ if(!ops || !fs_name) return -1; if(g_root_mount.ops) return -1; g_root_mount.mount_point = "/"; g_root_mount.ops = ops; g_root_mount.fs_name = fs_name; return 0; }
int vfs_replace_root(const vfs_fs_ops_t* ops, const char* fs_name){ if(!ops || !fs_name) return -1; g_root_mount.mount_point = "/"; g_root_mount.ops = ops; g_root_mount.fs_name = fs_name; return 0; }

// Simplified: underlying FS provides lookup returning allocated inode or cached object. For RAMFS adapter we build lightweight inode objects.
vfs_inode_t* vfs_lookup(const char* path){ if(!path || !*path) path="/"; if(g_root_mount.ops){ return g_root_mount.ops->lookup(path); } return NULL; }

int vfs_readdir(const char* path, vfs_iter_cb cb, void* user){ if(!path || !*path) path="/"; if(!cb) return -1; if(!g_root_mount.ops) return -1; return g_root_mount.ops->readdir(path, cb, user); }

int vfs_read_all(const char* path, void* buf, size_t bufsize){ if(!g_root_mount.ops) return -1; vfs_inode_t* ino = vfs_lookup(path); if(!ino || ino->type!=VFS_NODE_FILE) return -1; if(ino->size > bufsize) return -1; int r = g_root_mount.ops->read(ino,0,buf,ino->size); return (r>=0)? ino->size : -1; }

int vfs_create(const char* path, const void* data, size_t size){ if(!g_root_mount.ops) return -1; return g_root_mount.ops->create(path,data,size); }
int vfs_write(const char* path, size_t offset, const void* data, size_t len){ if(!g_root_mount.ops) return -1; vfs_inode_t* ino=vfs_lookup(path); if(!ino || ino->type!=VFS_NODE_FILE) return -1; return g_root_mount.ops->write(ino,offset,data,len); }
int vfs_mkdir(const char* path){ if(!g_root_mount.ops) return -1; return g_root_mount.ops->mkdir(path); }
int vfs_remove(const char* path){ if(!g_root_mount.ops) return -1; return g_root_mount.ops->remove(path); }
int vfs_rename(const char* oldp, const char* newp){ if(!g_root_mount.ops) return -1; return g_root_mount.ops->rename(oldp,newp); }
int vfs_truncate(const char* path, size_t new_size){ if(!g_root_mount.ops) return -1; return g_root_mount.ops->truncate(path,new_size); }
