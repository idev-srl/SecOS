#!/usr/bin/env bash
# tools/exfat-host.sh - host-side exFAT tooling for the SecOS M41 tests.
#
# Uses the real exFAT tools, independent of the SecOS driver:
#   - exfatprogs (mkfs.exfat / fsck.exfat) from PATH, or a user-local copy;
#   - exfat-fuse (relan/fuse-exfat) to put files on / read files off an image.
#
# No root needed: `setup` downloads the Ubuntu/Debian packages with
# `apt-get download` and unpacks them under $EXFAT_TOOLS. The FUSE mount runs
# inside an unprivileged user+mount namespace (`unshare -rm`), where the kernel
# allows FUSE mounts; the exfat-fuse binary is copied with two hardcoded mount
# options neutralised that only real root may use (allow_other, blksize).
#
# Usage:
#   tools/exfat-host.sh setup                 fetch + unpack tools (no sudo)
#   tools/exfat-host.sh mkfs IMG SIZE_MB      create an empty exFAT image
#   tools/exfat-host.sh fsck IMG              fsck.exfat -n (read-only check)
#   tools/exfat-host.sh run IMG 'CMD'         run CMD with the image mounted at $M
#   tools/exfat-host.sh mkgood IMG            build the M41 "good" test image
#   tools/exfat-host.sh verify IMG            check IMG after the SecOS M41 run
#   tools/exfat-host.sh mkcorrupt DIR         build the corrupt-image suite in DIR
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXFAT_TOOLS="${EXFAT_TOOLS:-$HOME/.local/secos-exfat}"
PY="python3 -I $REPO_ROOT/tools/exfat_img.py"

die() { echo "exfat-host: $*" >&2; exit 1; }

find_tool() { # find_tool mkfs.exfat
    if command -v "$1" >/dev/null 2>&1; then command -v "$1"; return 0; fi
    for d in "$EXFAT_TOOLS/usr/sbin" "$EXFAT_TOOLS/sbin" /usr/sbin /sbin; do
        [[ -x "$d/$1" ]] && { echo "$d/$1"; return 0; }
    done
    return 1
}

cmd_setup() {
    mkdir -p "$EXFAT_TOOLS"
    local tmp; tmp="$(mktemp -d)"
    ( cd "$tmp" && apt-get download exfatprogs exfat-fuse libfuse2 >/dev/null ) \
        || { rm -rf "$tmp"; die "apt-get download failed (packages: exfatprogs exfat-fuse libfuse2)"; }
    for deb in "$tmp"/*.deb; do dpkg-deb -x "$deb" "$EXFAT_TOOLS"; done
    rm -rf "$tmp"
    # Unprivileged copy of mount.exfat-fuse: drop allow_other/blkdev from the
    # default option string and turn the blksize= option (fuseblk only) into a
    # harmless subtype=. Same-length byte patches.
    python3 -I - "$EXFAT_TOOLS/sbin/mount.exfat-fuse" "$EXFAT_TOOLS/sbin/exfat-fuse-user" <<'EOF'
import sys
d = open(sys.argv[1], "rb").read()
a = b"allow_other,big_writes,blkdev,default_permissions"
b = b"big_writes,default_permissions,fsname=exfat_user1"
assert len(a) == len(b) and d.count(a) == 1, "unexpected exfat-fuse build"
d = d.replace(a, b)
assert d.count(b"\0blksize\0") == 1, "unexpected exfat-fuse build"
d = d.replace(b"\0blksize\0", b"\0subtype\0")
open(sys.argv[2], "wb").write(d)
EOF
    chmod +x "$EXFAT_TOOLS/sbin/exfat-fuse-user"
    echo "exfat-host: tools ready in $EXFAT_TOOLS"
    find_tool mkfs.exfat && find_tool fsck.exfat
}

# First use: fetch the tools automatically (no sudo needed).
auto_setup() {
    if ! find_tool mkfs.exfat >/dev/null || [[ ! -x "$EXFAT_TOOLS/sbin/exfat-fuse-user" ]]; then
        echo "exfat-host: exFAT tools missing, fetching them into $EXFAT_TOOLS" >&2
        cmd_setup >&2 || true
    fi
}
need_tools() {
    auto_setup
    MKFS="$(find_tool mkfs.exfat)" || die "mkfs.exfat not found - run: tools/exfat-host.sh setup"
    FSCK="$(find_tool fsck.exfat)" || die "fsck.exfat not found - run: tools/exfat-host.sh setup"
}
need_fuse() {
    auto_setup
    FUSE="$EXFAT_TOOLS/sbin/exfat-fuse-user"
    [[ -x "$FUSE" ]] || die "exfat-fuse-user missing - run: tools/exfat-host.sh setup"
    [[ -e /dev/fuse ]] || die "/dev/fuse not available"
}

cmd_mkfs() { need_tools; rm -f "$1"; truncate -s "${2}M" "$1"; "$MKFS" -L SECOSEXF "$1" >/dev/null; }
cmd_fsck() { need_tools; "$FSCK" -n "$1"; }

# Run a shell snippet with the image mounted at $M (inside a user namespace).
cmd_run() {
    need_fuse
    local img; img="$(readlink -f "$1")"; local script="$2"
    local mnt; mnt="$(mktemp -d)"
    unshare -rm env M="$mnt" IMG="$img" FUSE="$FUSE" PY="$PY" \
        LD_LIBRARY_PATH="$EXFAT_TOOLS/lib/x86_64-linux-gnu" SCRIPT="$script" bash -c '
        "$FUSE" "$IMG" "$M" >/dev/null 2>&1 || { echo "exfat-host: FUSE mount failed" >&2; exit 1; }
        bash -c "$SCRIPT"; rc=$?
        umount "$M"; exit $rc'
    local rc=$?
    sleep 0.3                     # let the FUSE daemon flush and exit
    rmdir "$mnt" 2>/dev/null
    return $rc
}

cmd_mkgood() {
    local img="$1"
    cmd_mkfs "$img" 64
    cmd_run "$img" '$PY populate "$M"' || die "populating $img failed"
    $PY fragmented "$img" frag_a.bin >/dev/null || die "frag_a.bin did not fragment"
    cmd_fsck "$img" >/dev/null || die "fresh image not fsck-clean"
    $PY deepcheck "$img" >/dev/null || die "fresh image fails deepcheck"
    echo "exfat-host: $img ready (64 MiB, long names, big + fragmented files)"
}

cmd_verify() {
    local img="$1" fail=0
    need_tools
    "$FSCK" -n "$img" || { echo "  host [FAIL] fsck.exfat -n not clean"; fail=1; }
    $PY deepcheck "$img" || fail=1
    [[ "$($PY dirty "$img")" == "0" ]] && echo "  host [PASS] VolumeDirty clear" \
        || { echo "  host [FAIL] VolumeDirty still set"; fail=1; }
    cmd_run "$img" '$PY verifytree "$M"' || fail=1
    $PY fragmented "$img" sfa.bin >/dev/null && echo "  host [PASS] sfa.bin is FAT-chained + fragmented" \
        || { echo "  host [FAIL] sfa.bin not fragmented"; fail=1; }
    [[ $fail -eq 0 ]] && echo "VERIFY OK" || echo "VERIFY FAILED"
    return $fail
}

cmd_mkcorrupt() {
    local dir="$1" base
    mkdir -p "$dir"; base="$dir/base.img"
    cmd_mkfs "$base" 8
    cmd_run "$base" '$PY populate-corrupt "$M"' || die "populating corrupt base failed"
    cmd_fsck "$base" >/dev/null || die "corrupt base not fsck-clean"
    $PY corrupt "$base" "$dir"
}

case "${1:-}" in
    setup)     cmd_setup ;;
    mkfs)      cmd_mkfs "$2" "$3" ;;
    fsck)      cmd_fsck "$2" ;;
    run)       cmd_run "$2" "$3" ;;
    mkgood)    cmd_mkgood "$2" ;;
    verify)    cmd_verify "$2" ;;
    mkcorrupt) cmd_mkcorrupt "$2" ;;
    *) sed -n '2,/^set /p' "$0" | grep '^#' | sed 's/^# \?//'; exit 2 ;;
esac
