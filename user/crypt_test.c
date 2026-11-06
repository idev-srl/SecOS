/* SecOS - in-guest known-answer test for crypt()/crypt_r() (vendored musl
 * 1.2.5, user/libcrypt.a) and the pwhash helpers. Checks the same vectors as
 * the host test tools/crypt_kat/run.sh (user/crypt_vectors.h). Prints one
 * "[crypt] PASS|FAIL ..." line per check and a final "[crypt] KAT OK" or
 * "[crypt] KAT FAILED"; exit status = number of failures (0 = pass).
 * SPDX-License-Identifier: MIT */
#include <crypt.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "pwhash.h"

#define KAT_PRINTF printf
#include "crypt_kat_body.h"

int main(void)
{
    int fails = crypt_kat_run();
    fflush(stdout);
    return fails;
}
