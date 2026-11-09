/*
 * vma.h — Per-process Virtual Memory Areas (demand paging).
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * A VMA describes a contiguous, page-aligned virtual range that is NOT eagerly
 * mapped: its pages are materialized one at a time by the #PF handler on first
 * touch (demand paging). Two kinds:
 *   - VMA_TYPE_ANON: zero-fill (user stack, BSS tail, future heap).
 *   - VMA_TYPE_FILE: backed by a pinned ELF image; the faulting page is filled
 *     from the image (and zero-padded past the file content -> BSS).
 * The set lives in the owning process_t; teardown is automatic because
 * vmm_space_destroy() frees every present leaf frame in the user range.
 */
#ifndef VMA_H
#define VMA_H
#include <stdint.h>
#include "vmm.h"
#include "spinlock.h"   // [M50-B1] the owner's mm_lock
struct vfs_inode;

#define VMA_MAX        256   // [U9] two per thread stack (guard + stack); U4a makes it dynamic
#define VMA_TYPE_ANON  0
#define VMA_TYPE_FILE  1
#define VMA_TYPE_PHYS  2    // [M40] device memory (framebuffer): phys_base + (va - start), shared
#define VMA_TYPE_NONE  255  // [M18] tombstone: a removed slot (kept so indices are stable)

typedef struct vma {
    uint64_t start;        // page-aligned, inclusive
    uint64_t end;          // page-aligned, exclusive
    uint64_t flags;        // leaf PTE flags (USER + RW/NX, or code RX)
    uint8_t  type;         // VMA_TYPE_ANON | VMA_TYPE_FILE
    // FILE backing. File content occupies region offsets [file_pad, file_pad+file_len);
    // everything outside that window (leading page padding + BSS tail) is zero.
    const uint8_t* file_base;  // pinned image base (M14 ELF image), or NULL
    uint64_t file_off;     // image offset of the first content byte (= p_offset)
    uint64_t file_pad;     // region offset where content begins (= vaddr & 0xFFF)
    uint64_t file_len;     // content length in bytes (= p_filesz)
    // [M20] Alternative FILE backing: a VFS inode served through the page cache
    // (file-backed mmap, MAP_PRIVATE). When file_inode != NULL the fault sources
    // bytes from pagecache_copy_page(file_inode, file_voff + (pg - start)).
    struct vfs_inode* file_inode;
    uint64_t file_voff;    // file offset corresponding to 'start'
    // [M40] VMA_TYPE_PHYS: physical address mapped at 'start'. The frames are
    // device memory, not PMM-owned: never allocated, refcounted or freed.
    uint64_t phys_base;
    // [M57] VMA_VF_* (zero for every region but the ring-3 driver mappings).
    uint8_t  vflags;
} vma_t;

// [M57] vma_t.vflags. A ring-3 driver's BAR and DMA mappings (kernel/udrv.c)
// are VMA_TYPE_PHYS regions the kernel owns: never inherited by fork (the child
// gets a hole), never unmapped or re-protected by the process (munmap/mprotect
// -> EINVAL), torn down only by the claim. The cache flag picks the PTE memory
// type of a PHYS region (default: Write-Combining, the M40 framebuffer).
#define VMA_VF_DONTFORK  0x01
#define VMA_VF_PINNED    0x02
#define VMA_VF_UC        0x04   // uncached (PAT slot 3): MMIO BARs
#define VMA_VF_WB        0x08   // write-back: kernel-allocated DMA frames

typedef struct vma_set {
    vma_t    v[VMA_MAX];
    uint32_t count;
} vma_set_t;

// Register a region. Returns 0 on success, -1 if the set is full.
int vma_add(vma_set_t* s, uint64_t start, uint64_t end, uint64_t flags,
            uint8_t type, const uint8_t* file_base, uint64_t file_off,
            uint64_t file_pad, uint64_t file_len);

// Locate the VMA containing 'addr', or NULL.
const vma_t* vma_find(const vma_set_t* s, uint64_t addr);

// [M18] Mutable lookup (for mprotect/munmap bookkeeping).
vma_t* vma_find_mut(vma_set_t* s, uint64_t addr);
// [M18] Mark a slot as a tombstone (removed). Indices of other slots are stable.
void vma_remove(vma_set_t* s, vma_t* v);
// Split 'v' at page-aligned 'addr' (v->start < addr < v->end): 'v' keeps
// [start, addr) and a new slot gets [addr, end) with the same type/flags and
// its backing (image window / page-cache offset) rebased. 0, or -1 if full.
int vma_split(vma_set_t* s, vma_t* v, uint64_t addr);
// [U8] Grow a live anonymous VMA that ends exactly at 'start' and has these
// leaf flags (and no M57 vflags) to cover [start, end): an anonymous mmap
// placed right after the previous one uses no new slot, as Linux merges
// adjacent anonymous mappings. 0 if merged, -1 if no such VMA.
int vma_extend_anon(vma_set_t* s, uint64_t start, uint64_t end, uint64_t flags);
// [U8] Slots a vma_add can still take (tombstones + never used).
uint32_t vma_free_slots(const vma_set_t* s);
// [M18] Does any live VMA overlap [start,end)?
int vma_overlaps(const vma_set_t* s, uint64_t start, uint64_t end);
// [M57] Does any live VMA_VF_PINNED region overlap [start,end)?
int vma_pinned_overlap(const vma_set_t* s, uint64_t start, uint64_t end);
// [M18] Sum of live VMA sizes (reserved virtual footprint, for max_mem checks).
uint64_t vma_total_bytes(const vma_set_t* s);

// [M20] Register a file-backed (page-cache) region for mmap. 'voff' is the file
// offset mapped at 'start'. Returns 0/-1.
int vma_add_file(vma_set_t* s, uint64_t start, uint64_t end, uint64_t flags,
                 struct vfs_inode* inode, uint64_t voff);

// [M40] Register a device-memory region (VMA_TYPE_PHYS): [start, end) maps the
// physical range starting at 'phys' (page-aligned). Returns 0/-1.
int vma_add_phys(vma_set_t* s, uint64_t start, uint64_t end, uint64_t flags, uint64_t phys);

// Materialize the page containing 'addr' in 'space' according to VMA 'v':
// allocate a frame, fill it (file content + zero / pure zero), map it.
// Returns 0 on success, negative on failure. [M50-B1] No locking: for a space
// nobody else can see yet (process creation, exec staging) or a caller that
// holds the owner's mm_lock and knows the fill does not reach the page cache.
int vma_fault_in(vmm_space_t* space, const vma_t* v, uint64_t addr);

// [M50-B1] Demand-fault 'addr' of a LIVE address space (the #PF path): 'set'
// and 'space' belong to a process whose mm_lock is 'mm'. The VMA is looked up
// and copied under the lock, the frame is allocated and filled WITHOUT it
// (a page-cache fill may call into a filesystem), then the lock is retaken,
// the VMA re-checked (still there, unchanged) and the PTE installed. A page
// some other path installed meanwhile counts as success. Must be called
// without 'mm' held. Returns 0 on success (also when the VMA changed meanwhile:
// the access is retried and faults again), negative when not serviceable.
int vma_fault_in_live(vmm_space_t* space, vma_set_t* set, spinlock_t* mm, uint64_t addr);

#endif // VMA_H
