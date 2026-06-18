/*
 * SecOS Kernel - VFS Core
 * Virtual filesystem layer supporting a root mount plus additional mount
 * points (e.g. a disk FS at /mnt). Paths are routed to the owning mount by
 * longest-prefix match; the mount-relative path (rooted at '/') is handed to
 * the underlying filesystem ops.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "vfs.h"
#include <stddef.h>

#define VFS_MAX_MOUNTS 12   // [M23] root + /mnt + /dev + /proc + /sys + headroom
static vfs_mount_t g_mounts[VFS_MAX_MOUNTS];
static int         g_mount_count = 0;

static size_t str_len(const char* s){ size_t n=0; while(s[n]) n++; return n; }

void vfs_init(void){ g_mount_count = 0; for(int i=0;i<VFS_MAX_MOUNTS;i++){ g_mounts[i].mount_point=NULL; g_mounts[i].ops=NULL; g_mounts[i].fs_name=NULL; } }

// Locate/replace the root ("/") mount slot.
static vfs_mount_t* find_mount_by_point(const char* mp){
    for(int i=0;i<g_mount_count;i++){ const char* a=g_mounts[i].mount_point; const char* b=mp; size_t k=0; int eq=1; while(a[k]||b[k]){ if(a[k]!=b[k]){ eq=0; break; } k++; } if(eq) return &g_mounts[i]; }
    return NULL;
}

int vfs_mount_root(const vfs_fs_ops_t* ops, const char* fs_name){
    if(!ops || !fs_name) return -1;
    if(find_mount_by_point("/")) return -1; // already mounted
    if(g_mount_count >= VFS_MAX_MOUNTS) return -1;
    g_mounts[g_mount_count].mount_point = "/";
    g_mounts[g_mount_count].ops = ops;
    g_mounts[g_mount_count].fs_name = fs_name;
    g_mount_count++;
    return 0;
}

int vfs_replace_root(const vfs_fs_ops_t* ops, const char* fs_name){
    if(!ops || !fs_name) return -1;
    vfs_mount_t* r = find_mount_by_point("/");
    if(r){ r->ops = ops; r->fs_name = fs_name; return 0; }
    return vfs_mount_root(ops, fs_name);
}

// Mount a filesystem at an arbitrary absolute mount point (e.g. "/mnt").
int vfs_mount(const char* mount_point, const vfs_fs_ops_t* ops, const char* fs_name){
    if(!mount_point || !ops || !fs_name) return -1;
    if(mount_point[0] != '/') return -1;
    if(find_mount_by_point(mount_point)) return -1;
    if(g_mount_count >= VFS_MAX_MOUNTS) return -1;
    g_mounts[g_mount_count].mount_point = mount_point;
    g_mounts[g_mount_count].ops = ops;
    g_mounts[g_mount_count].fs_name = fs_name;
    g_mount_count++;
    return 0;
}

int vfs_unmount(const char* mount_point){
    if(!mount_point) return -1;
    for(int i=0;i<g_mount_count;i++){
        const char* a=g_mounts[i].mount_point; const char* b=mount_point; size_t k=0; int eq=1;
        while(a[k]||b[k]){ if(a[k]!=b[k]){ eq=0; break; } k++; }
        if(eq){ for(int j=i;j<g_mount_count-1;j++) g_mounts[j]=g_mounts[j+1]; g_mount_count--; return 0; }
    }
    return -1;
}

// Resolve an absolute path to its owning mount and the mount-relative path.
// rel (size rel_sz) receives the relative path rooted at '/'. Returns the
// mount, or NULL if none matches.
static vfs_mount_t* vfs_resolve(const char* path, char* rel, size_t rel_sz){
    if(!path || !*path) path = "/";
    vfs_mount_t* best = NULL; size_t best_len = 0;
    for(int i=0;i<g_mount_count;i++){
        const char* mp = g_mounts[i].mount_point;
        size_t mlen = str_len(mp);
        // Compare prefix.
        size_t k=0; int pref=1;
        for(k=0;k<mlen;k++){ if(path[k]!=mp[k]){ pref=0; break; } }
        if(!pref) continue;
        // Boundary: root "/" matches anything; otherwise next char must end or be '/'.
        if(mlen==1 && mp[0]=='/'){
            if(best_len==0){ best=&g_mounts[i]; best_len=1; } // root is the weakest match
            continue;
        }
        char after = path[mlen];
        if(after!=0 && after!='/') continue;
        if(mlen > best_len){ best=&g_mounts[i]; best_len=mlen; }
    }
    if(!best) return NULL;
    // Build the relative path.
    const char* mp = best->mount_point; size_t mlen = str_len(mp);
    const char* tail;
    if(mlen==1 && mp[0]=='/') tail = path;           // root: relative == absolute
    else tail = path + mlen;                          // strip "/mnt"
    if(tail[0]==0){ if(rel_sz>=2){ rel[0]='/'; rel[1]=0; } }
    else {
        size_t j=0; if(tail[0] != '/' && j<rel_sz-1) rel[j++]='/';
        for(size_t i=0; tail[i] && j<rel_sz-1; i++) rel[j++]=tail[i];
        rel[j]=0;
    }
    return best;
}

vfs_inode_t* vfs_lookup(const char* path){
    char rel[256];
    vfs_mount_t* m = vfs_resolve(path, rel, sizeof(rel));
    if(!m || !m->ops) return NULL;
    vfs_inode_t* ino = m->ops->lookup(rel);
    if(ino) ino->ops = m->ops; // stamp owning ops for subsequent read/write
    return ino;
}

int vfs_readdir(const char* path, vfs_iter_cb cb, void* user){
    if(!cb) return -1;
    char rel[256];
    vfs_mount_t* m = vfs_resolve(path, rel, sizeof(rel));
    if(!m || !m->ops) return -1;
    return m->ops->readdir(rel, cb, user);
}

int vfs_read_all(const char* path, void* buf, size_t bufsize){
    vfs_inode_t* ino = vfs_lookup(path);
    if(!ino || ino->type!=VFS_NODE_FILE || !ino->ops) return -1;
    if(ino->size > bufsize) return -1;
    int r = ino->ops->read(ino, 0, buf, ino->size);
    return (r>=0)? (int)ino->size : -1;
}

int vfs_create(const char* path, const void* data, size_t size){
    char rel[256]; vfs_mount_t* m = vfs_resolve(path, rel, sizeof(rel));
    if(!m || !m->ops) return -1; return m->ops->create(rel, data, size);
}
int vfs_write(const char* path, size_t offset, const void* data, size_t len){
    vfs_inode_t* ino = vfs_lookup(path);
    if(!ino || ino->type!=VFS_NODE_FILE || !ino->ops) return -1;
    return ino->ops->write(ino, offset, data, len);
}
int vfs_mkdir(const char* path){ char rel[256]; vfs_mount_t* m=vfs_resolve(path,rel,sizeof(rel)); if(!m||!m->ops) return -1; return m->ops->mkdir(rel); }
int vfs_remove(const char* path){ char rel[256]; vfs_mount_t* m=vfs_resolve(path,rel,sizeof(rel)); if(!m||!m->ops) return -1; return m->ops->remove(rel); }
int vfs_rename(const char* oldp, const char* newp){
    char ro[256], rn[256];
    vfs_mount_t* mo=vfs_resolve(oldp,ro,sizeof(ro));
    vfs_mount_t* mn=vfs_resolve(newp,rn,sizeof(rn));
    if(!mo||!mn||mo!=mn||!mo->ops) return -1; // same-FS rename only
    return mo->ops->rename(ro, rn);
}
int vfs_truncate(const char* path, size_t new_size){ char rel[256]; vfs_mount_t* m=vfs_resolve(path,rel,sizeof(rel)); if(!m||!m->ops) return -1; return m->ops->truncate(rel, new_size); }

int vfs_mount_count(void){ return g_mount_count; }
int vfs_mount_info(int i, const char** mp, const char** fsname){
    if(i<0 || i>=g_mount_count) return -1;
    if(mp) *mp = g_mounts[i].mount_point;
    if(fsname) *fsname = g_mounts[i].fs_name;
    return 0;
}
