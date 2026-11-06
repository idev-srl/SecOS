#!/usr/bin/env bash
# tools/ntfs-host.sh - host-side NTFS tooling for the SecOS M42 tests.
#
# Uses the real NTFS tools, independent of the SecOS driver:
#   - ntfs-3g's mkntfs (format) and ntfsinfo, from PATH or a user-local copy;
#   - the ntfs-3g FUSE driver to put files on / read files off an image.
#
# No root needed: `setup` downloads the Ubuntu/Debian packages with
# `apt-get download` into $NTFS_TOOLS when they are not installed. The FUSE
# mount runs inside an unprivileged user+mount namespace (`unshare -rm`),
# where the kernel allows FUSE mounts. ntfs-3g drops supplementary groups with
# setgroups(), which a user namespace forbids, so it runs with a tiny
# LD_PRELOAD shim that makes that call a no-op ($NTFS_TOOLS/nosetgroups.so).
#
# Usage:
#   tools/ntfs-host.sh setup                 fetch tools + build the shim (no sudo)
#   tools/ntfs-host.sh mkfs IMG SIZE_MB      create an empty NTFS image
#   tools/ntfs-host.sh run IMG 'CMD'         run CMD with the image mounted at $M
#   tools/ntfs-host.sh mkgood IMG            build the M42 test image (+ IMG.list)
#   tools/ntfs-host.sh mkcorrupt DIR         build the corrupt-image suite in DIR
#   tools/ntfs-host.sh mkfuzz IMG            smaller full-featured image (fuzz base)
#   tools/ntfs-host.sh verify IMG LIST [MP]  compare a SecOS listing with IMG.list
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NTFS_TOOLS="${NTFS_TOOLS:-$HOME/.local/secos-ntfs}"
PY="python3 -I $REPO_ROOT/tools/ntfs_img.py"

die() { echo "ntfs-host: $*" >&2; exit 1; }

find_tool() { # find_tool mkntfs
    if command -v "$1" >/dev/null 2>&1; then command -v "$1"; return 0; fi
    for d in "$NTFS_TOOLS/usr/sbin" "$NTFS_TOOLS/usr/bin" "$NTFS_TOOLS/sbin" "$NTFS_TOOLS/bin" /usr/sbin /sbin; do
        [[ -x "$d/$1" ]] && { echo "$d/$1"; return 0; }
    done
    return 1
}

cmd_setup() {
    mkdir -p "$NTFS_TOOLS"
    if ! find_tool mkntfs >/dev/null || ! find_tool ntfs-3g >/dev/null; then
        local tmp; tmp="$(mktemp -d)"
        ( cd "$tmp" && apt-get download ntfs-3g libntfs-3g89 >/dev/null ) \
            || { rm -rf "$tmp"; die "apt-get download failed (packages: ntfs-3g libntfs-3g89)"; }
        for deb in "$tmp"/*.deb; do dpkg-deb -x "$deb" "$NTFS_TOOLS"; done
        rm -rf "$tmp"
    fi
    local src="$NTFS_TOOLS/nosetgroups.c"
    printf '#include <grp.h>\nint setgroups(size_t n, const gid_t* l){ (void)n; (void)l; return 0; }\n' > "$src"
    gcc -shared -fPIC -o "$NTFS_TOOLS/nosetgroups.so" "$src" || die "cannot build the setgroups shim"
    echo "ntfs-host: tools ready ($NTFS_TOOLS)"
}
auto_setup() {
    if ! find_tool mkntfs >/dev/null || ! find_tool ntfs-3g >/dev/null || [[ ! -f "$NTFS_TOOLS/nosetgroups.so" ]]; then
        echo "ntfs-host: preparing NTFS tools in $NTFS_TOOLS" >&2
        cmd_setup >&2 || true
    fi
    MKNTFS="$(find_tool mkntfs)" || die "mkntfs not found - run: tools/ntfs-host.sh setup"
    NTFS3G="$(find_tool ntfs-3g)" || die "ntfs-3g not found - run: tools/ntfs-host.sh setup"
    SHIM="$NTFS_TOOLS/nosetgroups.so"
    [[ -e /dev/fuse ]] || die "/dev/fuse not available"
    export LD_LIBRARY_PATH="$NTFS_TOOLS/usr/lib/x86_64-linux-gnu:$NTFS_TOOLS/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
}

cmd_mkfs() { auto_setup; rm -f "$1"; truncate -s "${2}M" "$1"; "$MKNTFS" -F -Q -L SECOSNTFS "$1" >/dev/null 2>&1 || die "mkntfs failed"; }

# Run a shell snippet with the image mounted at $M (inside a user namespace).
cmd_run() {
    auto_setup
    local img; img="$(readlink -f "$1")"; local script="$2"
    local mnt; mnt="$(mktemp -d)"
    unshare -rm env M="$mnt" IMG="$img" NTFS3G="$NTFS3G" SHIM="$SHIM" PY="$PY" SCRIPT="$script" bash -c '
        LD_PRELOAD="$SHIM" "$NTFS3G" -o compression,streams_interface=xattr "$IMG" "$M" >/dev/null 2>&1 \
            || { echo "ntfs-host: FUSE mount failed" >&2; exit 1; }
        bash -c "$SCRIPT"; rc=$?
        umount "$M"; exit $rc'
    local rc=$?
    sleep 0.3                     # let the FUSE daemon flush and exit
    rmdir "$mnt" 2>/dev/null
    return $rc
}

inode_of() { "$(find_tool ntfsinfo)" -F "$2" "$1" 2>/dev/null | sed -n 's/^Dumping Inode \([0-9]*\).*/\1/p' | head -1; }

cmd_mkgood() {
    local img="$1"
    cmd_mkfs "$img" 96
    cmd_run "$img" '$PY populate "$M"' || die "populating $img failed"
    local n; n="$(inode_of "$img" /secret.efs)"; [[ -n "$n" ]] || die "secret.efs not found"
    $PY efs "$img" "$n" || die "efs patch failed"
    # Reference listing, read through ntfs-3g (independent of SecOS).
    cmd_run "$img" '$PY list "$M"' > "$img.list" || die "host listing failed"
    local fr; fr="$(inode_of "$img" /fragmented.bin)"
    $PY info "$img" "$fr" | grep -q "attr 0x20" || die "fragmented.bin has no \$ATTRIBUTE_LIST"
    local md; md="$(inode_of "$img" /many)"
    $PY info "$img" "$md" | grep -q "internal_nodes=[1-9]" || die "/many index is not multi-level"
    echo "ntfs-host: $img ready ($(grep -c $'^F\t' "$img.list") files; listing in $img.list)"
}

cmd_mkcorrupt() {
    local dir="$1" base
    mkdir -p "$dir"; base="$dir/base.img"
    cmd_mkfs "$base" 16
    cmd_run "$base" '$PY populate-small "$M"' || die "populating corrupt base failed"
    $PY corrupt "$base" "$dir"
}

cmd_mkfuzz() {
    local img="$1"
    cmd_mkfs "$img" 32
    cmd_run "$img" '$PY populate-fuzz "$M"' || die "populating $img failed"
    local n; n="$(inode_of "$img" /secret.efs)"; [[ -n "$n" ]] && $PY efs "$img" "$n"
    cmd_run "$img" '$PY list "$M"' > "$img.list" || die "host listing failed"
    local fr; fr="$(inode_of "$img" /fragmented.bin)"
    $PY info "$img" "$fr" | grep -q "attr 0x20" || echo "ntfs-host: note: fuzz fragmented.bin has no \$ATTRIBUTE_LIST" >&2
    local md; md="$(inode_of "$img" /many)"
    $PY info "$img" "$md" | grep -q "internal_nodes=[1-9]" || echo "ntfs-host: note: fuzz /many index is single-level" >&2
    echo "ntfs-host: $img ready (fuzz base)"
}

cmd_verify() {
    # The listing ntfs-3g produced at build time is the reference.
    $PY compare "$1.list" "$2" "${3:-/mnt}"
}

case "${1:-}" in
    setup)     cmd_setup ;;
    mkfs)      cmd_mkfs "$2" "$3" ;;
    run)       cmd_run "$2" "$3" ;;
    mkgood)    cmd_mkgood "$2" ;;
    mkcorrupt) cmd_mkcorrupt "$2" ;;
    mkfuzz)    cmd_mkfuzz "$2" ;;
    verify)    cmd_verify "$2" "$3" "${4:-/mnt}" ;;
    *) sed -n '2,/^set /p' "$0" | grep '^#' | sed 's/^# \?//'; exit 2 ;;
esac
