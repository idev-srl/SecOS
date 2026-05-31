/*
 * SecOS Kernel - SHA-512 (FIPS 180-4)
 * Copyright (c) 2025 iDev srl
 * SPDX-License-Identifier: MIT
 *
 * Freestanding, no malloc. Used internally by Ed25519 (RFC 8032).
 */
#ifndef SECOS_SHA512_H
#define SECOS_SHA512_H

#include <stdint.h>
#include <stddef.h>

#define SHA512_DIGEST_SIZE 64
#define SHA512_BLOCK_SIZE  128

typedef struct {
    uint64_t state[8];
    uint64_t bitlen_lo, bitlen_hi;
    uint8_t  buf[SHA512_BLOCK_SIZE];
    size_t   buflen;
} sha512_ctx;

void sha512_init(sha512_ctx* c);
void sha512_update(sha512_ctx* c, const void* data, size_t len);
void sha512_final(sha512_ctx* c, uint8_t out[SHA512_DIGEST_SIZE]);
void sha512(const void* data, size_t len, uint8_t out[SHA512_DIGEST_SIZE]);

#endif /* SECOS_SHA512_H */
