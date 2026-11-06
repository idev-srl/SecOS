/* SecOS libc <crypt.h> - password hashing (musl 1.2.5 implementation,
 * vendored in third_party/musl-crypt/, linked from user/libcrypt.a).
 *
 * Supported settings: "$6$" SHA-512-crypt (use this), "$5$" SHA-256-crypt and
 * "$2a$"/"$2b$"/"$2y$" bcrypt. MD5 ("$1$") and DES are not supported.
 * crypt() returns a static buffer; crypt_r() is the reentrant form. Both
 * return NULL (errno = EINVAL) for an unsupported setting, and a string
 * starting with '*' when hashing fails: treat both as "no match".
 * SPDX-License-Identifier: MIT */
#ifndef SECOS_CRYPT_H
#define SECOS_CRYPT_H

/* Same layout as musl's struct crypt_data (used purely as output buffer). */
struct crypt_data {
    int initialized;
    char __buf[256];
};

char *crypt(const char *key, const char *setting);
char *crypt_r(const char *key, const char *setting, struct crypt_data *data);

#endif
