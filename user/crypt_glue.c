/* SecOS - glue between the vendored musl crypt sources and the SecOS libc.
 *
 * third_party/musl-crypt/ holds musl 1.2.5's crypt.c, crypt_sha256.c,
 * crypt_sha512.c and crypt_blowfish.c unmodified. musl's crypt.c calls
 * __crypt_r(), which upstream lives in crypt_r.c together with dispatch to the
 * MD5 and DES hashes. SecOS deliberately does not ship those two legacy
 * algorithms, so this file provides __crypt_r() with the same dispatch minus
 * MD5/DES, and the public crypt_r().
 *
 * Unsupported or malformed settings return NULL with errno = EINVAL (glibc
 * behaviour). Callers must also treat a result starting with '*' as failure:
 * that is what the musl back-ends return when the self-test after hashing
 * fails or the setting is invalid.
 *
 * Built with -Ithird_party/musl-crypt/src/include so <crypt.h> is musl's
 * internal header (it declares the hidden __crypt_* back-ends).
 * SPDX-License-Identifier: MIT */
#include <crypt.h>
#include <errno.h>

char *__crypt_r(const char *key, const char *salt, struct crypt_data *data)
{
    /* crypt_data is only used as an output buffer (as in musl). */
    char *output = (char *)data;

    if (!key || !salt) {
        errno = EINVAL;
        return 0;
    }
    if (salt[0] == '$' && salt[1] && salt[2]) {
        if (salt[1] == '2' && salt[3] == '$')
            return __crypt_blowfish(key, salt, output);
        if (salt[1] == '5' && salt[2] == '$')
            return __crypt_sha256(key, salt, output);
        if (salt[1] == '6' && salt[2] == '$')
            return __crypt_sha512(key, salt, output);
    }
    errno = EINVAL;
    return 0;
}

char *crypt_r(const char *key, const char *salt, struct crypt_data *data)
{
    return __crypt_r(key, salt, data);
}
