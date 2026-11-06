/* SecOS - host helper for `make ROOT_PASSWORD=...`: SHA-512 crypt through the
 * very same code the guest uses (the vendored musl crypt + user/pwhash.c),
 * built with the host compiler by tools/gen-root-pwhash.sh.
 *
 *   pwhash_host hash          print the $6$ hash of $ROOT_PASSWORD (fresh salt)
 *   pwhash_host verify HASH   exit 0 when $ROOT_PASSWORD matches HASH
 *
 * The password comes from the environment, never from the command line (it
 * would show up in the process list). SPDX-License-Identifier: MIT */
#include <crypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pwhash.h"

int main(int argc, char** argv)
{
    const char* pw = getenv("ROOT_PASSWORD");
    if (!pw || !*pw) {
        fprintf(stderr, "pwhash_host: ROOT_PASSWORD is empty\n");
        return 2;
    }
    if (argc == 3 && strcmp(argv[1], "verify") == 0)
        return secos_verify_password(pw, argv[2]) ? 0 : 1;
    if (argc != 2 || strcmp(argv[1], "hash") != 0) {
        fprintf(stderr, "usage: pwhash_host hash | verify HASH\n");
        return 2;
    }
    char salt[SECOS_SALT_LEN + 1];
    struct crypt_data cd;
    if (secos_make_salt(salt, sizeof salt) != 0) {
        fprintf(stderr, "pwhash_host: no random salt\n");
        return 1;
    }
    memset(&cd, 0, sizeof cd);
    const char* h = crypt_r(pw, salt, &cd);
    if (!h || h[0] != '$') {
        fprintf(stderr, "pwhash_host: crypt failed\n");
        return 1;
    }
    printf("%s\n", h);
    return 0;
}
