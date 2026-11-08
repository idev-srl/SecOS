#!/usr/bin/env bash
# tools/mkpkgdisk.sh — build a package transfer disk (U6, `make pkgdisk`).
#
#   tools/mkpkgdisk.sh <image> <pkg.spkg>...
#
# An exFAT volume (label SPKG) holding the packages at its root, made with the
# real host tools (tools/exfat-host.sh: exfatprogs + exfat-fuse, no sudo).
# Every package is verified on the host first (tools/secos-pkg verify): a
# disk is never built with a package SecOS would refuse. The image is sized
# to the packages (+ 25 % + 32 MiB), sparse. Attach it with
# `make run-serial PKGDISK=<image>` (USB storage, usb0), then in SecOS:
#   mkdir -p /media && mount -t exfat /dev/usb0 /media
#   spkg install /media/<pkg>.spkg
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
img="${1:?usage: mkpkgdisk.sh <image> <pkg.spkg>...}"; shift
(( $# > 0 )) || { echo "pkgdisk: no packages (make pkgdisk PKGS=\"a.spkg b.spkg\")" >&2; exit 2; }

total=0
for p in "$@"; do
    [[ -f "$p" ]] || { echo "pkgdisk: $p: no such file" >&2; exit 1; }
    python3 "$REPO/tools/secos-pkg" verify "$p" >/dev/null \
        || { echo "pkgdisk: $p: signature/structure check FAILED, not adding it" >&2; exit 1; }
    total=$(( total + $(stat -c %s "$p") ))
done
mb=$(( total * 5 / 4 / 1048576 + 32 ))
rm -f -- "$img"
bash "$REPO/tools/exfat-host.sh" mkfs "$img" "$mb" >/dev/null
abs=()
for p in "$@"; do abs+=("$(readlink -f "$p")"); done
list=$(printf '%q ' "${abs[@]}")
bash "$REPO/tools/exfat-host.sh" run "$img" "cp $list \"\$M/\" && sync"
bash "$REPO/tools/exfat-host.sh" fsck "$img" >/dev/null || { echo "pkgdisk: fsck.exfat failed on $img" >&2; exit 1; }
echo "pkgdisk: $img (${mb} MiB exFAT): $*"
