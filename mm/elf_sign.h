/*
 * SecOS Kernel - ELF signature verification (code-signing trust root)
 * Copyright (c) 2025 iDev srl
 * SPDX-License-Identifier: MIT
 *
 * Verifies the Ed25519 signature carried in an ELF's `.note.secos` QSIG note
 * against the kernel's embedded trusted public key. See docs/SIGNING.md.
 */
#ifndef SECOS_ELF_SIGN_H
#define SECOS_ELF_SIGN_H

#include <stdint.h>
#include <stddef.h>

#define SECOS_SIG_NOTE_TYPE 0x51534947U   /* 'QSIG' */

typedef struct secos_sig_raw {
    uint32_t version;   /* = 1 */
    uint32_t key_id;    /* 0 = project key (v0) */
    uint8_t  sig[64];   /* Ed25519 over SHA-256(file with these 64 bytes zeroed) */
} secos_sig_raw_t;

#define ELF_SIG_OK     0    /* valid signature */
#define ELF_SIG_NOSIG (-1)  /* no QSIG note present */
#define ELF_SIG_BAD   (-2)  /* signature present but invalid */
#define ELF_SIG_FMT   (-3)  /* malformed ELF / note */

/* Returns ELF_SIG_OK only for a present, valid signature. */
int elf_signature_verify(const void* elf_buf, size_t size);

/* [M35] Keyring revocation: a signature from a revoked key_id is refused even if
 * cryptographically valid (retire a compromised signer without re-keying). */
void elf_sign_revoke_key(uint32_t key_id);
int  elf_sign_key_revoked(uint32_t key_id);

#endif /* SECOS_ELF_SIGN_H */
