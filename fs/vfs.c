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
#include "klock.h"
#include "pagecache.h"
#include "debugcon.h"
#include <stddef.h>

extern uint64_t ktime_ms(void);

#define VFS_MAX_MOUNTS 12   // [M23] root + /mnt + /dev + /proc + /sys + headroom
static vfs_mount_t g_mounts[VFS_MAX_MOUNTS];
static int         g_mount_count = 0;

// [M29] Guards the mount-table array (g_mounts[]/g_mount_count) and the
// per-slot state below — NOT path resolution, which calls into filesystems
// that do I/O. Never held across a filesystem call. The lookup helper has a
// _nolock leaf so mount/unmount can hold the lock while using it (the lock is
// not recursive). S20: may be taken while holding an FS-instance lock (K30)
// — drivers ask vfs_mount_point_of() from inside their ops.
static spinlock_t g_mount_lock = SPINLOCK_INIT_CLASS(LK_S20_MOUNT, "mount");

// [M51 C1] vfs_ns_lock (K20): serializes the namespace changes that do
// filesystem I/O — mount (the driver's probe + vfs_mount), umount (drain +
// sync + detach), remount, replace_root. Taken by the mount/umount syscalls
// before the driver's mount routine runs, never by vfs_mount() itself (a
// driver calls that from inside its mount routine, possibly holding its K30
// lock: K30 -> K20 would invert the order). vfs_unmount()/vfs_unmount_all()
// take it themselves when their caller does not hold it.
static klock_t g_vfs_ns_lock = KLOCK_INIT(LK_K20_VFS_NS, "vfs_ns");

// [M51 C1] vfs_append_lock (K25): the atomic O_APPEND fallback for a
// filesystem without ->append (see vfs_append). Covers "read the current
// end of file, write there" and the truncates that could move that end.
static klock_t g_vfs_append_lock = KLOCK_INIT(LK_K25_VFS_APPEND, "vfs_append");

void vfs_ns_lock(void)   { klock_lock(&g_vfs_ns_lock); }
void vfs_ns_unlock(void) { klock_unlock(&g_vfs_ns_lock); }
int  vfs_ns_held(void)   { return klock_held(&g_vfs_ns_lock); }

static size_t str_len(const char* s){ size_t n=0; while(s[n]) n++; return n; }

// Mount-point strings are owned by the VFS. Callers pass whatever they have:
// the mount syscall a kernel-stack copy of the user string, the shell its line
// buffer. That pointer used to be stored verbatim, so a userland
// `mount -t vfat|ext2 ... /x` left the table entry pointing at dead stack.
// vfs_mount() now copies the path into one of these slots and vfs_unmount()
// releases it. The slots are static, never freed memory, so a pointer handed
// out by vfs_mount_info()/vfs_mount_point_of() stays a valid string even if it
// races an unmount; it changes only when a later mount reuses the slot.
//
// [M51 C1] Each mount (the root included) owns one slot id (`sid`) for its
// lifetime: the mount-point string, the in-flight operation count and the
// draining flag live there. g_mounts[] is kept dense and shifts on unmount,
// so nothing that must stay put across an unlock may point into it.
#define VFS_MP_MAX 256
static char              g_mp_store[VFS_MAX_MOUNTS][VFS_MP_MAX];
static uint8_t           g_mp_used[VFS_MAX_MOUNTS];
static volatile uint32_t g_sid_active[VFS_MAX_MOUNTS];   // ops in flight (refs)
static volatile uint8_t  g_sid_draining[VFS_MAX_MOUNTS]; // umount in progress

static int sid_get_nolock(void){
    for(int i=0;i<VFS_MAX_MOUNTS;i++) if(!g_mp_used[i]){
        g_mp_used[i]=1; g_sid_active[i]=0; g_sid_draining[i]=0; return i;
    }
    return -1;
}
static void sid_put_nolock(int sid){ if(sid>=0 && sid<VFS_MAX_MOUNTS) g_mp_used[sid]=0; }

// Device ids (st_dev). A block-backed mount gets the Linux dev_t of its device
// (vda=253:0, sda=8:0, sdb1=8:17, nvme0n1=259:0, ...), stable across boots and
// equal to what a Linux host would report. A virtual FS (ramfs, procfs, devfs,
// sysfs) gets an anonymous 0:N, N counted per mount like Linux's anon bdevs.
// Encoded like glibc makedev(), so major()/minor() in userland decode it.
static uint64_t g_anon_minor = 16;     // 0:1/0:2 are the console/socket pseudo-devs
static uint64_t vfs_makedev(uint64_t ma, uint64_t mi){
    return ((ma & 0xfffULL) << 8) | ((ma & ~0xfffULL) << 32) | (mi & 0xffULL) | ((mi & ~0xffULL) << 12);
}
static uint64_t vfs_part_suffix(const char* t, uint64_t* part){
    uint64_t v = 0;
    if(!*t){ *part = 0; return 1; }
    for(; *t; t++){ if(*t<'0' || *t>'9') return 0; v = v*10 + (uint64_t)(*t-'0'); }
    *part = v; return v < 16;
}
uint64_t vfs_blkdev_id(const char* n){
    if(!n || !n[0]) return 0;
    uint64_t part = 0;
    if((n[0]=='v' || n[0]=='s') && n[1]=='d' && n[2]>='a' && n[2]<='z'){
        uint64_t ma = (n[0]=='v') ? 253 : 8;
        if(vfs_part_suffix(n+3, &part)) return vfs_makedev(ma, (uint64_t)(n[2]-'a')*16 + part);
    } else if(n[0]=='n' && n[1]=='v' && n[2]=='m' && n[3]=='e'){
        // nvme<c>n<ns>[p<k>] -> 259:((c*16+ns-1)*16+k): deterministic (Linux
        // allocates these minors dynamically)
        const char* q = n + 4; uint64_t c = 0, ns = 0;
        while(*q>='0' && *q<='9') c = c*10 + (uint64_t)(*q++ - '0');
        if(*q=='n'){ q++; while(*q>='0' && *q<='9') ns = ns*10 + (uint64_t)(*q++ - '0'); }
        if(*q=='p') q++;
        if(ns && vfs_part_suffix(q, &part)) return vfs_makedev(259, ((c*16 + ns-1)*16 + part) & 0xfffff);
    } else if(n[0]=='u' && n[1]=='s' && n[2]=='b' && n[3]>='0' && n[3]<='9'){
        // USB mass storage is sd* on Linux: sdi.. here, clear of AHCI's sda..sdd
        const char* q = n + 4; if(*q=='p') q++;
        if(vfs_part_suffix(q, &part)) return vfs_makedev(8, (uint64_t)(n[3]-'0' + 8)*16 + part);
    }
    // Other names: a stable hash under a local/experimental major.
    uint32_t h = 2166136261u; for(const char* q=n; *q; q++){ h ^= (uint8_t)*q; h *= 16777619u; }
    return vfs_makedev(240, h & 0xfffff);
}
// st_dev for a new mount of `ops`: derived from its block device (->mntinfo
// names it), else 0 = take an anonymous id. Called without g_mount_lock.
static uint64_t vfs_dev_for(const vfs_fs_ops_t* ops){
    const char* src = 0; const char* ty = 0;
    if(ops && ops->mntinfo && ops->mntinfo(&src, &ty)==0 && src && src[0]){
        if(src[0]=='/' && src[1]=='d' && src[2]=='e' && src[3]=='v' && src[4]=='/') src += 5;
        return vfs_blkdev_id(src);
    }
    return 0;
}

void vfs_init(void){
    g_mount_count = 0;
    for(int i=0;i<VFS_MAX_MOUNTS;i++){
        g_mounts[i].mount_point=NULL; g_mounts[i].ops=NULL; g_mounts[i].fs_name=NULL; g_mounts[i].dev=0;
        g_mounts[i].sid=0; g_mp_used[i]=0; g_sid_active[i]=0; g_sid_draining[i]=0;
    }
}

static int str_eq(const char* a, const char* b){
    size_t k=0; while(a[k]||b[k]){ if(a[k]!=b[k]) return 0; k++; } return 1;
}
// Locate a mount by its mount point (lock-free leaf — caller holds
// g_mount_lock). Returns the index in g_mounts[] or -1.
static int find_mount_by_point_nolock(const char* mp){
    for(int i=0;i<g_mount_count;i++) if(str_eq(g_mounts[i].mount_point, mp)) return i;
    return -1;
}
// How many mounts use `ops` (an FS instance may be mounted twice).
static int ops_uses_nolock(const vfs_fs_ops_t* ops){
    int n=0; for(int i=0;i<g_mount_count;i++) if(g_mounts[i].ops==ops) n++; return n;
}

// Add a table entry (caller holds g_mount_lock). 0 / -1.
static int mount_add_nolock(const char* mp, size_t n, const vfs_fs_ops_t* ops, const char* fs_name, uint64_t dev){
    if(find_mount_by_point_nolock(mp) >= 0) return -1;          // already mounted
    if(g_mount_count >= VFS_MAX_MOUNTS) return -1;
    int sid = sid_get_nolock();
    if(sid < 0) return -1;
    char* own = g_mp_store[sid];
    for(size_t k=0;k<=n;k++) own[k] = mp[k];
    g_mounts[g_mount_count].mount_point = own;
    g_mounts[g_mount_count].ops = ops;
    g_mounts[g_mount_count].fs_name = fs_name;
    g_mounts[g_mount_count].dev = dev ? dev : vfs_makedev(0, ++g_anon_minor);
    g_mounts[g_mount_count].sid = (uint8_t)sid;
    g_mount_count++;
    return 0;
}

int vfs_mount_root(const vfs_fs_ops_t* ops, const char* fs_name){
    if(!ops || !fs_name) return -1;
    uint64_t dev = vfs_dev_for(ops);
    pagecache_invalidate_fs(ops);                // slots reused from an earlier mount
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    int r = mount_add_nolock("/", 1, ops, fs_name, dev);
    spin_unlock_irqrestore(&g_mount_lock, fl);
    return r;
}

// Swap the filesystem behind "/". The old root's in-flight operations are
// drained first (they hold refs on the root slot), so none of them is still
// inside the old FS when this returns.
static int vfs_drain_sid(int sid, const vfs_fs_ops_t* ops);
int vfs_replace_root(const vfs_fs_ops_t* ops, const char* fs_name){
    if(!ops || !fs_name) return -1;
    uint64_t dev = vfs_dev_for(ops);
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    int i = find_mount_by_point_nolock("/");
    if(i >= 0){
        int sid = g_mounts[i].sid;
        const vfs_fs_ops_t* old = g_mounts[i].ops;
        g_sid_draining[sid] = 1;                 // new lookups of "/" wait it out (fail)
        spin_unlock_irqrestore(&g_mount_lock, fl);
        if(old != ops) vfs_drain_sid(sid, old);  // best effort: replace even on timeout
        fl = spin_lock_irqsave(&g_mount_lock);
        i = find_mount_by_point_nolock("/");
        if(i >= 0){
            g_mounts[i].ops = ops; g_mounts[i].fs_name = fs_name;
            if(dev) g_mounts[i].dev = dev;       // virtual root keeps its anon id
        }
        g_sid_draining[sid] = 0;
        spin_unlock_irqrestore(&g_mount_lock, fl);
        if(old != ops) pagecache_invalidate_fs(old);
        pagecache_invalidate_fs(ops);
        return 0;
    }
    spin_unlock_irqrestore(&g_mount_lock, fl);
    // vfs_mount_root takes g_mount_lock itself — call it OUTSIDE the lock (not
    // recursive). It re-checks for an existing root, so the drop/re-take is safe.
    return vfs_mount_root(ops, fs_name);
}

// Mount a filesystem at an arbitrary absolute mount point (e.g. "/mnt").
// The mount point is copied into VFS-owned storage (g_mp_store), so callers
// may pass a temporary buffer. A trailing '/' is dropped so "/x/" and "/x"
// name the same mount. fs_name must be a static string.
int vfs_mount(const char* mount_point, const vfs_fs_ops_t* ops, const char* fs_name){
    if(!mount_point || !ops || !fs_name) return -1;
    if(mount_point[0] != '/') return -1;
    char mp[VFS_MP_MAX]; size_t n = 0;
    for(; mount_point[n]; n++){ if(n >= sizeof(mp)-1) return -1; mp[n] = mount_point[n]; }
    while(n > 1 && mp[n-1]=='/') n--;
    mp[n] = 0;
    if(n == 1) return -1;                        // "/" goes through vfs_mount_root
    uint64_t dev = vfs_dev_for(ops);
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    int r = mount_add_nolock(mp, n, ops, fs_name, dev);
    spin_unlock_irqrestore(&g_mount_lock, fl);
    if(r) return r;
    // The FS may just have reset its inode pool, handing out vfs_inode_t slots
    // an earlier mount used: the page cache must not serve that mount's pages.
    pagecache_invalidate_fs(ops);
    return 0;
}

// ---- [M51 C1] In-flight operation refs + unmount draining -------------------
// Every VFS entry point that routes a path to a mount takes a ref on that
// mount's slot while it calls into the filesystem (vfs_resolve -> vfs_mput).
// An unmount marks the slot draining (new resolutions of paths below it fail
// as if nothing were there) and waits until the refs drop to zero, then takes
// and releases the FS-instance lock (ops->lock, when the driver has one) as a
// barrier for calls that went straight through an open file's ino->ops: so
// when it detaches, no VFS operation is inside the filesystem any more.
#define VFS_DRAIN_MS 5000u

// 0 = drained; -16 (EBUSY) = refs still held after VFS_DRAIN_MS.
#ifdef SECOS_VFS_TEST
volatile int g_vfs_test_nodrain;   // fs/vfs_test.c negative control only
volatile uint32_t g_vfs_test_drain_waits;   // drains that found refs in flight
#endif
static int vfs_drain_sid(int sid, const vfs_fs_ops_t* ops){
#ifdef SECOS_VFS_TEST
    if(g_vfs_test_nodrain) return 0;
#endif
    uint64_t t0 = ktime_ms();
#ifdef SECOS_VFS_TEST
    if(__atomic_load_n(&g_sid_active[sid], __ATOMIC_ACQUIRE)) g_vfs_test_drain_waits++;
#endif
    while(__atomic_load_n(&g_sid_active[sid], __ATOMIC_ACQUIRE)){
        if(ktime_ms() - t0 > VFS_DRAIN_MS){
            debugcon_writestring("[VFS] umount: in-flight operations did not drain\n");
            return -16;
        }
        cpu_relax_poll();
    }
    if(ops && ops->lock){ klock_lock(ops->lock); klock_unlock(ops->lock); }
    return 0;
}

int vfs_unmount_ex(const char* mount_point, vfs_busy_cb busy, void* ctx, unsigned flags){
    if(!mount_point) return -1;
    // Never detach the root filesystem: every path resolves through it, so a
    // umount("/") (found by the syscall fuzzer) left an empty namespace — no
    // /bin, no /dev — and every later exec/stat failed for the whole system.
    if(mount_point[0]=='/' && mount_point[1]==0) return -1;
    int mine = !klock_held(&g_vfs_ns_lock);
    if(mine) klock_lock(&g_vfs_ns_lock);
    int r = -1;
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    int i = find_mount_by_point_nolock(mount_point);
    if(i < 0){ spin_unlock_irqrestore(&g_mount_lock, fl); goto out; }
    int sid = g_mounts[i].sid;
    const vfs_fs_ops_t* ops = g_mounts[i].ops;
    int last = (ops_uses_nolock(ops) == 1);      // mounted elsewhere too?
    g_sid_draining[sid] = 1;
    spin_unlock_irqrestore(&g_mount_lock, fl);

    r = vfs_drain_sid(sid, last ? ops : NULL);
    // In use (open fd / file mapping): -EBUSY, like Linux without MNT_DETACH.
    // Asked only now: no new open of a file below the mount can start while
    // it drains. An FS mounted twice is still reachable through its other
    // mount point, so only the last mount checks.
    if(r == 0 && last && busy && busy(ops, ctx)) r = -16;
    // Flush the filesystem before detaching it (exFAT clears VolumeDirty here,
    // ext2/ext4 closes any open transaction), so a umount leaves it clean.
    if(r == 0 && (flags & VFS_UMOUNT_SYNC) && ops->sync) ops->sync();

    fl = spin_lock_irqsave(&g_mount_lock);
    i = find_mount_by_point_nolock(mount_point);  // ns lock held: still there
    if(i >= 0 && r == 0){
        sid_put_nolock(sid);
        for(int j=i;j<g_mount_count-1;j++) g_mounts[j]=g_mounts[j+1];
        g_mount_count--;
        g_mounts[g_mount_count].mount_point=NULL; g_mounts[g_mount_count].ops=NULL; g_mounts[g_mount_count].fs_name=NULL;
    }
    g_sid_draining[sid] = 0;
    spin_unlock_irqrestore(&g_mount_lock, fl);
    if(r == 0 && last) pagecache_invalidate_fs(ops);
out:
    if(mine) klock_unlock(&g_vfs_ns_lock);
    return r;
}

int vfs_unmount(const char* mount_point){ return vfs_unmount_ex(mount_point, NULL, NULL, 0); }

// Orderly shutdown: flush every mounted filesystem. Disk FSes are write-through
// today (block drivers are polled/synchronous; ext2/ext4 commits + checkpoints
// each metadata transaction), so ->sync only has to close anything still open.
// The sync callbacks run without g_mount_lock (they do block I/O), each under a
// ref on its mount so a concurrent umount waits for it.
int vfs_sync_all(void){
    const vfs_fs_ops_t* ops[VFS_MAX_MOUNTS]; int sids[VFS_MAX_MOUNTS]; int n = 0, done = 0;
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    for(int i=0;i<g_mount_count;i++){
        int dup = 0;                                 // one FS instance may be mounted twice
        for(int j=0;j<n;j++) if(ops[j]==g_mounts[i].ops) dup = 1;
        if(dup || g_sid_draining[g_mounts[i].sid]) continue;
        ops[n] = g_mounts[i].ops; sids[n] = g_mounts[i].sid;
        __atomic_add_fetch(&g_sid_active[sids[n]], 1, __ATOMIC_ACQ_REL);
        n++;
    }
    spin_unlock_irqrestore(&g_mount_lock, fl);
    for(int i=0;i<n;i++){
        if(ops[i]->sync && ops[i]->sync()==0) done++;
        __atomic_sub_fetch(&g_sid_active[sids[i]], 1, __ATOMIC_ACQ_REL);
    }
    return done;
}

// Shutdown: detach every mount except the root (each one drained first).
int vfs_unmount_all(void){
    int n = 0;
    int mine = !klock_held(&g_vfs_ns_lock);
    if(mine) klock_lock(&g_vfs_ns_lock);
    for(;;){
        char mp[VFS_MP_MAX]; int found = 0;
        uint64_t fl = spin_lock_irqsave(&g_mount_lock);
        for(int i=g_mount_count-1;i>=0;i--){         // deepest (latest) first
            const char* p = g_mounts[i].mount_point;
            if(p[0]=='/' && p[1]==0) continue;       // keep the root
            size_t k=0; for(; p[k] && k<VFS_MP_MAX-1; k++) mp[k]=p[k]; mp[k]=0;
            found = 1; break;
        }
        spin_unlock_irqrestore(&g_mount_lock, fl);
        if(!found) break;
        if(vfs_unmount_ex(mp, NULL, NULL, 0) != 0) break;   // stuck: leave the rest
        n++;
    }
    if(mine) klock_unlock(&g_vfs_ns_lock);
    return n;
}

// A resolved mount: a snapshot of its table entry plus a ref on its slot,
// held while the caller is inside the filesystem (vfs_mput drops it).
typedef struct vfs_mref {
    const vfs_fs_ops_t* ops;
    const char* mount_point;   // stable string (slot storage)
    const char* fs_name;
    int sid;                   // -1 = no ref held
} vfs_mref_t;

static void vfs_mput(vfs_mref_t* r){
    if(r && r->sid >= 0){ __atomic_sub_fetch(&g_sid_active[r->sid], 1, __ATOMIC_ACQ_REL); r->sid = -1; }
}

// Resolve an absolute path to its owning mount and the mount-relative path.
// rel (size rel_sz) receives the relative path rooted at '/'. On success fills
// *out (taking a ref) and returns 0; -1 if no mount matches or the matching
// one is being unmounted.
static int vfs_resolve(const char* path, char* rel, size_t rel_sz, vfs_mref_t* out){
    out->ops = NULL; out->mount_point = NULL; out->fs_name = NULL; out->sid = -1;
    if(!path || !*path) path = "/";
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    int best = -1; size_t best_len = 0;
    for(int i=0;i<g_mount_count;i++){
        const char* mp = g_mounts[i].mount_point;
        size_t mlen = str_len(mp);
        // Compare prefix.
        size_t k=0; int pref=1;
        for(k=0;k<mlen;k++){ if(path[k]!=mp[k]){ pref=0; break; } }
        if(!pref) continue;
        // Boundary: root "/" matches anything; otherwise next char must end or be '/'.
        if(mlen==1 && mp[0]=='/'){
            if(best_len==0){ best=i; best_len=1; } // root is the weakest match
            continue;
        }
        char after = path[mlen];
        if(after!=0 && after!='/') continue;
        if(mlen > best_len){ best=i; best_len=mlen; }
    }
    if(best < 0 || g_sid_draining[g_mounts[best].sid] || !g_mounts[best].ops){
        spin_unlock_irqrestore(&g_mount_lock, fl);
        return -1;
    }
    out->ops = g_mounts[best].ops;
    out->mount_point = g_mounts[best].mount_point;
    out->fs_name = g_mounts[best].fs_name;
    out->sid = g_mounts[best].sid;
    __atomic_add_fetch(&g_sid_active[out->sid], 1, __ATOMIC_ACQ_REL);
    spin_unlock_irqrestore(&g_mount_lock, fl);
    // Build the relative path.
    const char* mp = out->mount_point; size_t mlen = str_len(mp);
    const char* tail;
    if(mlen==1 && mp[0]=='/') tail = path;           // root: relative == absolute
    else tail = path + mlen;                          // strip "/mnt"
    if(tail[0]==0){ if(rel_sz>=2){ rel[0]='/'; rel[1]=0; } }
    else {
        size_t j=0; if(tail[0] != '/' && j<rel_sz-1) rel[j++]='/';
        for(size_t i=0; tail[i] && j<rel_sz-1; i++) rel[j++]=tail[i];
        rel[j]=0;
    }
    return 0;
}

// Mount point of the filesystem that holds `path` ("/" for the root FS), or
// NULL. (kernel/cred.c: set-id bits are honoured on the root filesystem only.)
const char* vfs_mount_point_for(const char* path){
    char rel[256]; vfs_mref_t m;
    if(vfs_resolve(path, rel, sizeof(rel), &m)) return NULL;
    vfs_mput(&m);
    return m.mount_point;
}

// Lookup with the mount's ref kept (the caller drops it with vfs_mput once it
// is done with the inode).
static vfs_inode_t* vfs_lookup_ref(const char* path, vfs_mref_t* m){
    char rel[256];
    if(vfs_resolve(path, rel, sizeof(rel), m)) return NULL;
    vfs_inode_t* ino = m->ops->lookup(rel);
    if(ino) ino->ops = m->ops; // stamp owning ops for subsequent read/write
    return ino;
}

vfs_inode_t* vfs_lookup(const char* path){
    vfs_mref_t m;
    vfs_inode_t* ino = vfs_lookup_ref(path, &m);
    vfs_mput(&m);
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
    // The callback is called by the driver with NO FS lock held (contract in
    // vfs.h); only this mount's ref is held across the whole listing.
    vfs_mref_t m;
    if(vfs_resolve(path, rel, sizeof(rel), &m)) return -1;
    int r = m.ops->readdir ? m.ops->readdir(rel, cb, user) : -1;
    vfs_mput(&m);
    return r;
}

int vfs_read_all(const char* path, void* buf, size_t bufsize){
    vfs_mref_t m;
    vfs_inode_t* ino = vfs_lookup_ref(path, &m);      // ref kept across the read
    int r = -1;
    if(ino && ino->type==VFS_NODE_FILE && ino->ops && ino->size <= bufsize){
        size_t sz = ino->size;
        r = ino->ops->read(ino, 0, buf, sz);
        r = (r>=0) ? (int)sz : -1;
    }
    vfs_mput(&m);
    return r;
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
    return vfs_walk_checked(path, out, outsz, follow_final, NULL, NULL);
}

// The walk proper. `search` (may be NULL) is asked for every directory the walk
// descends through -- the root at each (re)start and every intermediate
// component, including the directories met while following a symlink -- and a
// non-zero return (-errno) aborts the resolution with that error. This is the
// "search (x) permission on every path component" rule (kernel/cred.c).
int vfs_walk_checked(const char* path, char* out, size_t outsz, int follow_final,
                     vfs_search_cb search, void* ctx){
    char todo[VFS_PATH];      // what is still to be resolved (absolute)
    char res[VFS_PATH];       // canonical prefix resolved so far ("" = root)
    if(!path || path[0]!='/') return -2;                       // ENOENT
    if(vfs_copy(todo, path, sizeof(todo))!=0) return -36;      // ENAMETOOLONG
    int hops = 0;
restart:
    res[0]=0;
    if(search){
        vfs_inode_t* r = vfs_lookup("/");
        int e = r ? search(ctx, r) : 0;
        if(e) return e;
    }
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
            if(!last && search){                                 // descending into it
                int se = search(ctx, ino);
                if(se) return se;
            }
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
    char rel[256]; vfs_mref_t m; if(vfs_resolve(cp, rel, sizeof(rel), &m)) return -1;
    int r = m.ops->create ? m.ops->create(rel, data, size) : -1;
    vfs_mput(&m);
    return r;
}
int vfs_write(const char* path, size_t offset, const void* data, size_t len){
    vfs_mref_t m;
    vfs_inode_t* ino = vfs_lookup_ref(path, &m);      // ref kept across the write
    if(ino && ino->type==VFS_NODE_SYMLINK){           // follow it (vfs_lookup_follow)
        char cp[VFS_PATH];
        vfs_mput(&m);
        ino = vfs_walk(path, cp, sizeof(cp), 1)==0 ? vfs_lookup_ref(cp, &m) : NULL;
    }
    int r = -1;
    if(ino && ino->type==VFS_NODE_FILE && ino->ops && ino->ops->write) r = ino->ops->write(ino, offset, data, len);
    vfs_mput(&m);
    return r;
}
int vfs_mkdir(const char* path){
    char cp[VFS_PATH]; int e=vfs_canon_entry(path, cp, sizeof(cp)); if(e) return e;
    char rel[256]; vfs_mref_t m; if(vfs_resolve(cp,rel,sizeof(rel),&m)) return -1;
    int r = m.ops->mkdir ? m.ops->mkdir(rel) : -1;
    vfs_mput(&m); return r;
}
int vfs_remove(const char* path){
    char cp[VFS_PATH]; int e=vfs_canon_entry(path, cp, sizeof(cp)); if(e) return e;
    char rel[256]; vfs_mref_t m; if(vfs_resolve(cp,rel,sizeof(rel),&m)) return -1;
    int r = m.ops->remove ? m.ops->remove(rel) : -1;
    vfs_mput(&m); return r;
}

static int vfs_is_mount_point(const char* p){
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    int r = find_mount_by_point_nolock(p) >= 0;
    spin_unlock_irqrestore(&g_mount_lock, fl);
    return r;
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
    char rel[256]; vfs_mref_t m; if(vfs_resolve(cp,rel,sizeof(rel),&m)) return -1;
    int r = m.ops->remove ? m.ops->remove(rel) : -1;
    vfs_mput(&m);
    return r == 0 ? 0 : (r == -30 ? -30 : -5);           // EROFS / EIO
}

// rename: atomic within one filesystem (the FS replaces an existing target in
// one step — on ext2/ext4 inside one journalled transaction); -EXDEV across
// mounts. POSIX type rules are checked here so every FS gets them.
static int vfs_rename_at(const char* co, const char* cn, const char* ro, const char* rn,
                         const struct vfs_mref* mo, const struct vfs_mref* mn);
int vfs_rename(const char* oldp, const char* newp){
    char co[VFS_PATH], cn[VFS_PATH]; int e;
    if((e=vfs_canon_entry(oldp, co, sizeof(co)))) return e;
    if((e=vfs_canon_entry(newp, cn, sizeof(cn)))) return e;
    char ro[256], rn[256];
    vfs_mref_t mo, mn;
    if(vfs_resolve(co,ro,sizeof(ro),&mo)) return -2;
    if(vfs_resolve(cn,rn,sizeof(rn),&mn)){ vfs_mput(&mo); return -2; }
    int r = vfs_rename_at(co, cn, ro, rn, &mo, &mn);
    vfs_mput(&mn); vfs_mput(&mo);
    return r;
}
// rename with both mounts resolved (refs held by the caller).
static int vfs_rename_at(const char* co, const char* cn, const char* ro, const char* rn,
                         const vfs_mref_t* mo, const vfs_mref_t* mn){
    vfs_inode_t* src = vfs_lookup(co);
    if(!src) return -2;                                   // ENOENT
    int src_dir = (src->type==VFS_NODE_DIR);
    if(mo->sid!=mn->sid) return -18;                      // EXDEV
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
    int r = mo->ops->rename(ro, rn);
    return r == 0 ? 0 : (r == -30 ? -30 : -5);           // EROFS / EIO
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
    vfs_mref_t mo, mn;
    if(vfs_resolve(co,ro,sizeof(ro),&mo)) return -2;
    if(vfs_resolve(cn,rn,sizeof(rn),&mn)){ vfs_mput(&mo); return -2; }
    int r;
    if(mo.sid!=mn.sid) r = -18;                           // EXDEV
    else if(!mo.ops->link) r = -1;                        // EPERM: FS has no hard links
    else { r = mo.ops->link(ro, rn); r = r == 0 ? 0 : (r == -30 ? -30 : -5); }   // EROFS / EIO
    vfs_mput(&mn); vfs_mput(&mo);
    return r;
}

int vfs_truncate(const char* path, size_t new_size){
    char cp[VFS_PATH]; int e;
    vfs_inode_t* ino = vfs_lookup_ex(path, 1, &e);        // truncate() follows symlinks
    if(!ino) return e ? e : -2;
    if(ino->type==VFS_NODE_DIR) return -21;               // EISDIR
    if((e=vfs_walk(path, cp, sizeof(cp), 1))) return e;
    char rel[256]; vfs_mref_t m; if(vfs_resolve(cp,rel,sizeof(rel),&m)) return -1;
    int r = -1;
    if(m.ops->truncate){
        // [M51 C1] Moves the end of file: serialized against the fallback
        // O_APPEND path (vfs_append) of a filesystem without ->append.
        int al = !m.ops->append;
        if(al) klock_lock(&g_vfs_append_lock);
        r = m.ops->truncate(rel, new_size);
        if(al) klock_unlock(&g_vfs_append_lock);
    }
    vfs_mput(&m);
    return r;
}

// [M51 C1] ftruncate of an open file (the fd pins the mount: no ref needed).
// Updates the cached size like the old open-coded kiface path did.
int vfs_ftruncate(vfs_inode_t* ino, size_t new_size){
    if(!ino || !ino->ops || !ino->ops->truncate) return -1;
    int al = !ino->ops->append;
    if(al) klock_lock(&g_vfs_append_lock);
    int r = ino->ops->truncate(ino->path, new_size);
    if(r == 0) ino->size = new_size;
    if(al) klock_unlock(&g_vfs_append_lock);
    return r;
}

// [M51 C1] Atomic O_APPEND write. With ->append the driver resolves the end of
// file and writes there under its own instance lock. Without it the VFS does
// the same under g_vfs_append_lock: the fd's inode does not see writes made
// through other fds (ramfs keeps the size in its entry), so the size is looked
// up fresh through the driver (mount-relative ino->path) and the write goes
// there before any other appender (or truncate) can move the end. The fd pins
// the mount (umount answers EBUSY), so no mount ref is taken. Returns bytes
// written or <0; *at = the offset the data landed at.
int vfs_append(vfs_inode_t* ino, const void* buf, size_t len, uint64_t* at){
    if(at) *at = 0;
    const vfs_fs_ops_t* ops = ino ? ino->ops : NULL;    // one read: see below
    if(!ops || !ops->write) return -1;
    if(ops->append) return ops->append(ino, buf, len, at);
    klock_lock(&g_vfs_append_lock);
    size_t end = ino->size;
    if(ops->lookup){
        // A driver's lookup returns its inode with ->ops unset (vfs_lookup
        // stamps it): restamp, the slot may be this very inode.
        vfs_inode_t* cur = ops->lookup(ino->path);
        if(cur) cur->ops = ops;
        if(cur == ino || (cur && cur->fs_data == ino->fs_data)) end = cur->size;
    }
    int r = ops->write(ino, end, buf, len);
    klock_unlock(&g_vfs_append_lock);
    if(at) *at = end;
    return r;
}

// [M26] Attribute / symlink routing (NULL-checked: FSes that don't implement the
// op return -1 instead of crashing — chmod on a read-only virtual FS is a no-op).
static int vfs_setattr_at(const char* cp, const vfs_attr_t* a, unsigned valid);
static int vfs_setattr_mnt(const struct vfs_mref* m, const char* cp, const char* rel,
                           const vfs_attr_t* a, unsigned valid);
int vfs_setattr(const char* path, const vfs_attr_t* a, unsigned valid){
    char cp[VFS_PATH]; int e=vfs_walk(path, cp, sizeof(cp), 1); if(e) return e;   // chmod follows
    return vfs_setattr_at(cp, a, valid);
}
// [M43] Same without following a final symlink (lchown; kernel/cred.c hands
// over paths it has already resolved).
int vfs_lsetattr(const char* path, const vfs_attr_t* a, unsigned valid){
    char cp[VFS_PATH]; int e=vfs_canon_entry(path, cp, sizeof(cp)); if(e) return e;
    return vfs_setattr_at(cp, a, valid);
}
static int vfs_setattr_at(const char* cp, const vfs_attr_t* a, unsigned valid){
    char rel[256]; vfs_mref_t m; if(vfs_resolve(cp,rel,sizeof(rel),&m)) return -1;
    int r = vfs_setattr_mnt(&m, cp, rel, a, valid);
    vfs_mput(&m);
    return r;
}
static int vfs_setattr_mnt(const vfs_mref_t* m, const char* cp, const char* rel,
                           const vfs_attr_t* a, unsigned valid){
    if(m->ops->setattr) return m->ops->setattr(rel, a, valid);
    // A filesystem with no on-disk metadata (ramfs, devfs incl. /dev/pts, the
    // virtual ones): keep the change on the in-memory inode, like devtmpfs, so
    // chmod/chown succeed (sshd/login chmod the pty slave) and stat shows them.
    // [M51 C1] The fields live in the driver's inode: written under its
    // instance lock (ops->lock) when it has one. The lookup runs first and
    // outside it (the driver's lookup takes that lock itself).
    vfs_inode_t* ino=vfs_lookup(cp); if(!ino) return -2;
    if(m->ops->lock) klock_lock(m->ops->lock);
    if(valid & VFS_ATTR_MODE){
        uint32_t t = ino->mode & 0170000u;
        if(!t) t = ino->type==VFS_NODE_DIR ? 0040000u : ino->type==VFS_NODE_SYMLINK ? 0120000u : 0100000u;
        ino->mode = t | (a->mode & 07777u);
    }
    if(valid & VFS_ATTR_UID)   ino->uid = a->uid;
    if(valid & VFS_ATTR_GID)   ino->gid = a->gid;
    if(valid & VFS_ATTR_ATIME) ino->atime = a->atime;
    if(valid & VFS_ATTR_MTIME) ino->mtime = a->mtime;
    if(m->ops->lock) klock_unlock(m->ops->lock);
    return 0;
}
int vfs_readlink(const char* path, char* buf, size_t len){
    char cp[VFS_PATH];
    if(vfs_canon_entry(path, cp, sizeof(cp))!=0) return -1;
    char rel[256]; vfs_mref_t m; if(vfs_resolve(cp,rel,sizeof(rel),&m)) return -1;
    int r = m.ops->readlink ? m.ops->readlink(rel, buf, len) : -1;
    vfs_mput(&m); return r;
}
int vfs_symlink(const char* target, const char* linkpath){
    char cp[VFS_PATH]; int e=vfs_canon_entry(linkpath, cp, sizeof(cp)); if(e) return e;
    char rel[256]; vfs_mref_t m; if(vfs_resolve(cp,rel,sizeof(rel),&m)) return -1;
    int r = m.ops->symlink ? m.ops->symlink(target, rel) : -1;
    vfs_mput(&m); return r;
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
static int vfs_statfs_mount(const vfs_mref_t* m, vfs_statfs_t* st){
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
    char rel[256]; vfs_mref_t m; if(vfs_resolve(cp,rel,sizeof(rel),&m)) return -2;
    int r = vfs_statfs_mount(&m, st);
    vfs_mput(&m);
    return r;
}
int vfs_statfs_ops(const vfs_fs_ops_t* ops, vfs_statfs_t* st){
    vfs_mref_t m; m.sid = -1;
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    for(int i=0;i<g_mount_count;i++) if(g_mounts[i].ops==ops && !g_sid_draining[g_mounts[i].sid]){
        m.ops = ops; m.mount_point = g_mounts[i].mount_point; m.fs_name = g_mounts[i].fs_name;
        m.sid = g_mounts[i].sid;
        __atomic_add_fetch(&g_sid_active[m.sid], 1, __ATOMIC_ACQ_REL);
        break;
    }
    spin_unlock_irqrestore(&g_mount_lock, fl);
    if(m.sid < 0) return -2;
    int r = vfs_statfs_mount(&m, st);
    vfs_mput(&m);
    return r;
}
// Mount point of the filesystem behind `ops` (the longest one when an FS is
// mounted twice), or NULL. Lets inode paths (mount-relative) be made absolute.
const char* vfs_mount_point_of(const vfs_fs_ops_t* ops){
    const char* best=NULL; size_t bl=0;
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    for(int i=0;i<g_mount_count;i++) if(g_mounts[i].ops==ops){ size_t l=str_len(g_mounts[i].mount_point); if(!best||l>bl){ best=g_mounts[i].mount_point; bl=l; } }
    spin_unlock_irqrestore(&g_mount_lock, fl);
    return best;
}
// Sync one filesystem (fsync/fdatasync of a file on it). 0 also when the FS is
// write-through (no ->sync).
int vfs_sync_ops(const vfs_fs_ops_t* ops){
    if(ops && ops->sync) return ops->sync();
    return 0;
}

// st_dev of the mount behind `ops` (0 if not mounted).
uint64_t vfs_dev_of_ops(const vfs_fs_ops_t* ops){
    uint64_t d = 0;
    uint64_t fl = spin_lock_irqsave(&g_mount_lock);
    for(int i=0;i<g_mount_count;i++) if(g_mounts[i].ops==ops){ d = g_mounts[i].dev; break; }
    spin_unlock_irqrestore(&g_mount_lock, fl);
    return d;
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
