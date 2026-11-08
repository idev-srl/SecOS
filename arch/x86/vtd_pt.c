/*
 * vtd_pt.c — VT-d second-level page tables (Phase 5, M54).
 *
 * Same shape as the CPU's 4-level tables: 512 entries of 8 bytes per 4 KiB
 * table, 9 bits of IOVA per level, R (bit 0) and W (bit 1) permissions, PS
 * (bit 7) for a 2 MiB / 1 GiB leaf. Non-leaf entries carry R|W, so the leaf is
 * the only protection authority (as the CPU tables since M19).
 *
 * Every table page is reached through the physmap (phys_to_virt) and comes
 * from vtd_table_alloc() (zeroed, below the host address width). Writes go
 * through vtd_flush_cache() so a unit that does not snoop its page walks
 * (ECAP.C == 0) sees them. Callers hold the domain lock (LK_S67_IOMMU_DOM);
 * the IOTLB flush is the caller's (vtd.c).
 *
 * Bounds: every walk is at most `levels` deep and every loop advances by at
 * least one page, so nothing here can spin on a bad table.
 *
 * SPDX-License-Identifier: MIT
 */
#include "vtd.h"
#include "vmm.h"
#include <stddef.h>

#define PAGE        4096ull
#define LVL_SHIFT(l) (12u + 9u * (uint32_t)((l) - 1))   /* level 1 = 4 KiB PTE */

static inline uint64_t* tbl(uint64_t phys) { return (uint64_t*)phys_to_virt(phys); }
static inline uint32_t  idx(uint64_t iova, int lvl) { return (uint32_t)((iova >> LVL_SHIFT(lvl)) & 511); }

static void set_entry(uint64_t* e, uint64_t v) {
    __atomic_store_n(e, v, __ATOMIC_RELEASE);
    vtd_flush_cache(e, 8);
}

int vtd_pt_init(vtd_dom_t* h, int levels) {
    if (levels != 3 && levels != 4) return IOMMU_EINVAL;
    uint64_t r = vtd_table_alloc();
    if (!r) return IOMMU_ENOMEM;
    h->levels = (uint8_t)levels;
    h->root = r;
    h->tables = 1;
    return 0;
}

/* The entry for `iova` at level `want` (1..levels), creating tables on the
 * way when `create`. Stops early at a superpage leaf above `want`: *hit_lvl
 * is the level of the returned entry. NULL = not present (no create) or out
 * of memory (*err). */
static uint64_t* walk(vtd_dom_t* h, uint64_t iova, int want, int create, int* err,
                      int* hit_lvl) {
    uint64_t t = h->root;
    *err = 0;
    for (int lvl = h->levels; lvl >= 1; lvl--) {
        uint64_t* e = &tbl(t)[idx(iova, lvl)];
        if (lvl == want) { *hit_lvl = lvl; return e; }
        uint64_t v = *e;
        if (v & (VTD_SL_R | VTD_SL_W)) {
            if (v & VTD_SL_PS) { *hit_lvl = lvl; return e; }
            t = v & VTD_SL_ADDR;
            continue;
        }
        if (!create) return NULL;
        uint64_t n = vtd_table_alloc();
        if (!n) { *err = IOMMU_ENOMEM; return NULL; }
        h->tables++;
        set_entry(e, n | VTD_SL_R | VTD_SL_W);
        t = n;
    }
    return NULL;
}

uint64_t vtd_pt_translate(vtd_dom_t* h, uint64_t iova, int* prot) {
    if (prot) *prot = 0;
    if (!h->root) return (uint64_t)-1;
    uint64_t t = h->root;
    for (int lvl = h->levels; lvl >= 1; lvl--) {
        uint64_t v = tbl(t)[idx(iova, lvl)];
        if (!(v & (VTD_SL_R | VTD_SL_W))) return (uint64_t)-1;
        if (lvl == 1 || (v & VTD_SL_PS)) {
            uint64_t span = 1ull << LVL_SHIFT(lvl);
            if (prot) *prot = (int)(((v & VTD_SL_R) ? IOMMU_READ : 0) | ((v & VTD_SL_W) ? IOMMU_WRITE : 0));
            return (v & VTD_SL_ADDR & ~(span - 1)) | (iova & (span - 1));
        }
        t = v & VTD_SL_ADDR;
    }
    return (uint64_t)-1;
}

static uint64_t leaf_bits(int prot) {
    return ((prot & IOMMU_READ) ? VTD_SL_R : 0) | ((prot & IOMMU_WRITE) ? VTD_SL_W : 0);
}

/* Clear [iova, iova+len) leaves without any check (rollback of a failed map,
 * whose leaves are all 4 KiB or superpages it created itself). */
static void clear_range(vtd_dom_t* h, uint64_t iova, uint64_t len) {
    uint64_t end = iova + len;
    while (iova < end) {
        uint64_t t = h->root, step = PAGE;
        for (int lvl = h->levels; lvl >= 1; lvl--) {
            uint64_t* e = &tbl(t)[idx(iova, lvl)];
            uint64_t v = *e;
            if (!(v & (VTD_SL_R | VTD_SL_W))) { step = 1ull << LVL_SHIFT(lvl); break; }
            if (lvl == 1 || (v & VTD_SL_PS)) { set_entry(e, 0); step = 1ull << LVL_SHIFT(lvl); break; }
            t = v & VTD_SL_ADDR;
        }
        uint64_t next = (iova & ~(step - 1)) + step;
        if (next <= iova) break;
        iova = next;
    }
}

int vtd_pt_map(vtd_dom_t* h, uint64_t iova, uint64_t phys, uint64_t len,
               int prot, uint32_t huge, int idem) {
    if (!h->root || !len || ((iova | phys | len) & (PAGE - 1))) return IOMMU_EINVAL;
    if (iova + len < iova || phys + len < phys) return IOMMU_EINVAL;
    if (!(prot & (IOMMU_READ | IOMMU_WRITE))) return IOMMU_EINVAL;
    uint64_t bits = leaf_bits(prot);
    uint64_t done = 0;
    int rc = 0;
    while (done < len) {
        uint64_t va = iova + done, pa = phys + done, left = len - done;
        int lvl = 1;
        if ((huge & 2) && h->levels >= 3 && !((va | pa) & ((1ull << 30) - 1)) && left >= (1ull << 30)) lvl = 3;
        else if ((huge & 1) && !((va | pa) & ((1ull << 21) - 1)) && left >= (1ull << 21)) lvl = 2;
        int err = 0, hit = 0;
        uint64_t* e;
        for (;;) {
            e = walk(h, va, lvl, 1, &err, &hit);
            if (!e) break;
            /* A table where a superpage would go: use a smaller page. */
            if (hit == lvl && lvl > 1 && (*e & (VTD_SL_R | VTD_SL_W)) && !(*e & VTD_SL_PS)) { lvl--; continue; }
            break;
        }
        if (!e) { rc = err ? err : IOMMU_ENOMEM; break; }
        if (hit != lvl || (*e & (VTD_SL_R | VTD_SL_W))) {
            /* Occupied by a leaf (here or a superpage above). */
            int p = 0;
            if (idem && vtd_pt_translate(h, va, &p) == pa && (p & prot) == prot) {
                uint64_t s = 1ull << LVL_SHIFT(hit);
                done += ((va & ~(s - 1)) + s) - va;
                continue;
            }
            rc = IOMMU_EEXIST;
            break;
        }
        set_entry(e, pa | bits | (lvl > 1 ? VTD_SL_PS : 0));
        done += 1ull << LVL_SHIFT(lvl);
    }
    if (rc && !idem && done) clear_range(h, iova, done);
    return rc;
}

int vtd_pt_unmap(vtd_dom_t* h, uint64_t iova, uint64_t len, uint64_t* cleared) {
    if (cleared) *cleared = 0;
    if (!h->root || !len || ((iova | len) & (PAGE - 1)) || iova + len < iova) return IOMMU_EINVAL;
    uint64_t end = iova + len;
    /* Pass 1: refuse a superpage that sticks out of the range. */
    for (uint64_t va = iova; va < end; ) {
        uint64_t t = h->root, step = PAGE;
        for (int lvl = h->levels; lvl >= 1; lvl--) {
            uint64_t v = tbl(t)[idx(va, lvl)];
            if (!(v & (VTD_SL_R | VTD_SL_W))) { step = 1ull << LVL_SHIFT(lvl); break; }
            if (lvl == 1 || (v & VTD_SL_PS)) {
                step = 1ull << LVL_SHIFT(lvl);
                uint64_t base = va & ~(step - 1);
                if (base < iova || base + step > end) return IOMMU_EINVAL;
                break;
            }
            t = v & VTD_SL_ADDR;
        }
        uint64_t next = (va & ~(step - 1)) + step;
        if (next <= va) break;
        va = next;
    }
    /* Pass 2: clear. Empty tables are kept until the domain is freed. */
    uint64_t n = 0;
    for (uint64_t va = iova; va < end; ) {
        uint64_t t = h->root, step = PAGE;
        for (int lvl = h->levels; lvl >= 1; lvl--) {
            uint64_t* e = &tbl(t)[idx(va, lvl)];
            uint64_t v = *e;
            if (!(v & (VTD_SL_R | VTD_SL_W))) { step = 1ull << LVL_SHIFT(lvl); break; }
            if (lvl == 1 || (v & VTD_SL_PS)) {
                step = 1ull << LVL_SHIFT(lvl);
                set_entry(e, 0);
                n += step / PAGE;
                break;
            }
            t = v & VTD_SL_ADDR;
        }
        uint64_t next = (va & ~(step - 1)) + step;
        if (next <= va) break;
        va = next;
    }
    if (cleared) *cleared = n;
    return 0;
}

static void free_level(vtd_dom_t* h, uint64_t t, int lvl) {
    if (lvl > 1) {
        uint64_t* e = tbl(t);
        for (int i = 0; i < 512; i++) {
            uint64_t v = e[i];
            if ((v & (VTD_SL_R | VTD_SL_W)) && !(v & VTD_SL_PS))
                free_level(h, v & VTD_SL_ADDR, lvl - 1);
        }
    }
    vtd_table_free(t);
    if (h->tables) h->tables--;
}

void vtd_pt_free(vtd_dom_t* h) {
    if (h->root) free_level(h, h->root, h->levels);
    h->root = 0;
}
