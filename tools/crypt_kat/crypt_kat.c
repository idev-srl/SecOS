/* SecOS - host-side known-answer test for the vendored musl crypt()
 * (third_party/musl-crypt/ + user/crypt_glue.c + user/pwhash.c), built with
 * the host compiler and libc by tools/crypt_kat/run.sh.
 * SPDX-License-Identifier: MIT */
#include <crypt.h>      /* musl internal header (-Ithird_party/musl-crypt/src/include) */
#include <stdio.h>
#include <string.h>
#include "pwhash.h"

#define KAT_PRINTF printf
#include "crypt_kat_body.h"

int main(void)
{
    return crypt_kat_run() ? 1 : 0;
}
