#!/usr/bin/env bash
# tools/selftest.sh — SECoS non-interactive self-test harness.
#
# Builds a self-checking image and asserts kernel behaviour from the debugcon
# log alone (no interactive shell needed).  This is the gate used for autonomous
# development: each milestone adds assertions here.
#
# Usage:
#   tools/selftest.sh [--timeout N]
#
# Exit codes:
#   0  PASS — all assertions held
#   1  FAIL — at least one assertion failed
#   2  BUILD/ENV error
#
# Assertions (current):
#   A. M4 isolation selftest reports 12/12 PASS.
#   B. M7 ring-3 cooperative scheduling: two ring-3 processes alternate via
#      SYS_YIELD (both 1->2 and 2->1 switches observed, no CPU exception).

set -euo pipefail

TIMEOUT=12
while [[ $# -gt 0 ]]; do
    case "$1" in
        --timeout) shift; TIMEOUT=$1 ;;
        -h|--help) sed -n '2,/^set /p' "$0" | grep '^#' | sed 's/^# \?//'; exit 0 ;;
        *) echo "[selftest] unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

LOG=/tmp/secos_selftest.log
PASS=0
FAIL=0
check() { # check "name" <0-if-ok>
    if [[ "$2" -eq 0 ]]; then echo "  [PASS] $1"; PASS=$((PASS+1));
    else echo "  [FAIL] $1"; FAIL=$((FAIL+1)); fi
}

echo "[selftest] Building self-test image (M7_RING3_DEMO=1, M4 selftest on)..."
make clean >/dev/null 2>&1 || true
if ! make CFLAGS_EXTRA="-DM7_RING3_DEMO=1 -DDEV_ALLOW_UNSIGNED" >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2
    exit 2
fi

echo "[selftest] Running (mb2, ${TIMEOUT}s)..."
# smoke.sh exits 0 when the kernel stays alive to the timeout (no triple fault).
if ! tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$LOG" >/dev/null 2>&1; then
    echo "[selftest] FAIL — kernel did not stay alive (triple fault / crash)"
    FAIL=$((FAIL+1))
fi

echo "[selftest] Assertions:"

# A. M4 isolation selftest 12/12
grep -q "12/12 PASS" "$LOG"; check "M4 isolation selftest 12/12" $?

# A2. M9 crypto known-answer self-tests
grep -q "\[CRYPTO\] --- all PASS ---" "$LOG"; check "M9 crypto KATs pass" $?

# B. M7 ring-3 cooperative scheduling
n_fwd=$(grep -c "switch 0x0000000000000001 -> 0x0000000000000002" "$LOG" || true)
n_rev=$(grep -c "switch 0x0000000000000002 -> 0x0000000000000001" "$LOG" || true)
[[ "$n_fwd" -ge 2 && "$n_rev" -ge 2 ]]; check "M7 ring-3 cooperative yield alternates (fwd=$n_fwd rev=$n_rev)" $?

# No CPU exception anywhere
! grep -q "\[EXC\]" "$LOG"; check "no CPU exception ([EXC]) during M7 run" $?

# ---- M8: preemptive multitasking + exit/reap + no-leak ----
M8LOG=/tmp/secos_selftest_m8.log
echo "[selftest] Building M8 image (M8_SCHED_DEMO=1)..."
make clean >/dev/null 2>&1 || true
if ! make CFLAGS_EXTRA="-DM8_SCHED_DEMO=1 -DDEV_ALLOW_UNSIGNED" >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M8 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running M8 (mb2, $((TIMEOUT+6))s)..."
tools/smoke.sh --mb2 --timeout "$((TIMEOUT+6))" --log "$M8LOG" >/dev/null 2>&1 || true

n_pre=$(grep -c "SCHED] preempt" "$M8LOG" || true)
[[ "$n_pre" -ge 2 ]]; check "M8 preemptive switches occur (preempt=$n_pre)" $?
n_exit=$(grep -c "SCHED] exit" "$M8LOG" || true)
[[ "$n_exit" -ge 4 ]]; check "M8 processes exit via SYS_EXIT (exit=$n_exit)" $?
grep -q "PMM stable across rounds: NO LEAK" "$M8LOG"; check "M8 no PMM leak across rounds" $?
grep -q "\[M8\] DONE" "$M8LOG"; check "M8 demo completed ([M8] DONE)" $?
! grep -q "\[EXC\]" "$M8LOG"; check "no CPU exception ([EXC]) during M8 run" $?

# ---- M9: signed userland (enforcement ON, no DEV_ALLOW_UNSIGNED) ----
M9LOG=/tmp/secos_selftest_m9.log
echo "[selftest] Building M9 image (M9_USER_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make CFLAGS_EXTRA=-DM9_USER_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M9 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running M9 (mb2, ${TIMEOUT}s)..."
tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$M9LOG" >/dev/null 2>&1 || true

grep -q "\[M9\] tampered ELF REFUSED" "$M9LOG"; check "M9 tampered ELF refused by loader gate" $?
grep -q "\[SEC\] ELF signature OK" "$M9LOG";    check "M9 signed ELF passes signature gate" $?
grep -q "signed SecOS user program running in ring 3" "$M9LOG"; check "M9 signed user program runs (SYS_WRITE)" $?
grep -q "\[M9\] user program exited; DONE" "$M9LOG"; check "M9 user program exits cleanly" $?
! grep -q "\[EXC\]" "$M9LOG"; check "no CPU exception ([EXC]) during M9 run" $?

# ---- M10: storage & persistence (virtio-blk + FAT32/ext2/ext4 + run from disk) ----
echo "[selftest] Building M10 image (M10_RUN_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make iso CFLAGS_EXTRA=-DM10_RUN_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M10 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi

# Run the M10 image against a virtio-blk disk of the given filesystem.
run_m10_fs() { # run_m10_fs <fs>   (fs = fat32 | ext2 | ext4)
    local fs="$1" log="/tmp/secos_selftest_m10_${1}.log"
    make "disk-${fs}" >/dev/null 2>&1 || { echo "  [FAIL] M10/${fs} disk image build"; FAIL=$((FAIL+1)); return; }
    : > "$log"
    set +e
    timeout "$((TIMEOUT+6))" qemu-system-x86_64 \
        -cdrom myos.iso \
        -drive file=disk.img,if=virtio,format=raw -boot d \
        -debugcon file:"$log" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -no-shutdown -display none -m 256M
    set -e
    grep -q "\[VIRTIO-BLK\] ready" "$log";                 check "M10/${fs} virtio-blk online" $?
    grep -q "\[M10\] disk mounted at /mnt"  "$log";        check "M10/${fs} disk FS mounted at /mnt" $?
    grep -q "\[M10\] disk write+readback: OK" "$log";      check "M10/${fs} VFS write+readback persists" $?
    grep -q "\[M10\] tampered disk ELF REFUSED" "$log";    check "M10/${fs} tampered disk ELF refused (signing gate)" $?
    grep -q "\[M10\] wrote signed hello.elf to disk" "$log"; check "M10/${fs} wrote signed ELF to disk" $?
    grep -q "signed SecOS user program running in ring 3" "$log"; check "M10/${fs} signed ELF runs ring-3 from disk" $?
    local nexit; nexit=$(grep -c "SCHED] exit" "$log" || true)
    [[ "$nexit" -ge 1 ]];                                  check "M10/${fs} program exits via SYS_EXIT (exit=$nexit)" $?
    ! grep -q "\[EXC\]" "$log";                            check "M10/${fs} no CPU exception ([EXC])" $?
}
echo "[selftest] Running M10 (fat32, ext2, ext4 with virtio-blk disk)..."
run_m10_fs fat32
run_m10_fs ext2
run_m10_fs ext4

echo "[selftest] ---"
echo "[selftest] RESULT: $PASS passed, $FAIL failed"
if [[ "$FAIL" -eq 0 ]]; then echo "[selftest] DONE: ALL PASS"; exit 0; fi
echo "[selftest] DONE: FAILURES PRESENT (log: $LOG)"
exit 1
