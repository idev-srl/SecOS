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

static int inode_is_dead(const vfs_inode_t* ino){ return ino->fs_data==NULL && !(ino->path[0]=='/' && ino->path[1]==0); }

// Removal leaves a free ramfs slot that a later creation reuses (ramfs.c), so
// an inode must not stay bound to its entry once the file is gone: an fd still
// open on a removed file would otherwise read and write whatever file takes the
// slot next. The gone entry's inode is marked dead (fs_data=NULL: reads/writes
// fail); a fresh lookup of the reused slot gets its own inode.
static void cache_fix_after_remove(const ramfs_entry_t* gone){
    for(size_t i=0;i<inode_cache_used;i++){
        if(inode_cache[i].fs_data==(const void*)gone){
            inode_cache[i].fs_data=NULL; inode_cache[i].path[0]='?'; inode_cache[i].path[1]=0;
        }
    }
}

static vfs_inode_t* inode_from_entry(const ramfs_entry_t* e){ if(!e) return NULL; // search cache
    // [M38] refresh the cached size on every lookup (a prior write may have grown
    // the entry) so e.g. `>>` append seeks to the real end of file; path and type
    // too (a rename changes the entry's name in place).
    vfs_inode_t* ino = NULL;
    for(size_t i=0;i<inode_cache_used;i++){ if(inode_cache[i].fs_data == (void*)e){ ino = &inode_cache[i]; break; } }
    if(!ino && inode_cache_used < sizeof(inode_cache)/sizeof(inode_cache[0]))
        ino = &inode_cache[inode_cache_used++];
    // Cache full: recycle a dead inode. Fresh slots go first so a dead inode an
    // fd may still hold is recycled as late as possible.
    if(!ino) for(size_t i=0;i<inode_cache_used;i++) if(inode_is_dead(&inode_cache[i])){ ino=&inode_cache[i]; break; }
    if(!ino) return NULL;
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
static int ramfs_remove_fixed(const char* p, int dir){
    const ramfs_entry_t* e=ramfs_find(p); if(!e) return -1;
    int r = dir ? ramfs_rmdir(p) : ramfs_remove(p);
    if(r==0) cache_fix_after_remove(e);
    return r;
}
static int ramfs_vfs_remove(const char* path){ if(!path) return -1; const char* p=path; if(p[0]=='/') p++; return ramfs_remove_fixed(p, 0); }
// rename: an existing target (the VFS checked the type rules) is dropped first;
// RAMFS is in-memory and the kernel is non-preemptible, so this is atomic.
static int ramfs_vfs_rename(const char* oldp, const char* newp){
    if(!oldp||!newp) return -1; const char* o=oldp; const char* n=newp; if(o[0]=='/') o++; if(n[0]=='/') n++;
    const ramfs_entry_t* src=ramfs_find(o); if(!src) return -1;
    if(src->flags & 1) return -1;                                  // immutable
    const ramfs_entry_t* t=ramfs_find(n);
    if(t && ramfs_remove_fixed(n, (t->flags & 2) ? 1 : 0)!=0) return -1;
    return ramfs_rename(o,n);
}
static int ramfs_vfs_truncate(const char* path, size_t ns){ if(!path) return -1; const char* p=path; if(p[0]=='/') p++; return ramfs_truncate(p, ns); }

// statfs: like tmpfs, the size is the file data plus the free RAM it can still
// grow into; inodes are the fixed entry table.
static int ramfs_vfs_statfs(vfs_statfs_t* st){
    extern uint64_t pmm_get_free_memory(void);
    static const ramfs_entry_t* all[RAMFS_MAX_FILES];
    size_t n = ramfs_list(all, RAMFS_MAX_FILES);
    uint64_t used = 0;
    for(size_t i=0;i<n;i++) used += (all[i]->size + 4095) / 4096;
    uint64_t freeb = pmm_get_free_memory() / 4096;
    st->bsize = st->frsize = 4096;
    st->blocks = used + freeb; st->bfree = freeb; st->bavail = freeb;
    st->files = RAMFS_MAX_FILES; st->ffree = RAMFS_MAX_FILES > n ? RAMFS_MAX_FILES - n : 0;
    return 0;
}

static vfs_fs_ops_t ramfs_ops = {
    .statfs = ramfs_vfs_statfs,
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
