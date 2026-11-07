#!/usr/bin/env bash
# tools/ext4_lock_test.sh — M51 C3 forced-concurrency gate for the ext2/ext4
# driver lock (fs/ext4locktest.c).
#
# Boots an ISO built with -DSECOS_EXT4_LOCKTEST (normally also -DSECOS_LOCKDEP)
# at -smp N on a ramfs root with a fresh scratch volume as virtio vda (the
# kernel mounts it at /mnt). At boot every CPU drives the volume through the
# driver's ops at once, outside the BKL; the run must end with
# "[E4LT] RESULT PASS", the debugcon log must be free of lockdep / lock-bug /
# fault markers, and the host's `e2fsck -fn` must find the volume clean.
# Done for each layout: a default mkfs.ext4 volume (journal, extents, 64-bit,
# flex_bg, metadata_csum: every op is a JBD2 transaction) and a plain ext2
# volume (no journal: direct writes).
#
# Output: "  [PASS] ..." / "  [FAIL] ..." lines, exit 0 when all passed.
# Usage: tools/ext4_lock_test.sh <iso> [smp] [workdir]
set -uo pipefail
ISO=${1:?usage: ext4_lock_test.sh <iso> [smp] [workdir]}
SMP=${2:-4}
W=${3:-$(mktemp -d "${TMPDIR:-/tmp}/secos_e4lt.XXXXXX")}
mkdir -p "$W"
PASSN=0; FAILN=0
ok() { if [[ "$2" -eq 0 ]]; then echo "  [PASS] E4LT $1"; PASSN=$((PASSN+1));
       else echo "  [FAIL] E4LT $1"; FAILN=$((FAILN+1)); fi; }

run_one() { # run_one <tag> <mkfs command...>
    local tag=$1; shift
    local img=$W/$tag.img log=$W/$tag.log
    rm -f "$img"; truncate -s 256M "$img"
    "$@" -q -F -L E4LT "$img" >/dev/null 2>&1 || { ok "$tag mkfs" 1; return; }
    : > "$log"
    qemu-system-x86_64 -cdrom "$ISO" -boot d -m 1G -smp "$SMP" \
        -drive file="$img",if=virtio,format=raw -nic none \
        -debugcon file:"$log" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial null &
    local qpid=$! n=0
    while (( n++ < 1800 )); do               # up to 6 minutes
        grep -aq "\[E4LT\] RESULT\|\[E4LT\] SKIP\|\[PANIC\]" "$log" && break
        kill -0 "$qpid" 2>/dev/null || break
        sleep 0.2
    done
    sleep 1
    kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
    grep -a "^\[E4LT\]" "$log" | grep -v "^\[E4LT\] cpu " | sed "s/^/    [$tag] /"
    grep -aq "\[E4LT\] RESULT PASS" "$log"; ok "$tag RESULT PASS" $?
    ! grep -aqE "LOCKDEP\]|\[LOCK\] BUG|\[EXC\] int=|\[PANIC\]|stack smashing" "$log"; ok "$tag no lockdep/lock/fault marker" $?
    local c; c=$(grep -ao "contended=[0-9]*" "$log" | head -1 | cut -d= -f2)
    [[ -n "$c" && "$c" -gt 0 ]]; ok "$tag ext2 lock contended (${c:-?} waits)" $?
    e2fsck -fn "$img" >"$W/$tag.fsck" 2>&1; ok "$tag host e2fsck -fn clean" $?
}

run_one ext4csum mkfs.ext4
run_one ext2 mkfs.ext2
echo "[e4lt] pass=$PASSN fail=$FAILN (artifacts in $W)"
[[ $FAILN -eq 0 ]]
