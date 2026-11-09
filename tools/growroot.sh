#!/usr/bin/env bash
# tools/growroot.sh — grow an existing SecOS system disk in place (U6).
#
#   tools/growroot.sh <image> [size]        (default size 8G; `make grow-root`)
#
# Offline, on the host, never while a VM uses the image:
#   1. checks: a regular file, a SecOS root (/.secosroot), not in use (QEMU's
#      image lock, fuser), only ever bigger;
#   2. refuses a size the SecOS ext4 driver could not handle afterwards (the
#      group-descriptor table must fit in its reserved blocks: no meta_bg);
#   3. a sparse backup copy <image>.pre-grow-<time> of the image as it was
#      (BACKUP=0 skips it);
#   4. e2fsck -f -p: replays a journal left by an unclean shutdown and fixes
#      what preen mode may fix; anything worse stops here (backup = original);
#   5. truncate (sparse) + resize2fs, then e2fsck -fn must be clean and the
#      feature set unchanged; on any failure the backup is put back.
# Files on the disk are not touched: resize2fs only adds block groups.
set -euo pipefail

img="${1:?usage: growroot.sh <image> [size]}"
size="${2:-8G}"
BACKUP="${BACKUP:-1}"
PATH="$PATH:/usr/sbin:/sbin"

die() { echo "grow-root: $*" >&2; exit 1; }
for t in e2fsck resize2fs dumpe2fs debugfs; do
    command -v "$t" >/dev/null 2>&1 || die "$t not found (install e2fsprogs)"
done
[[ -f "$img" && ! -L "$img" ]] || die "$img: not a regular file"

to_bytes() { # 8G / 8192M / 8589934592
    local s=$1 n u
    [[ "$s" =~ ^([0-9]+)([KMGT]?)i?B?$ ]] || die "bad size '$s' (use e.g. 8G, 16G, 12288M)"
    n=${BASH_REMATCH[1]}; u=${BASH_REMATCH[2]}
    case "$u" in K) n=$((n<<10));; M) n=$((n<<20));; G) n=$((n<<30));; T) n=$((n<<40));; esac
    echo "$n"
}
new=$(to_bytes "$size")
old=$(stat -c %s "$img")

# --- in use? ------------------------------------------------------------------
if command -v fuser >/dev/null 2>&1 && fuser "$img" >/dev/null 2>&1; then
    die "$img is open by another process (a running VM?): shut SecOS down first"
fi
if command -v qemu-img >/dev/null 2>&1; then
    qerr=$(qemu-img info -f raw "$img" 2>&1 >/dev/null || true)
    case "$qerr" in *[Ll]ock*) die "$img is locked by QEMU (a running VM): shut SecOS down first";; esac
fi

# --- a SecOS root? --------------------------------------------------------------
dumpe2fs -h "$img" >/dev/null 2>&1 || die "$img: no ext2/ext4 filesystem"
debugfs -R "stat /.secosroot" "$img" 2>/dev/null | grep -q "Type: regular" \
    || die "$img: no /.secosroot marker (not a SecOS system disk)"
hdr() { dumpe2fs -h "$img" 2>/dev/null | sed -n "s/^$1: *//p"; }
bs=$(hdr "Block size"); bpg=$(hdr "Blocks per group"); blocks=$(hdr "Block count")
feat=$(hdr "Filesystem features")
fsbytes=$((blocks * bs))
(( new % bs == 0 )) || die "size must be a multiple of the block size ($bs)"
if (( new <= fsbytes )); then
    if (( new == fsbytes )); then echo "grow-root: $img is already $size ($((fsbytes>>20)) MiB): nothing to do"; exit 0; fi
    die "$img holds a $((fsbytes>>20)) MiB filesystem; shrinking is not supported"
fi
case " $feat " in *" meta_bg "*) die "$img uses meta_bg, which SecOS does not support";; esac

# --- the group descriptor table must fit (SecOS has no meta_bg) ------------------
desc=32; case " $feat " in *" 64bit "*) desc=$(hdr "Group descriptor size"); desc=${desc:-64};; esac
resv=$(hdr "Reserved GDT blocks"); resv=${resv:-0}
groups_old=$(( (blocks + bpg - 1) / bpg ))
gdt_old=$(( (groups_old * desc + bs - 1) / bs ))
groups_new=$(( (new / bs + bpg - 1) / bpg ))
gdt_new=$(( (groups_new * desc + bs - 1) / bs ))
if (( gdt_new > gdt_old + resv )); then
    max=$(( ((gdt_old + resv) * bs / desc) * bpg * bs ))
    die "$size needs $gdt_new group-descriptor blocks, the disk has room for $((gdt_old + resv)) (largest size without meta_bg: $((max>>30)) GiB)"
fi
avail=$(df -P --block-size=1 "$(dirname "$img")" | awk 'NR==2 {print $4}')
echo "grow-root: $img: $((fsbytes>>20)) MiB -> $((new>>20)) MiB ($groups_old -> $groups_new block groups)"

# --- 1. backup (of the image as it was) ----------------------------------------------------------------------------
bak=
if [[ "$BACKUP" != 0 ]]; then
    used=$(( $(stat -c %b "$img") * 512 ))
    (( avail > used + (64 << 20) )) || die "not enough host space for the backup copy ($((used>>20)) MiB); BACKUP=0 skips it"
    bak="$img.pre-grow-$(date +%Y%m%d%H%M%S)"
    cp --sparse=always -- "$img" "$bak"
    echo "grow-root: backup copy $bak"
fi
# --- 2. check (and replay the journal) ------------------------------------------------
echo "grow-root: e2fsck -f -p (before)"
set +e; e2fsck -f -p "$img"; rc=$?; set -e
if (( rc >= 4 )); then
    die "e2fsck found problems it cannot fix in preen mode (exit $rc): not grown; inspect it with 'e2fsck -f $img'${bak:+ (the image as it was: $bak)}"
fi

restore() {
    if [[ -n "$bak" ]]; then
        echo "grow-root: FAILED - restoring $img from $bak" >&2
        cp --sparse=always -- "$bak" "$img"; truncate -s "$old" "$img"
    else
        echo "grow-root: FAILED - no backup (BACKUP=0); the image may need 'e2fsck -f'" >&2
    fi
    exit 1
}

# --- 3. grow ------------------------------------------------------------------------------------
truncate -s "$new" "$img" || restore
resize2fs "$img" || restore

# --- 4. verify --------------------------------------------------------------------------------------
echo "grow-root: e2fsck -fn (after)"
e2fsck -fn "$img" || restore
feat2=$(hdr "Filesystem features" | sed 's/ needs_recovery//')
feat1=$(echo "$feat" | sed 's/ needs_recovery//')
[[ "$feat1" == "$feat2" ]] || { echo "grow-root: feature set changed: '$feat1' -> '$feat2'" >&2; restore; }
blocks2=$(hdr "Block count")
(( blocks2 * bs == new )) || { echo "grow-root: resize2fs left $blocks2 blocks" >&2; restore; }
free2=$(hdr "Free blocks")
echo "grow-root: done: $img is now $((new>>20)) MiB ($((free2 * bs >> 20)) MiB free), e2fsck clean"
[[ -n "$bak" ]] && echo "grow-root: when SecOS boots fine from it, delete the backup: rm '$bak'"
exit 0
