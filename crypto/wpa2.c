/*
 * SecOS - [M38] WPA2-PSK crypto. KAT-validated (IEEE 802.11i PBKDF2, FIPS-197 AES).
 */
#include "wpa2.h"
#include "sha1.h"
#include "debugcon.h"

static void wmemcpy(void* d, const void* s, size_t n){ uint8_t* a=d; const uint8_t* b=s; while(n--) *a++=*b++; }
static void wmemset(void* d, int v, size_t n){ uint8_t* a=d; while(n--) *a++=(uint8_t)v; }
static size_t wstrlen(const char* s){ size_t n=0; while(s[n]) n++; return n; }

void hmac_sha1(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen, uint8_t out[20]) {
    uint8_t k[64]; wmemset(k, 0, 64);
    if (klen > 64) { sha1_ctx c; sha1_init(&c); sha1_update(&c, key, klen); sha1_final(&c, k); }
    else wmemcpy(k, key, klen);
    uint8_t ip[64], op[64];
    for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    uint8_t ih[20]; sha1_ctx c;
    sha1_init(&c); sha1_update(&c, ip, 64); sha1_update(&c, msg, mlen); sha1_final(&c, ih);
    sha1_init(&c); sha1_update(&c, op, 64); sha1_update(&c, ih, 20); sha1_final(&c, out);
}

void pbkdf2_sha1(const char* pass, const uint8_t* salt, size_t slen, int iter,
                 uint8_t* out, int olen) {
    size_t pl = wstrlen(pass);
    int blk = 1, done = 0;
    while (done < olen) {
        uint8_t s2[68]; wmemcpy(s2, salt, slen);
        s2[slen]=blk>>24; s2[slen+1]=blk>>16; s2[slen+2]=blk>>8; s2[slen+3]=blk;
        uint8_t u[20], t[20];
        hmac_sha1((const uint8_t*)pass, pl, s2, slen+4, u);
        wmemcpy(t, u, 20);
        for (int i = 1; i < iter; i++) {
            hmac_sha1((const uint8_t*)pass, pl, u, 20, u);
            for (int j = 0; j < 20; j++) t[j] ^= u[j];
        }
        int cp = (olen-done < 20) ? olen-done : 20;
        wmemcpy(out+done, t, cp); done += cp; blk++;
    }
}

void wpa2_pmk(const char* passphrase, const char* ssid, uint8_t pmk[32]) {
    pbkdf2_sha1(passphrase, (const uint8_t*)ssid, wstrlen(ssid), 4096, pmk, 32);
}

/* --- AES-128 (FIPS-197) --- */
static const uint8_t sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

void aes128_expand(const uint8_t key[16], uint8_t rk[176]) {
    wmemcpy(rk, key, 16); uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4]; wmemcpy(t, rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t tmp = t[0];
            t[0] = sbox[t[1]] ^ rcon; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[tmp];
            rcon = xt(rcon);
        }
        for (int j = 0; j < 4; j++) rk[i+j] = rk[i-16+j] ^ t[j];
    }
}

void aes128_encrypt(const uint8_t in[16], uint8_t out[16], const uint8_t rk[176]) {
    uint8_t s[16]; wmemcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    for (int r = 1; r < 10; r++) {
        uint8_t t[16]; for (int i = 0; i < 16; i++) t[i] = sbox[s[i]];
        uint8_t a[16];
        a[0]=t[0];a[4]=t[4];a[8]=t[8];a[12]=t[12];
        a[1]=t[5];a[5]=t[9];a[9]=t[13];a[13]=t[1];
        a[2]=t[10];a[6]=t[14];a[10]=t[2];a[14]=t[6];
        a[3]=t[15];a[7]=t[3];a[11]=t[7];a[15]=t[11];
        for (int col = 0; col < 4; col++) {
            uint8_t* p = a + col*4; uint8_t b0=p[0],b1=p[1],b2=p[2],b3=p[3];
            s[col*4+0]=xt(b0)^(xt(b1)^b1)^b2^b3;
            s[col*4+1]=b0^xt(b1)^(xt(b2)^b2)^b3;
            s[col*4+2]=b0^b1^xt(b2)^(xt(b3)^b3);
            s[col*4+3]=(xt(b0)^b0)^b1^b2^xt(b3);
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk[r*16+i];
    }
    uint8_t t[16]; for (int i = 0; i < 16; i++) t[i] = sbox[s[i]];
    uint8_t a[16];
    a[0]=t[0];a[4]=t[4];a[8]=t[8];a[12]=t[12];a[1]=t[5];a[5]=t[9];a[9]=t[13];a[13]=t[1];
    a[2]=t[10];a[6]=t[14];a[10]=t[2];a[14]=t[6];a[3]=t[15];a[7]=t[3];a[11]=t[7];a[15]=t[11];
    for (int i = 0; i < 16; i++) out[i] = a[i] ^ rk[160+i];
}

void aes128_ctr(const uint8_t rk[176], const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len) {
    uint8_t ctr[16], ks[16]; wmemcpy(ctr, iv, 16);
    size_t done = 0;
    while (done < len) {
        aes128_encrypt(ctr, ks, rk);
        size_t n = (len - done < 16) ? len - done : 16;
        for (size_t i = 0; i < n; i++) out[done+i] = in[done+i] ^ ks[i];
        for (int i = 15; i >= 0; i--) { if (++ctr[i]) break; }
        done += n;
    }
}

int wpa2_selftest(void) {
    int ok = 1;
    /* PBKDF2 IEEE 802.11i: ("password","IEEE",4096) PMK starts f42c6fc5... */
    uint8_t pmk[32];
    wpa2_pmk("password", "IEEE", pmk);
    static const uint8_t exp_pmk[32] = {
        0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
        0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e };
    for (int i = 0; i < 32; i++) if (pmk[i] != exp_pmk[i]) ok = 0;
    debugcon_writestring(ok ? "[WPA2] PBKDF2 PMK KAT PASS\n" : "[WPA2] PBKDF2 PMK KAT FAIL\n");

    /* FIPS-197 AES-128 vector. */
    uint8_t key[16], in[16], out[16], rk[176];
    for (int i = 0; i < 16; i++) { key[i] = i; in[i] = (uint8_t)(i*0x11); }
    aes128_expand(key, rk); aes128_encrypt(in, out, rk);
    static const uint8_t exp_aes[16] = {
        0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a };
    int aok = 1; for (int i = 0; i < 16; i++) if (out[i] != exp_aes[i]) aok = 0;
    debugcon_writestring(aok ? "[WPA2] AES-128 KAT PASS\n" : "[WPA2] AES-128 KAT FAIL\n");
    return ok && aok;
}
