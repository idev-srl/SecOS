#!/bin/bash
# tools/bkl_stress.sh — the M52 (BKL removal, phase D) userland stress gate
# (`make bklstress`). Builds one -DSECOS_LOCKDEP image (lock order / held-across
# checks, 2 s stuck watchdog, kfree poisoning with 0x6B) with CONSOLE=root, a
# throwaway SSH key and the chosen default kernel lock mode, then runs the
# userland stresses that race for real once syscalls leave the BKL, JOBS at a
# time (default 4):
#   ssd8   tools/smp_stress_drive.py           -smp 8
#   prd8   tools/proc_race_drive.py            -smp 8 (refcounts, mm_lock, /proc)
#   nsd4   tools/net_stress_drive.py           -smp 4, e1000e MSI-X, ssh sessions
#   tsd4   tools/tty_stress_drive.py           -smp 4 (pty pairs, pipelines, ISIG)
#   esd8   tools/exec_stress_drive.py          -smp 8, 500 parallel `bash -c true`
#   fsd4   tools/fs_stress_drive.py            -smp 4, ext4 root, then e2fsck -fn
#   fsd8   tools/fs_stress_drive.py            -smp 8, ext4 root, then e2fsck -fn
#   fsr4   tools/fs_stress_drive.py            -smp 4, ramfs /tmp
#   fsx4   tools/fs_stress_drive.py --data vfat  -smp 4, FAT32 sdb, fsck.vfat -n
#   fse4   tools/fs_stress_drive.py --data exfat -smp 4, exFAT sdb, fsck.exfat -n
#   fsc4   tools/fs_stress_drive.py --crash 3  -smp 4, kill -9 + journal replay
# A run passes when its driver says so and its debugcon log has no [LOCKDEP],
# "[LOCK] BUG", panic or CPU exception.
#
# Usage: tools/bkl_stress.sh [--mode off|on] [OUTDIR] [run...]
#        (default: mode off, a fresh /tmp directory, every run)
# Exit 0 = every run passed. Host ports are random (never 2222/8000).
# SPDX-License-Identifier: MIT
set -u
cd "$(dirname "$0")/.." || exit 2
MODE=off
if [ "${1:-}" = "--mode" ]; then MODE=$2; shift 2; fi
case $MODE in on|off) ;; *) echo "[bklstress] --mode on|off"; exit 2 ;; esac
OUT=${1:-$(mktemp -d /tmp/secos_bklstress.XXXXXX)}
[ $# -gt 0 ] && shift
mkdir -p "$OUT"
export PYENV_VERSION=${PYENV_VERSION:-3.11.8}
JOBS=${BKLSTRESS_JOBS:-4}
ALL="ssd8 prd8 nsd4 tsd4 esd8 fsd4 fsd8 fsr4 fsx4 fse4 fsc4"
RUNS=${*:-$ALL}

rm -f "$OUT/id" "$OUT/id.pub"
ssh-keygen -q -t ed25519 -N '' -C secos-bklstress -f "$OUT/id" || exit 2
make clean >/dev/null 2>&1 || true
echo "[bklstress] building the -DSECOS_LOCKDEP image, BKL_DEFAULT=$MODE..."
if ! make iso CONSOLE=root BKL_DEFAULT="$MODE" CFLAGS_EXTRA="-DSECOS_LOCKDEP" \
        SSH_AUTHORIZED_KEYS="$OUT/id.pub" >"$OUT/build.log" 2>&1; then
    echo "[bklstress] BUILD ERROR — see $OUT/build.log"; exit 2
fi
ISO="$OUT/bkl-$MODE.iso"
cp myos.iso "$ISO"

run() {
    local r=$1 log="$OUT/$1.log" out="$OUT/$1.out"
    case $r in
        ssd8) timeout 1800 python3 tools/smp_stress_drive.py --iso "$ISO" --smp 8 --log "$log" >"$out" 2>&1 ;;
        prd8) timeout 1500 python3 tools/proc_race_drive.py --iso "$ISO" --smp 8 --secs 20 --runs 3 \
                  --log "$log" >"$out" 2>&1 ;;
        nsd4) timeout 1200 python3 tools/net_stress_drive.py --iso "$ISO" --smp 4 --nic e1000e \
                  --key "$OUT/id" --log "$log" --out "$out" >/dev/null 2>&1 ;;
        tsd4) timeout 2400 python3 tools/tty_stress_drive.py --iso "$ISO" --smp 4 --log "$log" >"$out" 2>&1 ;;
        esd8) timeout 1800 python3 tools/exec_stress_drive.py --iso "$ISO" --smp 8 --count 500 \
                  --log "$log" >"$out" 2>&1 ;;
        fsd4|fsd8) local n=${r#fsd}
              timeout 3600 python3 tools/fs_stress_drive.py --boot 600 --iso "$ISO" --smp "$n" --work "$OUT/$r.w" \
                  --rounds 2 >"$out" 2>&1; local rc=$?; cp "$OUT/$r.w/run.log" "$log" 2>/dev/null
              rm -f "$OUT/$r.w"/*.img; return $rc ;;
        fsr4) timeout 2400 python3 tools/fs_stress_drive.py --boot 600 --iso "$ISO" --smp 4 --work "$OUT/$r.w" \
                  --target /tmp --rounds 2 >"$out" 2>&1; local rc=$?; cp "$OUT/$r.w/run.log" "$log" 2>/dev/null
              rm -f "$OUT/$r.w"/*.img; return $rc ;;
        fsx4|fse4) local k=vfat; [ "$r" = fse4 ] && k=exfat
              timeout 3600 python3 tools/fs_stress_drive.py --boot 600 --iso "$ISO" --smp 4 --work "$OUT/$r.w" \
                  --data "$k" --rounds 2 --mount-race 40 >"$out" 2>&1; local rc=$?; cp "$OUT/$r.w/run.log" "$log" 2>/dev/null
              rm -f "$OUT/$r.w"/*.img; return $rc ;;
        fsc4) timeout 3600 python3 tools/fs_stress_drive.py --boot 600 --iso "$ISO" --smp 4 --work "$OUT/$r.w" \
                  --crash 3 >"$out" 2>&1; local rc=$?; cat "$OUT/$r.w"/*.log >"$log" 2>/dev/null
              rm -f "$OUT/$r.w"/*.img "$OUT/$r.w"/*.img.host; return $rc ;;
        *) echo "unknown run $r" >"$out"; return 2 ;;
    esac
}

echo "[bklstress] running: $RUNS ($JOBS at a time)..."
for r in $RUNS; do
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 2; done
    ( run "$r"; echo $? >"$OUT/$r.rc" ) &
done
wait

pass=0; fail=0
chk() { if [ "$2" -eq 0 ]; then pass=$((pass+1)); echo "  [PASS] $1"; else fail=$((fail+1)); echo "  [FAIL] $1"; fi; }
for r in $RUNS; do
    [ "$(cat "$OUT/$r.rc" 2>/dev/null)" = 0 ]; rc=$?    # before the $(...) below resets $?
    chk "$r passed ($(grep -a 'RESULT' "$OUT/$r.out" 2>/dev/null | tail -n 1 | cut -c1-90))" $rc
    [ -s "$OUT/$r.log" ] && ! grep -aq "\[LOCKDEP\]\|\[LOCK\] BUG\|PANIC\|\[EXC\]" "$OUT/$r.log"
    chk "$r no lockdep report, panic or CPU exception" $?
done
grep -aq "\[BKL\] kernel lock mode: bkl=$MODE (default)" "$OUT/ssd8.log" 2>/dev/null || [[ " $RUNS " != *" ssd8 "* ]]
chk "the image booted in bkl=$MODE" $?
grep -ah "\[LOCKDEP\]\|\[LOCK\] BUG" "$OUT"/*.log 2>/dev/null | sort | uniq -c | head -20
echo "[bklstress] RESULT mode=$MODE pass=$pass fail=$fail (logs in $OUT)"
[ "$fail" -eq 0 ]
