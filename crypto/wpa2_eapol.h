/*
 * SecOS - [M39] WPA2 4-way handshake (EAPOL-Key) software core.
 *
 * The cryptographic heart of associating to a WPA2-PSK network, on top of the
 * M38 primitives (PBKDF2 PMK, AES-128, HMAC-SHA1). Everything here is independent
 * of the radio, so it is KAT-validated at boot like the other crypto; the ath9k
 * radio bring-up that actually exchanges these frames over the air is the
 * remaining real-hardware work (docs/devlog/M38/M39).
 *
 *   PMK --(4-way handshake: ANonce/SNonce + MACs)--> PTK = KCK|KEK|TK
 *   msg3 carries the GTK, AES-key-wrapped (RFC 3394) under the KEK.
 *   Each EAPOL-Key frame is authenticated by a Key MIC = HMAC-SHA1(KCK, frame)[:16].
 *
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 */
#ifndef SECOS_WPA2_EAPOL_H
#define SECOS_WPA2_EAPOL_H
#include <stdint.h>
#include <stddef.h>

/* AES-128 inverse cipher (FIPS-197). Needed by RFC 3394 key unwrap. */
void aes128_decrypt(const uint8_t in[16], uint8_t out[16], const uint8_t rk[176]);

/* RFC 3394 AES key wrap/unwrap. n = number of 64-bit blocks of key data
 * (out is (n+1)*8 for wrap, in is (n+1)*8 for unwrap). Unwrap returns 1 if the
 * integrity check value matched (A == 0xA6A6...), else 0. */
void aes_key_wrap  (const uint8_t kek[16], const uint8_t* kdata, int n, uint8_t* out);
int  aes_key_unwrap(const uint8_t kek[16], const uint8_t* in,    int n, uint8_t* out);

/* IEEE 802.11 PRF using HMAC-SHA1: out = PRF-(olen*8)(key, label || data). */
void wpa2_prf_sha1(const uint8_t* key, size_t klen, const char* label,
                   const uint8_t* data, size_t dlen, uint8_t* out, size_t olen);

/* Derive the Pairwise Transient Key from the PMK and the 4-way handshake nonces
 * and MAC addresses. ptk must hold ptk_len bytes (48 for CCMP: KCK|KEK|TK,
 * 16 each). aa = authenticator (AP) MAC, spa = supplicant (STA) MAC. */
void wpa2_derive_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
                     const uint8_t anonce[32], const uint8_t snonce[32],
                     uint8_t* ptk, size_t ptk_len);

/* EAPOL-Key MIC = HMAC-SHA1(KCK, frame)[:16] (the AKM SHA1 variant). The frame's
 * own MIC field must be zeroed before computing. */
void wpa2_eapol_mic(const uint8_t kck[16], const uint8_t* frame, size_t len, uint8_t mic[16]);

/* KAT: RFC 3394 §4.1 key-wrap vector + a PTK derivation smoke. Returns 1 on pass
 * (logs [WPA2] markers). */
int wpa2_eapol_selftest(void);

#endif
