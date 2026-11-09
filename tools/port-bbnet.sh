#!/usr/bin/env bash
# SecOS - "bbnet": a second, tiny BusyBox 1.36.1 on musl (U10).
#
# The main BusyBox (tools/port-busybox.sh) is built on the old home-made libc
# and is left alone. This one is built with tools/secos-cc against the musl
# sysroot (tools/port-musl.sh) and carries only the applets that need the
# Linux-shaped time/network syscalls:
#   ntpd     NTP client (+ the server/ntp.conf/auth features, BusyBox defaults)
#   hwclock  RTC read/write (/dev/rtc*)
# It is a multi-call binary dispatching on argv[0] (install it as
# /usr/sbin/ntpd and /sbin/hwclock, or run `bbnet ntpd ...`).
#
#   1. fetch BusyBox (pinned SHA-256 + upstream's detached GPG signature when
#      gpg is available), unpack into a fresh build/bbnet-src/;
#   2. allnoconfig + the two applets with their default features;
#   3. build with CC = secos-cc --secos-unsigned (CAP_NET manifest,
#      user/note_net.S). BusyBox strips busybox_unstripped after the link,
#      which would break a signature made by secos-cc, so the link is unsigned
#      and the final stripped binary is signed with tools/secos-sign;
#   4. embed it as crypto/user_bbnet_elf.h.
#
# BusyBox is GPL-2.0-only (docs/licenses/busybox-1.36.1-LICENSE).
# SPDX-License-Identifier: MIT (this script)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
V=1.36.1
SHA=b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314
URL="https://busybox.net/downloads/busybox-$V.tar.bz2"
# Denys Vlasenko <vda.linux@googlemail.com>, BusyBox release key (DSA;
# not on keys.openpgp.org, fetched from keyserver.ubuntu.com).
SIGFPR=C9E9416F76E610DBD09D040F47B70C55ACC9965B
# Reproducible: no build timestamp in the binary.
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1684800000}" KCONFIG_NOTIMESTAMP=1

DL="${SECOS_DL:-$ROOT/build/dl}"
SRC="$ROOT/build/bbnet-src"
SYSROOT="${SECOS_SYSROOT:-$ROOT/build/sysroot}"
JOBS="${JOBS:-$(nproc)}"

[ -f "$SYSROOT/lib/libc.a" ] || { echo "[port-bbnet] no musl sysroot: run tools/port-musl.sh"; exit 2; }

fetch() { # fetch <file> <url> [sha256]
    local f="$DL/$1"
    if [ ! -f "$f" ]; then
        # The old port keeps its tarball in /tmp/secos-busybox: reuse it (the
        # pin below still decides) instead of downloading again.
        if [ "$1" = "busybox-$V.tar.bz2" ] && [ -f "/tmp/secos-busybox/$1" ]; then
            cp "/tmp/secos-busybox/$1" "$f"
        else
            echo "[port-bbnet]   downloading $2"
            curl -fsSL -o "$f.part" "$2" || { rm -f "$f.part"; echo "[port-bbnet] download failed: $2"; return 3; }
            mv "$f.part" "$f"
        fi
    fi
    if [ -n "${3:-}" ]; then
        echo "$3  $f" | sha256sum -c --quiet - \
            || { echo "[port-bbnet] SHA-256 MISMATCH for $1 - refusing to build"; exit 1; }
    fi
}
verify_sig() {
    command -v gpg >/dev/null || { echo "[port-bbnet]   (gpg missing: SHA-256 pin only)"; return 0; }
    fetch "busybox-$V.tar.bz2.sig" "$URL.sig" \
        || { echo "[port-bbnet]   (no .sig reachable: SHA-256 pin only)"; return 0; }
    fetch "busybox-key-$SIGFPR.asc" "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x$SIGFPR" 2>/dev/null \
        || { echo "[port-bbnet]   (release key not reachable: SHA-256 pin only)"; return 0; }
    local g; g="$(mktemp -d)"
    gpg --homedir "$g" --batch --quiet --import "$DL/busybox-key-$SIGFPR.asc" 2>/dev/null || true
    if gpg --homedir "$g" --batch --status-fd 1 --verify "$DL/busybox-$V.tar.bz2.sig" \
           "$DL/busybox-$V.tar.bz2" 2>/dev/null | grep -q "VALIDSIG $SIGFPR"; then
        echo "[port-bbnet]   signature OK (key $SIGFPR)"
    else
        rm -rf "$g"; echo "[port-bbnet] BAD SIGNATURE for busybox-$V.tar.bz2 - refusing to build"; exit 1
    fi
    rm -rf "$g"
}

echo "[port-bbnet] 1/4 fetch + verify BusyBox $V"
mkdir -p "$DL"
fetch "busybox-$V.tar.bz2" "$URL" "$SHA"
verify_sig
rm -rf "$SRC" "$ROOT/build/bbnet-unpack"
mkdir -p "$ROOT/build/bbnet-unpack"
tar xjf "$DL/busybox-$V.tar.bz2" -C "$ROOT/build/bbnet-unpack" --no-same-owner
mv "$ROOT/build/bbnet-unpack/busybox-$V" "$SRC"
rmdir "$ROOT/build/bbnet-unpack"

echo "[port-bbnet] 2/4 configure (allnoconfig + ntpd + hwclock)"
# The compiler: secos-cc, unsigned, with the CAP_NET manifest. A wrapper,
# because Kbuild passes $(CC) around as one word.
W="$SRC/.secos"; mkdir -p "$W"
cat >"$W/cc" <<EOF
#!/bin/sh
exec "$ROOT/tools/secos-cc" --secos-unsigned --secos-note="$ROOT/user/note_net.S" "\$@"
EOF
chmod +x "$W/cc"
# LD: Kbuild defaults to "$(CC) -nostdlib" for its "-r" partial links,
# which secos-cc would turn into a static-PIE link.
MK=(make -C "$SRC" CC="$W/cc" LD=ld HOSTCC=gcc SKIP_STRIP=y V=0)
"${MK[@]}" allnoconfig >"$W/config.log" 2>&1
# allnoconfig clears the "default y" options too, so set the ones a normal
# build has: long options, the two applets and their
# default features. Everything else (CONFIG_STATIC/PIE) stays off: secos-cc
# always links static-PIE.
cfg() { sed -i "/^# CONFIG_$1 is not set\$/d; /^CONFIG_$1=/d" "$SRC/.config"; echo "CONFIG_$1=${2:-y}" >>"$SRC/.config"; }
for o in LONG_OPTS BUSYBOX SHOW_USAGE FEATURE_VERBOSE_USAGE \
         NTPD FEATURE_NTPD_SERVER FEATURE_NTPD_CONF FEATURE_NTP_AUTH \
         HWCLOCK SH_IS_NONE BASH_IS_NONE; do
    cfg "$o"
done
# The "sh" choice defaults to ash, which would build a whole shell in: none.
sed -i '/^CONFIG_SH_IS_ASH=y$/d; /^CONFIG_SHELL_ASH=y$/d' "$SRC/.config"
cfg EXTRA_CFLAGS "\"-ffile-prefix-map=$SRC=busybox-$V\""
yes "" | "${MK[@]}" oldconfig >>"$W/config.log" 2>&1 || true
for o in NTPD HWCLOCK FEATURE_NTP_AUTH SH_IS_NONE; do
    grep -q "^CONFIG_$o=y" "$SRC/.config" || { echo "[port-bbnet] CONFIG_$o did not stick"; exit 5; }
done
grep -E '^CONFIG_.*=y' "$SRC/.config" | sed 's/^/    /'

echo "[port-bbnet] 3/4 build (-j$JOBS)"
"${MK[@]}" -j"$JOBS" busybox >"$W/make.log" 2>&1 || { tail -40 "$W/make.log"; exit 6; }
ELF="$SRC/bbnet"
# The same strip BusyBox's own rule does, then sign the final bytes.
strip -s --remove-section=.note --remove-section=.comment "$SRC/busybox_unstripped" -o "$ELF"
readelf -SW "$ELF" | grep -q '\.note\.secos' || { echo "[port-bbnet] manifest note lost"; exit 6; }
readelf -lW "$ELF" | grep -q INTERP && { echo "[port-bbnet] dynamic binary?"; exit 6; }
readelf -hW "$ELF" | grep -q 'DYN (Position-Independent' || { echo "[port-bbnet] not a PIE"; exit 6; }
readelf -lW "$ELF" | grep GNU_STACK | grep -q ' RW ' || { echo "[port-bbnet] stack not RW-only"; exit 6; }
nm -u "$SRC/busybox_unstripped" | grep -v -E ' w ' | grep . && { echo "[port-bbnet] unresolved symbols"; exit 6; }
python3 "$ROOT/tools/secos-sign" "$ELF" --dev

echo "[port-bbnet] 4/4 embed: crypto/user_bbnet_elf.h"
cp "$ELF" "$ROOT/user/bbnet.elf"
( cd "$ROOT" && python3 tools/elf2h.py user/bbnet.elf user_bbnet_elf crypto/user_bbnet_elf.h )
ls -la "$ELF" | awk '{print "[port-bbnet] done: bbnet (ntpd, hwclock) " $5 " bytes, signed (CAP_NET) -> crypto/user_bbnet_elf.h"}'
sha256sum "$ELF" | awk '{print "[port-bbnet]   sha256 " $1}'
