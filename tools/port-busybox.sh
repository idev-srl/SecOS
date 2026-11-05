#!/usr/bin/env bash
# SecOS - reproducible BusyBox port (see docs/BUSYBOX_PORT.md).
#
# Fetches BusyBox 1.36.1 (GPL-2.0-only) and musl 1.2.5 (MIT; only its regex,
# fnmatch and getopt sources are compiled) into a work directory OUTSIDE the
# MIT tree, verifies both tarballs against pinned SHA-256 sums, configures
# BusyBox from the hand-written fragment user/busybox.config, cross-compiles
# every object with BusyBox's own Kbuild against the SecOS libc (-nostdinc +
# user/busybox-include overlay + force-included user/busybox_port.h), links a
# signed ring-3 busybox.elf (CAP_NET manifest) and embeds it as
# crypto/user_busybox_elf.h. It also builds the tiny signed applet stub
# (user/bbstub.c -> crypto/user_bbstub_elf.h) the kernel installs under every
# applet name, and regenerates the applet list kernel/busybox_applets.h.
#
# Env overrides: BUSYBOX_PORT_WORK (work dir), BUSYBOX_PORT_JOBS (make -j).
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BBV=busybox-1.36.1
BB_SHA256=b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314
BB_URL="https://busybox.net/downloads/$BBV.tar.bz2"
MUSLV=musl-1.2.5
MUSL_SHA256=a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
MUSL_URL="https://musl.libc.org/releases/$MUSLV.tar.gz"
WORK="${BUSYBOX_PORT_WORK:-/tmp/secos-busybox}"
JOBS="${BUSYBOX_PORT_JOBS:-$(nproc)}"
SRC="$WORK/$BBV"
MUSL="$WORK/$MUSLV"
OBJ="$WORK/secos-obj"
UOBJ="$WORK/secos-uobj"
UAPI="$WORK/uapi"

GCCINC="$(echo /usr/lib/gcc/x86_64-linux-gnu/*/include | tr ' ' '\n' | head -1)"
# Only the overlay, the SecOS libc headers, gcc's freestanding headers and
# (last, for `ip`'s netlink structures) the host's Linux UAPI headers are
# visible: no host libc header can leak in.
INCS="-I$ROOT/user/busybox-include -I$ROOT/user/include -I$ROOT/user -idirafter $UAPI"
NOSTDINC="-nostdinc $INCS -isystem $GCCINC"
# An implicitly declared libc function returns int and truncates 64-bit
# pointers (the bash-port lesson), so a missing prototype fails the build.
TFLAGS="-ffreestanding -fno-pie -mno-red-zone -mcmodel=large -m64 -fno-stack-protector \
 -fno-asynchronous-unwind-tables -fno-unwind-tables \
 -Werror=implicit-function-declaration -Werror=int-conversion -Werror=int-to-pointer-cast"
DIRS="applets archival archival/libarchive console-tools coreutils coreutils/libcoreutils \
 debianutils klibc-utils e2fsprogs editors findutils init libbb libpwdgrp loginutils \
 mailutils miscutils modutils networking networking/libiproute networking/udhcp \
 printutils procps runit selinux shell sysklogd util-linux util-linux/volume_id"

fetch() { # fetch <url> <file> <sha256>
    local url="$1" f="$WORK/$2" sum="$3"
    if [ ! -f "$f" ]; then wget -q -O "$f.part" "$url"; mv "$f.part" "$f"; fi
    echo "$sum  $f" | sha256sum -c --quiet - || { echo "[port-busybox] SHA-256 mismatch: $f" >&2; exit 1; }
}

echo "[port-busybox] 1/7 fetch + verify $BBV and $MUSLV"
mkdir -p "$WORK" "$UAPI"
fetch "$BB_URL" "$BBV.tar.bz2" "$BB_SHA256"
fetch "$MUSL_URL" "$MUSLV.tar.gz" "$MUSL_SHA256"
[ -d "$SRC" ]  || tar -C "$WORK" -xjf "$WORK/$BBV.tar.bz2"
[ -d "$MUSL" ] || tar -C "$WORK" -xzf "$WORK/$MUSLV.tar.gz"
ln -sfn /usr/include/linux "$UAPI/linux"
ln -sfn /usr/include/x86_64-linux-gnu/asm "$UAPI/asm"
ln -sfn /usr/include/asm-generic "$UAPI/asm-generic"
python3 -I "$ROOT/tools/bb_patch_src.py" "$SRC"

echo "[port-busybox] 2/7 configure (defconfig + user/busybox.config)"
cd "$SRC"
STAMP="$(sha256sum "$ROOT/user/busybox.config" | cut -d' ' -f1)"
if [ "$(cat .secos-conf 2>/dev/null)" != "$STAMP" ]; then
    make distclean >/dev/null 2>&1 || true
    make defconfig >/dev/null
    for pass in 1 2; do
        python3 -I "$ROOT/tools/bb_apply_config.py" "$ROOT/user/busybox.config" .config "$SRC"
        { yes "" || :; } | make oldconfig >/dev/null 2>&1
    done
    echo "$STAMP" > .secos-conf
fi

echo "[port-busybox] 3/7 cross-compile BusyBox objects (Kbuild, -j$JOBS)"
make -j"$JOBS" CC=gcc HOSTCC=gcc NOSTDINC_FLAGS="$NOSTDINC" \
     EXTRA_CFLAGS="$TFLAGS -include $ROOT/user/busybox_port.h" \
     $DIRS > "$WORK/kbuild.log" 2>&1 || { grep -E "error" "$WORK/kbuild.log" | head -40 >&2; exit 1; }

echo "[port-busybox] 4/7 musl regex/fnmatch/getopt + SecOS shims"
rm -rf "$OBJ"; mkdir -p "$OBJ" "$UOBJ"
MUSL_CF="$TFLAGS -Os -std=gnu99 $NOSTDINC -I$ROOT/user/busybox-include/musl -Dhidden= \
 -include $ROOT/user/busybox_port.h -w"
for f in regex/regcomp regex/regexec regex/tre-mem regex/regerror regex/fnmatch \
         misc/getopt misc/getopt_long; do
    gcc $MUSL_CF -c "$MUSL/src/$f.c" -o "$OBJ/musl_$(basename "$f").o"
done
gcc $TFLAGS -Os -std=gnu99 -Wall $NOSTDINC -DBB_COMPAT_IMPL -include "$ROOT/user/busybox_port.h" \
    -c "$ROOT/user/busybox_compat.c" -o "$OBJ/zz_compat.o"

echo "[port-busybox] 5/7 link busybox.elf"
UCF="-ffreestanding -nostdlib -fno-pie -mno-red-zone -mcmodel=large -m64 -O2 \
 -I$ROOT/user -I$ROOT/user/include -ffunction-sections -fdata-sections -fno-stack-protector"
for s in crt0.S note.S note_net.S setjmp.S; do gcc $UCF -c "$ROOT/user/$s" -o "$UOBJ/${s%.S}.o"; done
for c in libsecos.c libc.c libm.c bbstub.c; do gcc $UCF -c "$ROOT/user/$c" -o "$UOBJ/${c%.c}.o"; done
BUILTIN=""; LIBS=""
for d in $DIRS; do
    [ -f "$d/built-in.o" ] && BUILTIN="$BUILTIN $SRC/$d/built-in.o"
    [ -f "$d/lib.a" ] && LIBS="$LIBS $SRC/$d/lib.a"
done
ld --gc-sections -T "$ROOT/user/user.ld" -o "$ROOT/user/busybox.elf" \
   "$UOBJ/crt0.o" "$UOBJ/note_net.o" $BUILTIN --start-group $LIBS "$OBJ"/*.o \
   "$UOBJ/libsecos.o" "$UOBJ/libc.o" "$UOBJ/libm.o" "$UOBJ/setjmp.o" --end-group \
   -Map "$WORK/busybox.map" 2>&1 | grep -v "not in segment" || true
[ -s "$ROOT/user/busybox.elf" ] || { echo "[port-busybox] link failed" >&2; exit 1; }

echo "[port-busybox] 6/7 applet stub (user/bbstub.c)"
ld --gc-sections -T "$ROOT/user/user.ld" -o "$ROOT/user/bbstub.elf" \
   "$UOBJ/crt0.o" "$UOBJ/note.o" "$UOBJ/bbstub.o" "$UOBJ/libsecos.o" "$UOBJ/libc.o" \
   2>&1 | grep -v "not in segment" || true

echo "[port-busybox] 7/7 sign + embed"
cd "$ROOT"   # run python from the repo (a pyenv-local interpreter has 'cryptography')
python3 "$ROOT/tools/secos-sign" "$ROOT/user/busybox.elf" --dev
python3 "$ROOT/tools/secos-sign" "$ROOT/user/bbstub.elf" --dev
python3 "$ROOT/tools/elf2h.py" "$ROOT/user/busybox.elf" user_busybox_elf "$ROOT/crypto/user_busybox_elf.h"
python3 "$ROOT/tools/elf2h.py" "$ROOT/user/bbstub.elf" user_bbstub_elf "$ROOT/crypto/user_bbstub_elf.h"
python3 -I "$ROOT/tools/bb_applets.py" "$SRC/include/applet_tables.h" "$ROOT/kernel/busybox_applets.h"
echo "[port-busybox] DONE: busybox.elf $(stat -c %s "$ROOT/user/busybox.elf") bytes," \
     "bbstub.elf $(stat -c %s "$ROOT/user/bbstub.elf") bytes, $(grep -o 'NUM_APPLETS [0-9]*' "$ROOT/kernel/busybox_applets.h")"
