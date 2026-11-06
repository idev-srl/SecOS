/*
 * kaslr.h — kernel address-space layout randomization (M45): relocation table
 * format and the freestanding core shared by both boot paths.
 *
 * The kernel is linked at KASLR_LINK_BASE (0xFFFFFFFF80000000, -mcmodel=kernel)
 * and loaded at a fixed physical address. At boot the relocator (the early
 * 64-bit code of boot/boot.asm on Multiboot2, the UEFI loader on UEFI) picks a
 * random 2 MiB-aligned slide, adds it to every absolute reference to a high-half
 * address recorded in the table below, and maps the high-half window at
 * KASLR_LINK_BASE + slide. PC-relative references need no fixup because the
 * whole image moves as one block.
 *
 * The table is produced at link time by tools/kaslr-relocs.py from the
 * `--emit-relocs` output and linked into the kernel as section `.kaslr`, so it
 * is inside the signed file (secure boot covers it) and loaded with the image.
 *
 * Layout (little endian):
 *   struct kaslr_hdr                 (56 bytes)
 *   uint32_t entries[hdr.count]      sorted by address; bit 31 = 32-bit field
 *                                    (R_X86_64_32S), else 64-bit (R_X86_64_64);
 *                                    bits 0..30 = physical address of the field.
 *
 * The `slide`, `source` and `state` header fields are zero in the file and
 * filled by the relocator at boot; the kernel reads them back (kernel/kaslr.c).
 *
 * `guard_phys` is the physical address of the stack-protector guard
 * (__stack_chk_guard). The relocator overwrites it with a fresh random value
 * before the first instruction of kernel C code runs, so no protected frame
 * ever holds the compile-time constant. This is done even when the slide is
 * disabled (nokaslr / SECOS_NO_KASLR).
 *
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 */
#ifndef SECOS_KASLR_H
#define SECOS_KASLR_H

#include <stdint.h>

#define KASLR_MAGIC        0x4C53414Bu   /* 'KASL' */
#define KASLR_VERSION      2u
#define KASLR_LINK_BASE    0xFFFFFFFF80000000ULL
#define KASLR_ALIGN        (2ULL << 20)          /* slide granularity: 2 MiB */
#define KASLR_WINDOW       (512ULL << 20)        /* high-half window: phys 0..512 MiB */
/* The window [base+slide, base+slide+512 MiB) must stay inside the top 2 GiB
 * (-mcmodel=kernel), so the slide is one of (2 GiB - 512 MiB) / 2 MiB = 768
 * slots: ~9.58 bits of entropy. */
#define KASLR_SLOTS        768u

#define KASLR_ENT_32       0x80000000u
#define KASLR_ENT_ADDR     0x7FFFFFFFu

/* hdr.flags (build time) */
#define KASLR_F_DISABLED   0x0001u   /* built with -DSECOS_NO_KASLR */

/* hdr.state (written by the relocator) */
#define KASLR_ST_APPLIED   0x0001u   /* relocations applied (slide may be 0) */
#define KASLR_ST_RANDOM    0x0002u   /* slide chosen at random */
#define KASLR_ST_OFF_BUILD 0x0004u   /* disabled at build time */
#define KASLR_ST_OFF_CMD   0x0008u   /* disabled by the `nokaslr` boot option */
#define KASLR_ST_UEFI      0x0010u   /* relocated by the UEFI loader */
#define KASLR_ST_MB2       0x0020u   /* relocated by the Multiboot2 entry */
#define KASLR_ST_CANARY    0x0040u   /* stack-protector guard randomized */

/* hdr.source: entropy source of the slide */
#define KASLR_SRC_NONE     0u
#define KASLR_SRC_TSC      1u        /* RDTSC + PIT + RTC jitter only */
#define KASLR_SRC_RDRAND   2u
#define KASLR_SRC_RDSEED   3u
#define KASLR_SRC_EFI_RNG  4u        /* EFI_RNG_PROTOCOL (UEFI loader) */

struct kaslr_hdr {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t count;       /* number of entries */
    uint32_t slots;       /* number of 2 MiB slots the slide is drawn from */
    uint64_t link_base;   /* KASLR_LINK_BASE */
    uint64_t align;       /* KASLR_ALIGN */
    uint64_t slide;       /* runtime: chosen slide (bytes) */
    uint32_t source;      /* runtime: KASLR_SRC_* */
    uint32_t state;       /* runtime: KASLR_ST_* */
    uint32_t guard_phys;  /* physical address of __stack_chk_guard (0 = none) */
    uint32_t reserved;
};

static inline const uint32_t* kaslr_entries(const struct kaslr_hdr* h) {
    return (const uint32_t*)(const void*)(h + 1);
}

/* ---- Freestanding core (boot/kaslr_core.c) ----
 * Compiled twice: into the kernel's low .boot section (Multiboot2 relocator,
 * runs on the boot identity map) and into the UEFI loader. Pure code: no
 * globals, no library calls. Physical addresses are dereferenced directly, so
 * the caller must run with the low physical memory identity-mapped. */

/* Basic table sanity: magic/version/geometry. */
int kaslr_hdr_valid(const struct kaslr_hdr* h);
/* Collect entropy from RDSEED / RDRAND / RDTSC (+ PIT and RTC when io != 0).
 * `extra` is mixed in (e.g. EFI_RNG output). *source receives KASLR_SRC_*. */
uint64_t kaslr_entropy(uint64_t extra, int io, uint32_t* source);
/* Map 64 random bits to a 2 MiB-aligned slide below slots * 2 MiB. */
uint64_t kaslr_pick_slide(uint64_t rnd, uint32_t slots);
/* Add `slide` to every recorded field. Refuses (returns -1) if the table was
 * already applied. Writes slide/source/state into the header. */
int kaslr_apply(struct kaslr_hdr* h, uint64_t slide, uint32_t source, uint32_t state);
/* Store a random stack-protector guard (low byte zero, so a C-string overflow
 * cannot reproduce it) at h->guard_phys; sets KASLR_ST_CANARY. */
void kaslr_set_guard(struct kaslr_hdr* h, uint64_t rnd);
/* Word search for "nokaslr" in an 8-bit command line (n bytes, or NUL). */
int kaslr_cmdline_nokaslr(const char* s, uint64_t n);
/* Multiboot2: search the command-line tag of the MB2 info block. */
int kaslr_mb2_nokaslr(uint32_t magic, uint64_t info);
/* Fill the high-half window for `slide`: pdpt[510] -> pdt_a, pdpt[511] -> pdt_b,
 * 256 x 2 MiB pages mapping phys 0..512 MiB at KASLR_LINK_BASE + slide.
 * pdt_a/pdt_b are zeroed first. Entries are Present|Write|PS (the kernel
 * applies W^X after it installs its own tables). */
void kaslr_map_window(uint64_t* pdpt, uint64_t* pdt_a, uint64_t* pdt_b, uint64_t slide);

/* ---- Kernel side (kernel/kaslr.c), high-half code ---- */
extern uint64_t g_kaslr_slide;   /* runtime base = KASLR_LINK_BASE + g_kaslr_slide */
/* Read the slide back from the table and self-check it. First call in
 * kernel_main, before anything translates a kernel address. */
void kaslr_init(void);
/* Log "KASLR on/off" (never the slide, except on raw debugcon in a
 * -DSECOS_KASLR_DEBUG build). */
void kaslr_report(void);
/* SHA-256 of [start, end) of the kernel image as it is in the FILE (every
 * relocated field reverted to its link-time value): slide-independent. */
void kaslr_sha256_unrelocated(const uint8_t* start, const uint8_t* end, uint8_t out[32]);

#endif /* SECOS_KASLR_H */
