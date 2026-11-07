#!/bin/bash
# tools/lockdep_check.sh — the M50 lock-validator run (`make lockdep-check`).
# SPDX-License-Identifier: MIT
#
# Builds one image with -DSECOS_LOCKDEP (lock order / recursion / IRQ-safety /
# held-across checks, 2 s stuck watchdog, kfree poisoning with 0x6B) and
# CONSOLE=root plus a throwaway SSH key, then drives it in parallel:
#   ssd4, ssd8  tools/smp_stress_drive.py at -smp 4 and -smp 8
#   nsd4        tools/net_stress_drive.py at -smp 4 with e1000e (MSI-X NAPI)
#   off4        the same image booted with `bkl=off`, smp_stress_drive --quick
# Every driver step must pass and every debugcon log must be free of
# [LOCKDEP], "[LOCK] BUG", panics and CPU exceptions. ~25 minutes.
#
# Usage: tools/lockdep_check.sh [OUTDIR]     (default: a fresh /tmp directory)
# Exit 0 = clean. Host ports are random (never 2222/8000).
set -u
cd "$(dirname "$0")/.." || exit 2
OUT=${1:-$(mktemp -d /tmp/secos_lockdep.XXXXXX)}
mkdir -p "$OUT"
export PYENV_VERSION=${PYENV_VERSION:-3.11.8}

rm -f "$OUT/id" "$OUT/id.pub"
ssh-keygen -q -t ed25519 -N '' -C secos-lockdep-check -f "$OUT/id" || exit 2
make clean >/dev/null 2>&1 || true
echo "[lockdep] building the -DSECOS_LOCKDEP image..."
if ! make iso CONSOLE=root CFLAGS_EXTRA="-DSECOS_LOCKDEP" \
        SSH_AUTHORIZED_KEYS="$OUT/id.pub" >"$OUT/build.log" 2>&1; then
    echo "[lockdep] BUILD ERROR — see $OUT/build.log"; exit 2
fi
cp myos.iso "$OUT/ld.iso"
# The same kernel through a GRUB entry whose command line says bkl=off.
rm -rf "$OUT/iso_off"; cp -r isodir "$OUT/iso_off"
sed -i 's|^    multiboot2 /boot/kernel.bin$|    multiboot2 /boot/kernel.bin bkl=off|; s/^set timeout=3/set timeout=0/' \
    "$OUT/iso_off/boot/grub/grub.cfg"
grub-mkrescue --output="$OUT/ld_off.iso" "$OUT/iso_off" >/dev/null 2>&1 || { echo "[lockdep] grub-mkrescue failed"; exit 2; }

echo "[lockdep] running ssd4, ssd8, nsd4, off4 in parallel..."
timeout 1500 python3 tools/smp_stress_drive.py --iso "$OUT/ld.iso" --smp 4 \
    --log "$OUT/ssd4.log" >"$OUT/ssd4.out" 2>&1 &
timeout 1500 python3 tools/smp_stress_drive.py --iso "$OUT/ld.iso" --smp 8 \
    --log "$OUT/ssd8.log" >"$OUT/ssd8.out" 2>&1 &
timeout 900 python3 tools/net_stress_drive.py --iso "$OUT/ld.iso" --smp 4 --nic e1000e \
    --key "$OUT/id" --log "$OUT/nsd4.log" --out "$OUT/nsd4.out" >/dev/null 2>&1 &
timeout 1500 python3 tools/smp_stress_drive.py --iso "$OUT/ld_off.iso" --smp 4 --quick \
    --log "$OUT/off4.log" >"$OUT/off4.out" 2>&1 &
wait

pass=0; fail=0
chk() { if [ "$2" -eq 0 ]; then pass=$((pass+1)); echo "  [PASS] $1"; else fail=$((fail+1)); echo "  [FAIL] $1"; fi; }
for r in ssd4 ssd8 off4; do
    grep -q "^\[SSD\] RESULT " "$OUT/$r.out"; chk "$r driver ran to completion" $?
    ! grep -q "^\[SSD\] .* FAIL" "$OUT/$r.out"; chk "$r every step passed" $?
done
grep -q "^\[NSD\] RESULT " "$OUT/nsd4.out"; chk "nsd4 driver ran to completion" $?
! grep -q "^\[NSD\] .* FAIL" "$OUT/nsd4.out"; chk "nsd4 every step passed" $?
for r in ssd4 ssd8 nsd4 off4; do
    ! grep -aq "\[LOCKDEP\]\|\[LOCK\] BUG\|PANIC\|\[EXC\]" "$OUT/$r.log"
    chk "$r no lockdep report, panic or CPU exception" $?
done
grep -aq "\[LDTEST\] all 0x000000000000000A/0x000000000000000A" "$OUT/ssd4.log"
chk "ssd4 lockdep catches all 10 boot negative controls" $?
grep -aq "\[BKL\] WARNING: bkl=off" "$OUT/off4.log"; chk "off4 bkl=off boot word recognised" $?
grep -aq "\[BKL\] kernel lock mode: bkl=on" "$OUT/ssd4.log"; chk "ssd4 default mode is bkl=on" $?
grep -ah "\[LOCKDEP\]\|\[LOCK\] BUG" "$OUT"/*.log 2>/dev/null | sort | uniq -c | head -20
echo "[lockdep] RESULT pass=$pass fail=$fail (logs in $OUT)"
[ "$fail" -eq 0 ]
