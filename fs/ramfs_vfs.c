/*
 * SecOS Kernel - RAMFS VFS Adapter
 * Bridges RAMFS internal storage to generic VFS inode & operations interface.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "vfs.h"
#include "ramfs.h"
#include <stddef.h>

// Simple inode cache: build on lookup; no eviction.
static vfs_inode_t inode_cache[RAMFS_MAX_FILES+4];
static size_t inode_cache_used = 0;

static vfs_inode_t* inode_from_entry(const ramfs_entry_t* e){ if(!e) return NULL; // search cache
    for(size_t i=0;i<inode_cache_used;i++){ if(inode_cache[i].fs_data == (void*)e) return &inode_cache[i]; }
    if(inode_cache_used >= sizeof(inode_cache)/sizeof(inode_cache[0])) return NULL;
    vfs_inode_t* ino = &inode_cache[inode_cache_used++];
    // Path already absolute in ramfs_entry_t.name (without leading '/')
    // Build canonical path with leading '/' (root = "/")
    size_t k=0; const char* src=e->name; if(!src[0]){ ino->path[0]='/'; ino->path[1]=0; } else {
        // Prepend '/' for consistency
        ino->path[k++]='/'; for(size_t i=0; src[i] && k<sizeof(ino->path)-1; i++) ino->path[k++]=src[i]; ino->path[k]=0;
    }
    ino->type = (e->flags & 2)? VFS_NODE_DIR : VFS_NODE_FILE;
    ino->size = e->size;
    ino->fs_data = (void*)e;
    // ops pointer filled later after ops struct defined
    return ino;
}

static vfs_inode_t* ramfs_vfs_lookup(const char* path){
    if(!path) return NULL;
    // Normalize path: remove leading '/', ramfs stored names without leading slash
    const char* np = path;
    if(np[0]=='/' && np[1]==0){ // root
        // fabricate root inode
        for(size_t i=0;i<inode_cache_used;i++){ if(inode_cache[i].path[0]=='/' && inode_cache[i].path[1]==0) return &inode_cache[i]; }
        if(inode_cache_used >= sizeof(inode_cache)/sizeof(inode_cache[0])) return NULL; vfs_inode_t* root=&inode_cache[inode_cache_used++]; root->path[0]='/'; root->path[1]=0; root->type=VFS_NODE_DIR; root->size=0; root->fs_data=NULL; return root;
    }
    if(np[0]=='/') np++;
    const ramfs_entry_t* e = ramfs_find(np);
    if(!e){ // If path not found (only root implicitly exists) return NULL
        return NULL;
    }
    vfs_inode_t* ino = inode_from_entry(e);
    if(ino) ino->ops = NULL; // will be set by mount init externally if needed
    return ino;
}

struct readdir_ctx { vfs_iter_cb cb; void* user; };
static void ramfs_vfs_iter_children(const char* dir_path, vfs_iter_cb cb, void* user){
    const ramfs_entry_t* arr[RAMFS_MAX_FILES]; size_t n;
    // Convert path to ramfs representation
    int root = (!dir_path || (dir_path[0]=='/' && dir_path[1]==0));
    if(root){ n = ramfs_list_path("", arr, RAMFS_MAX_FILES); }
    else {
        // strip leading '/'
        const char* p = dir_path; if(p[0]=='/') p++; n = ramfs_list_path(p, arr, RAMFS_MAX_FILES);
    }
    for(size_t i=0;i<n;i++){ vfs_inode_t* ino = inode_from_entry(arr[i]); if(ino){ ino->ops=NULL; cb(ino,user); } }
}

static int ramfs_vfs_readdir(const char* dir_path, vfs_iter_cb cb, void* user){ if(!cb) return -1; ramfs_vfs_iter_children(dir_path, cb, user); return 0; }

static int ramfs_vfs_read(vfs_inode_t* inode, size_t offset, void* buf, size_t len){ if(!inode || inode->type!=VFS_NODE_FILE) return -1; const ramfs_entry_t* e=(const ramfs_entry_t*)inode->fs_data; if(!e) return -1; if(offset>e->size) return -1; size_t avail = e->size - offset; if(len>avail) len=avail; const uint8_t* src = e->data + offset; for(size_t i=0;i<len;i++) ((uint8_t*)buf)[i]=src[i]; return (int)len; }
static int ramfs_vfs_write(vfs_inode_t* inode, size_t offset, const void* data, size_t len){ if(!inode || inode->type!=VFS_NODE_FILE) return -1; const ramfs_entry_t* e=(const ramfs_entry_t*)inode->fs_data; if(!e) return -1; return ramfs_write(((const ramfs_entry_t*)e)->name, offset, data, len); }
static int ramfs_vfs_create(const char* path, const void* initial, size_t size){ if(!path) return -1; const char* p=path; if(p[0]=='/') p++; if(ramfs_find(p)) return -1; return ramfs_add(p, initial, size); }
static int ramfs_vfs_mkdir(const char* path){ if(!path) return -1; const char* p=path; if(p[0]=='/') p++; return ramfs_mkdir(p); }
static int ramfs_vfs_remove(const char* path){ if(!path) return -1; const char* p=path; if(p[0]=='/') p++; return ramfs_remove(p); }
static int ramfs_vfs_rename(const char* oldp, const char* newp){ if(!oldp||!newp) return -1; const char* o=oldp; const char* n=newp; if(o[0]=='/') o++; if(n[0]=='/') n++; return ramfs_rename(o,n); }
static int ramfs_vfs_truncate(const char* path, size_t ns){ if(!path) return -1; const char* p=path; if(p[0]=='/') p++; return ramfs_truncate(p, ns); }

static vfs_fs_ops_t ramfs_ops = {
    .lookup = ramfs_vfs_lookup,
    .readdir = ramfs_vfs_readdir,
    .read = ramfs_vfs_read,
    .write = ramfs_vfs_write,
    .create = ramfs_vfs_create,
    .mkdir = ramfs_vfs_mkdir,
    .remove = ramfs_vfs_remove,
    .rename = ramfs_vfs_rename,
    .truncate = ramfs_vfs_truncate
};

// Public helper to mount RAMFS into VFS root
int vfs_mount_ramfs(void){ return vfs_mount_root(&ramfs_ops, "ramfs"); }
