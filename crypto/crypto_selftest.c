/*
 * SecOS Kernel - Crypto known-answer self-tests
 * Copyright (c) 2025 iDev srl
 * SPDX-License-Identifier: MIT
 */
#include "crypto_selftest.h"
#include "sha256.h"
#include "debugcon.h"

static int eq(const uint8_t* a, const uint8_t* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int report(const char* name, int ok) {
    debugcon_writestring("[CRYPTO] ");
    debugcon_writestring(ok ? "PASS: " : "FAIL: ");
    debugcon_writestring(name);
    debugcon_writestring("\n");
    return ok ? 0 : 1;
}

int crypto_selftest(void) {
    int fails = 0;
    debugcon_writestring("[CRYPTO] --- begin ---\n");

    /* SHA-256("abc") — FIPS 180-4 example */
    {
        static const uint8_t want[32] = {
            0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
            0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
        };
        uint8_t out[32]; sha256("abc", 3, out);
        fails += report("SHA256(\"abc\")", eq(out, want, 32));
    }

    /* SHA-256("") — empty string */
    {
        static const uint8_t want[32] = {
            0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
            0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55
        };
        uint8_t out[32]; sha256("", 0, out);
        fails += report("SHA256(\"\")", eq(out, want, 32));
    }

    /* Multi-block / streaming: 1,000,000 'a' should match FIPS vector */
    {
        static const uint8_t want[32] = {
            0xcd,0xc7,0x6e,0x5c,0x99,0x14,0xfb,0x92,0x81,0xa1,0xc7,0xe2,0x84,0xd7,0x3e,0x67,
            0xf1,0x80,0x9a,0x48,0xa4,0x97,0x20,0x0e,0x04,0x6d,0x39,0xcc,0xc7,0x11,0x2c,0xd0
        };
        sha256_ctx c; sha256_init(&c);
        uint8_t blk[64]; for (int i=0;i<64;i++) blk[i]='a';
        for (int i = 0; i < 15625; i++) sha256_update(&c, blk, 64); /* 15625*64 = 1,000,000 */
        uint8_t out[32]; sha256_final(&c, out);
        fails += report("SHA256(1e6 x 'a')", eq(out, want, 32));
    }

    debugcon_writestring(fails ? "[CRYPTO] --- FAIL ---\n" : "[CRYPTO] --- all PASS ---\n");
    return fails;
}
