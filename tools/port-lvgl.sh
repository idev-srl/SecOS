#!/usr/bin/env bash
# SecOS - reproducible LVGL port. Fetches a pinned LVGL release, verifies its
# SHA-256, cross-compiles every file under src/ against SecOS's freestanding
# libc with user/lvgl/lv_conf.h (features not enabled there compile to
# nothing), links the LVGL system monitor (user/lvgl/sysmon.c) as an SWP client
# of secwm with the confined manifest, signs it and embeds it as
# crypto/user_sysmon_elf.h. [U5] The task manager (user/lvgl/taskmgr.c) is
# linked the same way with user/note_taskmgr.S (confined + CAP_SIGNAL) ->
# crypto/user_taskmgr_elf.h, installed as /bin/taskmgr. LVGL is MIT (docs/licenses/lvgl-9.2.2-LICENCE.txt).
# See docs/GUI.md.  SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LV=9.2.2
LSHA=129b4e00e06639fa79d7e8a6cab3c1ecce2445b1a246652ccd34f22e7b17ad6f
TARBALL=lvgl-$LV.tar.gz
URL="https://github.com/lvgl/lvgl/archive/refs/tags/v$LV.tar.gz"
WORK="${LVGL_PORT_WORK:-/tmp/lvgl-$LV}"
OBJ="${LVGL_PORT_OBJ:-/tmp/secos-lvglobj}"
JOBS="${JOBS:-$(nproc)}"

GCCINC="$(echo /usr/lib/gcc/x86_64-linux-gnu/*/include | tr ' ' '\n' | head -1)"
LFLAGS=(-ffreestanding -nostdlib -nostdinc -fno-pie -no-pie -mno-red-zone -mcmodel=large -m64 -O2
        -ffunction-sections -fdata-sections -w
        -DLV_CONF_INCLUDE_SIMPLE
        -I"$ROOT/user/lvgl" -I"$WORK" -I"$ROOT/user" -I"$ROOT/user/include" -I"$ROOT/user/secwm" -I"$ROOT/user/port-include"
        -isystem "$GCCINC")

echo "[port-lvgl] 1/4 fetch + verify $TARBALL"
if [ ! -f "$WORK/.secos-extracted" ]; then
    DL="$(dirname "$WORK")"; mkdir -p "$DL"
    [ -f "$DL/$TARBALL" ] || wget -q -O "$DL/$TARBALL" "$URL"
    echo "$LSHA  $DL/$TARBALL" | sha256sum -c --quiet - \
        || { echo "[port-lvgl] SHA-256 MISMATCH for $TARBALL - refusing to build"; exit 1; }
    rm -rf "$WORK"; ( cd "$DL" && tar xzf "$TARBALL" )
    [ "$DL/lvgl-$LV" = "$WORK" ] || mv "$DL/lvgl-$LV" "$WORK"
    touch "$WORK/.secos-extracted"
fi
cp "$WORK/LICENCE.txt" "$ROOT/docs/licenses/lvgl-$LV-LICENCE.txt"

echo "[port-lvgl] 2/4 compile LVGL ($(find "$WORK/src" -name '*.c' | wc -l) files, -j$JOBS)"
rm -rf "$OBJ"; mkdir -p "$OBJ"
export ROOT WORK OBJ
printf '%s\0' "${LFLAGS[@]}" > "$OBJ/.flags"
( cd "$WORK/src" && find . -name '*.c' -print0 ) | \
    xargs -0 -P "$JOBS" -I{} bash -c '
        src="$WORK/src/$1"; o="$OBJ/$(echo "$1" | sed "s#^\./##; s#/#_#g; s#\.c\$#.o#")"
        mapfile -d "" fl < "$OBJ/.flags"
        gcc "${fl[@]}" -c "$src" -o "$o" || { echo "[port-lvgl] FAILED: $1"; exit 255; }' _ {}

echo "[port-lvgl] 3/4 compile + link sysmon + taskmgr"
UF=(-ffreestanding -nostdlib -fno-pie -no-pie -mno-red-zone -mcmodel=large -m64 -O2 -Wall
    -ffunction-sections -fdata-sections -I"$ROOT/user" -I"$ROOT/user/include" -I"$ROOT/user/secwm")
gcc "${UF[@]}" -c "$ROOT/user/crt0.S" -o "$OBJ/crt0.o.u"
gcc "${UF[@]}" -c "$ROOT/user/note_confined.S" -o "$OBJ/note_confined.o.u"
gcc "${UF[@]}" -c "$ROOT/user/note_taskmgr.S" -o "$OBJ/note_taskmgr.o.u"
gcc "${UF[@]}" -c "$ROOT/user/libsecos.c" -o "$OBJ/libsecos.o.u"
gcc "${UF[@]}" -c "$ROOT/user/libc.c" -o "$OBJ/libc.o.u"
gcc "${UF[@]}" -c "$ROOT/user/libm.c" -o "$OBJ/libm.o.u"
gcc "${UF[@]}" -c "$ROOT/user/secwm/swpc.c" -o "$OBJ/swpc.o.u"
gcc "${UF[@]}" -c "$ROOT/user/secwm/gfx.c" -o "$OBJ/gfx.o.u"
gcc "${LFLAGS[@]}" -Wall -c "$ROOT/user/lvgl/sysmon.c" -o "$OBJ/sysmon.o.u"
ld --gc-sections -T "$ROOT/user/user.ld" -o "$ROOT/user/lvgl/sysmon.elf" \
    "$OBJ/crt0.o.u" "$OBJ/note_confined.o.u" "$OBJ/libsecos.o.u" "$OBJ/libc.o.u" "$OBJ/libm.o.u" \
    "$OBJ/sysmon.o.u" "$OBJ/swpc.o.u" "$OBJ/gfx.o.u" "$OBJ"/*.o 2>&1 | grep -v "not in segment" || true
[ -s "$ROOT/user/lvgl/sysmon.elf" ] || { echo "[port-lvgl] link failed"; exit 1; }
gcc "${LFLAGS[@]}" -Wall -Wextra -c "$ROOT/user/lvgl/taskmgr.c" -o "$OBJ/taskmgr.o.u"
ld --gc-sections -T "$ROOT/user/user.ld" -o "$ROOT/user/lvgl/taskmgr.elf" \
    "$OBJ/crt0.o.u" "$OBJ/note_taskmgr.o.u" "$OBJ/libsecos.o.u" "$OBJ/libc.o.u" "$OBJ/libm.o.u" \
    "$OBJ/taskmgr.o.u" "$OBJ/swpc.o.u" "$OBJ/gfx.o.u" "$OBJ"/*.o 2>&1 | grep -v "not in segment" || true
[ -s "$ROOT/user/lvgl/taskmgr.elf" ] || { echo "[port-lvgl] taskmgr link failed"; exit 1; }

echo "[port-lvgl] 4/4 sign + embed"
python3 "$ROOT/tools/secos-sign" "$ROOT/user/lvgl/sysmon.elf" --dev
python3 "$ROOT/tools/elf2h.py" "$ROOT/user/lvgl/sysmon.elf" user_sysmon_elf "$ROOT/crypto/user_sysmon_elf.h"
python3 "$ROOT/tools/secos-sign" "$ROOT/user/lvgl/taskmgr.elf" --dev
python3 "$ROOT/tools/elf2h.py" "$ROOT/user/lvgl/taskmgr.elf" user_taskmgr_elf "$ROOT/crypto/user_taskmgr_elf.h"
ls -l "$ROOT/user/lvgl/sysmon.elf" "$ROOT/user/lvgl/taskmgr.elf"
