/*
 * pagecache.c — [M20] Unified file page cache.
 * Copyright (c) 2025 iDev srl
 * SPDX-License-Identifier: MIT
 *
 * [M50 B2] Fill without the lock. pc_lock guards only the index (pc[], the
 * eviction hand, the counters); it is never held across a filesystem or block
 * call. A miss:
 *   1. under pc_lock: reserve a slot in state FILLING for (inode, offset),
 *      owned by the calling task;
 *   2. unlocked: allocate a private frame and fill it through ino->ops->read;
 *   3. under pc_lock: publish the frame (state VALID), unless the slot was
 *      marked STALE in the meantime, in which case the frame is discarded.
 * Invalidation (pagecache_invalidate / _fs, called after every write,
 * truncate, rename, unlink and inode-slot reuse) frees overlapping VALID pages
 * and marks overlapping FILLING slots stale. A fill that raced with a write is
 * therefore never published: the next lookup after the invalidation re-reads
 * the file. (This is the plan's "per-inode invalidation generation", kept per
 * in-flight fill: a fill only exists as a FILLING slot, so the slot is the one
 * place an invalidation has to reach, and a range-precise stale bit never
 * throws away fills of other pages of the inode.)
 *
 * Other rules:
 *   - A FILLING slot is never evicted (its frame belongs to the filler, not to
 *     the slot). With every slot filling, the miss reads straight into the
 *     caller's buffer without caching.
 *   - A second misser for a page being filled spins (caller's IF, pause +
 *     tlb_poll, no lock held) until the slot changes state, then looks again.
 *     The wait is bounded (PC_WAIT_US): past it, or when the filler is the
 *     calling task itself, it reads the page directly. Nothing can deadlock on
 *     a fill.
 *   - Cached bytes are only ever copied out under pc_lock (pagecache_read,
 *     pagecache_copy_page), so an eviction or invalidation on another CPU can
 *     never free a frame somebody is still reading.
 *   - A failed filesystem read is returned as an error and never cached.
 */
#include "pagecache.h"
#include "pmm.h"
#include "vmm.h"        // phys_to_virt
#include "vfs.h"        // vfs_inode_t + ops->read
#include "spinlock.h"   // SMP serialization of the cache index
#include "percpu.h"     // this_cpu(): the filler's identity

#define ADDRESS_MASK 0x000FFFFFFFFFF000ULL
#define PC_ENTRIES   128
#define PC_WAIT_US   20000ULL   /* longest wait for another task's fill */

enum { PC_FREE = 0, PC_FILLING = 1, PC_VALID = 2 };

typedef struct {
    vfs_inode_t* inode;
    const vfs_fs_ops_t* ops;  // filesystem the page was read from
    uint64_t     off;         // page-aligned file offset
    uint64_t     phys;        // cached frame (VALID only; a filler's frame is private)
    void*        owner;       // FILLING: the task (or CPU) doing the fill
    volatile uint32_t seq;    // bumped on every state change (waiters watch it)
    volatile uint8_t  state;  // PC_FREE / PC_FILLING / PC_VALID
    uint8_t      stale;       // FILLING: invalidated during the fill, do not publish
} pc_entry_t;

static pc_entry_t pc[PC_ENTRIES];
static uint32_t   pc_clock = 0;   // FIFO eviction hand
static spinlock_t pc_lock = SPINLOCK_INIT_CLASS(LK_S40_PAGECACHE, "pagecache");   // guards pc[] + pc_clock + g_pcs
static pagecache_stats_t g_pcs;

#ifdef SECOS_PAGECACHE_TEST
void (*pagecache_test_fill_hook)(vfs_inode_t* inode, uint64_t off);
int pagecache_test_nostale;    // negative control: ignore the stale bit
#endif

extern uint64_t ktime_us(void);

static inline void* pc_self(void) {
    cpu_t* c = this_cpu();
    return c->current ? (void*)c->current : (void*)c;
}

static void pc_copy(void* dst, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst; const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

// Read file bytes [off, off+4096) into 'page' (a kernel buffer), zero-padded
// past EOF. No lock held. Returns 0, or -1 when the filesystem read failed.
static int pc_fill(vfs_inode_t* ino, uint64_t off, uint8_t* page) {
    for (int i = 0; i < 4096; i++) page[i] = 0;
    uint64_t size = ino->size;
    if (ino->ops && ino->ops->read && off < size) {
        uint64_t avail = size - off;
        uint64_t n = avail < 4096 ? avail : 4096;
        if (n && ino->ops->read(ino, (size_t)off, page, (size_t)n) < 0) return -1;
    }
    return 0;
}

static void pc_set_state(pc_entry_t* e, uint8_t st) {
    e->state = st;
    e->seq++;
}

// Drop a VALID slot (caller holds pc_lock); returns its frame for the caller
// to free after unlocking.
static uint64_t pc_drop_valid(pc_entry_t* e) {
    uint64_t f = e->phys;
    e->phys = 0; e->inode = 0; e->ops = 0;
    pc_set_state(e, PC_FREE);
    return f;
}

typedef enum { PC_HIT, PC_RESERVED, PC_WAIT, PC_DIRECT } pc_look_t;

// Caller holds pc_lock. HIT: *slot is VALID for (inode, off). RESERVED: *slot
// is a new FILLING slot owned by the caller (*victim = an evicted frame to
// free after unlocking, or 0). WAIT: another task is filling *slot (*seq = its
// sequence). DIRECT: nothing cacheable (the caller fills a page itself, or
// every slot is filling).
static pc_look_t pc_lookup_locked(vfs_inode_t* inode, uint64_t off, int* slot,
                                  uint32_t* seq, uint64_t* victim) {
    *victim = 0;
    for (int i = 0; i < PC_ENTRIES; i++) {
        pc_entry_t* e = &pc[i];
        if (e->state == PC_FREE || e->inode != inode || e->off != off) continue;
        if (e->state == PC_VALID) { *slot = i; g_pcs.hits++; return PC_HIT; }
        if (e->stale) continue;                 // a doomed fill: ignore it
        *slot = i; *seq = e->seq;
        return e->owner == pc_self() ? PC_DIRECT : PC_WAIT;
    }
    // Miss: a free slot, else FIFO-evict a VALID one (never a FILLING one).
    int s = -1;
    for (int i = 0; i < PC_ENTRIES; i++) if (pc[i].state == PC_FREE) { s = i; break; }
    if (s < 0) {
        for (int n = 0; n < PC_ENTRIES; n++) {
            int i = (int)(pc_clock++ % PC_ENTRIES);
            if (pc[i].state == PC_VALID) { s = i; break; }
        }
        if (s < 0) { g_pcs.direct++; return PC_DIRECT; }
        *victim = pc_drop_valid(&pc[s]);
        g_pcs.evictions++;
    }
    pc_entry_t* e = &pc[s];
    e->inode = inode; e->ops = inode->ops; e->off = off; e->phys = 0;
    e->owner = pc_self(); e->stale = 0;
    pc_set_state(e, PC_FILLING);
    g_pcs.misses++;
    *slot = s;
    return PC_RESERVED;
}

// Spin until slot 'i' leaves sequence 'seq' or the deadline passes. No lock
// held; IF as the caller had it. Returns 1 if the slot changed.
static int pc_wait_slot(int i, uint32_t seq, uint64_t deadline) {
    while (pc[i].seq == seq) {
        if (ktime_us() >= deadline) return 0;
        cpu_relax_poll();
    }
    return 1;
}

// The core: deliver the page (inode, off) — page-aligned — into 'dst' (4096
// bytes, kernel memory), through the cache. If 'in_pg'/'len' are given, only
// bytes [in_pg, in_pg+len) of the page are copied to dst. Returns 0 or -1.
static int pc_get(vfs_inode_t* inode, uint64_t off, uint8_t* dst,
                  uint64_t in_pg, uint64_t len) {
    uint64_t deadline = 0;
    for (;;) {
        int slot = -1; uint32_t seq = 0; uint64_t victim = 0;
        uint64_t fl = spin_lock_irqsave(&pc_lock);
        pc_look_t r = pc_lookup_locked(inode, off, &slot, &seq, &victim);
        if (r == PC_HIT) {
            pc_copy(dst, (const uint8_t*)phys_to_virt(pc[slot].phys) + in_pg, len);
            spin_unlock_irqrestore(&pc_lock, fl);
            return 0;
        }
        if (r == PC_WAIT) {
            g_pcs.waits++;
            spin_unlock_irqrestore(&pc_lock, fl);
            if (!deadline) deadline = ktime_us() + PC_WAIT_US;
            if (pc_wait_slot(slot, seq, deadline)) continue;   // look again
            fl = spin_lock_irqsave(&pc_lock);
            g_pcs.wait_timeouts++; g_pcs.direct++;
            spin_unlock_irqrestore(&pc_lock, fl);
            r = PC_DIRECT;
        } else {
            if (r == PC_DIRECT && slot >= 0) g_pcs.direct++;   // own fill (recursion)
            spin_unlock_irqrestore(&pc_lock, fl);
        }
        if (victim) pmm_free_frame((void*)victim);

        if (r == PC_DIRECT) {
            // Uncached: read into a private frame, copy the requested bytes.
            void* f = pmm_alloc_frame();
            if (!f) return -1;
            uint64_t ph = (uint64_t)f & ADDRESS_MASK;
            uint8_t* page = (uint8_t*)phys_to_virt(ph);
            int rc = pc_fill(inode, off, page);
            if (rc == 0) pc_copy(dst, page + in_pg, len);
            pmm_free_frame(f);
            return rc;
        }

        // RESERVED: fill unlocked, then publish unless invalidated meanwhile.
        pc_entry_t* e = &pc[slot];
        void* f = pmm_alloc_frame();
        uint64_t ph = f ? ((uint64_t)f & ADDRESS_MASK) : 0;
        int rc = -1;
        if (ph) {
            uint8_t* page = (uint8_t*)phys_to_virt(ph);
            rc = pc_fill(inode, off, page);
#ifdef SECOS_PAGECACHE_TEST
            if (pagecache_test_fill_hook) pagecache_test_fill_hook(inode, off);
#endif
            if (rc == 0) pc_copy(dst, page + in_pg, len);
        }
        fl = spin_lock_irqsave(&pc_lock);
        int stale = e->stale;
#ifdef SECOS_PAGECACHE_TEST
        if (pagecache_test_nostale) stale = 0;
#endif
        if (rc == 0 && !stale) {
            e->phys = ph; e->owner = 0;
            pc_set_state(e, PC_VALID);
            g_pcs.published++;
            ph = 0;                                         // now the cache's
        } else {
            if (rc == 0) g_pcs.discarded++;
            e->inode = 0; e->ops = 0; e->owner = 0; e->stale = 0;
            pc_set_state(e, PC_FREE);
        }
        spin_unlock_irqrestore(&pc_lock, fl);
        if (ph) pmm_free_frame((void*)ph);
        return rc;
    }
}

int pagecache_copy_page(vfs_inode_t* inode, uint64_t off, void* dst) {
    if (!inode || !dst) return -1;
    return pc_get(inode, off & ~0xFFFULL, (uint8_t*)dst, 0, 4096);
}

int pagecache_read(vfs_inode_t* inode, uint64_t offset, void* kbuf, uint64_t len) {
    if (!inode) return -1;
    if (offset >= inode->size) return 0;
    if (offset + len > inode->size) len = inode->size - offset;
    uint8_t* dst = (uint8_t*)kbuf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t off    = offset + done;
        uint64_t pgoff  = off & ~0xFFFULL;
        uint64_t in_pg  = off - pgoff;
        uint64_t chunk = 4096 - in_pg;
        if (chunk > len - done) chunk = len - done;
        if (pc_get(inode, pgoff, dst + done, in_pg, chunk) != 0) {
            if (done == 0) return -1;
            break;
        }
        done += chunk;
    }
    return (int)done;
}

// Free the VALID pages / doom the FILLING slots of 'ops' (if given) or of
// 'inode' overlapping [offset, end). Frames are
// freed after unlocking (in batches: the free list is a small local array).
static void pc_invalidate_where(vfs_inode_t* inode, uint64_t offset, uint64_t end,
                                const vfs_fs_ops_t* ops) {
    int i = 0;
    while (i < PC_ENTRIES) {
        uint64_t freeq[16]; int nf = 0;
        uint64_t fl = spin_lock_irqsave(&pc_lock);
        if (i == 0) g_pcs.invalidations++;
        for (; i < PC_ENTRIES && nf < 16; i++) {
            pc_entry_t* e = &pc[i];
            if (e->state == PC_FREE) continue;
            int hit = ops ? (e->ops == ops)
                          : (e->inode == inode && e->off < end && e->off + 4096 > offset);
            if (!hit) continue;
            if (e->state == PC_VALID) freeq[nf++] = pc_drop_valid(e);
            else if (!e->stale) { e->stale = 1; g_pcs.doomed++; }
        }
        spin_unlock_irqrestore(&pc_lock, fl);
        for (int k = 0; k < nf; k++) if (freeq[k]) pmm_free_frame((void*)freeq[k]);
    }
}

void pagecache_invalidate(vfs_inode_t* inode, uint64_t offset, uint64_t len) {
    if (!inode) return;
    uint64_t end = offset + len;
    if (end < offset) end = ~0ULL;
    pc_invalidate_where(inode, offset, end, 0);
}

// Drop every page read from the filesystem behind 'ops'. The VFS calls this
// when that filesystem is mounted or unmounted: drivers reset their inode pool
// on mount, so a vfs_inode_t* that cached one file's pages under the previous
// mount is handed to a different file under the next one, and a lookup keyed
// by that pointer would serve the old file's bytes.
void pagecache_invalidate_fs(const struct vfs_fs_ops* ops) {
    if (!ops) return;
    pc_invalidate_where(0, 0, 0, ops);
}

void pagecache_get_stats(pagecache_stats_t* out) {
    if (!out) return;
    uint64_t fl = spin_lock_irqsave(&pc_lock);
    *out = g_pcs;
    uint32_t v = 0, f = 0;
    for (int i = 0; i < PC_ENTRIES; i++) {
        if (pc[i].state == PC_VALID) v++;
        else if (pc[i].state == PC_FILLING) f++;
    }
    out->valid = v; out->filling = f;
    spin_unlock_irqrestore(&pc_lock, fl);
}
