#!/bin/bash
# tools/locktests.sh — every forced-concurrency lock test of the BKL removal
# (M50 phase B, M51 phase C) on a -DSECOS_LOCKDEP build at -smp 4
# (`make locktests`). Each test is its own image, because every SECOS_*_TEST
# runs at boot on all CPUs outside the BKL:
#   pagecache  -DSECOS_PAGECACHE_TEST  tools/pc_stress_drive.py --pctest (B2)
#   b3         -DSECOS_B3_BLKSTRESS    tools/blk_stress_drive.py         (B3)
#   vfs-fat32  -DSECOS_VFS_TEST        boot, virtio FAT32 /mnt, [VFST]   (C1)
#   vfs-ext2   -DSECOS_VFS_TEST        boot, virtio ext2 /mnt, [VFST]    (C1)
#   fsvirt     -DSECOS_FSVIRT_TEST     boot, ramfs root, [FSVT]          (C2)
#   ext4       -DSECOS_EXT4_LOCKTEST   tools/ext4_lock_test.sh (ext4 + ext2) (C3)
#   c4         -DSECOS_C4_FSSTRESS     tools/c4_fs_drive.py (FAT32/exFAT/NTFS) (C4)
#   tty        -DSECOS_TTY_TEST        tools/tty_stress_drive.py --tttest (C5)
#   exec       -DSECOS_EXEC_TEST       tools/exec_stress_drive.py        (C6)
# A test passes when its driver/marker says so and its debugcon log has no
# [LOCKDEP], [LOCK] BUG, panic or CPU exception. The images are built one
# after the other (the tree has one object set), then run LOCKTESTS_JOBS at a
# time (default 3). make lockdep-check covers the generic SMP/net/proc stress.
#
# Usage: tools/locktests.sh [OUTDIR] [test...]     (default: all, fresh /tmp dir)
# Exit 0 = every test passed. Host ports are random (never 2222/8000).
# SPDX-License-Identifier: MIT
set -u
cd "$(dirname "$0")/.." || exit 2
OUT=${1:-$(mktemp -d /tmp/secos_locktests.XXXXXX)}
[ $# -gt 0 ] && shift
mkdir -p "$OUT"
export PYENV_VERSION=${PYENV_VERSION:-3.11.8}
JOBS=${LOCKTESTS_JOBS:-3}
ALL="pagecache b3 vfs fsvirt ext4 c4 tty exec"
TESTS=${*:-$ALL}

flag_of() {
    case $1 in
        pagecache) echo SECOS_PAGECACHE_TEST ;; b3) echo SECOS_B3_BLKSTRESS ;;
        vfs) echo SECOS_VFS_TEST ;; fsvirt) echo SECOS_FSVIRT_TEST ;;
        ext4) echo SECOS_EXT4_LOCKTEST ;; c4) echo SECOS_C4_FSSTRESS ;;
        tty) echo SECOS_TTY_TEST ;; exec) echo SECOS_EXEC_TEST ;;
        *) return 1 ;;
    esac
}

# ---- build ----
for t in $TESTS; do
    f=$(flag_of "$t") || { echo "[locktests] unknown test: $t"; exit 2; }
    echo "[locktests] building $t (-D$f -DSECOS_LOCKDEP)..."
    make clean >/dev/null 2>&1 || true
    if ! make iso CONSOLE=root CFLAGS_EXTRA="-D$f -DSECOS_LOCKDEP" >"$OUT/$t.build.log" 2>&1; then
        echo "[locktests] BUILD ERROR $t — see $OUT/$t.build.log"; exit 2
    fi
    cp myos.iso "$OUT/$t.iso"
done
if [[ " $TESTS " == *" c4 "* ]]; then
    make disk-ntfs DISK_IMG="$OUT/ntfs.img" >"$OUT/ntfs.build.log" 2>&1 \
        || { echo "[locktests] disk-ntfs failed — see $OUT/ntfs.build.log"; exit 2; }
fi

# ---- run ----
# boot_marker <tag> <iso> <prefix> <timeout s> [qemu disk args...]: boot until
# "[<prefix>] RESULT" appears in the debugcon log.
boot_marker() {
    local tag=$1 iso=$2 pfx=$3 to=$4; shift 4
    local log="$OUT/$tag.log"; : >"$log"
    qemu-system-x86_64 -cdrom "$iso" -boot d -smp 4 -m 1G "$@" -nic none \
        -debugcon file:"$log" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial null >/dev/null 2>&1 &
    local q=$! n=0
    while (( n++ < to )); do
        grep -aq "^\[$pfx\] RESULT \|\[PANIC\]" "$log" && break
        kill -0 "$q" 2>/dev/null || break
        sleep 1
    done
    sleep 1; kill "$q" 2>/dev/null; wait "$q" 2>/dev/null
    grep -a "^\[$pfx\] " "$log" >"$OUT/$tag.out"
    grep -aq "^\[$pfx\] RESULT PASS" "$log"
}
run_test() {
    local t=$1 iso="$OUT/$1.iso"
    case $t in
        pagecache) timeout 2400 python3 tools/pc_stress_drive.py --iso "$iso" --smp 4 --pctest \
                       --rounds 200 --log "$OUT/$t.log" >"$OUT/$t.out" 2>&1 ;;
        b3)        mkdir -p "$OUT/b3"
                   timeout 1200 python3 tools/blk_stress_drive.py --iso "$iso" --smp 4 --out "$OUT/b3" \
                       --log "$OUT/$t.log" >"$OUT/$t.out" 2>&1; local r=$?; rm -f "$OUT"/b3/*.img; return $r ;;
        vfs)       local d="$OUT/vfs" r=0; mkdir -p "$d"
                   rm -f "$d/fat.img"; truncate -s 64M "$d/fat.img"; mkfs.fat -F 32 -n VFST "$d/fat.img" >/dev/null
                   boot_marker vfs-fat32 "$iso" VFST 600 -drive file="$d/fat.img",if=virtio,format=raw || r=1
                   rm -f "$d/ext2.img"; truncate -s 64M "$d/ext2.img"; mkfs.ext2 -q -F -L VFST "$d/ext2.img"
                   boot_marker vfs-ext2 "$iso" VFST 600 -drive file="$d/ext2.img",if=virtio,format=raw || r=1
                   cat "$OUT/vfs-fat32.log" "$OUT/vfs-ext2.log" >"$OUT/vfs.log"
                   cat "$OUT/vfs-fat32.out" "$OUT/vfs-ext2.out" >"$OUT/vfs.out"
                   rm -f "$d"/*.img; return $r ;;
        fsvirt)    boot_marker fsvirt "$iso" FSVT 600 ;;
        ext4)      timeout 1200 tools/ext4_lock_test.sh "$iso" 4 "$OUT/ext4" >"$OUT/$t.out" 2>&1; local r=$?
                   cat "$OUT"/ext4/*.log >"$OUT/$t.log" 2>/dev/null; rm -f "$OUT"/ext4/*.img; return $r ;;
        c4)        mkdir -p "$OUT/c4"
                   timeout 1500 python3 tools/c4_fs_drive.py --iso "$iso" --ntfs "$OUT/ntfs.img" --smp 4 \
                       --out "$OUT/c4" >"$OUT/$t.out" 2>&1; local r=$?
                   cp "$OUT/c4/debugcon.log" "$OUT/$t.log" 2>/dev/null; rm -f "$OUT"/c4/*.img; return $r ;;
        tty)       timeout 2400 python3 tools/tty_stress_drive.py --iso "$iso" --smp 4 --tttest \
                       --log "$OUT/$t.log" >"$OUT/$t.out" 2>&1 ;;
        exec)      timeout 1800 python3 tools/exec_stress_drive.py --iso "$iso" --smp 4 --count 500 \
                       --log "$OUT/$t.log" >"$OUT/$t.out" 2>&1 ;;
    esac
}

echo "[locktests] running: $TESTS ($JOBS at a time)..."
for t in $TESTS; do
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 2; done
    ( run_test "$t"; echo $? >"$OUT/$t.rc" ) &
done
wait

pass=0; fail=0
chk() { if [ "$2" -eq 0 ]; then pass=$((pass+1)); echo "  [PASS] $1"; else fail=$((fail+1)); echo "  [FAIL] $1"; fi; }
for t in $TESTS; do
    [ "$(cat "$OUT/$t.rc" 2>/dev/null)" = 0 ]; rc=$?    # before the $(...) below resets $?
    chk "$t passed ($(grep -a 'RESULT' "$OUT/$t.out" 2>/dev/null | tail -n 1 | cut -c1-80))" $rc
    [ -s "$OUT/$t.log" ] && ! grep -aq "\[LOCKDEP\]\|\[LOCK\] BUG\|PANIC\|\[EXC\]" "$OUT/$t.log"
    chk "$t no lockdep report, panic or CPU exception" $?
done
echo "[locktests] RESULT pass=$pass fail=$fail (logs in $OUT)"
[ "$fail" -eq 0 ]
