/*

 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "heap.h"
#include "pmm.h"
#include "vmm.h"   // [M12] phys_to_virt for physmap-addressed heap
#include "terminal.h"
#include "spinlock.h"  // [M29] SMP: serialize the free-list across CPUs
#include "debugcon.h"
#include "panic.h"

/*
 * Hardened kernel heap.
 * =====================
 *
 * The heap is a single singly-linked list of blocks (free and used) carved out
 * of physically-contiguous PMM regions addressed through the physmap. Corruption
 * used to surface far from its cause (an overflow smashed the next header, a
 * later kmalloc handed out the inflated block, a third party's page table got
 * overwritten). Every block is now self-checking so corruption is caught at the
 * next heap operation that touches it — normally the kfree of the culprit.
 *
 * Block layout (all offsets 16-byte aligned):
 *
 *   +--------------------------- heap_block_t (48 bytes) ---------------------+
 *   | magic u32 | flags u32 | size | req | next | caller | check              |
 *   +-------------------------------------------------------------------------+
 *   | payload: req bytes for the caller | tail canary (8 bytes) | slack ...   |
 *   +-------------------------------------------------------------------------+
 *   <------------------------------- size ------------------------------------>
 *
 *   magic   HEAP_MAGIC_USED / HEAP_MAGIC_FREE (list members), HEAP_MAGIC_ALIGN
 *           (aligned-allocation stub, never on the list), HEAP_MAGIC_MERGED
 *           (header absorbed by coalescing; left so a double free is named).
 *   size    payload capacity (multiple of 16; >= req + 8 for a used block).
 *   req     bytes the caller asked for; the tail canary sits at payload+req,
 *           so even a 1-byte overflow is caught when the block is freed.
 *   next    next block in the list.
 *   caller  return address of the kmalloc caller (used) / kfree caller (free) —
 *           reported in every diagnostic as the block's owner / last freer.
 *   check   keyed hash of (own address, magic, flags, size, req, next, caller)
 *           under a boot-random secret: a smashed or forged header — or a
 *           pointer that is not a block start — fails it.
 *
 * Aligned allocations (kmalloc_aligned, alignment > 16) are first-class: the
 * owner block is over-allocated, and a 48-byte HEAP_MAGIC_ALIGN stub is written
 * just below the aligned pointer, its `next` pointing at the owner. kfree() of
 * the aligned pointer resolves the stub and frees the owner; the owner's tail
 * canary is moved to the end of the aligned region.
 *
 * Detection (always on): header magic+check on every block a kmalloc/kfree/
 * coalesce walk touches; tail canary on kfree; double free; invalid free
 * (misaligned, outside the heap, not a block start, raw pointer of an aligned
 * allocation). Every hit → kernel_panic with block, expected, found, caller and
 * owner. heap_check() validates the whole list (also from the shell `heapcheck`).
 *
 * Debug (-DSECOS_HEAP_DEBUG, or -DSECOS_HEAP_POISON for poisoning alone; a
 * -DSECOS_LOCKDEP build poisons too, with 0x6B):
 * freed payloads are filled with 0xDE and the poison is verified when the
 * memory is handed out again (use-after-free writes), kfree confirms list
 * membership, and heap_check() runs every HEAP_PERIODIC_OPS operations.
 */

#if defined(SECOS_HEAP_DEBUG)
#define HEAP_POISON      1
#define HEAP_PERIODIC    1
#define HEAP_MEMBERSHIP  1
#else
#define HEAP_PERIODIC    0
#define HEAP_MEMBERSHIP  0
#if defined(SECOS_HEAP_POISON) || defined(SECOS_LOCKDEP) || defined(SECOS_LOCKDEP_LITE)
#define HEAP_POISON      1
#else
#define HEAP_POISON      0
#endif
#endif

#define HEAP_PERIODIC_OPS 128

#define HEAP_MAGIC_USED   0xA110CA7Eu
#define HEAP_MAGIC_FREE   0xF7EEB10Cu
#define HEAP_MAGIC_ALIGN  0xA1167ED0u
#define HEAP_MAGIC_MERGED 0x3E46EDDDu
#if defined(SECOS_LOCKDEP) || defined(SECOS_LOCKDEP_LITE)
/* [M50] Lockdep builds poison freed memory with 0x6B (Linux POISON_FREE), so a
 * use-after-free of an object freed under a racing lock reads 0x6B6B... */
#define HEAP_POISON_BYTE  0x6B
#define HEAP_POISON_U32   0x6B6B6B6Bu
#else
#define HEAP_POISON_BYTE  0xDE
#define HEAP_POISON_U32   0xDEDEDEDEu
#endif

#define HB_F_ALIGNED      0x1u   // owner of a kmalloc_aligned allocation

#define HEAP_ALIGN        16
#define HEAP_TAIL         8      // tail canary bytes
#define HEAP_MIN_SPLIT    32     // smallest free remainder worth a header

// [M29] One lock guards the whole free list (kmalloc/kfree/expand). Uncontended
// on the single-core path. expand_heap() calls the PMM under this lock, but the
// PMM has its own separate lock, so there is no nesting on the same lock.
static spinlock_t heap_lock = SPINLOCK_INIT_CLASS(LK_S70_HEAP, "heap");

typedef struct heap_block {
    uint32_t magic;
    uint32_t flags;
    size_t   size;
    size_t   req;
    struct heap_block* next;
    uint64_t caller;
    uint64_t check;
} heap_block_t;

_Static_assert(sizeof(heap_block_t) == 48, "heap header must stay 48 bytes");
_Static_assert(sizeof(heap_block_t) % HEAP_ALIGN == 0, "header keeps payload 16-aligned");

#define HEAP_BLOCK_HEADER_SIZE sizeof(heap_block_t)
#define HDR HEAP_BLOCK_HEADER_SIZE

static heap_block_t* heap_start = NULL;
static uint64_t total_allocated = 0;
static uint64_t total_freed = 0;

static uint64_t  g_heap_secret;
static uintptr_t heap_lo, heap_hi;      // bounds of every heap region (virtual)
static volatile int g_heap_dead;        // corruption reported -> heap frozen
#if HEAP_PERIODIC
static uint32_t  g_heap_ops;            // periodic-check counter
#endif

// Selftest hook: in "expect" mode a detection is logged and counted and the
// offending operation is aborted, instead of panicking.
static volatile int g_heap_expect;
static volatile int g_heap_detected;
static const char* g_heap_last_what;

#define RET_ADDR() ((uint64_t)(uintptr_t)__builtin_return_address(0))

// Helper to align addresses
static inline size_t align_up(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

// Convert number to decimal string
static void itoa_dec(uint64_t value, char* buffer) {
    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    char temp[32];
    int i = 0;

    while (value > 0) {
        temp[i++] = '0' + (value % 10);
        value /= 10;
    }

    int j = 0;
    while (i > 0) {
        buffer[j++] = temp[--i];
    }
    buffer[j] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Integrity primitives                                                       */
/* ------------------------------------------------------------------------- */

static inline uint64_t hb_mix(uint64_t x) {   // splitmix64 finalizer
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27; x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

static uint64_t hb_calc(const heap_block_t* b) {
    uint64_t h = g_heap_secret ^ (uint64_t)(uintptr_t)b;
    h = hb_mix(h ^ (((uint64_t)b->magic << 32) | b->flags));
    h = hb_mix(h ^ b->size);
    h = hb_mix(h ^ b->req);
    h = hb_mix(h ^ (uint64_t)(uintptr_t)b->next);
    h = hb_mix(h ^ b->caller);
    return h;
}

static inline void hb_seal(heap_block_t* b) { b->check = hb_calc(b); }

static inline uint8_t* hb_payload(heap_block_t* b) { return (uint8_t*)b + HDR; }

static inline uint64_t hb_tail_value(const uint8_t* payload) {
    return hb_mix(g_heap_secret ^ (uint64_t)(uintptr_t)payload ^ 0x7A11CA9A11ULL);
}

static void hb_tail_write(heap_block_t* b) {
    uint8_t* p = hb_payload(b);
    uint64_t v = hb_tail_value(p);
    uint8_t* t = p + b->req;
    for (int i = 0; i < HEAP_TAIL; i++) t[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t hb_tail_read(heap_block_t* b) {
    const uint8_t* t = hb_payload(b) + b->req;
    uint64_t v = 0;
    for (int i = 0; i < HEAP_TAIL; i++) v |= (uint64_t)t[i] << (8 * i);
    return v;
}

static inline int hb_in_heap(const void* p) {
    uintptr_t a = (uintptr_t)p;
    return (a & (HEAP_ALIGN - 1)) == 0 && a >= heap_lo && a + HDR <= heap_hi;
}

static inline void hb_fill(uint8_t* p, size_t n, uint8_t v) {
    for (size_t i = 0; i < n; i++) p[i] = v;
}

#if HEAP_POISON
// Returns the offset of the first non-poison byte in [p, p+n), or n.
static size_t hb_poison_scan(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i] != HEAP_POISON_BYTE) return i;
    return n;
}
#endif

/* ------------------------------------------------------------------------- */
/* Fault reporting                                                            */
/* ------------------------------------------------------------------------- */

static char g_heap_msg[256];

static void msg_cat(size_t* n, const char* s) {
    while (*s && *n < sizeof(g_heap_msg) - 1) g_heap_msg[(*n)++] = *s++;
    g_heap_msg[*n] = '\0';
}

static void msg_hex(size_t* n, uint64_t v) {
    static const char h[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 17; i >= 2; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
    buf[18] = '\0';
    msg_cat(n, buf);
}

/* Report heap corruption. Called with heap_lock held. In expect (selftest)
 * mode it logs, counts and returns 0 so the caller aborts the operation;
 * otherwise it freezes the heap and panics (never returns). `owner` is the
 * block's recorded kmalloc caller (used) or last kfree caller (free). */
static int heap_fault(uint32_t line, const char* what, const void* block,
                      uint64_t expected, uint64_t found,
                      uint64_t caller, uint64_t owner) {
    size_t n = 0;
    g_heap_msg[0] = '\0';
    msg_cat(&n, "HEAP: ");
    msg_cat(&n, what);
    msg_cat(&n, " block="); msg_hex(&n, (uint64_t)(uintptr_t)block);
    msg_cat(&n, " expected="); msg_hex(&n, expected);
    msg_cat(&n, " found="); msg_hex(&n, found);
    // [M45] link-time (kernel.bin) addresses: resolvable, and no KASLR leak.
    msg_cat(&n, " caller="); msg_hex(&n, kaddr_link(caller));
    msg_cat(&n, " owner="); msg_hex(&n, kaddr_link(owner));

    if (g_heap_expect) {
        g_heap_detected++;
        g_heap_last_what = what;
        debugcon_writestring("[HEAP] (expected) ");
        debugcon_writestring(g_heap_msg);
        debugcon_writestring("\n");
        return 0;
    }
    g_heap_dead = 1;
    debugcon_writestring("[HEAP] CORRUPTION DETECTED: ");
    debugcon_writestring(g_heap_msg);
    debugcon_writestring("\n");
    // Release the lock so nothing on the panic path can deadlock on it; the
    // heap is frozen by g_heap_dead (kmalloc returns NULL, kfree is a no-op).
    spin_unlock(&heap_lock);
    kernel_panic(g_heap_msg, __FILE__, line);
    for (;;) __asm__ volatile("cli; hlt");
}

#define HEAP_FAULT(...) heap_fault(__LINE__, __VA_ARGS__)

/* Validate a list block before using any of its fields. Returns 1 if sound. */
static int hb_verify(heap_block_t* b, uint64_t caller) {
    if (!hb_in_heap(b))
        return HEAP_FAULT("block pointer outside the heap (corrupted next link)",
                          b, heap_lo, (uint64_t)(uintptr_t)b, caller, 0);
    if (b->magic != HEAP_MAGIC_USED && b->magic != HEAP_MAGIC_FREE)
        return HEAP_FAULT("corrupted block header (bad magic)", b,
                          HEAP_MAGIC_USED, b->magic, caller, b->caller);
    uint64_t want = hb_calc(b);
    if (b->check != want)
        return HEAP_FAULT("corrupted block header (check mismatch)", b,
                          want, b->check, caller, b->caller);
    if (b->size % HEAP_ALIGN != 0 ||
        (uintptr_t)hb_payload(b) + b->size > heap_hi)
        return HEAP_FAULT("corrupted block header (size out of range)", b,
                          heap_hi - (uintptr_t)hb_payload(b), b->size, caller, b->caller);
    if (b->magic == HEAP_MAGIC_USED && b->req + HEAP_TAIL > b->size)
        return HEAP_FAULT("corrupted block header (req > size)", b,
                          b->size, b->req, caller, b->caller);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Init / expansion                                                           */
/* ------------------------------------------------------------------------- */

static uint64_t heap_seed(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t s = ((uint64_t)hi << 32) | lo;
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (c & (1u << 30)) {           // RDRAND
        for (int i = 0; i < 10; i++) {
            uint64_t r; uint8_t ok;
            __asm__ volatile("rdrand %0; setc %1" : "=r"(r), "=qm"(ok));
            if (ok) { s ^= r; break; }
        }
    }
    s = hb_mix(s ^ 0x5EC05EC05EC05EC0ULL);
    return s ? s : 0x9E3779B97F4A7C15ULL;
}

// [M52] Bytes the heap holds from the PMM (it never gives frames back, so this
// is its high-water mark). Updated under heap_lock; read without it.
static volatile uint64_t g_heap_pmm_bytes;
static void heap_track_region(uintptr_t base, size_t len) {
    g_heap_pmm_bytes += len;
    if (heap_lo == 0 || base < heap_lo) heap_lo = base;
    if (base + len > heap_hi) heap_hi = base + len;
}

static void hb_init_free(heap_block_t* b, size_t size) {
    b->magic = HEAP_MAGIC_FREE;
    b->flags = 0;
    b->size = size;
    b->req = 0;
    b->next = NULL;
    b->caller = 0;
#if HEAP_POISON
    hb_fill(hb_payload(b), size, HEAP_POISON_BYTE);
#endif
    hb_seal(b);
}

// Initialize heap
void heap_init(void) {
    // [M12] Address heap memory through the physmap (phys_to_virt) rather than
    // the identity map, so the heap works with any physical frame the PMM hands
    // out — including frames above the 512 MB identity window. Physmap is active
    // by the time heap_init() runs (vmm_init_physmap is called in phase 1).
    void* frame = pmm_alloc_frame();
    heap_start = frame ? (heap_block_t*)phys_to_virt((uint64_t)frame) : NULL;

    if (heap_start == NULL) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[ERROR] Unable to allocate initial heap!\n");
        terminal_writestring("[DEBUG] pmm_alloc_frame() returned NULL\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        return;
    }

    g_heap_secret = heap_seed();
    heap_track_region((uintptr_t)heap_start, PMM_FRAME_SIZE);
    hb_init_free(heap_start, PMM_FRAME_SIZE - HDR);

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[OK] Heap initialized @ ");

    // Print heap address (debug)
    char hex_chars[] = "0123456789ABCDEF";
    uint64_t addr = (uint64_t)heap_start;
    for (int i = 60; i >= 0; i -= 4) {
        char c = hex_chars[(addr >> i) & 0xF];
        terminal_putchar(c);
    }
    terminal_writestring("\n");

    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
#if HEAP_POISON
    debugcon_writestring("[HEAP] hardened heap (debug: poison-on-free + periodic check)\n");
#endif
}

// Append a new region big enough for a `need`-byte payload after `last`.
// [M12] Allocate as many *physically-contiguous* frames as the request needs
// (the physmap maps contiguous physical memory to contiguous virtual memory, so
// the region is a single usable block).
static heap_block_t* expand_heap(heap_block_t* last, size_t need) {
    size_t frames = (need + HDR + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    if (frames == 0) frames = 1;

    void* base = pmm_alloc_contiguous(frames);
    if (base == NULL) {
        return NULL;
    }

    heap_block_t* nb = (heap_block_t*)phys_to_virt((uint64_t)base);
    heap_track_region((uintptr_t)nb, frames * PMM_FRAME_SIZE);
    hb_init_free(nb, frames * PMM_FRAME_SIZE - HDR);

    last->next = nb;
    hb_seal(last);
    return nb;
}

/* ------------------------------------------------------------------------- */
/* Full-list validator                                                        */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint64_t blocks, used_blocks, free_blocks;
    uint64_t used_bytes, free_bytes, largest_free;
} heap_walk_stats_t;

/* Walk and validate every block. Returns the number of problems found (only
 * non-zero in expect mode — otherwise the first problem panics). */
static int heap_check_locked(uint64_t caller, heap_walk_stats_t* st) {
    heap_walk_stats_t s = {0};
    int errs = 0;
    // A sane list can never hold more blocks than 16-byte slots in the heap.
    uint64_t limit = (heap_hi - heap_lo) / HEAP_ALIGN + 1;
    for (heap_block_t* b = heap_start; b != NULL; b = b->next) {
        if (++s.blocks > limit) {
            HEAP_FAULT("block list cycle", b, limit, s.blocks, caller, 0);
            return errs + 1;
        }
        if (!hb_verify(b, caller)) return errs + 1;   // can't follow next
        uintptr_t end = (uintptr_t)hb_payload(b) + b->size;
        uintptr_t nx = (uintptr_t)b->next;
        if (nx && nx > (uintptr_t)b && nx < end) {
            HEAP_FAULT("blocks overlap", b, end, nx, caller, b->caller);
            return errs + 1;
        }
        if (b->magic == HEAP_MAGIC_USED) {
            s.used_blocks++; s.used_bytes += b->size;
            uint64_t want = hb_tail_value(hb_payload(b));
            uint64_t got = hb_tail_read(b);
            if (got != want) {
                HEAP_FAULT("heap buffer overflow (tail canary smashed)", b,
                           want, got, caller, b->caller);
                errs++;
            }
        } else {
            s.free_blocks++; s.free_bytes += b->size;
            if (b->size > s.largest_free) s.largest_free = b->size;
#if HEAP_POISON
            size_t off = hb_poison_scan(hb_payload(b), b->size);
            if (off != b->size) {
                HEAP_FAULT("use-after-free write (poison overwritten in a free block)",
                           b, off, hb_payload(b)[off], caller, b->caller);
                errs++;
            }
#endif
        }
    }
    if (st) *st = s;
    return errs;
}

static inline void heap_periodic_locked(uint64_t caller) {
#if HEAP_PERIODIC
    if (++g_heap_ops % HEAP_PERIODIC_OPS == 0) heap_check_locked(caller, NULL);
#else
    (void)caller;
#endif
}

/* ------------------------------------------------------------------------- */
/* Allocation                                                                 */
/* ------------------------------------------------------------------------- */

static heap_block_t* heap_alloc_locked(size_t req, uint64_t caller) {
    size_t need = align_up(req + HEAP_TAIL, HEAP_ALIGN);
    if (need < req) return NULL;   // size overflow

    heap_block_t* cur = heap_start;
    heap_block_t* last = NULL;
    while (cur != NULL) {
        if (!hb_verify(cur, caller)) return NULL;
        if (cur->magic == HEAP_MAGIC_FREE && cur->size >= need) break;
        last = cur;
        cur = cur->next;
    }
    if (cur == NULL) {
        cur = expand_heap(last, need);
        if (cur == NULL) return NULL;
    }

#if HEAP_POISON
    size_t off = hb_poison_scan(hb_payload(cur), need);
    if (off != need) {
        HEAP_FAULT("use-after-free write (poison overwritten in a free block)",
                   cur, off, hb_payload(cur)[off], caller, cur->caller);
        return NULL;
    }
#endif

    size_t remaining = cur->size - need;
    if (remaining >= HDR + HEAP_MIN_SPLIT) {
        heap_block_t* nb = (heap_block_t*)(hb_payload(cur) + need);
        nb->magic = HEAP_MAGIC_FREE;
        nb->flags = 0;
        nb->size = remaining - HDR;
        nb->req = 0;
        nb->next = cur->next;
        nb->caller = cur->caller;     // keep the last freer for diagnostics
        hb_seal(nb);
        cur->next = nb;
        cur->size = need;
    }
    cur->magic = HEAP_MAGIC_USED;
    cur->flags = 0;
    cur->req = req;
    cur->caller = caller;
    hb_seal(cur);
    hb_tail_write(cur);
    total_allocated += cur->size;
    return cur;
}

static void heap_alloc_failed(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    terminal_writestring("[FAIL] Allocation failed (expand_heap NULL)\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

// Allocate memory
void* kmalloc(size_t size) {
    uint64_t caller = RET_ADDR();
    if (size == 0) return NULL;
    if (heap_start == NULL || g_heap_dead) return NULL;

    uint64_t fl = spin_lock_irqsave(&heap_lock);
    heap_block_t* b = heap_alloc_locked(size, caller);
    if (b) heap_periodic_locked(caller);
    spin_unlock_irqrestore(&heap_lock, fl);
    if (!b) {
        if (!g_heap_dead && !g_heap_expect) heap_alloc_failed();
        return NULL;
    }
    return hb_payload(b);
}

// Allocate memory with alignment guarantee. The returned pointer must be
// released with kfree() (it resolves the HEAP_MAGIC_ALIGN stub to the owner).
void* kmalloc_aligned(size_t size, size_t alignment) {
    uint64_t caller = RET_ADDR();
    if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) return NULL;
    if (heap_start == NULL || g_heap_dead) return NULL;

    uint64_t fl = spin_lock_irqsave(&heap_lock);
    heap_block_t* b;
    uint8_t* r;
    if (alignment <= HEAP_ALIGN) {
        b = heap_alloc_locked(size, caller);
        r = b ? hb_payload(b) : NULL;
    } else {
        size_t req = size + alignment + HDR;
        b = (req > size) ? heap_alloc_locked(req, caller) : NULL;
        r = NULL;
        if (b) {
            uint8_t* p = hb_payload(b);
            r = (uint8_t*)align_up((uintptr_t)p + HDR, alignment);
            heap_block_t* stub = (heap_block_t*)(r - HDR);
            stub->magic = HEAP_MAGIC_ALIGN;
            stub->flags = 0;
            stub->size = size;
            stub->req = size;
            stub->next = b;
            stub->caller = caller;
            hb_seal(stub);
            b->flags |= HB_F_ALIGNED;
            b->req = (size_t)(r - p) + size;
            hb_seal(b);
            hb_tail_write(b);
        }
    }
    if (b) heap_periodic_locked(caller);
    spin_unlock_irqrestore(&heap_lock, fl);
    if (!b && !g_heap_dead && !g_heap_expect) heap_alloc_failed();
    return r;
}

/* ------------------------------------------------------------------------- */
/* Free                                                                       */
/* ------------------------------------------------------------------------- */

/* Map a kfree() argument to its list block, rejecting anything that is not a
 * live allocation. Returns NULL (after reporting) on a bad pointer. */
static heap_block_t* heap_resolve_locked(void* ptr, uint64_t caller) {
    uintptr_t p = (uintptr_t)ptr;
    if (p & (HEAP_ALIGN - 1)) {
        HEAP_FAULT("invalid free (misaligned pointer, not a block start)",
                   ptr, align_up(p, HEAP_ALIGN), p, caller, 0);
        return NULL;
    }
    if (p < heap_lo + HDR || p > heap_hi) {
        HEAP_FAULT("invalid free (pointer outside the heap)", ptr, heap_lo, p, caller, 0);
        return NULL;
    }
    heap_block_t* h = (heap_block_t*)(p - HDR);
    uint32_t m = h->magic;

    if (m == HEAP_MAGIC_ALIGN) {
        uint64_t want = hb_calc(h);
        if (h->check != want) {
            HEAP_FAULT("corrupted aligned-allocation stub (check mismatch)", h,
                       want, h->check, caller, h->caller);
            return NULL;
        }
        heap_block_t* owner = h->next;
        if (!hb_verify(owner, caller)) return NULL;
        if (owner->magic == HEAP_MAGIC_FREE) {
            HEAP_FAULT("double free (aligned allocation)", ptr,
                       HEAP_MAGIC_USED, owner->magic, caller, owner->caller);
            return NULL;
        }
        uintptr_t lo = (uintptr_t)hb_payload(owner) + HDR;
        uintptr_t hi = (uintptr_t)hb_payload(owner) + owner->req;
        if (!(owner->flags & HB_F_ALIGNED) || p < lo || p >= hi) {
            HEAP_FAULT("invalid free (aligned stub does not match its owner)", ptr,
                       (uint64_t)(uintptr_t)owner, p, caller, owner->caller);
            return NULL;
        }
        return owner;
    }
    if (m == HEAP_MAGIC_FREE || m == HEAP_MAGIC_MERGED) {
        if (h->check == hb_calc(h)) {
            HEAP_FAULT("double free", ptr, HEAP_MAGIC_USED, m, caller, h->caller);
        } else {
            HEAP_FAULT("corrupted block header (check mismatch)", h,
                       hb_calc(h), h->check, caller, h->caller);
        }
        return NULL;
    }
    if (m == HEAP_POISON_U32) {
        HEAP_FAULT("double free (pointer into freed, poisoned memory)", ptr,
                   HEAP_MAGIC_USED, m, caller, 0);
        return NULL;
    }
    if (m != HEAP_MAGIC_USED) {
        HEAP_FAULT("invalid free (not a heap block start, or header smashed)", ptr,
                   HEAP_MAGIC_USED, m, caller, 0);
        return NULL;
    }
    if (!hb_verify(h, caller)) return NULL;
    if (h->flags & HB_F_ALIGNED) {
        HEAP_FAULT("invalid free (raw block of an aligned allocation)", ptr,
                   0, h->flags, caller, h->caller);
        return NULL;
    }
#if HEAP_MEMBERSHIP
    heap_block_t* c = heap_start;
    while (c != NULL && c != h) c = c->next;
    if (c == NULL) {
        HEAP_FAULT("invalid free (valid-looking header not on the block list)",
                   ptr, 0, p, caller, h->caller);
        return NULL;
    }
#endif
    return h;
}

static void heap_free_block_locked(heap_block_t* b, uint64_t caller) {
    uint64_t want = hb_tail_value(hb_payload(b));
    uint64_t got = hb_tail_read(b);
    if (got != want) {
        HEAP_FAULT("heap buffer overflow (tail canary smashed)", b,
                   want, got, caller, b->caller);
        return;
    }

    total_freed += b->size;
    b->magic = HEAP_MAGIC_FREE;
    b->flags = 0;
    b->req = 0;
    b->caller = caller;
#if HEAP_POISON
    hb_fill(hb_payload(b), b->size, HEAP_POISON_BYTE);
#endif
    hb_seal(b);

    // Coalesce adjacent free blocks.
    // CRITICAL: only merge blocks that are PHYSICALLY contiguous. Heap regions
    // come from separate PMM calls and are not necessarily adjacent in memory;
    // merging non-contiguous blocks would inflate `size` across the gap and let
    // a later allocation write outside the heap (e.g. over a live process's page
    // tables). Two halves of a split block ARE adjacent and coalesce correctly.
    heap_block_t* cur = heap_start;
    if (!hb_verify(cur, caller)) return;
    while (cur->next != NULL) {
        heap_block_t* nx = cur->next;
        if (!hb_verify(nx, caller)) return;
        uint8_t* cur_end = hb_payload(cur) + cur->size;
        if (cur->magic == HEAP_MAGIC_FREE && nx->magic == HEAP_MAGIC_FREE &&
            cur_end == (uint8_t*)nx) {
            cur->size += HDR + nx->size;
            cur->next = nx->next;
            hb_seal(cur);
#if HEAP_POISON
            hb_fill((uint8_t*)nx, HDR, HEAP_POISON_BYTE);
#else
            nx->magic = HEAP_MAGIC_MERGED;   // a later kfree(nx) is a double free
            hb_seal(nx);
#endif
        } else {
            cur = nx;
        }
    }
}

// Free memory
void kfree(void* ptr) {
    uint64_t caller = RET_ADDR();
    if (ptr == NULL) {
        return;
    }
    if (heap_start == NULL || g_heap_dead) return;

    uint64_t fl = spin_lock_irqsave(&heap_lock);
    heap_block_t* b = heap_resolve_locked(ptr, caller);
    if (b) {
        heap_free_block_locked(b, caller);
        heap_periodic_locked(caller);
    }
    spin_unlock_irqrestore(&heap_lock, fl);
}

/* ------------------------------------------------------------------------- */
/* Validation API / stats                                                     */
/* ------------------------------------------------------------------------- */

int heap_check(void) {
    if (heap_start == NULL || g_heap_dead) return -1;
    uint64_t fl = spin_lock_irqsave(&heap_lock);
    int errs = heap_check_locked(RET_ADDR(), NULL);
    spin_unlock_irqrestore(&heap_lock, fl);
    return errs;
}

// Shell `heapcheck`: validate the whole heap and print a summary.
void heap_check_report(void) {
    char buf[32];
    if (heap_start == NULL || g_heap_dead) {
        terminal_writestring("heapcheck: heap unavailable\n");
        return;
    }
    heap_walk_stats_t s;
    uint64_t fl = spin_lock_irqsave(&heap_lock);
    int errs = heap_check_locked(RET_ADDR(), &s);
    spin_unlock_irqrestore(&heap_lock, fl);
    terminal_writestring(errs ? "heapcheck: CORRUPTION FOUND\n" : "heapcheck: OK\n");
    terminal_writestring("  blocks: "); itoa_dec(s.blocks, buf); terminal_writestring(buf);
    terminal_writestring(" (used "); itoa_dec(s.used_blocks, buf); terminal_writestring(buf);
    terminal_writestring(", free "); itoa_dec(s.free_blocks, buf); terminal_writestring(buf);
    terminal_writestring(")\n  used bytes: "); itoa_dec(s.used_bytes, buf); terminal_writestring(buf);
    terminal_writestring("\n  free bytes: "); itoa_dec(s.free_bytes, buf); terminal_writestring(buf);
    terminal_writestring(" (largest "); itoa_dec(s.largest_free, buf); terminal_writestring(buf);
    terminal_writestring(")\n");
    debugcon_writestring(errs ? "[HEAP] heapcheck: CORRUPTION FOUND\n" : "[HEAP] heapcheck: OK\n");
}

// Print heap allocator statistics
void heap_print_stats(void) {
    char buffer[32];

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("\n=== Heap Statistics ===\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));

    terminal_writestring("Allocated:  ");
    itoa_dec(total_allocated, buffer);
    terminal_writestring(buffer);
    terminal_writestring(" bytes\n");

    terminal_writestring("Freed:      ");
    itoa_dec(total_freed, buffer);
    terminal_writestring(buffer);
    terminal_writestring(" bytes\n");

    terminal_writestring("In use:     ");
    itoa_dec(total_allocated - total_freed, buffer);
    terminal_writestring(buffer);
    terminal_writestring(" bytes\n\n");
}

/* ------------------------------------------------------------------------- */
/* Boot selftest                                                              */
/* ------------------------------------------------------------------------- */

static int hst_pass, hst_fail;

static void hst(const char* name, int ok) {
    debugcon_writestring(ok ? "[HEAP][SELFTEST] PASS " : "[HEAP][SELFTEST] FAIL ");
    debugcon_writestring(name);
    debugcon_writestring("\n");
    if (ok) hst_pass++; else hst_fail++;
}

/* [M51-C6] Bytes in use and the heap's whole footprint (every block with its
 * header: the frames the heap took from the PMM, which it never gives back).
 * A leak check compares PMM free + footprint (frames not leaked) and used. */
// [M52] O(1) figures for /proc/meminfo (no heap walk): bytes held from the
// PMM and bytes in allocated blocks (block sizes, counted at kmalloc/kfree).
void heap_counters(uint64_t* in_use, uint64_t* from_pmm) {
    if (in_use) *in_use = total_allocated - total_freed;
    if (from_pmm) *from_pmm = g_heap_pmm_bytes;
}

void heap_usage(uint64_t* used, uint64_t* footprint) {
    heap_walk_stats_t s = {0};
    uint64_t fl = spin_lock_irqsave(&heap_lock);
    heap_check_locked(0, &s);
    spin_unlock_irqrestore(&heap_lock, fl);
    if (used) *used = s.used_bytes;
    if (footprint) *footprint = s.used_bytes + s.free_bytes + s.blocks * HDR;
}

static uint64_t heap_used_bytes(void) {
    heap_walk_stats_t s = {0};
    uint64_t fl = spin_lock_irqsave(&heap_lock);
    heap_check_locked(0, &s);
    spin_unlock_irqrestore(&heap_lock, fl);
    return s.used_bytes;
}

/* Run one deliberately-bad operation in expect mode; 1 if exactly one fault
 * whose message starts with `prefix` was reported. */
static int hst_expect_begin(void) { g_heap_detected = 0; g_heap_last_what = 0; g_heap_expect = 1; return 0; }
static int hst_expect_end(const char* prefix) {
    g_heap_expect = 0;
    if (g_heap_detected != 1 || !g_heap_last_what) return 0;
    for (const char *a = prefix, *b = g_heap_last_what; *a; a++, b++)
        if (*a != *b) return 0;
    return 1;
}

int heap_selftest(void) {
    hst_pass = hst_fail = 0;
    if (heap_start == NULL) return -1;

    uint64_t base_used = heap_used_bytes();

    // 1. Mixed small allocations, patterned, freed out of order.
    {
        enum { N = 96 };
        uint8_t* p[N];
        int ok = 1;
        for (int i = 0; i < N; i++) {
            size_t sz = 1 + (size_t)(i * 37) % 300;
            p[i] = (uint8_t*)kmalloc(sz);
            if (!p[i] || ((uintptr_t)p[i] & (HEAP_ALIGN - 1))) { ok = 0; continue; }
            hb_fill(p[i], sz, (uint8_t)i);
        }
        for (int i = 0; i < N && ok; i++) {
            size_t sz = 1 + (size_t)(i * 37) % 300;
            for (size_t j = 0; j < sz; j++) if (p[i][j] != (uint8_t)i) { ok = 0; break; }
        }
        for (int i = 0; i < N; i += 2) kfree(p[i]);
        ok = ok && heap_check() == 0;
        for (int i = 1; i < N; i += 2) kfree(p[i]);
        hst("small alloc/pattern/free (96 blocks, 16-aligned)", ok && heap_check() == 0);
    }
    // 2. Fragmentation: free every other block, then refill with larger ones.
    {
        enum { N = 64 };
        void* a[N]; void* b[N / 2];
        for (int i = 0; i < N; i++) a[i] = kmalloc(48);
        for (int i = 0; i < N; i += 2) { kfree(a[i]); a[i] = NULL; }
        for (int i = 0; i < N / 2; i++) b[i] = kmalloc(160);
        int ok = heap_check() == 0;
        for (int i = 0; i < N; i++) kfree(a[i]);
        for (int i = 0; i < N / 2; i++) kfree(b[i]);
        hst("fragmentation + coalescing", ok && heap_check() == 0);
    }
    // 3. Large multi-frame blocks (M12 contiguous expansion).
    {
        size_t sz = 1024 * 1024 + 123;
        uint8_t* p = (uint8_t*)kmalloc(sz);
        int ok = (p != NULL);
        if (ok) {
            for (size_t i = 0; i < sz; i++) p[i] = (uint8_t)(i * 13 + 1);
            for (size_t i = 0; i < sz && ok; i++) if (p[i] != (uint8_t)(i * 13 + 1)) ok = 0;
            kfree(p);
        }
        hst("large kmalloc(1M+123) write/read/free", ok && heap_check() == 0);
    }
    // 4. Aligned allocations freed through kfree, no leak.
    {
        static const size_t al[] = { 8, 16, 32, 64, 256, 4096 };
        int ok = 1;
        uint64_t before = heap_used_bytes();
        for (int round = 0; round < 50 && ok; round++) {
            for (unsigned k = 0; k < sizeof(al) / sizeof(al[0]); k++) {
                size_t sz = 24 + (size_t)round * 7;
                uint8_t* p = (uint8_t*)kmalloc_aligned(sz, al[k]);
                if (!p || ((uintptr_t)p & (al[k] - 1))) { ok = 0; break; }
                hb_fill(p, sz, 0xA5);   // the whole requested region is usable
                kfree(p);
            }
        }
        ok = ok && heap_used_bytes() == before;
        hst("kmalloc_aligned(8..4096) freed by kfree, no leak", ok && heap_check() == 0);
    }
    // 5. Detection paths (expect mode: reported + aborted, no panic).
    {
        uint8_t* p = (uint8_t*)kmalloc(40);
        uint8_t saved = p[40];
        hst_expect_begin();
        p[40] = (uint8_t)(saved ^ 0xFF);        // 1-byte overflow
        kfree(p);
        int ok = hst_expect_end("heap buffer overflow");
        p[40] = saved;
        kfree(p);
        hst("1-byte overflow detected on kfree", ok && heap_check() == 0);

        p = (uint8_t*)kmalloc(64);
        kfree(p);
        hst_expect_begin();
        kfree(p);
        hst("double free detected", hst_expect_end("double free"));

        uint64_t local[8] = {0};    // align by hand: the stack may only be 8-aligned
        hst_expect_begin();
        kfree((void*)align_up((uintptr_t)&local[2], HEAP_ALIGN));
        hst("invalid free (stack pointer) detected",
            hst_expect_end("invalid free (pointer outside the heap)"));

        p = (uint8_t*)kmalloc(128);
        hb_fill(p, 128, 0);
        hst_expect_begin();
        kfree(p + 64);
        int ok2 = hst_expect_end("invalid free");
        hst_expect_begin();
        kfree(p + 3);
        ok2 = ok2 && hst_expect_end("invalid free (misaligned");
        kfree(p);
        hst("invalid free (interior/misaligned pointer) detected", ok2);

        // The 2026-11 bug: an overflow smashing the NEXT header's size field.
        p = (uint8_t*)kmalloc(64);
        uint8_t* q = (uint8_t*)kmalloc(64);
        heap_block_t* qh = (heap_block_t*)(q - HDR);
        size_t qsize = qh->size;
        hst_expect_begin();
        qh->size = qsize + 0x10000;
        heap_check();
        int ok3 = hst_expect_end("corrupted block header");
        qh->size = qsize;
        kfree(q); kfree(p);
        hst("smashed next-block header size detected", ok3 && heap_check() == 0);

        // Freeing the raw owner block of an aligned allocation is refused.
        p = (uint8_t*)kmalloc_aligned(100, 256);
        heap_block_t* owner = ((heap_block_t*)(p - HDR))->next;
        hst_expect_begin();
        kfree(hb_payload(owner));
        int ok4 = hst_expect_end("invalid free (raw block");
        kfree(p);
        hst("raw owner of an aligned allocation refused", ok4 && heap_check() == 0);
    }
    hst("no leak across the whole selftest", heap_used_bytes() == base_used);

    debugcon_writestring(hst_fail == 0 ? "[HEAP][SELFTEST] all PASS\n"
                                        : "[HEAP][SELFTEST] FAILURES\n");
    return hst_fail;
}
