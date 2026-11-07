/*
 * kaslr.c — kernel side of KASLR (M45). The relocation itself happens before
 * the kernel runs (boot/kaslr_core.c via boot/boot.asm on Multiboot2, the UEFI
 * loader on UEFI); this file reads the result back from the .kaslr table,
 * self-checks it, reports it, and provides the slide-independent measurement
 * used by measured boot (M37).
 *
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>
#include "kaslr.h"
#include "../crypto/sha256.h"
#include "../lib/debugcon.h"
#include "../lib/terminal.h"

/* .data, not .bss: written before anything could clear .bss. */
uint64_t g_kaslr_slide __attribute__((section(".data"))) = 0;

extern char _kaslr_start[];
static const struct kaslr_hdr* g_hdr;   /* NULL if the table is missing/invalid */

static int hdr_ok(const struct kaslr_hdr* h) {
    return h->magic == KASLR_MAGIC && h->version == KASLR_VERSION &&
           h->link_base == KASLR_LINK_BASE && h->align == KASLR_ALIGN;
}

static void __attribute__((noreturn)) kaslr_fatal(const char* why) {
    debugcon_writestring("[KASLR] FATAL: ");
    debugcon_writestring(why);
    debugcon_writestring("\n");
    for (;;) __asm__ volatile("cli; hlt");
}

void kaslr_init(void) {
    const struct kaslr_hdr* h = (const struct kaslr_hdr*)(const void*)_kaslr_start;
    /* Where the table really is (RIP-relative: correct whatever the slide) vs.
     * where the relocated absolute references say it is (a movabs immediate
     * the relocator patched). A mismatch means the relocations were not, or
     * were wrongly, applied — continuing would run on a corrupt image. */
    uint64_t rt, ab, phys;
    __asm__ volatile("leaq _kaslr_start(%%rip), %0" : "=r"(rt));
    __asm__ volatile("movabsq $_kaslr_start, %0" : "=r"(ab));
    __asm__ volatile("movabsq $_kaslr_phys, %0" : "=r"(phys));
    if (rt != ab) kaslr_fatal("absolute references do not match the load address");
    uint64_t derived = rt - phys - KASLR_LINK_BASE;   /* runtime base - link base */
    uint64_t slide = 0;
    if (hdr_ok(h)) {
        g_hdr = h;
        if (h->state & KASLR_ST_APPLIED) slide = h->slide;
    }
    if (slide != derived) kaslr_fatal("table slide does not match the load address");
    if (slide % KASLR_ALIGN || slide >= (uint64_t)KASLR_SLOTS * KASLR_ALIGN)
        kaslr_fatal("slide out of range");
    g_kaslr_slide = slide;
}

static const char* src_name(uint32_t s) {
    switch (s) {
    case KASLR_SRC_RDSEED:  return "RDSEED";
    case KASLR_SRC_RDRAND:  return "RDRAND";
    case KASLR_SRC_EFI_RNG: return "EFI_RNG";
    case KASLR_SRC_TSC:     return "TSC+PIT+RTC jitter (no hardware RNG)";
    default:                return "none";
    }
}

static void both(const char* s) { terminal_writestring(s); debugcon_writestring(s); }

static void both_dec(uint64_t v) {
    char b[21]; int i = 20; b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    both(&b[i]);
}

extern uintptr_t __stack_chk_guard;   // kernel/panic.c

static void canary_report(const struct kaslr_hdr* h) {
    if (h && (h->state & KASLR_ST_CANARY))
        both("[SSP] stack canary randomized at boot\n");
    else
        both("[SSP] stack canary NOT randomized (compile-time value)\n");
#ifdef SECOS_KASLR_DEBUG
    {
        static const char hx[] = "0123456789abcdef";
        const char* p = "[KASLR-DEBUG] canary=0x";
        while (*p) debugcon_putchar_raw(*p++);
        for (int i = 60; i >= 0; i -= 4)
            debugcon_putchar_raw(hx[((uint64_t)__stack_chk_guard >> i) & 0xF]);
        debugcon_putchar_raw('\n');
    }
#endif
}

static void slide_report(const struct kaslr_hdr* h);

void kaslr_report(void) {
    slide_report(g_hdr);
    canary_report(g_hdr);
}

static void slide_report(const struct kaslr_hdr* h) {
    if (!h) { both("[KASLR] off (no relocation table)\n"); return; }
    if (h->state & KASLR_ST_OFF_BUILD) { both("[KASLR] off (built with SECOS_NO_KASLR)\n"); return; }
    if (h->state & KASLR_ST_OFF_CMD)   { both("[KASLR] off (nokaslr boot option)\n"); return; }
    if (!(h->state & KASLR_ST_RANDOM)) { both("[KASLR] off (image not relocated)\n"); return; }
    /* 768 slots = log2(768) ~ 9.6 bits. */
    both("[KASLR] on: kernel base randomized, ");
    both_dec(h->slots);
    both(" slots x 2 MiB (~9.6 bits), entropy=");
    both(src_name(h->source));
    both(", relocs=");
    both_dec(h->count);
    both((h->state & KASLR_ST_UEFI) ? ", by UEFI loader\n" : ", by MB2 entry\n");
#ifdef SECOS_KASLR_DEBUG
    /* Test builds only, and only on raw debugcon (NOT the kernel log ring that
     * dmesg / /proc/kmsg expose to user space). */
    {
        static const char hx[] = "0123456789abcdef";
        const char* p = "[KASLR-DEBUG] slide=0x";
        while (*p) debugcon_putchar_raw(*p++);
        for (int i = 60; i >= 0; i -= 4) debugcon_putchar_raw(hx[(g_kaslr_slide >> i) & 0xF]);
        debugcon_putchar_raw('\n');
    }
#endif
}

void kaslr_sha256_unrelocated(const uint8_t* start, const uint8_t* end, uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    const uint8_t* cur = start;
    const struct kaslr_hdr* h = g_hdr;
    uint64_t slide = g_kaslr_slide;
    if (h && slide) {
        /* Entries hold physical addresses; image VA = phys + runtime base. */
        uint64_t base = KASLR_LINK_BASE + slide;
        const uint32_t* e = kaslr_entries(h);
        for (uint32_t i = 0; i < h->count; i++) {
            const uint8_t* f = (const uint8_t*)(uintptr_t)(base + (e[i] & KASLR_ENT_ADDR));
            int w = (e[i] & KASLR_ENT_32) ? 4 : 8;
            if (f + w <= cur) continue;          /* before the range (sorted) */
            if (f >= end) break;
            if (f < cur) continue;               /* straddles the start: cannot happen */
            sha256_update(&c, cur, (size_t)(f - cur));
            uint8_t orig[8];
            uint64_t v = 0;
            for (int k = 0; k < w; k++) v |= (uint64_t)f[k] << (8 * k);
            v -= slide;                          /* the 32S field: low 32 bits */
            for (int k = 0; k < w; k++) orig[k] = (uint8_t)(v >> (8 * k));
            size_t n = (f + w <= end) ? (size_t)w : (size_t)(end - f);
            sha256_update(&c, orig, n);
            cur = f + n;
        }
    }
    if (cur < end) sha256_update(&c, cur, (size_t)(end - cur));
    sha256_final(&c, out);
}
