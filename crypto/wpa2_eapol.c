/*
 * SecOS - [M39] WPA2 4-way handshake (EAPOL-Key) software core.
 * See wpa2_eapol.h. KAT-validated at boot (RFC 3394 AES key-wrap vector).
 * SPDX-License-Identifier: MIT
 */
#include "wpa2_eapol.h"
#include "wpa2.h"     /* aes128_expand, aes128_encrypt, hmac_sha1 */
#include "debugcon.h"

static void ememcpy(void* d, const void* s, size_t n){ uint8_t* a=d; const uint8_t* b=s; while(n--) *a++=*b++; }
static void ememset(void* d, int v, size_t n){ uint8_t* a=d; while(n--) *a++=(uint8_t)v; }
static int  ememcmp(const void* a, const void* b, size_t n){ const uint8_t* x=a; const uint8_t* y=b; for(size_t i=0;i<n;i++){ if(x[i]!=y[i]) return (int)x[i]-(int)y[i]; } return 0; }
static size_t estrlen(const char* s){ size_t n=0; while(s[n]) n++; return n; }

/* --- AES-128 inverse cipher (FIPS-197) --- */
static const uint8_t rsbox[256] = {
0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d};

static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        uint8_t hi = a & 0x80; a <<= 1; if (hi) a ^= 0x1b; b >>= 1;
    }
    return p;
}

void aes128_decrypt(const uint8_t in[16], uint8_t out[16], const uint8_t rk[176]) {
    uint8_t s[16]; ememcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= rk[160 + i];   // undo final AddRoundKey
    for (int round = 9; round >= 0; round--) {
        // InvShiftRows (state byte i: row=i%4, col=i/4; row r rotated right by r)
        uint8_t t[16];
        t[0]=s[0];  t[4]=s[4];  t[8]=s[8];   t[12]=s[12];           // row 0
        t[1]=s[13]; t[5]=s[1];  t[9]=s[5];   t[13]=s[9];            // row 1 >> 1
        t[2]=s[10]; t[6]=s[14]; t[10]=s[2];  t[14]=s[6];            // row 2 >> 2
        t[3]=s[7];  t[7]=s[11]; t[11]=s[15]; t[15]=s[3];            // row 3 >> 3
        for (int i = 0; i < 16; i++) s[i] = rsbox[t[i]];            // InvSubBytes
        for (int i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];    // AddRoundKey(round)
        if (round > 0) {                                           // InvMixColumns
            for (int c = 0; c < 4; c++) {
                uint8_t* p = s + c * 4;
                uint8_t b0=p[0],b1=p[1],b2=p[2],b3=p[3];
                p[0]=gmul(b0,14)^gmul(b1,11)^gmul(b2,13)^gmul(b3,9);
                p[1]=gmul(b0,9)^gmul(b1,14)^gmul(b2,11)^gmul(b3,13);
                p[2]=gmul(b0,13)^gmul(b1,9)^gmul(b2,14)^gmul(b3,11);
                p[3]=gmul(b0,11)^gmul(b1,13)^gmul(b2,9)^gmul(b3,14);
            }
        }
    }
    ememcpy(out, s, 16);
}

/* --- RFC 3394 AES key wrap/unwrap --- */
#define KW_MAX_N 8   /* up to 64 bytes of key data (GTK + padding) */

void aes_key_wrap(const uint8_t kek[16], const uint8_t* kdata, int n, uint8_t* out) {
    if (n < 1 || n > KW_MAX_N) return;
    uint8_t rk[176]; aes128_expand(kek, rk);
    uint8_t A[8]; ememset(A, 0xA6, 8);
    uint8_t R[KW_MAX_N][8];
    for (int i = 0; i < n; i++) ememcpy(R[i], kdata + i*8, 8);
    for (int j = 0; j <= 5; j++) {
        for (int i = 1; i <= n; i++) {
            uint8_t blk[16], enc[16];
            ememcpy(blk, A, 8); ememcpy(blk + 8, R[i-1], 8);
            aes128_encrypt(blk, enc, rk);
            ememcpy(A, enc, 8);
            uint64_t t = (uint64_t)n * j + i;
            for (int b = 0; b < 8; b++) A[7-b] ^= (uint8_t)(t >> (8*b));
            ememcpy(R[i-1], enc + 8, 8);
        }
    }
    ememcpy(out, A, 8);
    for (int i = 0; i < n; i++) ememcpy(out + 8 + i*8, R[i], 8);
}

int aes_key_unwrap(const uint8_t kek[16], const uint8_t* in, int n, uint8_t* out) {
    if (n < 1 || n > KW_MAX_N) return 0;
    uint8_t rk[176]; aes128_expand(kek, rk);
    uint8_t A[8]; ememcpy(A, in, 8);
    uint8_t R[KW_MAX_N][8];
    for (int i = 0; i < n; i++) ememcpy(R[i], in + 8 + i*8, 8);
    for (int j = 5; j >= 0; j--) {
        for (int i = n; i >= 1; i--) {
            uint64_t t = (uint64_t)n * j + i;
            uint8_t blk[16], dec[16];
            ememcpy(blk, A, 8);
            for (int b = 0; b < 8; b++) blk[7-b] ^= (uint8_t)(t >> (8*b));
            ememcpy(blk + 8, R[i-1], 8);
            aes128_decrypt(blk, dec, rk);
            ememcpy(A, dec, 8);
            ememcpy(R[i-1], dec + 8, 8);
        }
    }
    int ok = 1;
    for (int i = 0; i < 8; i++) if (A[i] != 0xA6) ok = 0;
    for (int i = 0; i < n; i++) ememcpy(out + i*8, R[i], 8);
    return ok;
}

/* --- IEEE 802.11 PRF (HMAC-SHA1) --- */
void wpa2_prf_sha1(const uint8_t* key, size_t klen, const char* label,
                   const uint8_t* data, size_t dlen, uint8_t* out, size_t olen) {
    size_t lab = estrlen(label) + 1;   /* label includes its NUL terminator */
    uint8_t buf[160]; uint8_t dig[20];
    size_t pos = 0; uint8_t counter = 0;
    while (pos < olen) {
        size_t k = 0;
        ememcpy(buf + k, label, lab); k += lab;
        ememcpy(buf + k, data, dlen); k += dlen;
        buf[k++] = counter;
        hmac_sha1(key, klen, buf, k, dig);
        size_t cp = (olen - pos < 20) ? (olen - pos) : 20;
        ememcpy(out + pos, dig, cp);
        pos += cp; counter++;
    }
}

void wpa2_derive_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
                     const uint8_t anonce[32], const uint8_t snonce[32],
                     uint8_t* ptk, size_t ptk_len) {
    uint8_t data[76];
    if (ememcmp(aa, spa, 6) < 0) { ememcpy(data, aa, 6); ememcpy(data + 6, spa, 6); }
    else                        { ememcpy(data, spa, 6); ememcpy(data + 6, aa, 6); }
    if (ememcmp(anonce, snonce, 32) < 0) { ememcpy(data + 12, anonce, 32); ememcpy(data + 44, snonce, 32); }
    else                                 { ememcpy(data + 12, snonce, 32); ememcpy(data + 44, anonce, 32); }
    wpa2_prf_sha1(pmk, 32, "Pairwise key expansion", data, 76, ptk, ptk_len);
}

void wpa2_eapol_mic(const uint8_t kck[16], const uint8_t* frame, size_t len, uint8_t mic[16]) {
    uint8_t full[20];
    hmac_sha1(kck, 16, frame, len, full);
    ememcpy(mic, full, 16);
}

int wpa2_eapol_selftest(void) {
    /* RFC 3394 §4.1: wrap 128 bits of key data with a 128-bit KEK. */
    static const uint8_t kek[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                                    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    static const uint8_t kdata[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                                      0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    static const uint8_t expect[24] = {
        0x1f,0xa6,0x8b,0x0a,0x81,0x12,0xb4,0x47,
        0xae,0xf3,0x4b,0xd8,0xfb,0x5a,0x7b,0x82,
        0x9d,0x3e,0x86,0x23,0x71,0xd2,0xcf,0xe5};
    uint8_t wrapped[24], unwrapped[16];
    aes_key_wrap(kek, kdata, 2, wrapped);
    int wok = (ememcmp(wrapped, expect, 24) == 0);
    int uok = aes_key_unwrap(kek, wrapped, 2, unwrapped) && (ememcmp(unwrapped, kdata, 16) == 0);
    debugcon_writestring(wok ? "[WPA2] RFC3394 key-wrap KAT PASS\n" : "[WPA2] RFC3394 key-wrap KAT FAIL\n");
    debugcon_writestring(uok ? "[WPA2] RFC3394 key-unwrap KAT PASS\n" : "[WPA2] RFC3394 key-unwrap KAT FAIL\n");

    /* PTK derivation smoke: deterministic, exercises PRF/HMAC end-to-end. */
    static const uint8_t pmk[32] = {
        0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
        0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e};
    static const uint8_t aa[6]  = {0x00,0x11,0x22,0x33,0x44,0x55};
    static const uint8_t spa[6] = {0x66,0x77,0x88,0x99,0xaa,0xbb};
    uint8_t anonce[32], snonce[32];
    for (int i = 0; i < 32; i++) { anonce[i] = (uint8_t)(0xA0 + i); snonce[i] = (uint8_t)(0x50 + i); }
    uint8_t ptk[48];
    wpa2_derive_ptk(pmk, aa, spa, anonce, snonce, ptk, 48);
    /* PTK must be non-trivial (PRF actually ran) and reproducible. */
    uint8_t ptk2[48];
    wpa2_derive_ptk(pmk, aa, spa, anonce, snonce, ptk2, 48);
    int pok = (ememcmp(ptk, ptk2, 48) == 0);
    int nontrivial = 0; for (int i = 0; i < 48; i++) if (ptk[i]) nontrivial = 1;
    debugcon_writestring((pok && nontrivial) ? "[WPA2] PTK derivation (KCK|KEK|TK) OK\n"
                                             : "[WPA2] PTK derivation FAIL\n");
    return wok && uok && pok && nontrivial;
}
