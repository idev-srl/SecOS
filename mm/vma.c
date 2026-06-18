/*
 * vma.c — Per-process Virtual Memory Areas (demand paging).
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "vma.h"
#include "pmm.h"
#include "vmm.h"

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
    v->file_base = file_base;
    v->file_off = file_off;
    v->file_pad = file_pad;
    v->file_len = file_len;
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

int vma_overlaps(const vma_set_t* s, uint64_t start, uint64_t end) {
    if (!s) return 0;
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->v[i].type == VMA_TYPE_NONE) continue;
        if (start < s->v[i].end && s->v[i].start < end) return 1;
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

int vma_fault_in(vmm_space_t* space, const vma_t* v, uint64_t addr) {
    if (!space || !v) return -1;
    uint64_t pg = addr & ~0xFFFULL;
    if (pg < v->start || pg >= v->end) return -2;

    void* frame = pmm_alloc_frame();
    if (!frame) return -3;
    uint64_t fphys = (uint64_t)frame & ADDRESS_MASK;
    // Fill the frame through the physmap (a kernel RW alias), so a code page can
    // be content-filled even though its USER mapping will be read-only (RX).
    uint8_t* dst = (uint8_t*)phys_to_virt(fphys);

    if (v->type == VMA_TYPE_FILE && v->file_base) {
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

    int r = vmm_map_in_space(space, pg, fphys, v->flags);
    if (r != 0) { pmm_free_frame(frame); return r; }
    return 0;
}
