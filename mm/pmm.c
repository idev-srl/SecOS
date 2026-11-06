/*

 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
/*
 * Physical memory manager.
 *
 * [M49] Scales from a few MiB to the 64 TiB physmap limit. Two phases:
 *
 *   1. EARLY (pmm_init*() .. pmm_late_init()): the firmware memory map is
 *      parsed into sorted, merged range lists (usable RAM, RAM-like memory the
 *      physmap must cover, reserved ranges). Frames are handed out by a bump
 *      allocator over the usable ranges ("memblock"); nothing is ever freed.
 *      Early frames are kept below 512 MiB because the page-table code reaches
 *      them through the boot identity map until the physmap is up.
 *
 *   2. SECTIONED (after pmm_late_init(), which runs once the physmap maps all
 *      RAM): physical memory is split into 1 GiB sections. A section that holds
 *      usable RAM gets a descriptor (~33 KiB) with a 1-bit-per-frame bitmap and a
 *      64-bit summary bitmap ("this bitmap word is full"), so allocation skips
 *      full words 64 at a time and full sections via a per-section free count.
 *      Sections without RAM (holes, MMIO windows) cost one NULL pointer. All
 *      metadata lives in the physmap, allocated from RAM by the early allocator.
 *
 * COW refcounts (one byte per frame, extra references beyond the owner) are
 * allocated lazily per 16 MiB chunk on the first pmm_share_frame() in it and
 * released when its last non-zero count drops, so memory that is not shared
 * (the kernel, the page cache, most of RAM) costs nothing. A count that
 * reaches 255 sticks: the frame is then never freed (a leak instead of a
 * use-after-free).
 *
 * Zones: frames below 4 GiB are the DMA32 zone. Ordinary allocations prefer
 * frames at or above 4 GiB and fall back to DMA32 only when that is exhausted;
 * pmm_alloc_below()/pmm_alloc_contiguous_below() serve devices with a 32-bit
 * (or narrower) DMA reach.
 */
#include "pmm.h"
#include "multiboot.h"
#include "multiboot2.h"
#include "terminal.h"
#include "debugcon.h"
#include "vmm.h"       // phys_to_virt / kvirt_to_phys
#include "spinlock.h"  // [M29] SMP: serialize the allocator across CPUs

extern void print_hex(uint64_t value);

// [M29] One lock guards the whole allocator. Uncontended on the single-core path.
static spinlock_t pmm_lock = SPINLOCK_INIT;

// ---- geometry ----
#define PMM_SECTION_SHIFT   18                              // 2^18 frames = 1 GiB
#define PMM_SECTION_FRAMES  (1ULL << PMM_SECTION_SHIFT)
#define PMM_SECTION_BYTES   (PMM_SECTION_FRAMES * PMM_FRAME_SIZE)
#define PMM_SECTION_WORDS   (PMM_SECTION_FRAMES / 64)       // 4096 bitmap words
#define PMM_SUMMARY_WORDS   (PMM_SECTION_WORDS / 64)        // 64 summary words
#define PMM_REF_CHUNK_FRAMES 4096ULL                        // 1 byte/frame -> 4 KiB chunk
#define PMM_REF_CHUNKS      (PMM_SECTION_FRAMES / PMM_REF_CHUNK_FRAMES) // 64
#define PMM_REF_STICKY      0xFF

#define PMM_DMA32_LIMIT     0x100000000ULL                  // 4 GiB
#define PMM_EARLY_LIMIT     0x20000000ULL                   // 512 MiB (boot identity map)
#define PMM_MAX_RANGES      256
#define PMM_MAX_RESERVED    16

typedef struct pmm_section {
    uint32_t free;                          // free frames in this section
    uint32_t hint;                          // bitmap word to resume scanning at
    uint8_t* refs[PMM_REF_CHUNKS];          // lazily allocated COW refcount chunks
    uint16_t refs_used[PMM_REF_CHUNKS];     // non-zero counts per chunk (0 = free the chunk)
    uint64_t summary[PMM_SUMMARY_WORDS];    // bit w set = bitmap word w is full
    uint64_t bitmap[PMM_SECTION_WORDS];     // bit set = frame used
} pmm_section_t;

typedef struct { uint64_t start, end; } pmm_range_t;   // [start, end), bytes

// ---- memory map (sorted, merged) ----
static pmm_range_t g_avail[PMM_MAX_RANGES];     // usable RAM (page-aligned inward)
static int         g_navail = 0;
static uint64_t    g_early_cur[PMM_MAX_RANGES]; // early bottom-up cursor per usable range
static uint64_t    g_early_top[PMM_MAX_RANGES]; // early top-down cursor per usable range
static pmm_range_t g_ram[PMM_MAX_RANGES];       // RAM-like memory for the physmap (outward)
static int         g_nram = 0;
static pmm_range_t g_resv[PMM_MAX_RESERVED];    // never handed out
static int         g_nresv = 0;

// ---- sectioned allocator state ----
static pmm_section_t** g_sec = NULL;            // physmap pointers, NULL = no RAM
static uint64_t g_nsec = 0;
static int      g_sectioned = 0;                // 1 after pmm_late_init()
static uint64_t g_hint_normal = 0;              // rolling section hint (>= 4 GiB)
static uint64_t g_hint_dma32 = 0;               // rolling section hint (< 4 GiB)

// ---- accounting ----
static uint64_t total_memory = 0;               // bytes of usable RAM (MemTotal)
static uint64_t g_managed_frames = 0;           // usable frames (sum of g_avail)
static uint64_t g_free_frames = 0;              // free frames (sectioned phase)
static uint64_t g_early_used = 0;               // frames consumed by the early allocator
static uint64_t max_phys_addr_seen = 0;         // end of the highest usable range
static uint64_t g_meta_bytes = 0;               // section array + descriptors
static uint64_t g_ref_chunks = 0;               // refcount chunks allocated

extern uint32_t _kernel_end;

// ------------------------------------------------------------------ helpers --

static void itoa_dec(uint64_t value, char* buffer) {
    if (value == 0) { buffer[0] = '0'; buffer[1] = '\0'; return; }
    char temp[32]; int i = 0;
    while (value > 0) { temp[i++] = (char)('0' + (value % 10)); value /= 10; }
    int j = 0;
    while (i > 0) buffer[j++] = temp[--i];
    buffer[j] = '\0';
}
static void pmm_dec(uint64_t v) { char b[32]; itoa_dec(v, b); terminal_writestring(b); }

// Freestanding popcount (no libgcc helper, no POPCNT assumption).
static inline uint64_t popcount64(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (x * 0x0101010101010101ULL) >> 56;
}

static inline uint64_t align_up(uint64_t v, uint64_t a)   { return (v + a - 1) & ~(a - 1); }
static inline uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }

// Insert [start,end) into a sorted range list, merging overlapping/adjacent ranges.
static void range_add(pmm_range_t* list, int* n, int max, uint64_t start, uint64_t end) {
    if (end <= start) return;
    int i = 0;
    while (i < *n && list[i].end < start) i++;
    if (i < *n && list[i].start <= end) {
        // Overlaps or touches list[i]: grow it, then absorb any followers.
        if (start < list[i].start) list[i].start = start;
        if (end > list[i].end) list[i].end = end;
        int j = i + 1;
        while (j < *n && list[j].start <= list[i].end) {
            if (list[j].end > list[i].end) list[i].end = list[j].end;
            j++;
        }
        int removed = j - (i + 1);
        for (int k = i + 1; k + removed < *n; k++) list[k] = list[k + removed];
        *n -= removed;
        return;
    }
    if (*n >= max) {
        debugcon_writestring("[PMM][WARN] range table full, dropping a memory range\n");
        return;
    }
    for (int k = *n; k > i; k--) list[k] = list[k - 1];
    list[i].start = start; list[i].end = end;
    (*n)++;
}

void pmm_add_ram(uint64_t addr, uint64_t len) {
    if (len == 0) return;
    range_add(g_ram, &g_nram, PMM_MAX_RANGES, align_down(addr, PMM_FRAME_SIZE),
              align_up(addr + len, PMM_FRAME_SIZE));
}
static void pmm_add_avail(uint64_t addr, uint64_t len) {
    if (len == 0) return;
    uint64_t s = align_up(addr, PMM_FRAME_SIZE), e = align_down(addr + len, PMM_FRAME_SIZE);
    if (e <= s) return;
    range_add(g_avail, &g_navail, PMM_MAX_RANGES, s, e);
    pmm_add_ram(s, e - s);
}
void pmm_reserve(uint64_t addr, uint64_t len) {
    if (len == 0) return;
    if (g_nresv >= PMM_MAX_RESERVED) { debugcon_writestring("[PMM][WARN] reserved table full\n"); return; }
    g_resv[g_nresv].start = align_down(addr, PMM_FRAME_SIZE);
    g_resv[g_nresv].end   = align_up(addr + len, PMM_FRAME_SIZE);
    g_nresv++;
}

// First reserved range overlapping [s,e), or NULL.
static const pmm_range_t* resv_overlap(uint64_t s, uint64_t e) {
    for (int i = 0; i < g_nresv; i++)
        if (g_resv[i].start < e && s < g_resv[i].end) return &g_resv[i];
    return NULL;
}

// ------------------------------------------------------- early allocator ----

// Bump-allocate `size` bytes aligned to `align` from usable RAM, bottom-up,
// ending at or below `limit`. Returns the physical address or 0. Never freed.
// Used for the page tables built before the physmap exists (identity-reachable).
static uint64_t early_alloc(uint64_t size, uint64_t align, uint64_t limit) {
    for (int i = 0; i < g_navail; i++) {
        uint64_t cur = align_up(g_early_cur[i], align);
        for (;;) {
            uint64_t end = cur + size;
            if (end > g_early_top[i] || end > limit) break;
            const pmm_range_t* r = resv_overlap(cur, end);
            if (r) { cur = align_up(r->end, align); continue; }
            uint64_t from = align_up(g_early_cur[i], PMM_FRAME_SIZE);
            if (align_down(cur, PMM_FRAME_SIZE) > from) from = align_down(cur, PMM_FRAME_SIZE);
            g_early_used += (align_up(end, PMM_FRAME_SIZE) - from) / PMM_FRAME_SIZE;
            g_early_cur[i] = end;
            return cur;
        }
    }
    return 0;
}

// Top-down variant (highest usable range first, from its top): the PMM's own
// metadata goes high, leaving the DMA32 zone to the devices that need it.
static uint64_t early_alloc_top(uint64_t size, uint64_t align) {
    for (int i = g_navail - 1; i >= 0; i--) {
        uint64_t top = g_early_top[i];
        for (;;) {
            if (top < size) break;
            uint64_t s = align_down(top - size, align);
            if (s < g_early_cur[i] || s < g_avail[i].start) break;
            const pmm_range_t* r = resv_overlap(s, s + size);
            if (r) { top = r->start; continue; }
            uint64_t to = align_down(g_early_top[i], PMM_FRAME_SIZE);
            if (align_up(s + size, PMM_FRAME_SIZE) < to) to = align_up(s + size, PMM_FRAME_SIZE);
            g_early_used += (to - align_down(s, PMM_FRAME_SIZE)) / PMM_FRAME_SIZE;
            g_early_top[i] = s;
            return s;
        }
    }
    return 0;
}

// --------------------------------------------------------- section bitmap ----

static inline pmm_section_t* sec_of(uint64_t frame) {
    uint64_t s = frame >> PMM_SECTION_SHIFT;
    return (s < g_nsec) ? g_sec[s] : NULL;
}

static inline void sec_summary_update(pmm_section_t* s, uint64_t w) {
    uint64_t bit = 1ULL << (w & 63);
    if (s->bitmap[w] == ~0ULL) s->summary[w >> 6] |= bit;
    else                       s->summary[w >> 6] &= ~bit;
}

// Mark frames [lo, hi) of a section (section-relative indices) free (set=0) or
// used (set=1). Returns the number of frames whose state changed.
static uint64_t sec_mark(pmm_section_t* s, uint64_t lo, uint64_t hi, int set) {
    uint64_t changed = 0;
    while (lo < hi) {
        uint64_t w = lo >> 6, b = lo & 63;
        uint64_t n = 64 - b;
        if (n > hi - lo) n = hi - lo;
        uint64_t mask = (n == 64) ? ~0ULL : (((1ULL << n) - 1) << b);
        uint64_t old = s->bitmap[w];
        uint64_t nw = set ? (old | mask) : (old & ~mask);
        changed += popcount64(old ^ nw);
        s->bitmap[w] = nw;
        sec_summary_update(s, w);
        lo += n;
    }
    if (set) s->free -= (uint32_t)changed; else s->free += (uint32_t)changed;
    return changed;
}

// Mark the physical frames [f_lo, f_hi) free/used across sections; keeps
// g_free_frames in sync. Frames in sections without RAM are ignored.
static void frames_mark(uint64_t f_lo, uint64_t f_hi, int set) {
    while (f_lo < f_hi) {
        uint64_t si = f_lo >> PMM_SECTION_SHIFT;
        uint64_t sec_end = (si + 1) << PMM_SECTION_SHIFT;
        uint64_t e = f_hi < sec_end ? f_hi : sec_end;
        pmm_section_t* s = (si < g_nsec) ? g_sec[si] : NULL;
        if (s) {
            uint64_t base = si << PMM_SECTION_SHIFT;
            uint64_t c = sec_mark(s, f_lo - base, e - base, set);
            if (set) g_free_frames -= c; else g_free_frames += c;
        }
        f_lo = e;
    }
}

// Find a free frame in section `s` with section-relative index < max, scanning
// from the section hint and wrapping. Returns the index or -1.
static int64_t sec_find(pmm_section_t* s, uint64_t max) {
    if (s->free == 0) return -1;
    uint64_t max_w = (max + 63) >> 6;
    uint64_t start = (s->hint < max_w) ? s->hint : 0;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t w = pass ? 0 : start;
        uint64_t w_end = pass ? start : max_w;
        while (w < w_end) {
            uint64_t sw = s->summary[w >> 6] | ((1ULL << (w & 63)) - 1);   // ignore words < w
            if (sw == ~0ULL) { w = (w | 63) + 1; continue; }              // 64 full words
            w = (w & ~63ULL) + (uint64_t)__builtin_ctzll(~sw);
            if (w >= w_end) break;
            uint64_t f = (w << 6) + (uint64_t)__builtin_ctzll(~s->bitmap[w]);
            if (f < max) { s->hint = (uint32_t)w; return (int64_t)f; }
            break;                                                       // only the tail word is partial
        }
    }
    return -1;
}

// Allocate one frame among sections [s_lo, s_hi), frame index < f_max, using a
// rolling section hint. Returns the frame number or -1.
static int64_t alloc_in(uint64_t s_lo, uint64_t s_hi, uint64_t f_max, uint64_t* hint) {
    if (s_hi > g_nsec) s_hi = g_nsec;
    if (s_lo >= s_hi) return -1;
    uint64_t h = (hint && *hint >= s_lo && *hint < s_hi) ? *hint : s_lo;
    for (uint64_t k = 0; k < s_hi - s_lo; k++) {
        uint64_t si = h + k;
        if (si >= s_hi) si -= (s_hi - s_lo);
        pmm_section_t* s = g_sec[si];
        if (!s || s->free == 0) continue;
        uint64_t base = si << PMM_SECTION_SHIFT;
        uint64_t max = PMM_SECTION_FRAMES;
        if (f_max <= base) continue;
        if (f_max - base < max) max = f_max - base;
        int64_t f = sec_find(s, max);
        if (f < 0) continue;
        sec_mark(s, (uint64_t)f, (uint64_t)f + 1, 1);
        g_free_frames--;
        if (hint) *hint = si;
        return (int64_t)(base + (uint64_t)f);
    }
    return -1;
}

static inline uint64_t sec_count_below(uint64_t limit) {
    return (limit + PMM_SECTION_BYTES - 1) >> 30;
}

// Allocate one frame below `limit` (bytes); limit = ~0 means anywhere, which
// prefers the normal zone (>= 4 GiB) and falls back to DMA32.
static uint64_t alloc_frame_nolock_below(uint64_t limit) {
    if (!g_sectioned) {
        uint64_t lim = limit < PMM_EARLY_LIMIT ? limit : PMM_EARLY_LIMIT;
        return early_alloc(PMM_FRAME_SIZE, PMM_FRAME_SIZE, lim);
    }
    uint64_t f_max = (limit == ~0ULL) ? ~0ULL : limit / PMM_FRAME_SIZE;
    int64_t f = -1;
    uint64_t dma_secs = PMM_DMA32_LIMIT >> 30;   // 4
    if (limit > PMM_DMA32_LIMIT)
        f = alloc_in(dma_secs, sec_count_below(limit == ~0ULL ? ~0ULL >> 1 : limit), f_max, &g_hint_normal);
    if (f < 0) {
        uint64_t lim = limit < PMM_DMA32_LIMIT ? limit : PMM_DMA32_LIMIT;
        f = alloc_in(0, sec_count_below(lim), lim / PMM_FRAME_SIZE, &g_hint_dma32);
    }
    return (f < 0) ? 0 : (uint64_t)f * PMM_FRAME_SIZE;
}

static void* alloc_frame_nolock(void) {
    uint64_t p = alloc_frame_nolock_below(~0ULL);
    return p ? (void*)p : NULL;
}

// Find and claim a run of `count` free frames inside one section of
// [s_lo, s_hi) with frame index < f_max. Returns the first frame or -1.
static int64_t run_in(uint64_t s_lo, uint64_t s_hi, uint64_t f_max, uint64_t count) {
    if (s_hi > g_nsec) s_hi = g_nsec;
    for (uint64_t si = s_lo; si < s_hi; si++) {
        pmm_section_t* s = g_sec[si];
        if (!s || s->free < count) continue;
        uint64_t base = si << PMM_SECTION_SHIFT;
        if (f_max <= base) break;
        uint64_t max = PMM_SECTION_FRAMES;
        if (f_max - base < max) max = f_max - base;
        uint64_t run = 0, run_start = 0;
        for (uint64_t f = 0; f < max; ) {
            uint64_t w = f >> 6;
            if ((f & 63) == 0 && f + 64 <= max) {
                uint64_t word = s->bitmap[w];
                if (word == ~0ULL) { run = 0; f += 64; continue; }
                if (word == 0) {
                    if (run == 0) run_start = f;
                    run += 64;
                    if (run >= count) goto found;
                    f += 64;
                    continue;
                }
            }
            if (!(s->bitmap[w] & (1ULL << (f & 63)))) {
                if (run == 0) run_start = f;
                if (++run >= count) goto found;
            } else {
                run = 0;
            }
            f++;
            continue;
        found:
            sec_mark(s, run_start, run_start + count, 1);
            g_free_frames -= count;
            return (int64_t)(base + run_start);
        }
    }
    return -1;
}

static uint64_t alloc_run_nolock_below(uint64_t count, uint64_t limit) {
    if (count == 0 || count > PMM_SECTION_FRAMES) return 0;
    if (!g_sectioned) {
        uint64_t lim = limit < PMM_EARLY_LIMIT ? limit : PMM_EARLY_LIMIT;
        return early_alloc(count * PMM_FRAME_SIZE, PMM_FRAME_SIZE, lim);
    }
    uint64_t f_max = (limit == ~0ULL) ? ~0ULL : limit / PMM_FRAME_SIZE;
    int64_t f = -1;
    uint64_t dma_secs = PMM_DMA32_LIMIT >> 30;
    if (limit > PMM_DMA32_LIMIT) f = run_in(dma_secs, g_nsec, f_max, count);
    if (f < 0) {
        uint64_t lim = limit < PMM_DMA32_LIMIT ? limit : PMM_DMA32_LIMIT;
        f = run_in(0, sec_count_below(lim), lim / PMM_FRAME_SIZE, count);
    }
    return (f < 0) ? 0 : (uint64_t)f * PMM_FRAME_SIZE;
}

// -------------------------------------------------------------- building ----

// Common tail of the pmm_init* parsers: protect the kernel image, report.
static void pmm_setup(void) {
    // The kernel image (and everything below it: real-mode area, AP trampoline
    // page at 0x8000, boot page tables in .boot) is never handed out.
    uint64_t kend = kvirt_to_phys((uint64_t)&_kernel_end);
    pmm_reserve(0, kend);
    total_memory = 0; g_managed_frames = 0; max_phys_addr_seen = 0;
    for (int i = 0; i < g_navail; i++) {
        total_memory += g_avail[i].end - g_avail[i].start;
        g_early_cur[i] = g_avail[i].start;
        g_early_top[i] = g_avail[i].end;
        if (g_avail[i].end > max_phys_addr_seen) max_phys_addr_seen = g_avail[i].end;
    }
    g_managed_frames = total_memory / PMM_FRAME_SIZE;
    terminal_writestring("[PMM] usable ranges="); pmm_dec((uint64_t)g_navail);
    terminal_writestring(" ram ranges="); pmm_dec((uint64_t)g_nram);
    terminal_writestring(" max_addr="); print_hex(max_phys_addr_seen);
    terminal_writestring(" total="); pmm_dec(total_memory >> 20); terminal_writestring(" MB\n");
    for (int i = 0; i < g_navail && i < 8; i++) {
        terminal_writestring("[PMM] usable "); print_hex(g_avail[i].start);
        terminal_writestring(" - "); print_hex(g_avail[i].end); terminal_writestring("\n");
    }
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[OK] PMM early allocator ready\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

static uint64_t pmm_sections_needed(void) {
    return (max_phys_addr_seen + PMM_SECTION_BYTES - 1) >> 30;
}

void pmm_late_init(void) {
    if (g_sectioned) return;
    if (!vmm_physmap_ready()) { terminal_writestring("[PMM][ERR] late init before physmap\n"); return; }
    uint64_t t0 = 0;
    { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); t0 = ((uint64_t)hi << 32) | lo; }

    g_nsec = pmm_sections_needed();
    uint64_t arr_bytes = g_nsec * sizeof(pmm_section_t*);
    uint64_t arr_phys = early_alloc_top(arr_bytes, 64);
    if (!arr_phys) { terminal_writestring("[PMM][ERR] cannot allocate the section array\n"); return; }
    g_sec = (pmm_section_t**)phys_to_virt(arr_phys);
    for (uint64_t i = 0; i < g_nsec; i++) g_sec[i] = NULL;
    g_meta_bytes = arr_bytes;

    uint64_t nsec_present = 0;
    for (int r = 0; r < g_navail; r++) {
        uint64_t s_lo = g_avail[r].start >> 30, s_hi = (g_avail[r].end - 1) >> 30;
        for (uint64_t si = s_lo; si <= s_hi; si++) {
            if (g_sec[si]) continue;
            uint64_t p = early_alloc_top(sizeof(pmm_section_t), 64);
            if (!p) { terminal_writestring("[PMM][ERR] cannot allocate a section descriptor\n"); continue; }
            pmm_section_t* s = (pmm_section_t*)phys_to_virt(p);
            s->free = 0; s->hint = 0;
            for (uint64_t k = 0; k < PMM_REF_CHUNKS; k++) { s->refs[k] = NULL; s->refs_used[k] = 0; }
            for (uint64_t k = 0; k < PMM_SUMMARY_WORDS; k++) s->summary[k] = ~0ULL;
            for (uint64_t k = 0; k < PMM_SECTION_WORDS; k++) s->bitmap[k] = ~0ULL;
            g_sec[si] = s;
            g_meta_bytes += sizeof(pmm_section_t);
            nsec_present++;
        }
    }
    // No early allocation past this point: free every usable frame the early
    // allocator did not consume, then re-reserve the reserved ranges.
    g_free_frames = 0;
    for (int r = 0; r < g_navail; r++) {
        uint64_t s = align_up(g_early_cur[r], PMM_FRAME_SIZE);
        uint64_t e = align_down(g_early_top[r], PMM_FRAME_SIZE);
        if (s < e) frames_mark(s / PMM_FRAME_SIZE, e / PMM_FRAME_SIZE, 0);
    }
    for (int i = 0; i < g_nresv; i++)
        frames_mark(g_resv[i].start / PMM_FRAME_SIZE, g_resv[i].end / PMM_FRAME_SIZE, 1);
    g_sectioned = 1;
    g_hint_normal = PMM_DMA32_LIMIT >> 30;
    g_hint_dma32 = 0;

    uint64_t t1;
    { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); t1 = ((uint64_t)hi << 32) | lo; }

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[OK] PMM initialized\n");
    pmm_print_stats();
    // [M12] debugcon report so memory scalability is verifiable non-interactively.
    debugcon_writestring("[PMM] total_MB=");
    debugcon_print_hex(total_memory / 1024 / 1024);
    debugcon_writestring(" free_MB=");
    debugcon_print_hex(pmm_get_free_memory() / 1024 / 1024);
    debugcon_writestring(" frames=");
    debugcon_print_hex(g_managed_frames);
    debugcon_writestring("\n");
    // [M49] Metadata cost and section layout (decimal, for the bigmem gate).
    {
        char b[32];
        debugcon_writestring("[PMM] sections=");
        itoa_dec(nsec_present, b); debugcon_writestring(b);
        debugcon_writestring("/"); itoa_dec(g_nsec, b); debugcon_writestring(b);
        debugcon_writestring(" meta_KiB="); itoa_dec(g_meta_bytes >> 10, b); debugcon_writestring(b);
        debugcon_writestring(" early_frames="); itoa_dec(g_early_used, b); debugcon_writestring(b);
        debugcon_writestring(" max_addr_MiB="); itoa_dec(max_phys_addr_seen >> 20, b); debugcon_writestring(b);
        debugcon_writestring(" init_tsc="); itoa_dec(t1 - t0, b); debugcon_writestring(b);
        debugcon_writestring("\n");
    }
}

// ---------------------------------------------------------- map parsers ----

uint64_t pmm_get_max_phys(void) { return max_phys_addr_seen; }

// Initialize PMM (Multiboot1)
void pmm_init(void* mboot_info_ptr) {
    if (!mboot_info_ptr) { terminal_writestring("[ERROR] Multiboot info NULL!\n"); return; }
    struct multiboot_info* mboot = (struct multiboot_info*)mboot_info_ptr;
    pmm_reserve((uint64_t)(uintptr_t)mboot_info_ptr, sizeof(*mboot));
    if (!(mboot->flags & (1 << 6))) {
        terminal_writestring("[WARN] No MB1 memory map, synthetic fallback\n");
        uint64_t upper_kb = (mboot->flags & 1) ? mboot->mem_upper : 0;
        if (upper_kb == 0) upper_kb = 64 * 1024;
        pmm_add_avail(0x00100000ULL, upper_kb * 1024);
        pmm_setup();
        return;
    }
    pmm_reserve(mboot->mmap_addr, mboot->mmap_length);
    struct multiboot_mmap_entry* mmap = (struct multiboot_mmap_entry*)((uint64_t)mboot->mmap_addr);
    struct multiboot_mmap_entry* mmap_end = (struct multiboot_mmap_entry*)((uint64_t)mboot->mmap_addr + mboot->mmap_length);
    while (mmap < mmap_end) {
        if (mmap->type == MULTIBOOT_MEMORY_AVAILABLE) pmm_add_avail(mmap->addr, mmap->len);
        else if (mmap->type == 3 || mmap->type == 4) pmm_add_ram(mmap->addr, mmap->len); // ACPI reclaim/NVS
        mmap = (struct multiboot_mmap_entry*)((uint64_t)mmap + mmap->size + sizeof(mmap->size));
    }
    if (g_navail == 0) pmm_add_avail(0x00100000ULL, 63ULL * 1024 * 1024);
    pmm_setup();
}

// Initialize PMM (Multiboot2)
void pmm_init_mb2(void* mb2_info_ptr) {
    if (!mb2_info_ptr) { terminal_writestring("[ERROR] MB2 info NULL!\n"); return; }
    uint8_t* base = (uint8_t*)mb2_info_ptr;
    uint32_t total_size = *(uint32_t*)base;
    // The MB2 info block is read again in phase 2 (framebuffer, ACPI RSDP):
    // never hand its frames out.
    pmm_reserve((uint64_t)(uintptr_t)mb2_info_ptr, total_size);
    uint8_t* ptr = base + 8; // skip size + reserved
    while (ptr < base + total_size) {
        struct multiboot2_tag* tag = (struct multiboot2_tag*)ptr;
        if (tag->type == 0) break; // end
        if (tag->type == 6) { // mmap
            struct multiboot2_tag_mmap* mtag = (struct multiboot2_tag_mmap*)tag;
            uint8_t* entry_ptr = (uint8_t*)mtag + sizeof(struct multiboot2_tag_mmap);
            uint8_t* entry_end = (uint8_t*)mtag + mtag->size;
            while (entry_ptr + mtag->entry_size <= entry_end) {
                struct multiboot2_mmap_entry* e = (struct multiboot2_mmap_entry*)entry_ptr;
                if (e->type == 1) pmm_add_avail(e->addr, e->len);             // available
                else if (e->type == 3 || e->type == 4) pmm_add_ram(e->addr, e->len); // ACPI reclaim/NVS
                entry_ptr += mtag->entry_size;
            }
        }
        ptr += (tag->size + 7) & ~7;
    }
    if (g_navail == 0) {
        terminal_writestring("[WARN] No MB2 regions reported, synthetic fallback 1MB..64MB\n");
        pmm_add_avail(0x00100000ULL, 63ULL * 1024 * 1024);
    } else if (g_navail == 1 && g_avail[0].end <= 0x100000) {
        terminal_writestring("[WARN] Only low memory in mmap; adding synthetic region 1MB..256MB\n");
        pmm_add_avail(0x00100000ULL, 255ULL * 1024 * 1024);
    }
    pmm_setup();
}

// Initialize PMM (UEFI) from the EFI_MEMORY_DESCRIPTOR array handed over by the
// loader. Layout: uint32 Type; uint64 PhysicalStart @8; VirtualStart @16;
// NumberOfPages @24; Attribute @32.
void pmm_init_uefi(void* mem_descs, uint64_t desc_count, uint64_t desc_size, uint64_t desc_version) {
    (void)desc_version;
    if (!mem_descs || desc_count == 0 || desc_size < 32) {
        terminal_writestring("[UEFI][PMM][WARN] invalid memory map, synthetic fallback 1MB..128MB\n");
        pmm_add_avail(0x00100000ULL, 127ULL * 1024 * 1024);
        pmm_setup();
        return;
    }
    uint8_t* base = (uint8_t*)mem_descs;
    for (uint64_t i = 0; i < desc_count; i++) {
        uint8_t* d = base + i * desc_size;
        uint32_t type = *(uint32_t*)d;
        uint64_t start = *(uint64_t*)(d + 8);
        uint64_t len = *(uint64_t*)(d + 24) * 4096ULL;
        if (type == 7) {                         // EfiConventionalMemory: usable
            pmm_add_avail(start, len);
        } else if ((type >= 1 && type <= 6) ||   // loader/boot-services/runtime code+data
                   type == 9 || type == 10 ||    // ACPI reclaim / ACPI NVS
                   type == 14) {                 // persistent memory
            pmm_add_ram(start, len);             // RAM the physmap covers (WB), not handed out
        }
    }
    if (g_navail == 0) {
        terminal_writestring("[UEFI][PMM][WARN] no EfiConventionalMemory, synthetic fallback\n");
        pmm_add_avail(0x00100000ULL, 63ULL * 1024 * 1024);
    }
    pmm_setup();
}

// ------------------------------------------------------------ public API ----

int pmm_ram_range_count(void) { return g_nram; }
int pmm_ram_range(int i, uint64_t* start, uint64_t* end) {
    if (i < 0 || i >= g_nram) return -1;
    *start = g_ram[i].start; *end = g_ram[i].end;
    return 0;
}

void* pmm_alloc_frame(void) {
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    void* r = alloc_frame_nolock();
    spin_unlock_irqrestore(&pmm_lock, fl);
    return r;
}

void* pmm_alloc_below(uint64_t limit) {
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    uint64_t p = alloc_frame_nolock_below(limit);
    spin_unlock_irqrestore(&pmm_lock, fl);
    return p ? (void*)p : NULL;
}

void* pmm_alloc_contiguous(size_t count) {
    return pmm_alloc_contiguous_below(count, ~0ULL);
}

void* pmm_alloc_contiguous_below(size_t count, uint64_t limit) {
    if (count == 0) return NULL;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    uint64_t p = (count == 1) ? alloc_frame_nolock_below(limit)
                              : alloc_run_nolock_below(count, limit);
    spin_unlock_irqrestore(&pmm_lock, fl);
    return p ? (void*)p : NULL;
}

void pmm_free_contiguous(void* addr, size_t count) {
    uint64_t base = (uint64_t)addr / PMM_FRAME_SIZE;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    if (g_sectioned) {
        for (uint64_t f = base; f < base + count; f++) {
            pmm_section_t* s = sec_of(f);
            uint64_t i = f & (PMM_SECTION_FRAMES - 1);
            if (s && (s->bitmap[i >> 6] & (1ULL << (i & 63)))) {
                sec_mark(s, i, i + 1, 0);
                g_free_frames++;
                if (s->hint > (i >> 6)) s->hint = (uint32_t)(i >> 6);
            }
        }
    }
    spin_unlock_irqrestore(&pmm_lock, fl);
}

void pmm_free_frame(void* addr) {
    pmm_free_contiguous(addr, 1);
}

// [M19] Copy-on-write refcounting. The count stored is the number of EXTRA
// references (0 = single owner). Caller holds pmm_lock.
static uint8_t* ref_slot(uint64_t frame, int create) {
    pmm_section_t* s = g_sectioned ? sec_of(frame) : NULL;
    if (!s) return NULL;
    uint64_t i = frame & (PMM_SECTION_FRAMES - 1);
    uint64_t c = i / PMM_REF_CHUNK_FRAMES;
    if (!s->refs[c]) {
        if (!create) return NULL;
        uint64_t p = alloc_frame_nolock_below(~0ULL);
        if (!p) return NULL;
        uint8_t* chunk = (uint8_t*)phys_to_virt(p);
        for (uint64_t k = 0; k < PMM_FRAME_SIZE; k++) chunk[k] = 0;
        s->refs[c] = chunk;
        g_ref_chunks++;
    }
    return &s->refs[c][i % PMM_REF_CHUNK_FRAMES];
}

// Drop chunk c of section s once no frame in it is shared any more.
static void ref_chunk_put(pmm_section_t* s, uint64_t c) {
    if (--s->refs_used[c] != 0) return;
    uint64_t phys = virt_to_phys((uint64_t)(uintptr_t)s->refs[c]);
    s->refs[c] = NULL;
    g_ref_chunks--;
    pmm_section_t* fs = sec_of(phys / PMM_FRAME_SIZE);
    uint64_t i = (phys / PMM_FRAME_SIZE) & (PMM_SECTION_FRAMES - 1);
    if (fs) { sec_mark(fs, i, i + 1, 0); g_free_frames++; }
}

int pmm_share_frame(void* addr) {
    uint64_t frame = (uint64_t)addr / PMM_FRAME_SIZE;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    uint8_t* r = ref_slot(frame, 1);
    int rc = -1;
    if (r) {
        if (*r == 0) sec_of(frame)->refs_used[(frame & (PMM_SECTION_FRAMES - 1)) / PMM_REF_CHUNK_FRAMES]++;
        if (*r != PMM_REF_STICKY) (*r)++;   // saturate: the frame is then pinned forever
        rc = 0;
    }
    spin_unlock_irqrestore(&pmm_lock, fl);
    return rc;
}

int pmm_unref_frame(void* addr) {
    uint64_t frame = (uint64_t)addr / PMM_FRAME_SIZE;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    uint8_t* r = ref_slot(frame, 0);
    int still = 0;
    if (r && *r > 0) {
        if (*r != PMM_REF_STICKY) {         // a sticky count never drops
            if (--(*r) == 0)
                ref_chunk_put(sec_of(frame), (frame & (PMM_SECTION_FRAMES - 1)) / PMM_REF_CHUNK_FRAMES);
        }
        still = 1;
    }
    spin_unlock_irqrestore(&pmm_lock, fl);
    return still;
}

int pmm_frame_shared(void* addr) {
    uint64_t frame = (uint64_t)addr / PMM_FRAME_SIZE;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    uint8_t* r = ref_slot(frame, 0);
    int shared = (r && *r > 0);
    spin_unlock_irqrestore(&pmm_lock, fl);
    return shared;
}

uint64_t pmm_get_total_memory(void) { return total_memory; }

uint64_t pmm_get_free_memory(void) {
    if (g_sectioned) return g_free_frames * PMM_FRAME_SIZE;
    return (g_managed_frames > g_early_used ? g_managed_frames - g_early_used : 0) * PMM_FRAME_SIZE;
}

uint64_t pmm_get_used_memory(void) {
    uint64_t fr = pmm_get_free_memory() / PMM_FRAME_SIZE;
    return (g_managed_frames > fr ? g_managed_frames - fr : 0) * PMM_FRAME_SIZE;
}

uint64_t pmm_get_meta_bytes(void) {
    return g_meta_bytes + g_ref_chunks * PMM_FRAME_SIZE;
}

uint64_t pmm_free_frames_below(uint64_t limit) {
    uint64_t n = 0;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    if (g_sectioned) {
        uint64_t hi = sec_count_below(limit);
        if (hi > g_nsec) hi = g_nsec;
        for (uint64_t si = 0; si < hi; si++) if (g_sec[si]) n += g_sec[si]->free;
    }
    spin_unlock_irqrestore(&pmm_lock, fl);
    return n;
}

void pmm_print_stats(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("     Total memory:   "); pmm_dec(total_memory >> 20); terminal_writestring(" MB\n");
    terminal_writestring("     Used memory:    "); pmm_dec(pmm_get_used_memory() >> 20); terminal_writestring(" MB\n");
    terminal_writestring("     Free memory:    "); pmm_dec(pmm_get_free_memory() >> 20); terminal_writestring(" MB\n");
    terminal_writestring("     PMM metadata:   "); pmm_dec(pmm_get_meta_bytes() >> 10); terminal_writestring(" KB\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

// ------------------------------------------------------------- selftest ----

// Claim the highest free frame in RAM (selftest: touch the top of the map).
static int64_t alloc_top_nolock(void) {
    for (uint64_t si = g_nsec; si-- > 0; ) {
        pmm_section_t* s = g_sec[si];
        if (!s || s->free == 0) continue;
        for (uint64_t w = PMM_SECTION_WORDS; w-- > 0; ) {
            if (s->bitmap[w] == ~0ULL) continue;
            uint64_t f = (w << 6) + 63 - (uint64_t)__builtin_clzll(~s->bitmap[w]);
            sec_mark(s, f, f + 1, 1);
            g_free_frames--;
            return (int64_t)((si << PMM_SECTION_SHIFT) + f);
        }
    }
    return -1;
}

static void st_dec(uint64_t v) { char b[32]; itoa_dec(v, b); debugcon_writestring(b); }

// [M49] Big-memory self-test (boot, cheap): allocate one frame in up to 64
// RAM sections spread over the whole map (always including the highest one),
// write a frame-specific pattern through the physmap into every one BEFORE
// verifying any (so aliasing between distant frames is caught), then check the
// DMA32 paths, the zone preference, COW refcounts and that the free count is
// restored. Logs "[BIGMEM] ..." lines; returns 0 on success.
int pmm_selftest(void) {
    if (!g_sectioned) return -1;
    enum { NS = 65 };
    uint64_t frames[NS]; int n = 0;
    int fail = 0;
    uint64_t chunks0 = g_ref_chunks;
    uint64_t free0 = g_free_frames;

    // 1. Sample sections: indices of present sections, evenly spread.
    uint64_t npresent = 0, last = 0;
    uint64_t last_free = 0;   // highest section with a free frame (metadata may fill the top one)
    for (uint64_t si = 0; si < g_nsec; si++) if (g_sec[si]) { npresent++; last = si; if (g_sec[si]->free) last_free = si; }
    uint64_t step = npresent > 64 ? npresent / 64 : 1, k = 0;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    for (uint64_t si = 0; si < g_nsec && n < NS - 1; si++) {
        if (!g_sec[si]) continue;
        if ((k++ % step) != 0 && si != last) continue;
        int64_t f = alloc_in(si, si + 1, ~0ULL, NULL);
        if (f >= 0) frames[n++] = (uint64_t)f;
    }
    {   // plus the highest free frame of all RAM
        int64_t f = alloc_top_nolock();
        if (f >= 0) frames[n++] = (uint64_t)f;
    }
    spin_unlock_irqrestore(&pmm_lock, fl);
    uint64_t highest = 0;
    for (int i = 0; i < n; i++) {
        volatile uint64_t* p = (volatile uint64_t*)phys_to_virt(frames[i] * PMM_FRAME_SIZE);
        for (int w = 0; w < 512; w++) p[w] = (frames[i] << 12) ^ ((uint64_t)w * 0x9E3779B97F4A7C15ULL);
        if (frames[i] > highest) highest = frames[i];
    }
    int bad = 0;
    for (int i = 0; i < n; i++) {
        volatile uint64_t* p = (volatile uint64_t*)phys_to_virt(frames[i] * PMM_FRAME_SIZE);
        for (int w = 0; w < 512; w++)
            if (p[w] != ((frames[i] << 12) ^ ((uint64_t)w * 0x9E3779B97F4A7C15ULL))) { bad++; break; }
    }
    debugcon_writestring("[BIGMEM] touched="); st_dec((uint64_t)n);
    debugcon_writestring(" sections="); st_dec(npresent);
    debugcon_writestring(" highest_frame_MiB="); st_dec((highest * PMM_FRAME_SIZE) >> 20);
    debugcon_writestring(bad ? " pattern FAIL\n" : " pattern OK\n");
    if (bad || n == 0 || (highest >> PMM_SECTION_SHIFT) != last_free) fail = 1;

    // 2. Zones: an ordinary frame comes from >= 4 GiB when such RAM is free;
    //    pmm_alloc_below / pmm_alloc_contiguous_below stay under the limit.
    int have_normal = 0;
    for (uint64_t si = PMM_DMA32_LIMIT >> 30; si < g_nsec; si++)
        if (g_sec[si] && g_sec[si]->free) { have_normal = 1; break; }
    uint64_t a = (uint64_t)pmm_alloc_frame();
    uint64_t b = (uint64_t)pmm_alloc_below(PMM_DMA32_LIMIT);
    uint64_t c = (uint64_t)pmm_alloc_contiguous_below(16, PMM_DMA32_LIMIT);
    int zone_ok = a && b && c && b + PMM_FRAME_SIZE <= PMM_DMA32_LIMIT &&
                  c + 16 * PMM_FRAME_SIZE <= PMM_DMA32_LIMIT &&
                  (!have_normal || a >= PMM_DMA32_LIMIT);
    debugcon_writestring("[BIGMEM] zones: normal="); debugcon_print_hex(a);
    debugcon_writestring(" dma32="); debugcon_print_hex(b);
    debugcon_writestring(" dma32x16="); debugcon_print_hex(c);
    debugcon_writestring(zone_ok ? " OK\n" : " FAIL\n");
    if (!zone_ok) fail = 1;

    // 3. COW refcount on the highest sampled frame (lazy chunk allocation).
    void* hf = (void*)(highest * PMM_FRAME_SIZE);
    int refs_ok = n > 0 && !pmm_frame_shared(hf) && pmm_share_frame(hf) == 0 &&
                  pmm_frame_shared(hf) && pmm_unref_frame(hf) == 1 &&
                  !pmm_frame_shared(hf) && pmm_unref_frame(hf) == 0;
    debugcon_writestring(refs_ok ? "[BIGMEM] refcount OK\n" : "[BIGMEM] refcount FAIL\n");
    if (!refs_ok) fail = 1;

    // 4. Free everything; the free count must come back (minus new ref chunks).
    if (a) pmm_free_frame((void*)a);
    if (b) pmm_free_frame((void*)b);
    if (c) pmm_free_contiguous((void*)c, 16);
    for (int i = 0; i < n; i++) pmm_free_frame((void*)(frames[i] * PMM_FRAME_SIZE));
    uint64_t expect = free0 - (g_ref_chunks - chunks0);   // chunks are released again
    int free_ok = (g_free_frames == expect) &&
                  pmm_get_free_memory() <= pmm_get_total_memory() &&
                  pmm_get_used_memory() + pmm_get_free_memory() == pmm_get_total_memory();
    debugcon_writestring("[BIGMEM] meminfo total_MiB="); st_dec(pmm_get_total_memory() >> 20);
    debugcon_writestring(" free_MiB="); st_dec(pmm_get_free_memory() >> 20);
    debugcon_writestring(" dma32_free_MiB="); st_dec((pmm_free_frames_below(PMM_DMA32_LIMIT) * PMM_FRAME_SIZE) >> 20);
    debugcon_writestring(" meta_KiB="); st_dec(pmm_get_meta_bytes() >> 10);
    debugcon_writestring(free_ok ? " OK\n" : " FAIL\n");
    if (!free_ok) fail = 1;

    debugcon_writestring(fail ? "[BIGMEM] selftest FAIL\n" : "[BIGMEM] selftest PASS\n");
    return fail ? -1 : 0;
}
