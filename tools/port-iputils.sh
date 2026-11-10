#!/usr/bin/env bash
# SecOS - iputils ping port (U10, docs/USERLAND_PLAN.md).
#
#   1. fetch iputils (pinned SHA-256; upstream's PGP-signed sha256sums.asc is
#      checked against the maintainer's key when gpg is available), unpack it
#      into a fresh build/iputils-src/ (never committed);
#   2. build ONLY ping (ping/{ping,ping_common,ping6_common,node_info}.c +
#      iputils_common.c + md5.c, i.e. what meson's ping target and libcommon
#      contain) directly with tools/secos-cc: no meson, no libcap, no libidn2,
#      no gettext. config.h / git-version.h are written here by hand with the
#      values meson would detect on musl;
#   3. link it static-PIE with the CAP_NET manifest (user/note_net.S), signed
#      by secos-cc, and embed it as crypto/user_ping_elf.h.
#
# No source patch is applied. On SecOS a normal user pings through Linux-style
# ICMP "ping sockets" (socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)), which ping
# tries first; `-e 0` forces SOCK_RAW (root only). Without libcap, ping's
# privilege handling is the classic set-uid one (geteuid/seteuid), and the
# binary is not set-uid on SecOS.
#
# The Linux UAPI headers ping needs (<linux/errqueue.h>, <linux/filter.h>,
# <linux/sockios.h>, <asm/byteorder.h>, ...) are not in the musl sysroot: they
# are copied from the build host (linux-libc-dev), as tools/port-nano.sh does.
# They only carry constants and structure layouts that are part of the stable
# Linux ABI, so the output does not depend on the host's header version.
#
# ping is BSD-3-Clause (docs/licenses/iputils-$V-LICENSE.BSD3).
# SPDX-License-Identifier: MIT (this script)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
V=20250605
SHA=6f213700dbf96b5cc4499ca70cb15ecd69c09f405b06785bb4a1a10b572b6276
BASE="https://github.com/iputils/iputils/releases/download/$V"
# Petr Vorel <pvorel@suse.cz>, iputils maintainer: the key that signs
# sha256sums.asc (fetched from keys.openpgp.org, matches github.com/pevik.gpg).
SIGFPR=2016FEA4858B1C36B32E833AC0DEC2EE72F33A5F
KEYURL="https://keys.openpgp.org/vks/v1/by-fingerprint/$SIGFPR"

DL="${SECOS_DL:-$ROOT/build/dl}"
SRC="$ROOT/build/iputils-src"
OBJ="$ROOT/build/iputils-obj"
SYSROOT="${SECOS_SYSROOT:-$ROOT/build/sysroot}"
CC="$ROOT/tools/secos-cc"

[ -f "$SYSROOT/lib/libc.a" ] || { echo "[port-iputils] no musl sysroot: run tools/port-musl.sh"; exit 2; }

fetch() { # fetch <file> <url> [sha256]
    local f="$DL/$1"
    if [ ! -f "$f" ]; then
        echo "[port-iputils]   downloading $2"
        curl -fsSL -o "$f.part" "$2" || { rm -f "$f.part"; echo "[port-iputils] download failed: $2"; exit 3; }
        mv "$f.part" "$f"
    fi
    if [ -n "${3:-}" ]; then
        echo "$3  $f" | sha256sum -c --quiet - \
            || { echo "[port-iputils] SHA-256 MISMATCH for $1 - refusing to build"; exit 1; }
    fi
}
verify_sig() { # the signed sha256sums file must list our pinned hash
    command -v gpg >/dev/null || { echo "[port-iputils]   (gpg missing: SHA-256 pin only)"; return 0; }
    fetch "iputils-$V-sha256sums.asc" "$BASE/sha256sums.asc"
    fetch "iputils-key-$SIGFPR.asc" "$KEYURL" 2>/dev/null || return 0
    local g; g="$(mktemp -d)"
    gpg --homedir "$g" --batch --quiet --import "$DL/iputils-key-$SIGFPR.asc" 2>/dev/null || true
    if gpg --homedir "$g" --batch --status-fd 1 --verify "$DL/iputils-$V-sha256sums.asc" 2>/dev/null \
           | grep -q "VALIDSIG $SIGFPR" \
       && gpg --homedir "$g" --batch --decrypt "$DL/iputils-$V-sha256sums.asc" 2>/dev/null \
           | grep -qx "$SHA  iputils-$V.tar.xz"; then
        echo "[port-iputils]   signature OK (key $SIGFPR), signed list has our SHA-256"
    else
        rm -rf "$g"; echo "[port-iputils] BAD SIGNATURE for sha256sums.asc - refusing to build"; exit 1
    fi
    rm -rf "$g"
}

echo "[port-iputils] 1/4 fetch + verify iputils $V"
mkdir -p "$DL"
fetch "iputils-$V.tar.xz" "$BASE/iputils-$V.tar.xz" "$SHA"
verify_sig
# Untrusted archive: always unpack into a fresh, empty directory.
rm -rf "$SRC" "$ROOT/build/iputils-unpack"
mkdir -p "$ROOT/build/iputils-unpack"
tar xJf "$DL/iputils-$V.tar.xz" -C "$ROOT/build/iputils-unpack" --no-same-owner
mv "$ROOT/build/iputils-unpack/iputils-$V" "$SRC"
rmdir "$ROOT/build/iputils-unpack"
mkdir -p "$ROOT/docs/licenses"
cp "$SRC/Documentation/LICENSE.BSD3" "$ROOT/docs/licenses/iputils-$V-LICENSE.BSD3"

echo "[port-iputils] 2/4 config.h + compile"
rm -rf "$OBJ"; mkdir -p "$OBJ/compat"
cp -r /usr/include/linux /usr/include/asm-generic "$OBJ/compat/"
cp -r "/usr/include/$(gcc -print-multiarch)/asm" "$OBJ/compat/asm"
# What meson.build detects on musl: __fpending, getrandom and nanosleep exist,
# struct tm exists; no <error.h> (iputils_common.c supplies error()), no
# struct icmp6_nodeinfo (node_info.c uses its own iputils_ni.h), little endian.
cat >"$OBJ/config.h" <<EOF
#define PACKAGE_NAME "iputils"
#define HAVE___FPENDING 1
#define HAVE_GETRANDOM 1
#define HAVE_NANOSLEEP 1
#define TIME_WITH_SYS_TIME 1
EOF
# git-version.h.meson with vcs_tag's fallback (the project version).
sed 's/@VCS_TAG@/'"$V"'/' "$SRC/git-version.h.meson" >"$OBJ/git-version.h"
CFLAGS=(-std=c99 -O2 -Wall -Wextra -include config.h -include git-version.h
        -I"$OBJ" -I"$SRC" -I"$OBJ/compat"
        -ffile-prefix-map="$SRC"=iputils-$V -ffile-prefix-map="$OBJ"=iputils-obj)
OBJS=()
for f in ping/ping.c ping/ping_common.c ping/ping6_common.c ping/node_info.c \
         iputils_common.c md5.c; do
    o="$OBJ/$(basename "$f" .c).o"
    ( cd "$OBJ" && "$CC" "${CFLAGS[@]}" -c "$SRC/$f" -o "$o" ) 2>&1 | sed 's/^/    /'
    [ -f "$o" ] || { echo "[port-iputils] compile failed: $f"; exit 5; }
    OBJS+=("$o")
done

echo "[port-iputils] 3/4 link (static-PIE, CAP_NET manifest) + check"
ELF="$OBJ/ping"
( cd "$OBJ" && "$CC" --secos-note="$ROOT/user/note_net.S" -s -o "$ELF" "${OBJS[@]}" -lm -lresolv ) \
    || { echo "[port-iputils] link failed"; exit 6; }
readelf -lW "$ELF" | grep -q INTERP && { echo "[port-iputils] dynamic binary?"; exit 6; }
readelf -hW "$ELF" | grep -q 'DYN (Position-Independent' || { echo "[port-iputils] not a PIE"; exit 6; }
readelf -lW "$ELF" | grep GNU_STACK | grep -q ' RW ' || { echo "[port-iputils] stack not RW-only"; exit 6; }
# Unresolved references in a static link fail at link time; check the objects
# anyway so a missing libc function never hides behind a weak symbol.
nm -u "${OBJS[@]}" | awk 'NF==2 {print $2}' | sort -u >"$OBJ/undef.txt"
nm "$SYSROOT/lib/libc.a" "$SYSROOT/lib/libm.a" 2>/dev/null | awk '$2 ~ /^[TWDBRVi]$/ {print $3}' \
    | sort -u >"$OBJ/libc-def.txt"
nm "${OBJS[@]}" | awk '$2 ~ /^[TDBR]$/ {print $3}' | sort -u >"$OBJ/own-def.txt"
missing="$(grep -vx _GLOBAL_OFFSET_TABLE_ "$OBJ/undef.txt" | comm -23 - <(sort -u "$OBJ/libc-def.txt" "$OBJ/own-def.txt"))"
[ -z "$missing" ] || { echo "[port-iputils] unresolved: $missing"; exit 6; }
python3 "$ROOT/tools/secos-sign" "$ELF" --dev >/dev/null    # idempotent

echo "[port-iputils] 4/4 embed: crypto/user_ping_elf.h"
cp "$ELF" "$ROOT/user/ping.elf"
( cd "$ROOT" && python3 tools/elf2h.py user/ping.elf user_ping_elf crypto/user_ping_elf.h )
ls -la "$ELF" | awk '{print "[port-iputils] done: ping " $5 " bytes, signed (CAP_NET) -> crypto/user_ping_elf.h"}'
sha256sum "$ELF" | awk '{print "[port-iputils]   sha256 " $1}'
