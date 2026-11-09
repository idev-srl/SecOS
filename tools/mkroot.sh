#!/usr/bin/env bash
# tools/mkroot.sh — create the SecOS persistent system disk (M41).
#
#   tools/mkroot.sh <image> [size]          create <image> if it does not exist
#   tools/mkroot.sh --reset <image> [size]  delete <image> first (make reset-root)
#
# The image is a sparse raw file holding an ext4 volume in the default
# mkfs.ext4 layout (journal, extents, 64-bit, flex_bg, metadata_csum). SecOS
# journals its own metadata writes into it (checksum-v3 JBD2, kernel M27b/M41),
# so a hard power-off never leaves it torn. The volume carries only a skeleton
# and the /.secosroot marker that makes the kernel adopt it as "/". Everything
# else (programs in /bin, the default /etc files) is installed by the kernel at
# the first boot (kernel/sysroot.c).
#
# Without --reset an existing image is NEVER touched: this script refuses and
# exits 0 ("already exists"), so `make run*` can call it every time.
set -euo pipefail

reset=0
if [[ "${1:-}" == "--reset" ]]; then reset=1; shift; fi
img="${1:?usage: mkroot.sh [--reset] <image> [size]}"
size="${2:-8G}"   # [U6] 8 GiB sparse: costs only what is used on the host

if [[ -e "$img" && $reset -eq 0 ]]; then
    echo "mkroot: $img already exists (kept; 'make reset-root' recreates it)"
    exit 0
fi
command -v mkfs.ext4 >/dev/null 2>&1 || { echo "mkroot: mkfs.ext4 not found (install e2fsprogs)" >&2; exit 2; }

if [[ -e "$img" ]]; then
    echo "reset-root: DELETING $img and everything on it:"
    echo "  size on disk: $(du -h "$img" | cut -f1) (apparent $(du -h --apparent-size "$img" | cut -f1))"
    if command -v debugfs >/dev/null 2>&1; then
        for d in /root /home /etc /etc/dropbear /var/log; do
            # ls -p: /inode/mode/uid/gid/name/size/ per entry
            list=$(debugfs -R "ls -p $d" "$img" 2>/dev/null | awk -F/ '$6!="." && $6!=".." && $6!="" {printf "%s ", $6}' || true)
            [[ -n "$list" ]] && echo "  $d: $list"
        done
    fi
    echo "  (files in /root and /home, /etc edits, keymap, the SSH host key, logs)"
    rm -f -- "$img"
fi

stage=$(mktemp -d "${TMPDIR:-/tmp}/secos-rootstage.XXXXXX")
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage"/{bin,etc,dev,proc,sys,tmp,usr/bin,usr/local/bin,opt,home,lib,root,var/log,var/lib/secos,etc/dropbear,mnt}
chmod 1777 "$stage/tmp"
chmod 0700 "$stage/root" "$stage/etc/dropbear"
printf 'SecOS persistent root marker (tools/mkroot.sh). Do not delete: without it\nthe kernel does not use this disk as "/".\n' > "$stage/.secosroot"
printf 'Welcome to SecOS (persistent ext4 root: files you create survive reboot).\n' > "$stage/etc/motd"

truncate -s "$size" "$img"
mkfs.ext4 -q -F -L SECOSROOT -E root_owner=0:0 -d "$stage" "$img"
# mkfs -d copies the build user's uid/gid: give every staged entry to root.
if command -v debugfs >/dev/null 2>&1; then
    ( cd "$stage" && find . -mindepth 1 | sed 's|^\.||' | while read -r p; do
          printf 'sif "%s" uid 0\nsif "%s" gid 0\n' "$p" "$p"; done ) > "$stage.cmds"
    debugfs -w -f "$stage.cmds" "$img" >/dev/null 2>&1 || echo "mkroot: warning: could not chown the skeleton to root"
    # Journal checksums (JBD2 csum v3 + 64-bit tags), as Linux turns them on
    # for a metadata_csum volume at its first mount: every transaction SecOS
    # commits then carries crc32c tag/descriptor/commit checksums that e2fsck
    # and Linux verify on replay.
    printf 'jo -c\njc\n' > "$stage.cmds"
    debugfs -w -f "$stage.cmds" "$img" >/dev/null 2>&1 || true
    rm -f "$stage.cmds"
    dumpe2fs -h "$img" 2>/dev/null | grep -q journal_checksum_v3 \
        || echo "mkroot: note: journal checksums not enabled (old debugfs); journalling still works"
fi
echo "mkroot: created $img ($size sparse, ext4 + journal + metadata_csum, label SECOSROOT)"
