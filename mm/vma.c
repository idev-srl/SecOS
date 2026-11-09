/*
 * vma.c — Per-process Virtual Memory Areas (demand paging).
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "vma.h"
#include "pmm.h"
#include "vmm.h"
#include "pagecache.h"  // [M20] file-backed mmap via the page cache

#define ADDRESS_MASK 0x000FFFFFFFFFF000ULL

// Local byte copy/fill — the kernel rolls its own; no libc memcpy/memset.
static void vma_memcpy(uint8_t* d, const uint8_t* s, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}
static void vma_memset(uint8_t* d, uint8_t val, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) d[i] = val;
}

int vma_add(vma_set_t* s, uint64_t start, uint64_t end, uint64_t flags,
            uint8_t type, const uint8_t* file_base, uint64_t file_off,
            uint64_t file_pad, uint64_t file_len) {
    if (!s) return -1;
    vma_t* v = 0;
    // [M18] Reuse a tombstone slot if one exists, else append.
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->v[i].type == VMA_TYPE_NONE) { v = &s->v[i]; break; }
    }
    if (!v) {
        if (s->count >= VMA_MAX) return -1;
        v = &s->v[s->count++];
    }
    v->start = start;
    v->end = end;
    v->flags = flags;
    v->type = type;
    v->vflags = 0;
    v->file_base = file_base;
    v->file_off = file_off;
    v->file_pad = file_pad;
    v->file_len = file_len;
    v->file_inode = 0;          // [M20] not a page-cache mapping
    v->file_voff = 0;
    v->phys_base = 0;
    return 0;
}

int vma_add_phys(vma_set_t* s, uint64_t start, uint64_t end, uint64_t flags, uint64_t phys) {
    if ((start & 0xFFF) || (end & 0xFFF) || (phys & 0xFFF) || end <= start) return -1;
    if (vma_add(s, start, end, flags, VMA_TYPE_PHYS, 0, 0, 0, 0) != 0) return -1;
    vma_t* v = vma_find_mut(s, start);
    if (!v) return -1;
    v->phys_base = phys;
    return 0;
}

int vma_add_file(vma_set_t* s, uint64_t start, uint64_t end, uint64_t flags,
                 struct vfs_inode* inode, uint64_t voff) {
    if (vma_add(s, start, end, flags, VMA_TYPE_FILE, 0, 0, 0, 0) != 0) return -1;
    // The slot just added is the one vma_find returns for 'start'.
    vma_t* v = vma_find_mut(s, start);
    if (!v) return -1;
    v->file_inode = inode;
    v->file_voff = voff;
    return 0;
}

const vma_t* vma_find(const vma_set_t* s, uint64_t addr) {
    if (!s) return 0;
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->v[i].type == VMA_TYPE_NONE) continue;
        if (addr >= s->v[i].start && addr < s->v[i].end) return &s->v[i];
    }
    return 0;
}

vma_t* vma_find_mut(vma_set_t* s, uint64_t addr) {
    if (!s) return 0;
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->v[i].type == VMA_TYPE_NONE) continue;
        if (addr >= s->v[i].start && addr < s->v[i].end) return &s->v[i];
    }
    return 0;
}

void vma_remove(vma_set_t* s, vma_t* v) {
    if (!s || !v) return;
    v->type = VMA_TYPE_NONE;       // tombstone; indices stay stable
    v->start = v->end = 0;
}

int vma_split(vma_set_t* s, vma_t* v, uint64_t addr) {
    if (!s || !v || v->type == VMA_TYPE_NONE) return -1;
    if ((addr & 0xFFF) || addr <= v->start || addr >= v->end) return -1;
    vma_t hi = *v;                              // copy before vma_add may reuse a slot
    uint64_t d = addr - v->start;
    hi.start = addr;
    if (hi.type == VMA_TYPE_PHYS) {
        hi.phys_base += d;                      // device memory: shift the physical base
    } else if (hi.type == VMA_TYPE_FILE && hi.file_inode) {
        hi.file_voff += d;                      // page-cache backing: shift the file offset
    } else if (hi.type == VMA_TYPE_FILE) {
        // Image backing: content sits at region offsets [file_pad, file_pad+file_len).
        if (d <= hi.file_pad) {
            hi.file_pad -= d;
        } else {
            uint64_t skip = d - hi.file_pad;
            if (skip >= hi.file_len) { hi.file_len = 0; hi.file_pad = 0; }
            else { hi.file_off += skip; hi.file_len -= skip; hi.file_pad = 0; }
        }
    }
    if (vma_add(s, hi.start, hi.end, hi.flags, hi.type, hi.file_base,
                hi.file_off, hi.file_pad, hi.file_len) != 0) return -1;
    // 'v' still covers addr until it is trimmed below, so vma_find_mut could
    // return it: look for the new slot explicitly.
    vma_t* n = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        vma_t* c = &s->v[i];
        if (c != v && c->type != VMA_TYPE_NONE && c->start == addr && c->end == hi.end) { n = c; break; }
    }
    if (!n) return -1;
    n->file_inode = hi.file_inode;
    n->file_voff = hi.file_voff;
    n->phys_base = hi.phys_base;
    n->vflags = hi.vflags;
    v->end = addr;
    return 0;
}

int vma_overlaps(const vma_set_t* s, uint64_t start, uint64_t end) {
    if (!s) return 0;
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->v[i].type == VMA_TYPE_NONE) continue;
        if (start < s->v[i].end && s->v[i].start < end) return 1;
    }
    return 0;
}

int vma_pinned_overlap(const vma_set_t* s, uint64_t start, uint64_t end) {
    if (!s) return 0;
    for (uint32_t i = 0; i < s->count; i++) {
        const vma_t* v = &s->v[i];
        if (v->type == VMA_TYPE_NONE || !(v->vflags & VMA_VF_PINNED)) continue;
        if (start < v->end && v->start < end) return 1;
    }
    return 0;
}

uint64_t vma_total_bytes(const vma_set_t* s) {
    if (!s) return 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->v[i].type == VMA_TYPE_NONE) continue;
        total += (s->v[i].end - s->v[i].start);
    }
    return total;
}

// [M40] Device memory: map the backing physical page directly (no frame
// allocation). DEVICE keeps the PMM away from it; PWT selects PAT slot 1
// (Write-Combining once fb_apply_wc/vmm_pat_init ran). Never executable: the
// VMA flags already carry NX (mmap/mprotect refuse PROT_EXEC here).
static int vma_map_phys_page(vmm_space_t* space, const vma_t* v, uint64_t pg) {
    uint64_t phys = v->phys_base + (pg - v->start);
    // [M57] Ring-3 driver regions carry their own memory type and the
    // DONTFORK PTE bit (vmm_fork_space skips them).
    uint64_t cache = (v->vflags & VMA_VF_UC) ? (VMM_FLAG_PWT | VMM_FLAG_PCD) :
                     (v->vflags & VMA_VF_WB) ? 0 : VMM_FLAG_PWT;
    uint64_t df = (v->vflags & VMA_VF_DONTFORK) ? VMM_FLAG_DONTFORK : 0;
    return vmm_map_in_space(space, pg, phys,
                            v->flags | VMM_FLAG_DEVICE | cache | df | VMM_FLAG_NOEXEC);
}

// Allocate a frame for page 'pg' of VMA 'v' and fill it (file content + zero /
// pure zero). Touches no page table. Returns the frame or NULL.
static void* vma_fill_frame(const vma_t* v, uint64_t pg) {
    void* frame = pmm_alloc_frame();
    if (!frame) return 0;
    uint64_t fphys = (uint64_t)frame & ADDRESS_MASK;
    // Fill the frame through the physmap (a kernel RW alias), so a code page can
    // be content-filled even though its USER mapping will be read-only (RX).
    uint8_t* dst = (uint8_t*)phys_to_virt(fphys);

    if (v->type == VMA_TYPE_FILE && v->file_inode) {
        // [M20] Page-cache-backed mmap (MAP_PRIVATE): copy the file's cached page
        // into this private frame. The cache provides shared, coherent bytes; the
        // process gets its own writable copy.
        // [M50 B2] Copied out by the cache itself (under its lock): a cache
        // frame may be evicted by another CPU as soon as the lock drops.
        uint64_t foff = v->file_voff + (pg - v->start);
        if (pagecache_copy_page(v->file_inode, foff, dst) != 0)
            vma_memset(dst, 0, 0x1000);
    } else if (v->type == VMA_TYPE_FILE && v->file_base) {
        // File content occupies region offsets [file_pad, file_pad+file_len).
        // Intersect it with this page [poff, poff+0x1000) and copy the overlap;
        // zero everything else (leading page padding + BSS tail).
        uint64_t poff = pg - v->start;          // offset of this page within region
        uint64_t cstart = v->file_pad;
        uint64_t cend   = v->file_pad + v->file_len;
        uint64_t ov_s = poff > cstart ? poff : cstart;
        uint64_t ov_e = (poff + 0x1000) < cend ? (poff + 0x1000) : cend;
        vma_memset(dst, 0, 0x1000);             // base: zero the whole page
        if (ov_s < ov_e) {
            uint64_t copy_len = ov_e - ov_s;
            uint64_t dst_off  = ov_s - poff;
            uint64_t src_off  = v->file_off + (ov_s - cstart);
            vma_memcpy(dst + dst_off, v->file_base + src_off, copy_len);
        }
    } else {
        vma_memset(dst, 0, 0x1000);             // ANON: zero-fill
    }
    return frame;
}

int vma_fault_in(vmm_space_t* space, const vma_t* v, uint64_t addr) {
    if (!space || !v) return -1;
    uint64_t pg = addr & ~0xFFFULL;
    if (pg < v->start || pg >= v->end) return -2;
    if (v->flags & VMM_FLAG_PROTNONE) return -4;   // reserved, inaccessible
    if (v->type == VMA_TYPE_PHYS) return vma_map_phys_page(space, v, pg);

    void* frame = vma_fill_frame(v, pg);
    if (!frame) return -3;
    int r = vmm_map_in_space(space, pg, (uint64_t)frame & ADDRESS_MASK, v->flags);
    if (r != 0) { pmm_free_frame(frame); return r; }
    return 0;
}

static int vma_same(const vma_t* a, const vma_t* b) {
    return a->start == b->start && a->end == b->end && a->flags == b->flags &&
           a->type == b->type && a->file_base == b->file_base &&
           a->file_off == b->file_off && a->file_pad == b->file_pad &&
           a->file_len == b->file_len && a->file_inode == b->file_inode &&
           a->file_voff == b->file_voff && a->phys_base == b->phys_base;
}

int vma_fault_in_live(vmm_space_t* space, vma_set_t* set, spinlock_t* mm, uint64_t addr) {
    if (!space || !set || !mm) return -1;
    uint64_t pg = addr & ~0xFFFULL;
    uint64_t fl = spin_lock_irqsave(mm);   // [M52] mm_lock is irqsave (kernel/process.h)
    const vma_t* v = vma_find(set, pg);
    if (!v || (v->flags & VMM_FLAG_PROTNONE)) { spin_unlock_irqrestore(mm, fl); return -1; }
    if (v->type == VMA_TYPE_PHYS) {             // nothing to fill: install now
        int r = vma_map_phys_page(space, v, pg);
        spin_unlock_irqrestore(mm, fl);
        return (r == -3) ? 0 : r;
    }
    vma_t snap = *v;
    spin_unlock_irqrestore(mm, fl);

    void* frame = vma_fill_frame(&snap, pg);   // may read the page cache: unlocked
    if (!frame) return -3;

    fl = spin_lock_irqsave(mm);
    v = vma_find(set, pg);
    int r;
    if (!v || !vma_same(v, &snap)) {
        r = -2;                                 // unmapped or changed meanwhile
    } else {
        r = vmm_map_in_space(space, pg, (uint64_t)frame & ADDRESS_MASK, snap.flags);
        if (r == -3) r = 1;                     // installed by someone else: fine
    }
    spin_unlock_irqrestore(mm, fl);
    if (r != 0) pmm_free_frame(frame);
    return r > 0 ? 0 : r;
}
