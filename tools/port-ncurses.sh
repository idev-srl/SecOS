#!/usr/bin/env bash
# SecOS - ncurses port (U8, docs/USERLAND_PLAN.md D12, docs/devlog/U8-nano.md).
#
#   1. fetch ncurses (pinned SHA-256; the tarball is also checked against
#      upstream's detached signature when gpg + the key are available, see
#      verify_sig below), unpack it into build/ncurses-src/ (never committed);
#   2. build the host tools (tic, infocmp) of the SAME version natively:
#      the fallback entries and the terminfo database are compiled with them;
#   3. cross-build libncursesw.a (wide characters, static, no C++/Ada, no
#      programs) with tools/secos-cc against the musl sysroot; the entries in
#      FALLBACKS are compiled into the library, so a program finds them even
#      with no database on disk (ramfs root, broken /usr/share/terminfo);
#   4. install headers + libraries into build/sysroot (include/ and
#      include/ncursesw/, lib/lib{ncurses,panel,menu,form}w.a + the
#      non-w / tinfo names as symlinks);
#   5. compile the small terminfo database (TERMINFO_SET) into
#      build/terminfo/ and turn it into kernel/terminfo_db.h, which
#      kernel/sysroot.c installs under /usr/share/terminfo.
# ncurses is MIT-X11 (docs/licenses/ncurses-$NV-COPYING).  SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NV=6.6
NSHA=355b4cbbed880b0381a04c46617b7656e362585d52e9cf84a67e2009b749ff11
URL="https://ftp.gnu.org/gnu/ncurses/ncurses-$NV.tar.gz"
SIGURL="$URL.sig"
# Thomas E. Dickey's release key (also in the GNU keyring).
SIGFPR=19882D92DDA4C400C22C0D56CC2AF4472167BE03
FALLBACKS="linux,xterm,xterm-256color,vt100,screen,screen-256color,tmux,tmux-256color,dumb"
TERMINFO_SET="linux xterm xterm-256color xterm-color vt100 vt102 vt220 screen screen-256color tmux tmux-256color ansi dumb"

DL="${SECOS_DL:-$ROOT/build/dl}"
SRC="$ROOT/build/ncurses-src"
HOST="$ROOT/build/ncurses-host"
OBJ="$ROOT/build/ncurses-obj"
SYSROOT="${SECOS_SYSROOT:-$ROOT/build/sysroot}"
TI="$ROOT/build/terminfo"
JOBS="${JOBS:-$(nproc)}"
CC="$ROOT/tools/secos-cc"

[ -f "$SYSROOT/lib/libc.a" ] || { echo "[port-ncurses] no musl sysroot: run tools/port-musl.sh"; exit 2; }

fetch() { # fetch <file> <url> [sha256]
    local f="$DL/$1"
    if [ ! -f "$f" ]; then
        echo "[port-ncurses]   downloading $2"
        curl -fsSL -o "$f.part" "$2" || { rm -f "$f.part"; echo "[port-ncurses] download failed: $2"; exit 3; }
        mv "$f.part" "$f"
    fi
    if [ -n "${3:-}" ]; then
        echo "$3  $f" | sha256sum -c --quiet - \
            || { echo "[port-ncurses] SHA-256 MISMATCH for $1 - refusing to build"; exit 1; }
    fi
}
# The SHA-256 pin is the gate. The signature is an extra check, done when the
# GNU keyring can be fetched: a bad signature fails, a missing tool only warns.
verify_sig() { # verify_sig <file> <sigfile> <fingerprint>
    command -v gpg >/dev/null || { echo "[port-ncurses]   (gpg missing: SHA-256 pin only)"; return 0; }
    fetch gnu-keyring.gpg https://ftp.gnu.org/gnu/gnu-keyring.gpg 2>/dev/null || return 0
    local g; g="$(mktemp -d)"
    if gpg --homedir "$g" --no-default-keyring --keyring "$DL/gnu-keyring.gpg" --status-fd 1 \
           --verify "$DL/$2" "$DL/$1" 2>/dev/null | grep -q "VALIDSIG $3"; then
        echo "[port-ncurses]   signature OK (key $3)"
    else
        rm -rf "$g"; echo "[port-ncurses] BAD SIGNATURE for $1 - refusing to build"; exit 1
    fi
    rm -rf "$g"
}

echo "[port-ncurses] 1/5 fetch + verify ncurses $NV"
mkdir -p "$DL"
fetch "ncurses-$NV.tar.gz" "$URL" "$NSHA"
fetch "ncurses-$NV.tar.gz.sig" "$SIGURL"
verify_sig "ncurses-$NV.tar.gz" "ncurses-$NV.tar.gz.sig" "$SIGFPR"
if [ ! -f "$SRC/.secos-$NV" ]; then
    rm -rf "$SRC" "$ROOT/build/ncurses-$NV"
    tar xzf "$DL/ncurses-$NV.tar.gz" -C "$ROOT/build"
    mv "$ROOT/build/ncurses-$NV" "$SRC"
    touch "$SRC/.secos-$NV"
fi
mkdir -p "$ROOT/docs/licenses"
cp "$SRC/COPYING" "$ROOT/docs/licenses/ncurses-$NV-COPYING"

echo "[port-ncurses] 2/5 host tic/infocmp $NV"
if [ ! -x "$HOST/bin/tic" ]; then
    rm -rf "$HOST" "$HOST-obj"; mkdir -p "$HOST-obj"
    ( cd "$HOST-obj" && "$SRC/configure" --prefix="$HOST" --without-shared --without-cxx \
        --without-cxx-binding --without-ada --without-manpages --without-tests --enable-widec \
        --disable-db-install --without-debug >/dev/null \
      && make -j"$JOBS" >/dev/null 2>&1 && make install.progs >/dev/null 2>&1 )
fi
[ -x "$HOST/bin/tic" ] || { echo "[port-ncurses] host tic build failed"; exit 4; }

echo "[port-ncurses] 3/5 cross-build libncursesw.a (fallbacks: $FALLBACKS)"
rm -rf "$OBJ"; mkdir -p "$OBJ"
( cd "$OBJ" && PATH="$HOST/bin:$PATH" CC="$CC" CFLAGS="-O2" \
    "$SRC/configure" --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu --prefix=/usr \
        --with-build-cc=gcc --without-shared --with-normal --without-debug --without-profile \
        --without-cxx --without-cxx-binding --without-ada --without-manpages --without-progs \
        --without-tests --without-gpm --without-dlsym --disable-termcap --disable-db-install \
        --enable-widec --enable-ext-colors --enable-ext-mouse --enable-sigwinch \
        --disable-home-terminfo --disable-stripping --disable-pc-files \
        --with-terminfo-dirs=/usr/share/terminfo --with-default-terminfo-dir=/usr/share/terminfo \
        --with-fallbacks="$FALLBACKS" --with-tic-path="$HOST/bin/tic" \
        --with-infocmp-path="$HOST/bin/infocmp" >"$OBJ/configure.log" 2>&1 ) \
    || { tail -30 "$OBJ/configure.log"; exit 5; }
( cd "$OBJ" && PATH="$HOST/bin:$PATH" make -j"$JOBS" libs >"$OBJ/make.log" 2>&1 ) \
    || { tail -30 "$OBJ/make.log"; exit 6; }

echo "[port-ncurses] 4/5 install into ${SYSROOT#$ROOT/}"
( cd "$OBJ" && make install.libs install.includes DESTDIR="$OBJ/dest" >/dev/null 2>&1 ) \
    || { echo "[port-ncurses] install failed"; exit 7; }
mkdir -p "$SYSROOT/include/ncursesw" "$SYSROOT/lib"
# Headers both at the top level (<curses.h>) and under ncursesw/ (<ncursesw/curses.h>).
cp -a "$OBJ/dest/usr/include/." "$SYSROOT/include/ncursesw/"
cp -a "$OBJ/dest/usr/include/." "$SYSROOT/include/"
for l in ncursesw panelw menuw formw; do cp "$OBJ/dest/usr/lib/lib$l.a" "$SYSROOT/lib/"; done
for l in ncurses curses tinfo tinfow; do ln -sf libncursesw.a "$SYSROOT/lib/lib$l.a"; done
for l in panel menu form; do ln -sf "lib${l}w.a" "$SYSROOT/lib/lib$l.a"; done

echo "[port-ncurses] 5/5 terminfo database ($TERMINFO_SET)"
rm -rf "$TI"; mkdir -p "$TI"
"$HOST/bin/tic" -x -o "$TI" -e "${TERMINFO_SET// /,}" "$SRC/misc/terminfo.src" >/dev/null 2>&1
python3 - "$TI" "$ROOT/kernel/terminfo_db.h" "$NV" $TERMINFO_SET <<'PY'
import os, sys
ti, out, nv, names = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
# Only the names asked for (tic also writes their aliases, e.g. vt200).
ents = []
for n in sorted(names):
    ents.append((n[0] + "/" + n, open(os.path.join(ti, n[0], n), "rb").read()))
with open(out, "w") as f:
    f.write("// Generated by tools/port-ncurses.sh from ncurses %s misc/terminfo.src\n" % nv)
    f.write("// (compiled with tic %s -x). Do not edit. ncurses: MIT-X11.\n" % nv)
    f.write("#pragma once\n#include <stdint.h>\n#include <stddef.h>\n")
    for i, (p, b) in enumerate(ents):
        f.write("static const uint8_t terminfo_e%d[%d] = {" % (i, len(b)))
        f.write(",".join(str(x) for x in b))
        f.write("};\n")
    f.write("static const struct { const char* path; const uint8_t* data; size_t len; } terminfo_db[] = {\n")
    for i, (p, b) in enumerate(ents):
        f.write('    { "%s", terminfo_e%d, %d },\n' % (p, i, len(b)))
    f.write("    { 0, 0, 0 }\n};\n")
print("[port-ncurses]   %d entries -> %s" % (len(ents), out))
PY
echo "[port-ncurses] done: $SYSROOT/lib/libncursesw.a"
