#!/usr/bin/env bash
# SecOS - musl C library port (U1, docs/USERLAND_PLAN.md D1/D2, docs/devlog/U1-musl-base.md).
#
#   1. fetch musl 1.2.6 + the upstream security patches published after it
#      (openwall musl list), verify every SHA-256 (pinned below);
#   2. unpack the pristine tree into third_party/musl/ (git-ignored, never
#      edited: the SecOS changes live in user/musl-secos/);
#   3. copy it to build/musl-src/, apply the upstream patches, then the SecOS
#      overlay: user/musl-secos/{arch,src}/ files replace or add musl files,
#      user/musl-secos/patches/*.patch are applied, DELETE lists the files
#      whose generic version must win;
#   4. configure (static only, -fPIE, no wrapper) + build + install into
#      build/sysroot/ (include/, lib/libc.a, lib/{rcrt1,crt1,crti,crtn}.o);
#   5. check that no `syscall` instruction is left in libc.a (every call must
#      go through the translation layer, src/secos/syscall.c);
#   6. build build/sysroot/lib/libsecos.a + include/secos/secos.h (SecOS-only
#      calls) and lib/secos-note.o (the default signed-manifest note).
# tools/secos-cc then compiles, links (static-PIE) and signs programs.
# musl is MIT (docs/licenses/musl-1.2.6-COPYRIGHT).  SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MV=1.2.6
MSHA=d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a
URL="https://musl.libc.org/releases/musl-$MV.tar.gz"
# Upstream fixes released after 1.2.6 (https://musl.libc.org/ advisories):
#   CVE-2026-6042    iconv gb18030 decoder (DoS on untrusted input)
#   CVE-2026-107891  regcomp integer overflow on untrusted regular expressions
PATCHES=(
  "cve-2026-6042-iconv.patch https://www.openwall.com/lists/musl/2026/04/03/2/1 444fa70e52ca158fb7d4bad560637790bbf8f72e80b82fff840dd66fa83091e3"
  "cve-2026-107891-regcomp.patch https://www.openwall.com/lists/musl/2026/10/21/1/1 0cf0090ab433e8f9ed08ae03a0c93aff557c1d3e347260f62a61a10281f5333b"
)
# musl files removed after the overlay so the generic C version is built.
DELETE=( src/process/x86_64/vfork.s )

DL="${MUSL_DL:-$ROOT/build/dl}"
PRISTINE="$ROOT/third_party/musl"
SRC="$ROOT/build/musl-src"
OBJ="$ROOT/build/musl-obj"
SYSROOT="${SECOS_SYSROOT:-$ROOT/build/sysroot}"
OVL="$ROOT/user/musl-secos"
JOBS="${JOBS:-$(nproc)}"

fetch() { # fetch <file> <url> <sha256>
    local f="$DL/$1"
    if [ ! -f "$f" ]; then
        echo "[port-musl]   downloading $2"
        curl -fsSL -o "$f.part" "$2" || wget -q -O "$f.part" "$2" \
            || { rm -f "$f.part"; echo "[port-musl] download failed: $2"; exit 3; }
        mv "$f.part" "$f"
    fi
    echo "$3  $f" | sha256sum -c --quiet - \
        || { echo "[port-musl] SHA-256 MISMATCH for $1 - refusing to build"; exit 1; }
}

echo "[port-musl] 1/6 fetch + verify musl $MV and ${#PATCHES[@]} upstream patches"
mkdir -p "$DL"
fetch "musl-$MV.tar.gz" "$URL" "$MSHA"
for p in "${PATCHES[@]}"; do set -- $p; fetch "$1" "$2" "$3"; done

echo "[port-musl] 2/6 pristine tree -> third_party/musl"
if [ ! -f "$PRISTINE/.secos-$MV" ]; then
    rm -rf "$PRISTINE" "$DL/musl-$MV"
    ( cd "$DL" && tar xzf "musl-$MV.tar.gz" )
    mv "$DL/musl-$MV" "$PRISTINE"
    touch "$PRISTINE/.secos-$MV"
fi
mkdir -p "$ROOT/docs/licenses"
cp "$PRISTINE/COPYRIGHT" "$ROOT/docs/licenses/musl-$MV-COPYRIGHT"

echo "[port-musl] 3/6 build tree: upstream patches + SecOS overlay"
rm -rf "$SRC" "$OBJ"; mkdir -p "$OBJ"
cp -a "$PRISTINE" "$SRC"; rm -f "$SRC/.secos-$MV"
for p in "${PATCHES[@]}"; do set -- $p; patch -s -d "$SRC" -p1 < "$DL/$1"; done
( cd "$OVL" && find arch src -type f ) | while read -r f; do
    mkdir -p "$SRC/$(dirname "$f")"; cp "$OVL/$f" "$SRC/$f"
done
for p in "$OVL"/patches/*.patch; do patch -s -d "$SRC" -p1 < "$p"; done
for f in "${DELETE[@]}"; do rm -f "$SRC/$f"; done

echo "[port-musl] 4/6 configure + build (-j$JOBS) + install -> ${SYSROOT#$ROOT/}"
rm -rf "$SYSROOT"
( cd "$OBJ" && AR=ar RANLIB=ranlib "$SRC/configure" --target=x86_64 --prefix="$SYSROOT" \
      --disable-shared --enable-static --disable-wrapper \
      CC=gcc CFLAGS="-O2 -fPIE -g0" >"$OBJ/configure.log" 2>&1 ) \
    || { tail -20 "$OBJ/configure.log"; exit 1; }
# The translation layer runs before TLS (thus before the stack-protector
# canary at %fs:0x28) exists: build it like musl's own early-startup files.
cat >> "$OBJ/config.mak" <<'EOF'
obj/src/secos/%.o obj/src/secos/%.lo: CFLAGS_ALL += $(CFLAGS_NOSSP)
EOF
make -C "$OBJ" -j"$JOBS" >"$OBJ/build.log" 2>&1 || { grep -E "error:|\*\*\*" "$OBJ/build.log" | head -20; exit 1; }
make -C "$OBJ" install >"$OBJ/install.log" 2>&1 || { tail -20 "$OBJ/install.log"; exit 1; }

echo "[port-musl] 5/6 check: no raw Linux syscall left in libc.a"
if objdump -d "$SYSROOT/lib/libc.a" | grep -qwE "syscall|sysenter"; then
    objdump -d "$SYSROOT/lib/libc.a" | grep -wE "syscall|sysenter|^[0-9a-f]+ <" | grep -B1 -wE "syscall|sysenter" | head -20
    echo "[port-musl] FAIL: libc.a still issues Linux syscalls directly"; exit 1
fi
nobj=$(ar t "$SYSROOT/lib/libc.a" | wc -l)
echo "[port-musl]   libc.a: $nobj objects, every syscall through __secos_syscall"

echo "[port-musl] 6/6 libsecos.a, <secos/secos.h>, secos-note.o"
mkdir -p "$SYSROOT/include/secos"
cp "$OVL/include/secos/secos.h" "$SYSROOT/include/secos/secos.h"
gcc -nostdinc -isystem "$SYSROOT/include" -O2 -fPIE -Wall -Wextra -c "$OVL/libsecos/secos.c" -o "$OBJ/secos.o"
ar rcs "$SYSROOT/lib/libsecos.a" "$OBJ/secos.o"
gcc -c "$ROOT/user/note.S" -o "$SYSROOT/lib/secos-note.o"
echo "[port-musl] done: $SYSROOT (CC=tools/secos-cc)"
