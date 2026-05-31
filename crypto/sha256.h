/*
 * SecOS Kernel - SHA-256 (FIPS 180-4)
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Freestanding, no malloc. Used to compute the signed digest of an ELF image
 * for the SecOS code-signing trust model (docs/SIGNING.md).
 */
#ifndef SECOS_SHA256_H
#define SECOS_SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE  64

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[SHA256_BLOCK_SIZE];
    size_t   buflen;
} sha256_ctx;

void sha256_init(sha256_ctx* c);
void sha256_update(sha256_ctx* c, const void* data, size_t len);
void sha256_final(sha256_ctx* c, uint8_t out[SHA256_DIGEST_SIZE]);

/* One-shot helper. */
void sha256(const void* data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

#endif /* SECOS_SHA256_H */
