/*
 * SecOS - [M38] WPA2-PSK crypto building blocks.
 * HMAC-SHA1, PBKDF2-HMAC-SHA1 (the WPA2-PSK PMK derivation), AES-128 and AES-CCM
 * (CCMP). KAT-validated at boot (wpa2_selftest): the PBKDF2 IEEE 802.11i vector
 * and the FIPS-197 AES vector. These are the cipher foundation an ath9k driver
 * needs to associate to a WPA2 network; the radio bring-up itself is M38 TODO.
 */
#ifndef SECOS_WPA2_H
#define SECOS_WPA2_H
#include <stdint.h>
#include <stddef.h>

void hmac_sha1(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen, uint8_t out[20]);

/* PBKDF2-HMAC-SHA1. For WPA2-PSK: pbkdf2(passphrase, ssid, ssid_len, 4096, pmk, 32). */
void pbkdf2_sha1(const char* pass, const uint8_t* salt, size_t slen, int iter,
                 uint8_t* out, int olen);

/* WPA2-PSK PMK = PBKDF2-HMAC-SHA1(passphrase, SSID, 4096, 256 bits). */
void wpa2_pmk(const char* passphrase, const char* ssid, uint8_t pmk[32]);

/* AES-128. rk must hold 176 bytes of expanded key. */
void aes128_expand(const uint8_t key[16], uint8_t rk[176]);
void aes128_encrypt(const uint8_t in[16], uint8_t out[16], const uint8_t rk[176]);

/* AES-CTR (used by CCMP for confidentiality). Encrypts/decrypts len bytes. */
void aes128_ctr(const uint8_t rk[176], const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len);

/* Run the known-answer tests. Returns 1 on success (logs [WPA2] markers). */
int wpa2_selftest(void);

#endif
