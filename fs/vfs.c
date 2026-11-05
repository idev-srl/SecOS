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
#include "spinlock.h"
#include <stddef.h>

#define VFS_MAX_MOUNTS 12   // [M23] root + /mnt + /dev + /proc + /sys + headroom
static vfs_mount_t g_mounts[VFS_MAX_MOUNTS];
static int         g_mount_count = 0;

// [M29] Guards the mount-table array (g_mounts[]/g_mount_count) only — NOT path
// resolution, which calls into filesystems that block on I/O. The lookup helper
// has a _nolock leaf so mount/unmount can hold the lock while using it (the lock
// is not recursive).
static spinlock_t g_mount_lock = SPINLOCK_INIT;

static size_t str_len(const char* s){ size_t n=0; while(s[n]) n++; return n; }

void vfs_init(void){ g_mount_count = 0; for(int i=0;i<VFS_MAX_MOUNTS;i++){ g_mounts[i].mount_point=NULL; g_mounts[i].ops=NULL; g_mounts[i].fs_name=NULL; } }

// Locate/replace the root ("/") mount slot (lock-free leaf — caller holds
// g_mount_lock).
static vfs_mount_t* find_mount_by_point_nolock(const char* mp){
    for(int i=0;i<g_mount_count;i++){ const char* a=g_mounts[i].mount_point; const char* b=mp; size_t k=0; int eq=1; while(a[k]||b[k]){ if(a[k]!=b[k]){ eq=0; break; } k++; } if(eq) return &g_mounts[i]; }
    return NULL;
}

int vfs_mount_root(const vfs_fs_ops_t* ops, const char* fs_name){
    if(!ops || !fs_name) return -1;
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    if(find_mount_by_point_nolock("/")){ spin_unlock_irqrestore(&g_mount_lock, fl); return -1; } // already mounted
    if(g_mount_count >= VFS_MAX_MOUNTS){ spin_unlock_irqrestore(&g_mount_lock, fl); return -1; }
    g_mounts[g_mount_count].mount_point = "/";
    g_mounts[g_mount_count].ops = ops;
    g_mounts[g_mount_count].fs_name = fs_name;
    g_mount_count++;
    spin_unlock_irqrestore(&g_mount_lock, fl);
    return 0;
}

int vfs_replace_root(const vfs_fs_ops_t* ops, const char* fs_name){
    if(!ops || !fs_name) return -1;
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    vfs_mount_t* r = find_mount_by_point_nolock("/");
    if(r){ r->ops = ops; r->fs_name = fs_name; spin_unlock_irqrestore(&g_mount_lock, fl); return 0; }
    spin_unlock_irqrestore(&g_mount_lock, fl);
    // vfs_mount_root takes g_mount_lock itself — call it OUTSIDE the lock (not
    // recursive). It re-checks for an existing root, so the drop/re-take is safe.
    return vfs_mount_root(ops, fs_name);
}

// Mount a filesystem at an arbitrary absolute mount point (e.g. "/mnt").
int vfs_mount(const char* mount_point, const vfs_fs_ops_t* ops, const char* fs_name){
    if(!mount_point || !ops || !fs_name) return -1;
    if(mount_point[0] != '/') return -1;
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    if(find_mount_by_point_nolock(mount_point)){ spin_unlock_irqrestore(&g_mount_lock, fl); return -1; }
    if(g_mount_count >= VFS_MAX_MOUNTS){ spin_unlock_irqrestore(&g_mount_lock, fl); return -1; }
    g_mounts[g_mount_count].mount_point = mount_point;
    g_mounts[g_mount_count].ops = ops;
    g_mounts[g_mount_count].fs_name = fs_name;
    g_mount_count++;
    spin_unlock_irqrestore(&g_mount_lock, fl);
    return 0;
}

int vfs_unmount(const char* mount_point){
    if(!mount_point) return -1;
    // Never detach the root filesystem: every path resolves through it, so a
    // umount("/") (found by the syscall fuzzer) left an empty namespace — no
    // /bin, no /dev — and every later exec/stat failed for the whole system.
    if(mount_point[0]=='/' && mount_point[1]==0) return -1;
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    for(int i=0;i<g_mount_count;i++){
        const char* a=g_mounts[i].mount_point; const char* b=mount_point; size_t k=0; int eq=1;
        while(a[k]||b[k]){ if(a[k]!=b[k]){ eq=0; break; } k++; }
        if(eq){ for(int j=i;j<g_mount_count-1;j++) g_mounts[j]=g_mounts[j+1]; g_mount_count--; spin_unlock_irqrestore(&g_mount_lock, fl); return 0; }
    }
    spin_unlock_irqrestore(&g_mount_lock, fl);
    return -1;
}

// Orderly shutdown: flush every mounted filesystem. Disk FSes are write-through
// today (block drivers are polled/synchronous; ext2/ext4 commits + checkpoints
// each metadata transaction), so ->sync only has to close anything still open.
// The sync callbacks run without g_mount_lock (they do block I/O).
int vfs_sync_all(void){
    const vfs_fs_ops_t* ops[VFS_MAX_MOUNTS]; int n = 0, done = 0;
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    for(int i=0;i<g_mount_count;i++){
        int dup = 0;                                 // one FS instance may be mounted twice
        for(int j=0;j<n;j++) if(ops[j]==g_mounts[i].ops) dup = 1;
        if(!dup) ops[n++] = g_mounts[i].ops;
    }
    spin_unlock_irqrestore(&g_mount_lock, fl);
    for(int i=0;i<n;i++) if(ops[i]->sync && ops[i]->sync()==0) done++;
    return done;
}

int vfs_unmount_all(void){
    int n = 0;
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    int w = 0;
    for(int i=0;i<g_mount_count;i++){
        const char* mp = g_mounts[i].mount_point;
        if(mp[0]=='/' && mp[1]==0) g_mounts[w++] = g_mounts[i];   // keep the root
        else n++;
    }
    g_mount_count = w;
    spin_unlock_irqrestore(&g_mount_lock, fl);
    return n;
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

int vfs_walk(const char* path, char* out, size_t outsz, int follow_final);
int vfs_readdir(const char* path, vfs_iter_cb cb, void* user){
    if(!cb) return -1;
    char rel[256], cp[256];
    // A directory reached through a symlink (/proc/self, a linked dir, ...):
    // list the canonical target. Plain directories keep their exact path.
    vfs_inode_t* d = vfs_lookup(path);
    if((!d || d->type!=VFS_NODE_DIR) && path && path[0]=='/' && vfs_walk(path, cp, sizeof(cp), 1)==0) path = cp;
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

// ---- Path walking: symlinks in any component --------------------------------
// vfs_walk() turns an absolute path into the canonical path of the object it
// names: "." / ".." / "//" collapse, and every symlink met on the way is
// replaced by its target (absolute targets restart at "/", relative ones are
// resolved against the directory holding the link). The final component is
// followed only when follow_final is set (stat/open vs lstat/unlink/rename).
// A nonexistent final component is fine (create, mkdir, rename target); a
// nonexistent or non-directory intermediate one is not. Bounded at VFS_MAXHOPS
// symlinks per resolution, like Linux's MAXSYMLINKS -> -ELOOP.
#define VFS_MAXHOPS 40
#define VFS_PATH    256

static int vfs_copy(char* d, const char* s, size_t cap){
    size_t i=0; for(; s[i]; i++){ if(i>=cap-1){ d[i]=0; return -1; } d[i]=s[i]; } d[i]=0; return 0;
}

int vfs_walk(const char* path, char* out, size_t outsz, int follow_final){
    char todo[VFS_PATH];      // what is still to be resolved (absolute)
    char res[VFS_PATH];       // canonical prefix resolved so far ("" = root)
    if(!path || path[0]!='/') return -2;                       // ENOENT
    if(vfs_copy(todo, path, sizeof(todo))!=0) return -36;      // ENAMETOOLONG
    int hops = 0;
restart:
    res[0]=0;
    {
        size_t rl=0, i=0;
        while(todo[i]){
            while(todo[i]=='/') i++;
            if(!todo[i]) break;
            size_t cs=i; while(todo[i] && todo[i]!='/') i++;
            size_t cl=i-cs;
            while(todo[i]=='/') i++;
            int last = (todo[i]==0);
            if(cl==1 && todo[cs]=='.') continue;
            if(cl==2 && todo[cs]=='.' && todo[cs+1]=='.'){       // physical parent
                while(rl>0 && res[rl-1]!='/') rl--;
                if(rl>0) rl--;
                res[rl]=0;
                continue;
            }
            if(rl+1+cl >= sizeof(res)) return -36;
            size_t base=rl;
            res[rl++]='/'; for(size_t k=0;k<cl;k++) res[rl++]=todo[cs+k]; res[rl]=0;
            if(last && !follow_final) break;
            vfs_inode_t* ino = vfs_lookup(res);
            if(!ino){
                if(last) break;                                  // to be created
                return -2;                                       // ENOENT
            }
            if(ino->type==VFS_NODE_SYMLINK){
                if(++hops > VFS_MAXHOPS) return -40;             // ELOOP
                char tgt[VFS_PATH];
                int n = vfs_readlink(res, tgt, sizeof(tgt));
                if(n<=0) return -2;
                char nt[VFS_PATH]; size_t o=0;
                if(tgt[0]!='/'){                                 // relative: link's directory
                    for(size_t k=0;k<base;k++){ if(o>=sizeof(nt)-1) return -36; nt[o++]=res[k]; }
                    if(o>=sizeof(nt)-1) return -36;
                    nt[o++]='/';
                }
                for(size_t k=0; tgt[k]; k++){ if(o>=sizeof(nt)-1) return -36; nt[o++]=tgt[k]; }
                if(todo[i]){                                     // the rest of the path
                    if(o>=sizeof(nt)-1) return -36;
                    nt[o++]='/';
                    for(size_t k=i; todo[k]; k++){ if(o>=sizeof(nt)-1) return -36; nt[o++]=todo[k]; }
                }
                nt[o]=0;
                vfs_copy(todo, nt, sizeof(todo));
                goto restart;
            }
            if(!last && ino->type!=VFS_NODE_DIR) return -20;     // ENOTDIR
        }
    }
    if(res[0]==0){ res[0]='/'; res[1]=0; }
    if(vfs_copy(out, res, outsz)!=0) return -36;
    return 0;
}

// The path's parent directory as a string ("/" for top-level entries).
static void vfs_parent_of(const char* p, char* out, size_t cap){
    size_t ls=0; for(size_t i=0;p[i];i++) if(p[i]=='/') ls=i;
    if(ls==0){ out[0]='/'; out[1]=0; return; }
    if(ls>cap-1) ls=cap-1;
    for(size_t i=0;i<ls;i++) out[i]=p[i];
    out[ls]=0;
}

// Canonical path for an operation on the entry `path` names (intermediate
// symlinks followed, the final component not). Fast path: when the parent
// already resolves to a real directory the path is used as is, so the common
// case costs one lookup and filesystems keep their exact historical paths.
static int vfs_canon_entry(const char* path, char* out, size_t cap){
    if(!path || path[0]!='/') return -2;
    char par[VFS_PATH]; vfs_parent_of(path, par, sizeof(par));
    vfs_inode_t* pi = vfs_lookup(par);
    int dots = 0;
    for(size_t i=0; path[i]; i++)
        if(path[i]=='.' && (i==0 || path[i-1]=='/') && (path[i+1]==0 || path[i+1]=='/' ||
           (path[i+1]=='.' && (path[i+2]==0 || path[i+2]=='/')))) dots = 1;
    if(pi && pi->type==VFS_NODE_DIR && !dots) return vfs_copy(out, path, cap)==0 ? 0 : -36;
    return vfs_walk(path, out, cap, 0);
}

vfs_inode_t* vfs_lookup_ex(const char* path, int follow, int* err){
    if(err) *err = 0;
    vfs_inode_t* ino = vfs_lookup(path);
    if(ino && (!follow || ino->type!=VFS_NODE_SYMLINK)) return ino;  // fast path
    char cp[VFS_PATH];
    int r = vfs_walk(path, cp, sizeof(cp), follow);
    if(r!=0){ if(err) *err = r; return NULL; }
    ino = vfs_lookup(cp);
    if(!ino && err) *err = -2;
    return ino;
}
// [M26] Lookup following symlinks (final component included). Used by stat/open.
vfs_inode_t* vfs_lookup_follow(const char* path){ return vfs_lookup_ex(path, 1, NULL); }

int vfs_create(const char* path, const void* data, size_t size){
    char cp[VFS_PATH]; int e=vfs_canon_entry(path, cp, sizeof(cp)); if(e) return e;
    char rel[256]; vfs_mount_t* m = vfs_resolve(cp, rel, sizeof(rel));
    if(!m || !m->ops) return -1;
    return m->ops->create(rel, data, size);
}
int vfs_write(const char* path, size_t offset, const void* data, size_t len){
    vfs_inode_t* ino = vfs_lookup_follow(path);
    if(!ino || ino->type!=VFS_NODE_FILE || !ino->ops) return -1;
    return ino->ops->write(ino, offset, data, len);
}
int vfs_mkdir(const char* path){
    char cp[VFS_PATH]; int e=vfs_canon_entry(path, cp, sizeof(cp)); if(e) return e;
    char rel[256]; vfs_mount_t* m=vfs_resolve(cp,rel,sizeof(rel)); if(!m||!m->ops) return -1; return m->ops->mkdir(rel);
}
int vfs_remove(const char* path){
    char cp[VFS_PATH]; int e=vfs_canon_entry(path, cp, sizeof(cp)); if(e) return e;
    char rel[256]; vfs_mount_t* m=vfs_resolve(cp,rel,sizeof(rel)); if(!m||!m->ops) return -1; return m->ops->remove(rel);
}

static int vfs_is_mount_point(const char* p){
    for(int i=0;i<g_mount_count;i++){ const char* a=g_mounts[i].mount_point; size_t k=0; int eq=1; while(a[k]||p[k]){ if(a[k]!=p[k]){ eq=0; break; } k++; } if(eq) return 1; }
    return 0;
}
struct vfs_count_ctx { int n; };
static void vfs_count_cb(const vfs_inode_t* c, void* u){ (void)c; ((struct vfs_count_ctx*)u)->n++; }

// rmdir: the entry must be an empty directory that is not a mount point.
int vfs_rmdir(const char* path){
    char cp[VFS_PATH]; int e=vfs_canon_entry(path, cp, sizeof(cp)); if(e) return e;
    vfs_inode_t* ino = vfs_lookup(cp);
    if(!ino) return -2;                                   // ENOENT
    if(ino->type!=VFS_NODE_DIR) return -20;               // ENOTDIR
    if(vfs_is_mount_point(cp)) return -16;                // EBUSY (incl. "/")
    struct vfs_count_ctx c = { 0 };
    if(vfs_readdir(cp, vfs_count_cb, &c)!=0) return -5;   // EIO
    if(c.n) return -39;                                   // ENOTEMPTY
    char rel[256]; vfs_mount_t* m=vfs_resolve(cp,rel,sizeof(rel)); if(!m||!m->ops) return -1;
    return m->ops->remove(rel) == 0 ? 0 : -5;
}

// rename: atomic within one filesystem (the FS replaces an existing target in
// one step — on ext2/ext4 inside one journalled transaction); -EXDEV across
// mounts. POSIX type rules are checked here so every FS gets them.
int vfs_rename(const char* oldp, const char* newp){
    char co[VFS_PATH], cn[VFS_PATH]; int e;
    if((e=vfs_canon_entry(oldp, co, sizeof(co)))) return e;
    if((e=vfs_canon_entry(newp, cn, sizeof(cn)))) return e;
    char ro[256], rn[256];
    vfs_mount_t* mo=vfs_resolve(co,ro,sizeof(ro));
    vfs_mount_t* mn=vfs_resolve(cn,rn,sizeof(rn));
    if(!mo||!mn||!mo->ops) return -2;
    vfs_inode_t* src = vfs_lookup(co);
    if(!src) return -2;                                   // ENOENT
    int src_dir = (src->type==VFS_NODE_DIR);
    if(mo!=mn) return -18;                                // EXDEV
    if(vfs_is_mount_point(co) || vfs_is_mount_point(cn)) return -16;   // EBUSY
    size_t ol=str_len(co);
    { size_t k=0; while(k<ol && co[k]==cn[k]) k++;
      if(k==ol && cn[ol]==0) return 0;                    // same entry: no-op
      if(src_dir && k==ol && cn[ol]=='/') return -22; }   // EINVAL: into itself
    vfs_inode_t* dst = vfs_lookup(cn);
    if(dst){
        int dst_dir = (dst->type==VFS_NODE_DIR);
        if(src_dir && !dst_dir) return -20;               // ENOTDIR
        if(!src_dir && dst_dir) return -21;               // EISDIR
        if(dst_dir){
            struct vfs_count_ctx c = { 0 };
            vfs_readdir(cn, vfs_count_cb, &c);
            if(c.n) return -39;                           // ENOTEMPTY
        }
    }
    if(!mo->ops->rename) return -1;
    return mo->ops->rename(ro, rn) == 0 ? 0 : -5;         // EIO
}

// link: a new name for an existing non-directory, same filesystem only.
int vfs_link(const char* oldp, const char* newp){
    char co[VFS_PATH], cn[VFS_PATH]; int e;
    if((e=vfs_canon_entry(oldp, co, sizeof(co)))) return e;
    if((e=vfs_canon_entry(newp, cn, sizeof(cn)))) return e;
    vfs_inode_t* src = vfs_lookup(co);
    if(!src) return -2;                                   // ENOENT
    if(src->type==VFS_NODE_DIR) return -1;                // EPERM
    if(vfs_lookup(cn)) return -17;                        // EEXIST
    char ro[256], rn[256];
    vfs_mount_t* mo=vfs_resolve(co,ro,sizeof(ro));
    vfs_mount_t* mn=vfs_resolve(cn,rn,sizeof(rn));
    if(!mo||!mn) return -2;
    if(mo!=mn) return -18;                                // EXDEV
    if(!mo->ops->link) return -1;                         // EPERM: FS has no hard links
    return mo->ops->link(ro, rn) == 0 ? 0 : -5;
}

int vfs_truncate(const char* path, size_t new_size){
    char cp[VFS_PATH]; int e;
    vfs_inode_t* ino = vfs_lookup_ex(path, 1, &e);        // truncate() follows symlinks
    if(!ino) return e ? e : -2;
    if(ino->type==VFS_NODE_DIR) return -21;               // EISDIR
    if((e=vfs_walk(path, cp, sizeof(cp), 1))) return e;
    char rel[256]; vfs_mount_t* m=vfs_resolve(cp,rel,sizeof(rel)); if(!m||!m->ops||!m->ops->truncate) return -1;
    return m->ops->truncate(rel, new_size);
}

// [M26] Attribute / symlink routing (NULL-checked: FSes that don't implement the
// op return -1 instead of crashing — chmod on a read-only virtual FS is a no-op).
int vfs_setattr(const char* path, const vfs_attr_t* a, unsigned valid){
    char cp[VFS_PATH]; int e=vfs_walk(path, cp, sizeof(cp), 1); if(e) return e;   // chmod follows
    char rel[256]; vfs_mount_t* m=vfs_resolve(cp,rel,sizeof(rel)); if(!m||!m->ops) return -1;
    if(m->ops->setattr) return m->ops->setattr(rel, a, valid);
    // A filesystem with no on-disk metadata (ramfs, devfs incl. /dev/pts, the
    // virtual ones): keep the change on the in-memory inode, like devtmpfs, so
    // chmod/chown succeed (sshd/login chmod the pty slave) and stat shows them.
    vfs_inode_t* ino=vfs_lookup_follow(cp); if(!ino) return -2;
    if(valid & VFS_ATTR_MODE){
        uint32_t t = ino->mode & 0170000u;
        if(!t) t = ino->type==VFS_NODE_DIR ? 0040000u : ino->type==VFS_NODE_SYMLINK ? 0120000u : 0100000u;
        ino->mode = t | (a->mode & 07777u);
    }
    if(valid & VFS_ATTR_UID)   ino->uid = a->uid;
    if(valid & VFS_ATTR_GID)   ino->gid = a->gid;
    if(valid & VFS_ATTR_ATIME) ino->atime = a->atime;
    if(valid & VFS_ATTR_MTIME) ino->mtime = a->mtime;
    return 0;
}
int vfs_readlink(const char* path, char* buf, size_t len){
    char cp[VFS_PATH];
    if(vfs_canon_entry(path, cp, sizeof(cp))!=0) return -1;
    char rel[256]; vfs_mount_t* m=vfs_resolve(cp,rel,sizeof(rel)); if(!m||!m->ops||!m->ops->readlink) return -1; return m->ops->readlink(rel, buf, len);
}
int vfs_symlink(const char* target, const char* linkpath){
    char cp[VFS_PATH]; int e=vfs_canon_entry(linkpath, cp, sizeof(cp)); if(e) return e;
    char rel[256]; vfs_mount_t* m=vfs_resolve(cp,rel,sizeof(rel)); if(!m||!m->ops||!m->ops->symlink) return -1; return m->ops->symlink(target, rel);
}

// ---- statfs ---------------------------------------------------------------
// Linux f_type magics for the filesystems that carry no ->statfs of their own.
static uint64_t vfs_magic_by_name(const char* n){
    struct { const char* n; uint64_t m; } t[] = {
        {"ramfs", 0x858458f6}, {"procfs", 0x9fa0}, {"devfs", 0x01021994 /* tmpfs, as devtmpfs */},
        {"sysfs", 0x62656572}, {"fat32", 0x4d44}, {"ext2", 0xEF53},
    };
    for(unsigned i=0;i<sizeof(t)/sizeof(t[0]);i++){
        size_t k=0; while(t[i].n[k] && n[k]==t[i].n[k]) k++;
        if(!t[i].n[k] && (!n[k] || n[k]=='/' || n[k]=='-' || n[k]=='(')) return t[i].m;
    }
    return 0;
}
static int vfs_statfs_mount(vfs_mount_t* m, vfs_statfs_t* st){
    for(size_t i=0;i<sizeof(*st);i++) ((uint8_t*)st)[i]=0;
    st->namelen = 255; st->bsize = 4096; st->frsize = 4096;
    int r = (m->ops->statfs) ? m->ops->statfs(st) : 0;
    if(!st->type) st->type = vfs_magic_by_name(m->fs_name ? m->fs_name : "");
    if(!st->frsize) st->frsize = st->bsize;
    return r;
}
int vfs_statfs(const char* path, vfs_statfs_t* st){
    int e; vfs_inode_t* ino = vfs_lookup_ex(path, 1, &e);
    if(!ino) return e ? e : -2;
    char cp[VFS_PATH]; if((e=vfs_walk(path, cp, sizeof(cp), 1))) return e;
    char rel[256]; vfs_mount_t* m=vfs_resolve(cp,rel,sizeof(rel)); if(!m||!m->ops) return -2;
    return vfs_statfs_mount(m, st);
}
int vfs_statfs_ops(const vfs_fs_ops_t* ops, vfs_statfs_t* st){
    for(int i=0;i<g_mount_count;i++) if(g_mounts[i].ops==ops) return vfs_statfs_mount(&g_mounts[i], st);
    return -2;
}
// Mount point of the filesystem behind `ops` (the longest one when an FS is
// mounted twice), or NULL. Lets inode paths (mount-relative) be made absolute.
const char* vfs_mount_point_of(const vfs_fs_ops_t* ops){
    const char* best=NULL; size_t bl=0;
    for(int i=0;i<g_mount_count;i++) if(g_mounts[i].ops==ops){ size_t l=str_len(g_mounts[i].mount_point); if(!best||l>bl){ best=g_mounts[i].mount_point; bl=l; } }
    return best;
}
// Sync one filesystem (fsync/fdatasync of a file on it). 0 also when the FS is
// write-through (no ->sync).
int vfs_sync_ops(const vfs_fs_ops_t* ops){
    if(ops && ops->sync) return ops->sync();
    return 0;
}

int vfs_mount_count(void){
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    int n = g_mount_count;
    spin_unlock_irqrestore(&g_mount_lock, fl);
    return n;
}
int vfs_mount_info(int i, const char** mp, const char** fsname){
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    if(i<0 || i>=g_mount_count){ spin_unlock_irqrestore(&g_mount_lock, fl); return -1; }
    if(mp) *mp = g_mounts[i].mount_point;
    if(fsname) *fsname = g_mounts[i].fs_name;
    spin_unlock_irqrestore(&g_mount_lock, fl);
    return 0;
}
// Operations of the i-th mount (for /proc/mounts' device + type), or NULL.
const vfs_fs_ops_t* vfs_mount_ops_at(int i){
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    const vfs_fs_ops_t* o = (i>=0 && i<g_mount_count) ? g_mounts[i].ops : NULL;
    spin_unlock_irqrestore(&g_mount_lock, fl);
    return o;
}
