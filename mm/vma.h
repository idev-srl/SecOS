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
struct vfs_inode;

#define VMA_MAX        64
#define VMA_TYPE_ANON  0
#define VMA_TYPE_FILE  1
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
    // bytes from pagecache_get_phys(file_inode, file_voff + (pg - start)).
    struct vfs_inode* file_inode;
    uint64_t file_voff;    // file offset corresponding to 'start'
} vma_t;

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
// [M18] Does any live VMA overlap [start,end)?
int vma_overlaps(const vma_set_t* s, uint64_t start, uint64_t end);
// [M18] Sum of live VMA sizes (reserved virtual footprint, for max_mem checks).
uint64_t vma_total_bytes(const vma_set_t* s);

// [M20] Register a file-backed (page-cache) region for mmap. 'voff' is the file
// offset mapped at 'start'. Returns 0/-1.
int vma_add_file(vma_set_t* s, uint64_t start, uint64_t end, uint64_t flags,
                 struct vfs_inode* inode, uint64_t voff);

// Materialize the page containing 'addr' in 'space' according to VMA 'v':
// allocate a frame, fill it (file content + zero / pure zero), map it.
// Returns 0 on success, negative on failure.
int vma_fault_in(vmm_space_t* space, const vma_t* v, uint64_t addr);

#endif // VMA_H
