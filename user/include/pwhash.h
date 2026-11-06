/* SecOS password-hash helpers for account tools (passwd, login, useradd...).
 * Implemented in user/pwhash.c, linked from user/libcrypt.a.
 * SPDX-License-Identifier: MIT */
#ifndef SECOS_PWHASH_H
#define SECOS_PWHASH_H

#include <stddef.h>

/* Length of the setting secos_make_salt() writes: "$6$" + 16 salt chars + "$". */
#define SECOS_SALT_LEN 20

/* Write a fresh SHA-512-crypt setting "$6$<16 chars from [./0-9A-Za-z]>$"
 * into out, NUL-terminated, using /dev/urandom. n must be at least
 * SECOS_SALT_LEN + 1. Returns 0 on success, -1 on error (out is then ""). */
int secos_make_salt(char *out, size_t n);

/* Return 1 if crypt(plain, hash) reproduces hash exactly, else 0. Locked or
 * empty hashes ("!", "*", "") and unsupported schemes never match. The final
 * comparison is constant-time over the hash length. */
int secos_verify_password(const char *plain, const char *hash);

#endif
