/*
 * SecOS UEFI loader — full secure boot.
 *
 * Ed25519-verify the kernel ELF before jumping to it. The kernel.bin carries a
 * `.note.secos` QSIG note (boot/kernel_note.S) signed by tools/secos-sign with
 * the project key; this verifies it against the same trusted public key the
 * kernel uses for user ELFs (crypto/secos_pubkey.h). Trust now extends from the
 * firmware down through the loader to the kernel — no unsigned/tampered kernel
 * runs on the UEFI path.
 *
 * The digest is identical to mm/elf_sign.c and tools/secos_signlib.py:
 *   SHA-256(entire file, with the 64-byte QSIG sig field zeroed), Ed25519 over it.
 *
 * Crypto (crypto/sha256.c, sha512.c, ed25519.c) is freestanding and compiled
 * into the loader with the UEFI flags (see the Makefile UEFI_CRYPTO_OBJS).
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>
#include "sha256.h"
#include "ed25519.h"
#include "secos_pubkey.h"

/* freestanding mem* for the crypto objects (the loader links -nostdlib and gnu-efi
 * does not reliably export these). Non-static so the compiler-emitted calls bind. */
void* memcpy(void* d, const void* s, size_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
    return d;
}
void* memset(void* d, int c, size_t n) {
    uint8_t* dd = (uint8_t*)d;
    while (n--) *dd++ = (uint8_t)c;
    return d;
}
int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* aa = (const uint8_t*)a; const uint8_t* bb = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) { if (aa[i] != bb[i]) return (int)aa[i] - (int)bb[i]; }
    return 0;
}

static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd_u64(const uint8_t* p) {
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}
static uint16_t rd_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

#define SB_PT_NOTE   4u
#define SB_QSIG_TYPE 0x51534947u   /* 'QSIG' */

/* Locate the QSIG note's sig[64] field; returns its absolute file offset, or 0.
 * Mirrors mm/elf_sign.c find_qsig_sig_offset (PT_NOTE walk). */
static uint64_t sb_find_qsig(const uint8_t* base, uint64_t size) {
    if (size < 64) return 0;
    if (!(base[0] == 0x7f && base[1] == 'E' && base[2] == 'L' && base[3] == 'F')) return 0;
    uint64_t e_phoff   = rd_u64(base + 0x20);
    uint16_t e_phentsz = rd_u16(base + 0x36);
    uint16_t e_phnum   = rd_u16(base + 0x38);
    if (e_phentsz < 56) return 0;
    for (uint16_t i = 0; i < e_phnum; i++) {
        uint64_t ph = e_phoff + (uint64_t)i * e_phentsz;
        if (ph + 56 > size) return 0;
        uint32_t p_type   = rd_u32(base + ph + 0);
        uint64_t p_offset = rd_u64(base + ph + 8);
        uint64_t p_filesz = rd_u64(base + ph + 32);
        if (p_type != SB_PT_NOTE) continue;
        if (p_offset + p_filesz > size) return 0;
        uint64_t note = p_offset, end = p_offset + p_filesz;
        while (note + 12 <= end) {
            uint32_t namesz = rd_u32(base + note);
            uint32_t descsz = rd_u32(base + note + 4);
            uint32_t type   = rd_u32(base + note + 8);
            uint64_t desc   = note + 12 + ((namesz + 3) & ~3u);
            uint64_t next   = desc + ((descsz + 3) & ~3u);
            if (next > end) break;
            if (type == SB_QSIG_TYPE && namesz >= 6 && descsz >= 72 &&
                base[note + 12] == 'S' && base[note + 13] == 'E' && base[note + 14] == 'C' &&
                base[note + 15] == 'O' && base[note + 16] == 'S') {
                return desc + 8;   /* sig is at offset 8 within { u32 ver; u32 key_id; u8 sig[64] } */
            }
            note = next;
        }
    }
    return 0;
}

/* Returns 1 if the kernel image is correctly signed, 0 otherwise. */
int secure_boot_verify(const uint8_t* buf, uint64_t len) {
    if (!buf || len < 64) return 0;
    uint64_t sig_off = sb_find_qsig(buf, len);
    if (sig_off == 0 || sig_off + 64 > len) return 0;

    static const uint8_t zeros[64] = {0};
    uint8_t digest[32];
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, buf, sig_off);
    sha256_update(&c, zeros, 64);
    sha256_update(&c, buf + sig_off + 64, len - sig_off - 64);
    sha256_final(&c, digest);

    return ed25519_verify(digest, sizeof(digest), buf + sig_off, secos_trusted_pubkey) == 1 ? 1 : 0;
}
