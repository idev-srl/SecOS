/* SecOS - crypt()/pwhash known-answer test body, shared by the host test
 * (tools/crypt_kat/crypt_kat.c) and the in-guest test (user/crypt_test.c).
 * The includer provides KAT_PRINTF (printf-like) and the libc headers.
 * Returns the number of failed checks.
 * SPDX-License-Identifier: MIT */
#ifndef SECOS_CRYPT_KAT_BODY_H
#define SECOS_CRYPT_KAT_BODY_H

#include "crypt_vectors.h"

static int kat_fail;

static void kat_check(int ok, const char *what, const char *detail)
{
    if (!ok)
        kat_fail++;
    KAT_PRINTF("[crypt] %s %s%s%s\n", ok ? "PASS" : "FAIL", what,
               detail ? " " : "", detail ? detail : "");
}

static int kat_is_salt_char(char c)
{
    return c == '.' || c == '/' || (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int crypt_kat_run(void)
{
    struct crypt_data cd;
    char s1[SECOS_SALT_LEN + 1], s2[SECOS_SALT_LEN + 1], small[8];
    char hashed[128];
    const char *r;
    int i, ok;

    kat_fail = 0;
    for (i = 0; i < CRYPT_NVECTORS; i++) {
        const struct crypt_vector *v = &crypt_vectors[i];
        r = crypt(v->key, v->setting);
        ok = r && strcmp(r, v->expected) == 0;
        kat_check(ok, v->setting, ok ? 0 : (r ? r : "(null)"));
        /* crypt_r must agree with crypt. */
        memset(&cd, 0, sizeof cd);
        r = crypt_r(v->key, v->setting, &cd);
        kat_check(r && strcmp(r, v->expected) == 0, "crypt_r", v->setting);
    }

    /* Unsupported legacy schemes are refused, not silently DES-hashed. */
    kat_check(crypt("x", "$1$saltsalt$") == 0, "MD5 setting refused", 0);
    kat_check(crypt("x", "ab") == 0, "DES setting refused", 0);

    /* secos_verify_password */
    kat_check(secos_verify_password("Hello world!", crypt_vectors[0].expected) == 1,
              "verify: correct password", 0);
    kat_check(secos_verify_password("Hello world?", crypt_vectors[0].expected) == 0,
              "verify: wrong password", 0);
    kat_check(secos_verify_password("Hello world!", crypt_vectors[7].expected) == 1,
              "verify: correct password ($5$)", 0);
    kat_check(secos_verify_password("U*U", crypt_vectors[15].expected) == 1,
              "verify: correct password ($2b$)", 0);
    kat_check(secos_verify_password("", "") == 0, "verify: empty hash", 0);
    kat_check(secos_verify_password("x", "!") == 0, "verify: locked '!'", 0);
    kat_check(secos_verify_password("x", "*") == 0, "verify: locked '*'", 0);
    kat_check(secos_verify_password("Hello world!", "$6$saltstring$truncated") == 0,
              "verify: truncated hash", 0);

    /* secos_make_salt */
    kat_check(secos_make_salt(small, sizeof small) == -1 && small[0] == '\0',
              "make_salt: short buffer refused", 0);
    ok = secos_make_salt(s1, sizeof s1) == 0 && secos_make_salt(s2, sizeof s2) == 0;
    if (ok) {
        ok = strlen(s1) == SECOS_SALT_LEN && strncmp(s1, "$6$", 3) == 0 &&
             s1[SECOS_SALT_LEN - 1] == '$';
        for (i = 3; ok && i < SECOS_SALT_LEN - 1; i++)
            ok = kat_is_salt_char(s1[i]);
    }
    kat_check(ok, "make_salt: format", ok ? s1 : 0);
    kat_check(strcmp(s1, s2) != 0, "make_salt: two salts differ", 0);
    r = crypt("correct horse", s1);
    ok = r && r[0] == '$' && strlen(r) < sizeof hashed;
    if (ok) {
        strcpy(hashed, r);
        ok = secos_verify_password("correct horse", hashed) == 1 &&
             secos_verify_password("correct horsf", hashed) == 0;
    }
    kat_check(ok, "make_salt + crypt + verify round-trip", 0);

    KAT_PRINTF("[crypt] %s: %d failure(s)\n", kat_fail ? "KAT FAILED" : "KAT OK", kat_fail);
    return kat_fail;
}

#endif
