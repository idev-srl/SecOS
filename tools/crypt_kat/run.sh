#!/usr/bin/env bash
# SecOS - build the vendored musl crypt sources with the host compiler and run
# the SHA-crypt / bcrypt known-answer tests (user/crypt_vectors.h).
# The host libc provides <features.h>, so musl's `hidden` macro (supplied by
# user/musl-crypt-compat/features.h in the SecOS build) is defined empty here.
# Exit 0 = all vectors pass.  SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${CRYPT_KAT_OUT:-/tmp/secos-crypt-kat}"
MC="$ROOT/third_party/musl-crypt"
mkdir -p "$OUT"
CF=(-std=c99 -D_XOPEN_SOURCE=700 -O2 -Wall -Wextra -Wno-unused-parameter
    -Wno-sign-compare -Wno-missing-braces   # upstream musl style, not ours
    -I"$MC/src/include" -Dhidden=
    -iquote "$ROOT/user" -iquote "$ROOT/user/include")
gcc "${CF[@]}" -o "$OUT/crypt_kat" \
    "$MC/src/crypt/crypt.c" "$MC/src/crypt/crypt_sha256.c" \
    "$MC/src/crypt/crypt_sha512.c" "$MC/src/crypt/crypt_blowfish.c" \
    "$ROOT/user/crypt_glue.c" "$ROOT/user/pwhash.c" \
    "$ROOT/tools/crypt_kat/crypt_kat.c"
"$OUT/crypt_kat"
