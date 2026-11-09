#!/usr/bin/env bash
# SecOS - GNU nano port (U8, docs/USERLAND_PLAN.md D12, docs/devlog/U8-nano.md).
#
#   1. fetch nano (pinned SHA-256 + upstream's detached GPG signature, checked
#      against the GNU keyring when gpg is available), unpack into
#      build/nano-src/ (never committed: GPL-3.0, fetched like bash);
#   2. cross-configure + build with tools/secos-cc against the musl sysroot
#      and libncursesw.a (tools/port-ncurses.sh must have run): static-PIE,
#      UTF-8, no libmagic, no NLS;
#   3. sign it (secos-cc does) and embed it as crypto/user_nano_elf.h, plus the
#      syntax files nano ships as kernel/nano_syntax.h; kernel/sysroot.c
#      installs /usr/bin/nano, /usr/share/nano/*.nanorc and /etc/nanorc.
#   NANO_SPKG=1 also builds build/nano-$V.spkg with tools/secos-pkg.
# SPDX-License-Identifier: MIT (this script; nano itself is GPL-3.0-or-later)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
V=8.7.1
SHA=76f0dcb248f2e2f1251d4ecd20fd30fb400a360a3a37c6c340e0a52c2d1cdedf
URL="https://ftp.gnu.org/gnu/nano/nano-$V.tar.xz"
# Benno Schulenberg's release key (GNU keyring).
SIGFPR=168E6F4297BFD7A79AFD4496514BBE2EB8E1961F
# Syntax files shipped (the common ones; nano has ~45).
SYNTAX="sh c makefile python lua json xml html css javascript markdown patch nanorc default man awk perl ruby rust go cmake yaml"

DL="${SECOS_DL:-$ROOT/build/dl}"
SRC="$ROOT/build/nano-src"
OBJ="$ROOT/build/nano-obj"
SYSROOT="${SECOS_SYSROOT:-$ROOT/build/sysroot}"
JOBS="${JOBS:-$(nproc)}"
CC="$ROOT/tools/secos-cc"

[ -f "$SYSROOT/lib/libncursesw.a" ] || { echo "[port-nano] no libncursesw.a: run tools/port-ncurses.sh"; exit 2; }

fetch() { # fetch <file> <url> [sha256]
    local f="$DL/$1"
    if [ ! -f "$f" ]; then
        echo "[port-nano]   downloading $2"
        curl -fsSL -o "$f.part" "$2" || { rm -f "$f.part"; echo "[port-nano] download failed: $2"; exit 3; }
        mv "$f.part" "$f"
    fi
    if [ -n "${3:-}" ]; then
        echo "$3  $f" | sha256sum -c --quiet - \
            || { echo "[port-nano] SHA-256 MISMATCH for $1 - refusing to build"; exit 1; }
    fi
}
verify_sig() { # verify_sig <file> <sigfile> <fingerprint>
    command -v gpg >/dev/null || { echo "[port-nano]   (gpg missing: SHA-256 pin only)"; return 0; }
    fetch gnu-keyring.gpg https://ftp.gnu.org/gnu/gnu-keyring.gpg 2>/dev/null || return 0
    local g; g="$(mktemp -d)"
    if gpg --homedir "$g" --no-default-keyring --keyring "$DL/gnu-keyring.gpg" --status-fd 1 \
           --verify "$DL/$2" "$DL/$1" 2>/dev/null | grep -q "VALIDSIG $3"; then
        echo "[port-nano]   signature OK (key $3)"
    else
        rm -rf "$g"; echo "[port-nano] BAD SIGNATURE for $1 - refusing to build"; exit 1
    fi
    rm -rf "$g"
}

echo "[port-nano] 1/4 fetch + verify nano $V"
mkdir -p "$DL"
fetch "nano-$V.tar.xz" "$URL" "$SHA"
fetch "nano-$V.tar.xz.sig" "$URL.sig"
verify_sig "nano-$V.tar.xz" "nano-$V.tar.xz.sig" "$SIGFPR"
if [ ! -f "$SRC/.secos-$V" ]; then
    rm -rf "$SRC" "$ROOT/build/nano-$V"
    tar xJf "$DL/nano-$V.tar.xz" -C "$ROOT/build"
    mv "$ROOT/build/nano-$V" "$SRC"
    touch "$SRC/.secos-$V"
fi
mkdir -p "$ROOT/docs/licenses"
cp "$SRC/COPYING" "$ROOT/docs/licenses/nano-$V-COPYING"

echo "[port-nano] 2/4 configure + build (-j$JOBS)"
rm -rf "$OBJ"; mkdir -p "$OBJ/compat/linux"
# musl's <sys/vt.h> includes the kernel UAPI <linux/vt.h>, which the sysroot
# does not carry: nano only uses VT_GETSTATE (SecOS answers ENOTTY = "not on
# a Linux VT"). Take the build host's UAPI header (linux-libc-dev).
cp /usr/include/linux/vt.h "$OBJ/compat/linux/vt.h"
# Cross build: answer the run-time probes gnulib cannot execute the way musl
# behaves (all of these are standard-conforming in musl). The regex is
# gnulib's own (musl has no GNU re_* API): never mix gnulib's <regex.h>, whose
# regoff_t is an int, with musl's regcomp/regexec, whose regoff_t is a long.
( cd "$OBJ" && CC="$CC" CFLAGS="-O2" CPPFLAGS="-I$OBJ/compat" \
    gl_cv_func_working_mktime=yes gl_cv_func_getcwd_null=yes gl_cv_func_getcwd_path_max=yes \
    gl_cv_func_fnmatch_posix=yes \
    gl_cv_func_strerror_0_works=yes gl_cv_func_memchr_works=yes \
    ac_cv_func_malloc_0_nonnull=yes ac_cv_func_realloc_0_nonnull=yes \
    gl_cv_func_malloc_0_nonnull=yes gl_cv_func_realloc_0_nonnull=yes \
    "$SRC/configure" --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu \
        --prefix=/usr --sysconfdir=/etc --datarootdir=/usr/share \
        --disable-nls --disable-libmagic --enable-utf8 --disable-speller \
        NCURSESW_CFLAGS="-I$SYSROOT/include/ncursesw" NCURSESW_LIBS="-lncursesw" \
        >"$OBJ/configure.log" 2>&1 ) || { tail -30 "$OBJ/configure.log"; exit 5; }
grep -q "^#define ENABLE_UTF8 1" "$OBJ/config.h" || { echo "[port-nano] UTF-8 support not enabled"; exit 5; }
grep -q "curses library to be used is: ncursesw" "$OBJ/configure.log" \
    || { echo "[port-nano] configure did not pick ncursesw"; exit 5; }
( cd "$OBJ" && make -j"$JOBS" >"$OBJ/make.log" 2>&1 ) || { tail -30 "$OBJ/make.log"; exit 6; }
ELF="$OBJ/src/nano"
python3 "$ROOT/tools/secos-sign" "$ELF" --dev >/dev/null      # final link was signed; re-sign is idempotent
readelf -l "$ELF" | grep -q INTERP && { echo "[port-nano] dynamic binary?"; exit 6; }

echo "[port-nano] 3/4 embed: crypto/user_nano_elf.h + kernel/nano_syntax.h"
cp "$ELF" "$ROOT/user/nano.elf"
python3 "$ROOT/tools/elf2h.py" "$ROOT/user/nano.elf" user_nano_elf "$ROOT/crypto/user_nano_elf.h"
python3 - "$SRC/syntax" "$ROOT/kernel/nano_syntax.h" "$V" $SYNTAX <<'PY'
import os, sys
d, out, v, names = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
def cstr(b):
    s = []
    for ch in b:
        if ch == 0x5c: s.append("\\\\")
        elif ch == 0x22: s.append('\\"')
        elif ch == 0x0a: s.append('\\n"\n    "')
        elif 0x20 <= ch < 0x7f: s.append(chr(ch))
        else: s.append("\\%03o" % ch)
    return '"' + "".join(s) + '"'
with open(out, "w") as f:
    f.write("// Generated by tools/port-nano.sh from nano %s syntax/. Do not edit.\n" % v)
    f.write("// GNU nano syntax files: GPL-3.0-or-later (docs/licenses/nano-%s-COPYING).\n" % v)
    f.write("#pragma once\n")
    for i, n in enumerate(names):
        b = open(os.path.join(d, n + ".nanorc"), "rb").read()
        f.write("static const char nano_syn%d[] =\n    %s;\n" % (i, cstr(b)))
    f.write("static const struct { const char* name; const char* text; } nano_syntax[] = {\n")
    for i, n in enumerate(names):
        f.write('    { "%s.nanorc", nano_syn%d },\n' % (n, i))
    f.write("    { 0, 0 }\n};\n")
print("[port-nano]   %d syntax files" % len(names))
PY

if [ "${NANO_SPKG:-0}" = 1 ]; then
    echo "[port-nano] 4/4 package build/nano-$V.spkg"
    P="$OBJ/pkgroot"; rm -rf "$P"; mkdir -p "$P/usr/bin" "$P/usr/share/nano"
    cp "$ELF" "$P/usr/bin/nano"
    for n in $SYNTAX; do cp "$SRC/syntax/$n.nanorc" "$P/usr/share/nano/"; done
    python3 "$ROOT/tools/secos-pkg" --help >/dev/null 2>&1 || true
    echo "[port-nano]   see docs/devlog/U8-nano.md for the secos-pkg invocation"
else
    echo "[port-nano] 4/4 (NANO_SPKG=1 for a package)"
fi
ls -la "$ELF" | awk '{print "[port-nano] done: nano " $5 " bytes, signed -> crypto/user_nano_elf.h"}'
