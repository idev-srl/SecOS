/*
 * kaslr_core.c — freestanding KASLR core shared by the Multiboot2 entry and the
 * UEFI loader (see boot/kaslr.h for the table format and the design).
 *
 * Built twice:
 *   - boot/kaslr_core.o: kernel flags, no stack protector, every allocated
 *     section prefixed with ".boot" (Makefile), so it lands in the LOW identity
 *     .boot section next to the 32-bit entry and runs before the high half is
 *     mapped. It must not reference any high-half symbol (the link would fail
 *     with a truncated PC32 relocation, which is the intended tripwire).
 *   - uefi/u_kaslr_core.o: UEFI loader flags.
 *
 * Rules: no globals, no string literals (they would go to .rodata), no library
 * calls (GCC is told not to synthesize memset/memcpy). Physical addresses are
 * dereferenced directly: the caller runs on an identity map of low memory.
 *
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 */
#include "kaslr.h"

typedef uint64_t kaslr_u64_ua __attribute__((aligned(1), may_alias));
typedef uint32_t kaslr_u32_ua __attribute__((aligned(1), may_alias));

static inline void kc_cpuid(uint32_t leaf, uint32_t sub, uint32_t* a, uint32_t* b,
                            uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static inline uint64_t kc_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void kc_outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint8_t kc_inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* splitmix64 finalizer: a bijective avalanche mix (every input bit affects
 * every output bit), so low-entropy inputs such as TSC jitter spread over the
 * whole word before the slot is reduced. */
static inline uint64_t kc_mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static int kc_rdseed(uint64_t* out) {
    for (int i = 0; i < 64; i++) {
        uint64_t v; uint8_t ok;
        __asm__ volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
        if (ok) { *out = v; return 1; }
        __asm__ volatile("pause");
    }
    return 0;
}

static int kc_rdrand(uint64_t* out) {
    for (int i = 0; i < 16; i++) {
        uint64_t v; uint8_t ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
        if (ok) { *out = v; return 1; }
    }
    return 0;
}

int kaslr_hdr_valid(const struct kaslr_hdr* h) {
    if (!h || h->magic != KASLR_MAGIC || h->version != KASLR_VERSION) return 0;
    if (h->link_base != KASLR_LINK_BASE || h->align != KASLR_ALIGN) return 0;
    if (h->slots > KASLR_SLOTS) return 0;
    return 1;
}

uint64_t kaslr_entropy(uint64_t extra, int io, uint32_t* source) {
    uint32_t src = KASLR_SRC_TSC;
    uint64_t x = kc_mix(kc_rdtsc() ^ extra);
    uint32_t a, b, c, d, maxleaf;
    kc_cpuid(0, 0, &maxleaf, &b, &c, &d);
    if (maxleaf >= 7) {
        kc_cpuid(7, 0, &a, &b, &c, &d);
        uint64_t v;
        if ((b & (1u << 18)) && kc_rdseed(&v)) { x = kc_mix(x ^ v); src = KASLR_SRC_RDSEED; }
    }
    if (src == KASLR_SRC_TSC && maxleaf >= 1) {
        kc_cpuid(1, 0, &a, &b, &c, &d);
        uint64_t v;
        if ((c & (1u << 30)) && kc_rdrand(&v)) { x = kc_mix(x ^ v); src = KASLR_SRC_RDRAND; }
    }
    if (io) {
        /* Legacy timer/clock jitter: the PIT channel-0 count (latched) and
         * the RTC seconds/minutes. Weak alone, but free and independent of the
         * TSC; only mixed in, never relied on when a hardware RNG exists. */
        kc_outb(0x43, 0x00);
        uint64_t pit = kc_inb(0x40);
        pit |= (uint64_t)kc_inb(0x40) << 8;
        kc_outb(0x70, 0x00); uint64_t sec = kc_inb(0x71);
        kc_outb(0x70, 0x02); uint64_t min = kc_inb(0x71);
        x = kc_mix(x ^ (pit << 16) ^ (sec << 40) ^ (min << 48));
    }
    x = kc_mix(x ^ kc_rdtsc());
    if (extra && src == KASLR_SRC_TSC) src = KASLR_SRC_EFI_RNG;
    if (source) *source = src;
    return x;
}

uint64_t kaslr_pick_slide(uint64_t rnd, uint32_t slots) {
    if (slots <= 1) return 0;
    if (slots > KASLR_SLOTS) slots = KASLR_SLOTS;
    /* 64-bit value mod <= 768: the modulo bias is below 2^-54. */
    return (rnd % slots) * KASLR_ALIGN;
}

int kaslr_apply(struct kaslr_hdr* h, uint64_t slide, uint32_t source, uint32_t state) {
    if (!kaslr_hdr_valid(h)) return -1;
    if (h->state & KASLR_ST_APPLIED) return -1;      /* never relocate twice */
    const uint32_t* e = kaslr_entries(h);
    for (uint32_t i = 0; i < h->count; i++) {
        uint64_t addr = e[i] & KASLR_ENT_ADDR;
        if (e[i] & KASLR_ENT_32) {
            kaslr_u32_ua* p = (kaslr_u32_ua*)addr;
            *p = (uint32_t)(*p + (uint32_t)slide);
        } else {
            kaslr_u64_ua* p = (kaslr_u64_ua*)addr;
            *p = *p + slide;
        }
    }
    h->slide = slide;
    h->source = source;
    h->state = state | KASLR_ST_APPLIED;
    return 0;
}

void kaslr_set_guard(struct kaslr_hdr* h, uint64_t rnd) {
    if (!kaslr_hdr_valid(h) || !h->guard_phys || (h->state & KASLR_ST_CANARY)) return;
    uint64_t g = kc_mix(rnd) & ~0xFFULL;
    if (!g) g = 0x5EC05EC05EC05E00ULL;
    *(kaslr_u64_ua*)(uint64_t)h->guard_phys = g;
    h->state |= KASLR_ST_CANARY;
}

static inline int kc_is_sep(char c) {
    return c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int kaslr_cmdline_nokaslr(const char* s, uint64_t n) {
    /* "nokaslr" spelled as bytes (a string literal would land in .rodata). */
    char w[7];
    w[0] = 'n'; w[1] = 'o'; w[2] = 'k'; w[3] = 'a'; w[4] = 's'; w[5] = 'l'; w[6] = 'r';
    if (!s) return 0;
    for (uint64_t i = 0; i < n && s[i]; i++) {
        if (i > 0 && !kc_is_sep(s[i - 1])) continue;
        uint64_t k = 0;
        while (k < 7 && i + k < n && s[i + k] == w[k]) k++;
        if (k == 7 && (i + 7 >= n || kc_is_sep(s[i + 7]))) return 1;
    }
    return 0;
}

int kaslr_mb2_nokaslr(uint32_t magic, uint64_t info) {
    if (magic != 0x36d76289u || !info) return 0;
    uint32_t total = *(const kaslr_u32_ua*)info;
    if (total < 16 || total > (1u << 20)) return 0;
    uint64_t p = info + 8, end = info + total;
    while (p + 8 <= end) {
        uint32_t type = *(const kaslr_u32_ua*)p;
        uint32_t size = *(const kaslr_u32_ua*)(p + 4);
        if (type == 0 || size < 8) break;
        if (type == 1)                                   /* boot command line */
            return kaslr_cmdline_nokaslr((const char*)(p + 8), size - 8);
        p += ((uint64_t)size + 7) & ~7ULL;
    }
    return 0;
}

void kaslr_map_window(uint64_t* pdpt, uint64_t* pdt_a, uint64_t* pdt_b, uint64_t slide) {
    for (int i = 0; i < 512; i++) { pdt_a[i] = 0; pdt_b[i] = 0; }
    for (uint64_t i = 0; i < KASLR_WINDOW / KASLR_ALIGN; i++) {
        uint64_t va = KASLR_LINK_BASE + slide + i * KASLR_ALIGN;
        uint64_t* pdt = (((va >> 30) & 0x1FF) == 510) ? pdt_a : pdt_b;
        pdt[(va >> 21) & 0x1FF] = (i * KASLR_ALIGN) | 0x83;   /* Present|Write|PS */
    }
    pdpt[510] = (uint64_t)(uintptr_t)pdt_a | 0x3;
    pdpt[511] = (uint64_t)(uintptr_t)pdt_b | 0x3;
}

/* Multiboot2 relocator, called from boot/boot.asm (long_mode_low) on the boot
 * identity map before the jump to the high half. Returns the slide. */
uint64_t kaslr_boot_mb2(uint32_t magic, uint64_t info, struct kaslr_hdr* h,
                        uint64_t* pdpt, uint64_t* pdt_a, uint64_t* pdt_b) {
    uint64_t slide = 0;
    if (kaslr_hdr_valid(h) && (h->state & KASLR_ST_APPLIED)) {
        slide = h->slide;                    /* already relocated: keep it */
    } else if (kaslr_hdr_valid(h)) {
        uint32_t state = KASLR_ST_MB2, source = KASLR_SRC_NONE;
        if (h->flags & KASLR_F_DISABLED) {
            state |= KASLR_ST_OFF_BUILD;
        } else if (kaslr_mb2_nokaslr(magic, info)) {
            state |= KASLR_ST_OFF_CMD;
        } else {
            slide = kaslr_pick_slide(kaslr_entropy(0, 1, &source), h->slots);
            state |= KASLR_ST_RANDOM;
        }
        if (kaslr_apply(h, slide, source, state) != 0) slide = 0;
        /* A second, independent draw for the stack canary. */
        kaslr_set_guard(h, kaslr_entropy(slide, 1, &source));
    }
    kaslr_map_window(pdpt, pdt_a, pdt_b, slide);
    return slide;
}
