#!/bin/bash
# tools/pci_gate.sh — the M53a PCI registry gate.
#
# Boots the existing myos.iso (and dist/ for --uefi) on i440fx, q35,
# q35 + intel-iommu and q35 + amd-iommu with a mix of devices (virtio-blk,
# AHCI, NVMe, xHCI + keyboard + mass storage, e1000, e1000e, a PCIe root port
# and a PCIe-to-PCI bridge) and checks with tools/pci_check.py that:
#   - the kernel's boot PCI listing equals QEMU's query-pci;
#   - q35 switches config space to ECAM after "[PCI] ecam self-check OK",
#     i440fx (no MCFG) stays on CF8/CFC;
#   - the drivers still probe (AHCI, NVMe, xHCI, MSC, e1000/e1000e, virtio);
#   - no [EXC], panic or lockdep report.
#
# usage: tools/pci_gate.sh [--uefi] [--qemu BIN] [--workdir DIR]   (UEFI_ONLY=1: only the UEFI guest)
# Build first: make iso [CONSOLE=root]; make uefi (for --uefi).
# Runs in a private /tmp (unshare -rm); QEMU is stopped via QMP or its pid file.
# SPDX-License-Identifier: MIT
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ -z "${PCI_GATE_NS:-}" ]; then
    exec unshare -rm env PCI_GATE_NS=1 bash "$REPO/tools/pci_gate.sh" "$@"
fi
mount -t tmpfs tmpfs /tmp

UEFI=0; QEMU=qemu-system-x86_64; WD="$REPO/build/pci-gate"
while [ $# -gt 0 ]; do
    case "$1" in
        --uefi) UEFI=1 ;;
        --qemu) shift; QEMU=$1 ;;
        --workdir) shift; WD=$1 ;;
        *) echo "usage: $0 [--uefi] [--qemu BIN] [--workdir DIR]" >&2; exit 2 ;;
    esac
    shift
done
mkdir -p "$WD"
cd "$REPO"
[ -f myos.iso ] || { echo "[pci-gate] myos.iso missing (make iso)" >&2; exit 2; }
for d in d1 d2 d3; do truncate -s 64M "$WD/$d.img"; done

USB="-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -drive if=none,id=u1,file=$WD/d2.img,format=raw -device usb-storage,bus=xhci.0,drive=u1"
# The CD sits on ide.2 (both QEMU 6.2 and 9.2), so the SATA data disk uses ide.3.
Q35="-drive if=none,id=s1,file=$WD/d1.img,format=raw -device ide-hd,bus=ide.3,drive=s1 -drive if=none,id=n1,file=$WD/d3.img,format=raw -device nvme,serial=m53a,drive=n1 -device e1000e $USB -device pcie-root-port,id=rp1,chassis=1,slot=1 -device e1000e,bus=rp1 -device pcie-pci-bridge,id=pb1,bus=pcie.0 -device e1000,bus=pb1,addr=1"
CD="-cdrom $REPO/myos.iso -boot d"
COMMON="-m 1G -smp 2"
PASS=0; FAILN=0

run() {   # name expect-ecam probe-regex... -- qemu args
    local name=$1 ecam=$2; shift 2
    local probes=()
    while [ "$1" != "--" ]; do probes+=("$1"); shift; done
    shift
    if python3 "$REPO/tools/pci_check.py" --name "$name" --workdir "$WD" --qemu "$QEMU" \
            --expect-ecam "$ecam" -- $COMMON "$@"; then
        local ok=1
        for pr in "${probes[@]}"; do
            if ! grep -aqE "$pr" "$WD/$name.log"; then
                echo "[pci] $name: FAIL driver probe /$pr/ not in the log"; ok=0
            fi
        done
        if [ $ok = 1 ]; then PASS=$((PASS+1)); else FAILN=$((FAILN+1)); fi
    else
        FAILN=$((FAILN+1))
    fi
}

Q35PROBES=('\[AHCI\] ready' '\[NVME\] ready' '\[XHCI\] running' '\[MSC\] usb0 ready' '\[HID\] boot keyboard ready' '\[E1000E\] ready' '\[NET\] up')
[ "${UEFI_ONLY:-0}" = 1 ] || run i440fx off '\[VIRTIO-BLK\] ready' '\[XHCI\] running' '\[MSC\] usb0 ready' '\[E1000\] ready' -- \
    -machine pc $CD -drive file="$WD/d1.img",format=raw,if=virtio -device e1000 $USB
[ "${UEFI_ONLY:-0}" = 1 ] || run q35 on "${Q35PROBES[@]}" -- -machine q35 $CD $Q35
[ "${UEFI_ONLY:-0}" = 1 ] || run q35-intel-iommu on "${Q35PROBES[@]}" -- -machine q35 -device intel-iommu $CD $Q35
[ "${UEFI_ONLY:-0}" = 1 ] || run q35-amd-iommu on "${Q35PROBES[@]}" -- -machine q35 -device amd-iommu $CD $Q35
if [ $UEFI = 1 ]; then
    OVMF=/usr/share/ovmf/OVMF.fd
    [ -f "$OVMF" ] || OVMF=/usr/share/OVMF/OVMF.fd
    # vvfat writes back into its directory (NvVars, and it has been seen to
    # damage kernel.elf), so boot a fresh copy of dist/.
    rm -rf "$WD/esp"; cp -r "$REPO/dist" "$WD/esp"; rm -f "$WD/esp/NvVars"
    run q35-uefi on "${Q35PROBES[@]}" -- -machine q35 -bios "$OVMF" \
        -drive format=raw,file=fat:rw:"$WD/esp/" $Q35
fi
echo "[pci-gate] $PASS passed, $FAILN failed"
[ $FAILN = 0 ]
