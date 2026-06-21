/* SecOS - SHA-1. [M38] Verified: SHA1("abc")=a9993e36...9cd0d89d. */
#include "sha1.h"

static uint32_t rol(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }

static void sha1_block(sha1_ctx* c, const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a=c->h[0], b=c->h[1], d=c->h[2], e=c->h[3], f=c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t t, k;
        if (i < 20)      { t = (b & d) | ((~b) & e);          k = 0x5A827999; }
        else if (i < 40) { t = b ^ d ^ e;                     k = 0x6ED9EBA1; }
        else if (i < 60) { t = (b & d) | (b & e) | (d & e);   k = 0x8F1BBCDC; }
        else             { t = b ^ d ^ e;                     k = 0xCA62C1D6; }
        t = rol(a, 5) + t + f + k + w[i];
        f = e; e = d; d = rol(b, 30); b = a; a = t;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=d; c->h[3]+=e; c->h[4]+=f;
}

void sha1_init(sha1_ctx* c) {
    c->h[0]=0x67452301; c->h[1]=0xEFCDAB89; c->h[2]=0x98BADCFE;
    c->h[3]=0x10325476; c->h[4]=0xC3D2E1F0; c->len=0; c->n=0;
}
void sha1_update(sha1_ctx* c, const void* data, size_t l) {
    const uint8_t* p = data; c->len += l;
    while (l--) { c->buf[c->n++] = *p++; if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; } }
}
void sha1_final(sha1_ctx* c, uint8_t out[20]) {
    uint64_t bits = c->len * 8; uint8_t pad = 0x80; sha1_update(c, &pad, 1);
    uint8_t z = 0; while (c->n != 56) sha1_update(c, &z, 1);
    uint8_t lb[8]; for (int i = 0; i < 8; i++) lb[i] = (bits >> (56 - i*8)) & 0xff;
    sha1_update(c, lb, 8);
    for (int i = 0; i < 5; i++) { out[i*4]=c->h[i]>>24; out[i*4+1]=c->h[i]>>16; out[i*4+2]=c->h[i]>>8; out[i*4+3]=c->h[i]; }
}
void sha1(const void* data, size_t len, uint8_t out[20]) {
    sha1_ctx c; sha1_init(&c); sha1_update(&c, data, len); sha1_final(&c, out);
}
