/*
 * SecOS Kernel - Ed25519 signature verification (RFC 8032)
 * Copyright (c) 2025 iDev srl
 * SPDX-License-Identifier: MIT
 *
 * Verify-only, freestanding, no malloc. The kernel embeds the trusted public
 * key and only ever verifies; the private key never ships on-device. Field
 * arithmetic adapted from TweetNaCl (public domain, D. J. Bernstein et al.).
 */
#ifndef SECOS_ED25519_H
#define SECOS_ED25519_H

#include <stdint.h>
#include <stddef.h>

/* Verify a detached Ed25519 signature.
 *   msg/mlen : the signed message
 *   sig[64]  : R (32) || S (32)
 *   pk[32]   : public key
 * Returns 1 if the signature is valid, 0 otherwise. */
int ed25519_verify(const uint8_t* msg, size_t mlen,
                   const uint8_t sig[64], const uint8_t pk[32]);

#endif /* SECOS_ED25519_H */
