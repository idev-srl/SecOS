/*
 * SecOS Kernel - RAMFS VFS Adapter
 * Bridges RAMFS internal storage to generic VFS inode & operations interface.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M51 C2] Every vfs_fs_ops entry is one section under the ramfs instance
 * lock (ramfs_fs_lock(), LK_K30_FS, shared with fs/ramfs.c): the entry table
 * and this inode cache change together. The readdir callback runs unlocked, on
 * a private copy of each child inode (callers such as the kernel shell's ls
 * may wait for a key in it). Lookup hands out a pointer into the cache; its
 * fields are only rewritten under the lock, and only when they change.
 */
#include "vfs.h"
#include "ramfs.h"
#include "pagecache.h"
#include <stddef.h>

#define RLOCK()   klock_lock(ramfs_fs_lock())
#define RUNLOCK() klock_unlock(ramfs_fs_lock())
#define RAMFS_LOCKED() KLOCK_ASSERT_HELD(ramfs_fs_lock())

static vfs_fs_ops_t ramfs_ops;

// Simple inode cache: build on lookup; no eviction.
static vfs_inode_t inode_cache[RAMFS_MAX_FILES+4];
static size_t inode_cache_used = 0;
#define INODE_CACHE_N (sizeof(inode_cache)/sizeof(inode_cache[0]))

static int is_root(const char* p){ return p[0]=='/' && p[1]==0; }
static int inode_is_dead(const vfs_inode_t* ino){ return ino->fs_data==NULL && !is_root(ino->path); }

// Removal leaves a free ramfs slot that a later creation reuses (ramfs.c), so
// an inode must not stay bound to its entry once the file is gone: an fd still
// open on a removed file would otherwise read and write whatever file takes the
// slot next. The gone entry's inode is marked dead (fs_data=NULL: reads/writes
// fail); a fresh lookup of the reused slot gets its own inode.
static void cache_fix_after_remove(const ramfs_entry_t* gone){
    RAMFS_LOCKED();
    for(size_t i=0;i<inode_cache_used;i++){
        if(inode_cache[i].fs_data==(const void*)gone){
            inode_cache[i].fs_data=NULL; inode_cache[i].path[0]='?'; inode_cache[i].path[1]=0;
            // A dead inode is recycled for the next file once the cache is
            // full; its cached pages must not become that file's contents.
            // (Kernel-internal vfs_remove callers don't invalidate first.)
            // pc_lock (S40) nests inside the FS lock (K30).
            pagecache_invalidate(&inode_cache[i], 0, 1ULL<<62);
        }
    }
}

static vfs_inode_t* inode_from_entry_locked(const ramfs_entry_t* e){
    RAMFS_LOCKED();
    if(!e) return NULL;
    // [M38] refresh the cached size on every lookup (a prior write may have grown
    // the entry) so e.g. `>>` append seeks to the real end of file; path and type
    // too (a rename changes the entry's name in place).
    vfs_inode_t* ino = NULL;
    for(size_t i=0;i<inode_cache_used;i++){ if(inode_cache[i].fs_data == (void*)e){ ino = &inode_cache[i]; break; } }
    int fresh = 0;
    if(!ino && inode_cache_used < INODE_CACHE_N){
        ino = &inode_cache[inode_cache_used++]; fresh = 1;
    }
    // Cache full: recycle a dead inode. Fresh slots go first so a dead inode an
    // fd may still hold is recycled as late as possible.
    if(!ino) for(size_t i=0;i<inode_cache_used;i++) if(inode_is_dead(&inode_cache[i])){ ino=&inode_cache[i]; fresh=1; break; }
    if(!ino) return NULL;
    // [M43] The cached inode is the ramfs's only copy of mode/uid/gid (chmod and
    // chown land here through ramfs_vfs_setattr). A slot bound to a new entry
    // starts from "no mode, root-owned" -- never with a previous file's owner.
    if(fresh){
        ino->mode = 0; ino->uid = 0; ino->gid = 0; ino->nlink = 1;
        ino->atime = ino->mtime = ino->ctime = 0;
    }
    // Canonical path with a leading '/' (entry names have none). Rewritten only
    // when it changed: lock-free readers of an inode they already hold (procfs
    // fd links, the O_APPEND lookup) then never see a path in mid-rewrite
    // except across a real rename.
    char np[sizeof(ino->path)]; size_t k=0; const char* src=e->name;
    np[k++]='/';
    for(size_t i=0; src[i] && k<sizeof(np)-1; i++) np[k++]=src[i];
    np[k]=0;
    int same = !fresh;
    for(size_t i=0; same && i<=k; i++) if(ino->path[i]!=np[i]) same = 0;
    if(!same) for(size_t i=0;i<=k;i++) ino->path[i]=np[i];
    vfs_node_type_t t = (e->flags & 2)? VFS_NODE_DIR : VFS_NODE_FILE;
    if(fresh || ino->type != t) ino->type = t;
    if(fresh || ino->size != e->size) ino->size = e->size;
    if(fresh) ino->fs_data = (void*)e;
    // Never NULL: another CPU may be calling through this inode's ops right
    // now (the VFS used to restamp it after a lookup reset it to NULL).
    ino->ops = &ramfs_ops;
    return ino;
}

static vfs_inode_t* ramfs_vfs_lookup_locked(const char* path){
    RAMFS_LOCKED();
    if(!path) return NULL;
    if(is_root(path)){ // fabricate the root inode once
        for(size_t i=0;i<inode_cache_used;i++){ if(is_root(inode_cache[i].path)) return &inode_cache[i]; }
        if(inode_cache_used >= INODE_CACHE_N) return NULL;
        vfs_inode_t* root=&inode_cache[inode_cache_used++];
        root->path[0]='/'; root->path[1]=0; root->type=VFS_NODE_DIR; root->size=0; root->fs_data=NULL;
        root->mode=0; root->uid=root->gid=0; root->nlink=1; root->atime=root->mtime=root->ctime=0;
        root->ops=&ramfs_ops;
        return root;
    }
    // ramfs stores names without the leading '/'
    const char* np = path; if(np[0]=='/') np++;
    return inode_from_entry_locked(ramfs_find_locked(np));
}
static vfs_inode_t* ramfs_vfs_lookup(const char* path){
    RLOCK(); vfs_inode_t* r = ramfs_vfs_lookup_locked(path); RUNLOCK(); return r;
}

// One child per lock section; the callback gets a private copy, unlocked.
static int ramfs_vfs_readdir(const char* dir_path, vfs_iter_cb cb, void* user){
    if(!cb) return -1;
    const char* p = (!dir_path || is_root(dir_path)) ? "" : (dir_path[0]=='/' ? dir_path+1 : dir_path);
    size_t cursor = 0;
    for(;;){
        vfs_inode_t copy; int have = 0;
        RLOCK();
        const ramfs_entry_t* e = ramfs_next_child_locked(p, &cursor);
        if(e){ vfs_inode_t* ino = inode_from_entry_locked(e); if(ino){ copy = *ino; have = 1; } }
        RUNLOCK();
        if(!e) break;
        if(have){ copy.ops = NULL; cb(&copy, user); }
    }
    return 0;
}

// An inode whose entry is gone (or that was recycled) has fs_data == NULL;
// read under the lock so a concurrent removal cannot free the data mid-copy.
static int ramfs_vfs_read_locked(vfs_inode_t* inode, size_t offset, void* buf, size_t len){
    RAMFS_LOCKED();
    if(!inode || inode->type!=VFS_NODE_FILE) return -1;
    const ramfs_entry_t* e=(const ramfs_entry_t*)inode->fs_data; if(!e) return -1;
    if(offset>e->size) return -1;
    size_t avail = e->size - offset; if(len>avail) len=avail;
    if(len > 0x7FFFFFFFu) len = 0x7FFFFFFFu;
    const uint8_t* src = e->data + offset;
    for(size_t i=0;i<len;i++) ((uint8_t*)buf)[i]=src[i];
    return (int)len;
}
static int ramfs_vfs_read(vfs_inode_t* inode, size_t offset, void* buf, size_t len){
    RLOCK(); int r = ramfs_vfs_read_locked(inode, offset, buf, len); RUNLOCK(); return r;
}
// [M50 B2] Keep the inode's size in step with the entry: the page cache fills
// (and zero-pads) by inode->size, so a file-mmap fault between a growing write
// and the next lookup cached a page cut at the old size, which no later
// invalidation removed (read() clamps to the same stale size and hid it).
static int ramfs_vfs_write_locked(vfs_inode_t* inode, size_t offset, const void* data, size_t len){
    RAMFS_LOCKED();
    if(!inode || inode->type!=VFS_NODE_FILE) return -1;
    ramfs_entry_t* e=(ramfs_entry_t*)inode->fs_data; if(!e) return -1;
    int r = ramfs_write_entry_locked(e, offset, data, len);
    if(r > 0 && inode->size != e->size) inode->size = e->size;
    return r;
}
static int ramfs_vfs_write(vfs_inode_t* inode, size_t offset, const void* data, size_t len){
    RLOCK(); int r = ramfs_vfs_write_locked(inode, offset, data, len); RUNLOCK(); return r;
}
static int ramfs_vfs_create(const char* path, const void* initial, size_t size){
    if(!path) return -1;
    const char* p = path[0]=='/' ? path+1 : path;
    RLOCK();
    int r = ramfs_find_locked(p) ? -1 : ramfs_add_locked(p, initial, size);
    RUNLOCK();
    return r;
}
static int ramfs_vfs_mkdir(const char* path){
    if(!path) return -1;
    const char* p = path[0]=='/' ? path+1 : path;
    RLOCK(); int r = ramfs_mkdir_locked(p); RUNLOCK(); return r;
}
static int ramfs_remove_fixed_locked(const char* p, int dir){
    const ramfs_entry_t* e=ramfs_find_locked(p); if(!e) return -1;
    int r = dir ? ramfs_rmdir_locked(p) : ramfs_remove_locked(p);
    if(r==0) cache_fix_after_remove(e);
    return r;
}
static int ramfs_vfs_remove(const char* path){
    if(!path) return -1;
    const char* p = path[0]=='/' ? path+1 : path;
    RLOCK(); int r = ramfs_remove_fixed_locked(p, 0); RUNLOCK(); return r;
}
// rename: an existing target (the VFS checked the type rules) is dropped
// first; both steps run in one lock section, so the replacement is atomic.
static int ramfs_vfs_rename(const char* oldp, const char* newp){
    if(!oldp||!newp) return -1;
    const char* o = oldp[0]=='/' ? oldp+1 : oldp;
    const char* n = newp[0]=='/' ? newp+1 : newp;
    RLOCK();
    int r = -1;
    const ramfs_entry_t* src=ramfs_find_locked(o);
    if(src && !(src->flags & 1)){                                  // exists, not immutable
        const ramfs_entry_t* t=ramfs_find_locked(n);
        if(!t || ramfs_remove_fixed_locked(n, (t->flags & 2) ? 1 : 0)==0) r = ramfs_rename_locked(o,n);
    }
    RUNLOCK();
    return r;
}
static int ramfs_vfs_truncate(const char* path, size_t ns){
    if(!path) return -1;
    const char* p = path[0]=='/' ? path+1 : path;
    RLOCK();
    int r = ramfs_truncate_locked(p, ns);
    // Same reason as the write: keep the inode's size current.
    if(r == 0){ const ramfs_entry_t* e = ramfs_find_locked(p);
                for(size_t i=0;e && i<inode_cache_used;i++)
                    if(inode_cache[i].fs_data==(const void*)e && inode_cache[i].size!=e->size) inode_cache[i].size=e->size; }
    RUNLOCK();
    return r;
}

// [M51 C2] chmod/chown/utimes: ramfs keeps no metadata of its own, so this is
// what the VFS fallback did (update the cached inode), but under the lock that
// also rebinds and resets cache slots.
static int ramfs_vfs_setattr(const char* path, const vfs_attr_t* a, unsigned valid){
    if(!path || !a) return -1;
    RLOCK();
    vfs_inode_t* ino = ramfs_vfs_lookup_locked(path);
    if(ino){
        if(valid & VFS_ATTR_MODE){
            uint32_t t = ino->mode & 0170000u;
            if(!t) t = ino->type==VFS_NODE_DIR ? 0040000u : 0100000u;
            ino->mode = t | (a->mode & 07777u);
        }
        if(valid & VFS_ATTR_UID)   ino->uid = a->uid;
        if(valid & VFS_ATTR_GID)   ino->gid = a->gid;
        if(valid & VFS_ATTR_ATIME) ino->atime = a->atime;
        if(valid & VFS_ATTR_MTIME) ino->mtime = a->mtime;
    }
    RUNLOCK();
    return ino ? 0 : -2;                                           // -ENOENT
}

// statfs: like tmpfs, the size is the file data plus the free RAM it can still
// grow into; inodes are the fixed entry table.
static int ramfs_vfs_statfs(vfs_statfs_t* st){
    extern uint64_t pmm_get_free_memory(void);
    static const ramfs_entry_t* all[RAMFS_MAX_FILES];   // guarded by the lock
    uint64_t used = 0;
    RLOCK();
    size_t n = ramfs_list_locked(all, RAMFS_MAX_FILES);
    for(size_t i=0;i<n;i++) used += (all[i]->size + 4095) / 4096;
    RUNLOCK();
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
    .truncate = ramfs_vfs_truncate,
    .setattr = ramfs_vfs_setattr,
};

// Public helper to mount RAMFS into VFS root
int vfs_mount_ramfs(void){ return vfs_mount_root(&ramfs_ops, "ramfs"); }
// The ramfs ops even when another FS is the root (self-tests).
const vfs_fs_ops_t* ramfs_vfs_ops(void){ return &ramfs_ops; }
