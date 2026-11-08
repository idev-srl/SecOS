#!/usr/bin/env bash
# tools/acpi_dmar_gate.sh - M53b gate: ACPI DMAR / IVRS parsing (read-only)
#
#   tools/acpi_dmar_gate.sh [--no-build] [--iters N]
#
# 1. Host: build tools/acpi_fuzz (ASan+UBSan), check the parser against the
#    synthesized tables of tools/acpi_tables.py (known answers), then fuzz
#    N (default 50000) mutated tables.
# 2. Guests (MB2 ISO, -smp 2): q35 + intel-iommu, q35 + intel-iommu behind
#    PCIe root ports / a PCI bridge, q35 + amd-iommu (QEMU 6.2, and 9.2 when
#    ~/qemu-local has it), plain q35 and i440fx ([IOMMU] none), and UEFI/OVMF
#    q35 + intel-iommu (the loader hands the RSDP over). Every guest must
#    reach /bin/init with no CPU exception.
#
# Logs go to build/m53b-gate/. Every QEMU runs in a private /tmp (unshare -rm)
# and is stopped by timeout(1), never by pkill. Exit 0 = every check passed.
#
# Copyright (c) 2026 iDev srl
# Author: Luigi De Astis <l.deastis@idev-srl.com>
# SPDX-License-Identifier: MIT
set -u
cd "$(dirname "$0")/.."
ROOT=$(pwd)
OUT="$ROOT/build/m53b-gate"
ITERS=50000
BUILD=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build) BUILD=0 ;;
        --iters) ITERS="$2"; shift ;;
        *) echo "usage: $0 [--no-build] [--iters N]" >&2; exit 2 ;;
    esac
    shift
done
mkdir -p "$OUT"
PASS=0; FAIL=0
check() { if [[ "$2" == 0 ]]; then echo "  [PASS] $1"; PASS=$((PASS+1)); else echo "  [FAIL] $1"; FAIL=$((FAIL+1)); fi; }

# ---- host: known answers + fuzz ----
if make -C tools/acpi_fuzz >"$OUT/fuzz_build.log" 2>&1; then
    python3 tools/acpi_tables.py check tools/acpi_fuzz/acpi_fuzz "$OUT/seeds" >"$OUT/kat.log" 2>&1
    check "M53b parser: synthesized DMAR/IVRS tables give the encoded values" $?
    timeout 600 tools/acpi_fuzz/acpi_fuzz fuzz "$ITERS" 53 "$OUT"/seeds/*.bin >"$OUT/fuzz.log" 2>&1
    check "M53b host fuzz: $ITERS mutated tables, no crash/UB/hang" $?
else
    echo "  [SKIP] M53b host fuzz (gcc with ASan/UBSan unavailable)"
fi

# ---- guests ----
if [[ $BUILD == 1 ]]; then
    make iso >"$OUT/build.log" 2>&1 || { echo "  [FAIL] make iso (see $OUT/build.log)"; exit 1; }
fi
QEMU6=qemu-system-x86_64
QEMU9="$HOME/qemu-local/bin/qemu-system-x86_64"
cp myos.iso "$OUT/gate.iso"
boot() { # boot <name> <qemu> <timeout> args...
    local n="$1" q="$2" t="$3"; shift 3
    : > "$OUT/$n.log"
    unshare -rm bash -c 'mount -t tmpfs tmpfs /tmp; exec "$@"' _ \
        timeout "$t" "$q" "$@" -debugcon file:"$OUT/$n.log" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -m 512M -smp 2 -net none >"$OUT/$n.qemu" 2>&1
}
alive() { grep -aq "\[INIT\] /bin/init started" "$OUT/$1.log" && ! grep -aq "\[EXC\]\|PANIC" "$OUT/$1.log"; }
has() { grep -aqF -- "$2" "$OUT/$1.log"; }
ISO=(-cdrom "$OUT/gate.iso" -boot d)
BR=(-device pcie-root-port,id=rp1,chassis=1,slot=1 -device e1000e,bus=rp1
    -device pcie-root-port,id=rp2,chassis=2,slot=2 -device pcie-pci-bridge,id=pb,bus=rp2
    -device e1000,bus=pb,addr=3)
echo "[m53b] booting the guests (60 s each, in parallel)..."
boot vtd   $QEMU6 60 -machine q35 -device intel-iommu "${ISO[@]}" &
boot vtdbr $QEMU6 60 -machine q35 -device intel-iommu,intremap=on -machine kernel-irqchip=split "${BR[@]}" "${ISO[@]}" &
boot amd   $QEMU6 60 -machine q35 -device amd-iommu "${BR[@]}" "${ISO[@]}" &
boot q35   $QEMU6 60 -machine q35 "${ISO[@]}" &
boot pc    $QEMU6 60 "${ISO[@]}" &
HAVE9=0
if [[ -x "$QEMU9" ]]; then
    HAVE9=1
    boot amd9 "$QEMU9" 60 -machine q35 -device amd-iommu "${BR[@]}" "${ISO[@]}" &
fi
OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/OVMF/x64/OVMF.fd \
         /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/edk2-ovmf/OVMF_CODE.fd; do
    [[ -f "$f" ]] && { OVMF="$f"; break; }
done
HAVEUEFI=0
if [[ -n "$OVMF" ]] && { [[ $BUILD == 0 ]] || make uefi >"$OUT/uefi_build.log" 2>&1; } && [[ -f dist/kernel.elf ]]; then
    HAVEUEFI=1
    rm -rf "$OUT/esp"; cp -r dist "$OUT/esp"
    boot uefi $QEMU6 90 -machine q35 -device intel-iommu -bios "$OVMF" \
        -drive format=raw,file=fat:rw:"$OUT/esp" &
fi
wait

has vtd "[IOMMU] DMAR: 1 DRHD haw=39 flags=0x1 INTR_REMAP"; check "M53b q35+intel-iommu: DMAR with one DRHD, INTR_REMAP" $?
has vtd "[IOMMU] DRHD 0: base=0xfed90000 seg=0"; check "M53b q35+intel-iommu: DRHD register base 0xfed90000" $?
has vtd "ioapic#0=ff:00.0"; check "M53b q35+intel-iommu: IOAPIC source id ff:00.0" $?
has vtd "dev=00:01.0(display)"; check "M53b q35+intel-iommu: scopes resolved (display endpoint 00:01.0)" $?
alive vtd; check "M53b q35+intel-iommu: boots to init, no exception" $?
has vtdbr "bridge=00:02.0[bus 01-01]" && has vtdbr "bridge=00:03.0[bus 02-03]"
check "M53b q35+intel-iommu: bridge scopes resolved to their bus ranges" $?
alive vtdbr; check "M53b q35+intel-iommu (bridges): boots to init" $?
has amd "[IOMMU] IVRS: 1 IVHD primary-type=0x10" && has amd "[IOMMU] IVHD 0: type=0x10 (used) iommu=00:"
check "M53b q35+amd-iommu: IVRS with one IVHD" $?
has amd "special-ioapic#0=" && has amd "alias=03:00.0..03:1f.7->03:00.0"
check "M53b q35+amd-iommu: IOAPIC special entry + alias range behind the PCI bridge" $?
alive amd; check "M53b q35+amd-iommu: boots to init" $?
if [[ $HAVE9 == 1 ]]; then
    has amd9 "[IOMMU] IVRS: 2 IVHD primary-type=0x11" && has amd9 "type=0x10 (legacy)" && has amd9 "type=0x11 (used)"
    check "M53b QEMU 9.2 amd-iommu: IVHD 10h + 11h, 11h used" $?
    alive amd9; check "M53b QEMU 9.2 amd-iommu: boots to init" $?
fi
has q35 "[IOMMU] none" && ! has q35 "[IOMMU] DMAR" ; check "M53b plain q35: [IOMMU] none" $?
alive q35; check "M53b plain q35: boots to init" $?
has pc "[IOMMU] none"; check "M53b i440fx: [IOMMU] none" $?
alive pc; check "M53b i440fx: boots to init" $?
if [[ $HAVEUEFI == 1 ]]; then
    has uefi "[IOMMU] DMAR: 1 DRHD haw=39"; check "M53b UEFI q35+intel-iommu: DMAR found through the loader's RSDP" $?
    grep -aq "\[INIT\] /bin/init started\|\[ROOT\]\|\[SHELL\]" "$OUT/uefi.log" && ! has uefi "[EXC]"
    check "M53b UEFI q35+intel-iommu: boots, no exception" $?
else
    echo "  [SKIP] M53b UEFI boot (no OVMF or make uefi failed)"
fi
! cat "$OUT"/{vtd,vtdbr,amd,q35,pc}.log | grep -aq "\[IOMMU\] .*\(WARN\|unusable\|refused\)"
check "M53b no parser warning on any of QEMU's tables" $?
echo "[m53b] $PASS passed, $FAIL failed (logs in $OUT)"
[[ $FAIL == 0 ]]
