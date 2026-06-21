/*
 * SecOS Kernel - ELF signature verification
 * Copyright (c) 2025 iDev srl
 * SPDX-License-Identifier: MIT
 */
#include "elf_sign.h"
#include "elf.h"
#include "sha256.h"
#include "ed25519.h"
#include "secos_pubkey.h"

/* Locate the QSIG note's sig[64] field; returns its file offset, or 0 if none. */
static size_t find_qsig_sig_offset(const uint8_t* base, size_t size) {
    if (size < sizeof(Elf64_Ehdr)) return 0;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)base;
    if (eh->e_phoff == 0 || eh->e_phnum == 0) return 0;
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) return 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr* ph = (const Elf64_Phdr*)(base + eh->e_phoff + (size_t)i * sizeof(Elf64_Phdr));
        if ((const uint8_t*)ph + sizeof(Elf64_Phdr) > base + size) return 0;
        if (ph->p_type != PT_NOTE) continue;
        if (ph->p_offset + ph->p_filesz > size) return 0;
        const uint8_t* note = base + ph->p_offset;
        const uint8_t* end  = note + ph->p_filesz;
        while (note + 12 <= end) {
            uint32_t namesz = *(const uint32_t*)(note);
            uint32_t descsz = *(const uint32_t*)(note + 4);
            uint32_t type   = *(const uint32_t*)(note + 8);
            const char* name = (const char*)(note + 12);
            const uint8_t* desc = note + 12 + ((namesz + 3) & ~3u);
            const uint8_t* next = desc + ((descsz + 3) & ~3u);
            if (next > end) break;
            if (type == SECOS_SIG_NOTE_TYPE && namesz >= 6 &&
                name[0]=='S'&&name[1]=='E'&&name[2]=='C'&&name[3]=='O'&&name[4]=='S' &&
                descsz >= sizeof(secos_sig_raw_t)) {
                /* sig is at offset 8 within secos_sig_raw_t */
                return (size_t)((desc + 8) - base);
            }
            note = next;
        }
    }
    return 0;
}

/* [M35] Keyring + revocation. The signing format already carries a per-signature
 * key_id (QSIG note, offset 4). A signed-but-revoked key is refused even though
 * its signature still verifies — this is how a compromised signer is retired
 * without re-keying every other binary. The list is empty by default (the single
 * project dev key, id 0, is trusted); revoke at boot/policy time. */
#define SECOS_MAX_REVOKED 16
static uint32_t g_revoked_keys[SECOS_MAX_REVOKED];
static int      g_revoked_n;

void elf_sign_revoke_key(uint32_t key_id) {
    for (int i = 0; i < g_revoked_n; i++) if (g_revoked_keys[i] == key_id) return;
    if (g_revoked_n < SECOS_MAX_REVOKED) g_revoked_keys[g_revoked_n++] = key_id;
}
int elf_sign_key_revoked(uint32_t key_id) {
    for (int i = 0; i < g_revoked_n; i++) if (g_revoked_keys[i] == key_id) return 1;
    return 0;
}

int elf_signature_verify(const void* elf_buf, size_t size) {
    const uint8_t* base = (const uint8_t*)elf_buf;
    if (!base || size < sizeof(Elf64_Ehdr)) return ELF_SIG_FMT;

    size_t sig_off = find_qsig_sig_offset(base, size);
    if (sig_off == 0) return ELF_SIG_NOSIG;
    if (sig_off + 64 > size) return ELF_SIG_FMT;

    /* [M35] reject signatures from a revoked key (key_id is at sig_off-4). */
    if (sig_off >= 4) {
        uint32_t key_id = *(const uint32_t*)(base + sig_off - 4);
        if (elf_sign_key_revoked(key_id)) return ELF_SIG_BAD;
    }

    /* digest = SHA-256(file with the 64 sig bytes zeroed) */
    static const uint8_t zeros[64] = {0};
    uint8_t digest[32];
    sha256_ctx c; sha256_init(&c);
    sha256_update(&c, base, sig_off);
    sha256_update(&c, zeros, 64);
    sha256_update(&c, base + sig_off + 64, size - sig_off - 64);
    sha256_final(&c, digest);

    if (ed25519_verify(digest, sizeof(digest), base + sig_off, secos_trusted_pubkey) != 1)
        return ELF_SIG_BAD;
    return ELF_SIG_OK;
}
