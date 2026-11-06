#!/usr/bin/env bash
# SecOS - vendor musl's crypt() implementation (MIT) into third_party/musl-crypt/.
# Fetches the pinned musl release, verifies its SHA-256 and copies only the
# SHA-256-crypt, SHA-512-crypt and bcrypt sources plus the two headers they
# include, keeping musl's directory layout so the files stay byte-identical to
# upstream. SecOS glue lives outside this tree (user/crypt_glue.c,
# user/musl-crypt-compat/). Re-run after bumping MUSLV/MUSL_SHA256.
# Env override: MUSL_CRYPT_WORK (download/extract dir).
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MUSLV=musl-1.2.5
MUSL_SHA256=a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
MUSL_URL="https://musl.libc.org/releases/$MUSLV.tar.gz"
WORK="${MUSL_CRYPT_WORK:-/tmp/secos-musl-crypt}"
DST="$ROOT/third_party/musl-crypt"
FILES=(COPYRIGHT
       include/crypt.h
       src/include/crypt.h
       src/crypt/crypt.c
       src/crypt/crypt_sha256.c
       src/crypt/crypt_sha512.c
       src/crypt/crypt_blowfish.c)

mkdir -p "$WORK"
TARBALL="$WORK/$MUSLV.tar.gz"
# Reuse a tarball already fetched by tools/port-busybox.sh (same pin); the
# checksum below is verified either way.
if [ ! -f "$TARBALL" ] && [ -f "/tmp/secos-busybox/$MUSLV.tar.gz" ]; then
    cp "/tmp/secos-busybox/$MUSLV.tar.gz" "$TARBALL"
fi
[ -f "$TARBALL" ] || wget -q -O "$TARBALL" "$MUSL_URL"
echo "$MUSL_SHA256  $TARBALL" | sha256sum -c --quiet - \
    || { echo "[vendor-musl-crypt] SHA-256 MISMATCH for $TARBALL - refusing to vendor"; exit 1; }

rm -rf "${WORK:?}/$MUSLV"
tar -C "$WORK" -xzf "$TARBALL" "${FILES[@]/#/$MUSLV/}"

rm -rf "${DST:?}"; mkdir -p "$DST"
for f in "${FILES[@]}"; do
    mkdir -p "$DST/$(dirname "$f")"
    cp "$WORK/$MUSLV/$f" "$DST/$f"
done
( cd "$DST" && sha256sum "${FILES[@]}" ) > "$DST/SHA256SUMS"
echo "[vendor-musl-crypt] $MUSLV crypt sources -> third_party/musl-crypt/ (${#FILES[@]} files)"
