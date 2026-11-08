#!/bin/bash
# tools/ir_gate.sh — the M56 VT-d interrupt-remapping gate (docs/PHASE5_PLAN.md
# §3 "M56", docs/devlog/M56-vtd-intremap.md).
#
# CONSOLE=root images on q35 with QEMU's intel-iommu, checked by
# tools/iommu_check.py (the M54 per-guest checker, with --ir / --irq / ps2):
#   q6-xapic      QEMU 6.2, intremap=on (xAPIC), -smp 4: the in-kernel IR
#                 tests (edu genuine MSI delivered / retargeted to an AP /
#                 source-id mismatch blocked / stale entry after
#                 irq_vec_free blocked; DMA-injected messages naming a free
#                 entry or an index past the table blocked; a 60-message
#                 injection storm), the M54 edu DMA tests, every driver's
#                 I/O (AHCI, NVMe, USB storage + keyboard, e1000e DHCP and
#                 1 MiB each way) with the e1000e MSI-X counted in
#                 /proc/interrupts, /proc/iommu "ir on";
#   q6-ps2        the same machine without USB input: PS/2 keyboard (IRQ1)
#                 and mouse (IRQ12) through the remapped IOAPIC lines, seen by
#                 /bin/guitest input;
#   q6-irqdrv     -DXHCI_USE_IRQ -DNVME_USE_IRQ: NVMe and xHCI on remapped MSI
#                 (their vectors counted), disk + USB I/O;
#   q9-x2apic     QEMU 9.2, -cpu max,+x2apic, intremap=on,eim=on, -smp 8: the
#                 same tests with EIM (32-bit destinations); QEMU 9.2 records
#                 IR faults, so the source-id (0x26) and not-present (0x22)
#                 faults and the rate-limited storm are checked in the log;
#   q9-ps2        PS/2 keyboard + mouse on x2APIC + EIM;
#   q9-irqdrv     NVMe/xHCI MSI on x2APIC + EIM;
#   smp-ipi       tools/smp_stress_drive.py --quick on an IR image, -smp 4
#                 (6.2) and -smp 8 x2APIC (9.2): IPIs / TLB shootdowns /
#                 LAPIC timer with remapping on;
#   ir-off-word   boot word iommu.ir=off: remapping stays off, DMA remapping
#                 on, every interrupt source works (the M54 behaviour);
#   no-ir-hw      intel-iommu,intremap=off (no ECAP.IR; under TCG QEMU's
#                 intremap=auto means on, so it must be off explicitly): IR off, logged,
#                 the M54 edu tests pass, the IR tests skip;
#   qi-off        iommu.qi=off: no queued invalidation, so no IR (logged);
#   default       the default image (no iommu=) on intremap=on hardware:
#                 nothing touched, all I/O as before;
#   lockdep-smp4  -DSECOS_LOCKDEP, intremap=on, -smp 4: [LDTEST] and no
#                 lockdep report with the IR tests.
#
# QEMU 6.2 and 9.2 do not implement two VT-d checks (see the devlog): a DMA
# write carries no requester id (so source-id validation of an *injected*
# message is skipped) and compatibility-format requests are delivered even
# with GSTS.CFIS = 0. The IR tests report those two as "LIMIT" lines, which
# this gate expects; real hardware blocks both.
#
# usage: tools/ir_gate.sh [--no-build] [--only g1,g2] [--qemu9 BIN] [--workdir DIR]
# Private /tmp (unshare -rm), QEMU stopped through QMP or its pid file, no
# host port forwarded. Prints "[iommu] <guest>: PASS|FAIL" per guest and
# "[ir-gate] N passed, M failed".
# SPDX-License-Identifier: MIT
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ -z "${IR_GATE_NS:-}" ]; then
    exec unshare -rm env IR_GATE_NS=1 bash "$REPO/tools/ir_gate.sh" "$@"
fi
mount -t tmpfs tmpfs /tmp

BUILD=1; WD="$REPO/build/ir-gate"; ONLY=""
Q6=qemu-system-x86_64
Q9="${SECOS_QEMU_X2APIC:-$HOME/qemu-local/bin/qemu-system-x86_64}"
while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) BUILD=0 ;;
        --qemu9) shift; Q9=$1 ;;
        --workdir) shift; WD=$1 ;;
        --only) shift; ONLY=" $(echo "$1" | tr , ' ') " ;;
        *) echo "usage: $0 [--no-build] [--only g1,g2] [--qemu9 BIN] [--workdir DIR]" >&2; exit 2 ;;
    esac
    shift
done
export PYENV_VERSION="${PYENV_VERSION:-3.11.8}"
mkdir -p "$WD"
cd "$REPO"
PASS=0; FAILN=0

build() { # build <tag> <kernel cmdline> <cflags>  -> $WD/<tag>.iso
    local tag=$1 cl=$2 cf=$3
    [ $BUILD = 1 ] || { [ -f "$WD/$tag.iso" ] && return 0; }
    echo "[ir-gate] build $tag (cmdline '$cl', CFLAGS_EXTRA '$cf')"
    make clean >/dev/null 2>&1
    if make -j8 iso CONSOLE=root KERNEL_CMDLINE="$cl" CFLAGS_EXTRA="$cf" >"$WD/build-$tag.log" 2>&1; then
        cp myos.iso "$WD/$tag.iso"
    else
        echo "[iommu] build-$tag: FAIL (see $WD/build-$tag.log)"; FAILN=$((FAILN+1)); return 1
    fi
}

disks() { for d in s n u; do rm -f "$WD/$1-$d.img"; truncate -s 64M "$WD/$1-$d.img"; done; }
# devs <name> [ps2]: edu with a 32-bit DMA mask (its DMA must reach the MSI
# window 0xFEExxxxx), AHCI data disk, NVMe, xHCI + usb-storage (+ usb-kbd
# unless ps2), e1000e.
devs() {
    local kbd="-device usb-kbd,bus=xhci.0"
    [ "${2:-}" = ps2 ] && kbd=""
    echo "-device edu,dma_mask=0xffffffff" \
         "-drive if=none,id=s1,file=$WD/$1-s.img,format=raw -device ide-hd,bus=ide.3,drive=s1" \
         "-drive if=none,id=n1,file=$WD/$1-n.img,format=raw -device nvme,serial=m56,drive=n1" \
         "-device qemu-xhci,id=xhci $kbd" \
         "-drive if=none,id=u1,file=$WD/$1-u.img,format=raw -device usb-storage,bus=xhci.0,drive=u1" \
         "-netdev user,id=n0 -device e1000e,netdev=n0"
}
selected() { [ -z "$ONLY" ] || [[ "$ONLY" == *" $1 "* ]]; }
guest() { # guest <name> <qemu> <iso> <ps2|usb> <checker args...> -- <machine args...>
    local name=$1 q=$2 iso=$3 kind=$4; shift 4
    selected "$name" || return 0
    [ -f "$WD/$iso" ] || { echo "[iommu] $name: FAIL (no image $iso)"; FAILN=$((FAILN+1)); return 0; }
    local chk=()
    while [ "$1" != "--" ]; do chk+=("$1"); shift; done
    shift
    disks "$name"
    if python3 "$REPO/tools/iommu_check.py" --name "$name" --workdir "$WD" --qemu "$q" \
            --disk "sata=$WD/$name-s.img" --disk "nvme=$WD/$name-n.img" --disk "usb=$WD/$name-u.img" \
            "${chk[@]}" -- "$@" -m 1G -cdrom "$WD/$iso" -boot d $(devs "$name" "$kind") >"$WD/$name.out" 2>&1; then
        PASS=$((PASS+1))
    else
        FAILN=$((FAILN+1))
    fi
    grep -E ": (PASS|FAIL)$| FAIL " "$WD/$name.out"
}
M6="-machine q35,kernel-irqchip=split -device intel-iommu,intremap=on"
M9="-machine q35,kernel-irqchip=split -cpu max,+x2apic -device intel-iommu,intremap=on,eim=on,aw-bits=48"
ON='\[IOMMU\] on: vt-d, [0-9]+ function\(s\) in the kernel passthrough domain, 0 not translated, 0 failed'
IRX='\[IOMMU\] IR: on: 1 unit\(s\), 256 entries, xapic, ioapic lines 2, compatibility format blocked'
IR2='\[IOMMU\] IR: on: 1 unit\(s\), 256 entries, x2apic eim, ioapic lines 2, compatibility format blocked'
LIM=(--expect '\[IR-TEST\] ok dma-free-entry-blocked' --expect '\[IR-TEST\] ok dma-index-over-blocked'
     --expect '\[IR-TEST\] (ok|LIMIT) dma-foreign-entry' --expect '\[IR-TEST\] (ok|LIMIT) dma-compat')
PROBES=(--expect '\[AHCI\] ready' --expect '\[NVME\] ready' --expect '\[XHCI\] running'
        --expect '\[MSC\] usb0 ready' --expect '\[E1000E\] ready' --expect '\[DHCP\] lease ip='
        --expect '\[NET\] MSI-X NAPI armed')
IRF=(--expect '\[IOMMU\] IR fault bdf=00:[0-9a-f]{2}\.0 index=0x[0-9a-f]+ reason=0x26 \(source id check failed\)'
     --expect '\[IOMMU\] IR fault bdf=00:[0-9a-f]{2}\.0 index=0x[0-9a-f]+ reason=0x22 \(remapping entry not present\)'
     --expect '\[IOMMU\] [0-9]+ IR fault message\(s\) suppressed \(rate limit\)'
     --expect '\[IR-TEST\] ok ir-storm-rate-limited')

# ---------------------------------------------------------------- images
build ir "iommu=on iommu.testloops=10" "-DSECOS_IOMMU_TEST"
build irirq "iommu=on iommu.testloops=10" "-DSECOS_IOMMU_TEST -DXHCI_USE_IRQ -DNVME_USE_IRQ"
build iroff "iommu=on iommu.ir=off iommu.testloops=10" "-DSECOS_IOMMU_TEST"
build qioff "iommu=on iommu.qi=off iommu.testloops=10" "-DSECOS_IOMMU_TEST"
build lockdep "iommu=on iommu.testloops=10" "-DSECOS_IOMMU_TEST -DSECOS_LOCKDEP"
build default "" ""

# ---------------------------------------------------------------- guests
guest q6-xapic "$Q6" ir.iso usb --edu --ir --io net,hid,proc --irq net --expect "$ON" --expect "$IRX" \
    "${LIM[@]}" "${PROBES[@]}" --expect '\[HID\] boot keyboard ready' -- $M6 -smp 4
guest q6-ps2 "$Q6" ir.iso ps2 --edu --ir --io ps2,net --irq net --expect "$IRX" "${PROBES[@]}" -- $M6 -smp 2
guest q6-irqdrv "$Q6" irirq.iso usb --edu --ir --io net,hid --irq net --irq nvme --irq xhci --expect "$IRX" \
    "${PROBES[@]}" -- $M6 -smp 2
if [ -x "$Q9" ]; then
    guest q9-x2apic "$Q9" ir.iso usb --edu --ir --io net,hid,proc --irq net --expect "$ON" --expect "$IR2" \
        --expect 'mode=x2apic' "${LIM[@]}" "${IRF[@]}" "${PROBES[@]}" -- $M9 -smp 8
    guest q9-ps2 "$Q9" ir.iso ps2 --edu --ir --io ps2 --expect "$IR2" -- $M9 -smp 4
    guest q9-irqdrv "$Q9" irirq.iso usb --edu --ir --io net,hid --irq net --irq nvme --irq xhci --expect "$IR2" \
        "${PROBES[@]}" -- $M9 -smp 4
else
    echo "[ir-gate] SKIP QEMU 9.2 guests ($Q9 not found)"
fi
guest ir-off-word "$Q6" iroff.iso usb --edu --ir-off --io net,hid --irq net --expect "$ON" \
    --expect '\[IOMMU\] IR: off: boot word iommu.ir=off' --expect '\[IR-TEST\] interrupt remapping off: skipped' \
    "${PROBES[@]}" -- $M6 -smp 2
guest no-ir-hw "$Q6" ir.iso usb --edu --ir-off --io net,hid --irq net --expect "$ON" \
    --expect '\[IOMMU\] IR: off: a DMA-remapping unit has no interrupt remapping' \
    --expect '\[IR-TEST\] interrupt remapping off: skipped' "${PROBES[@]}" -- -machine q35 -device intel-iommu,intremap=off -smp 2
guest qi-off "$Q6" qioff.iso usb --edu --ir-off --io net,hid --expect 'register invalidation' \
    --expect '\[IOMMU\] IR: off: queued invalidation off' "${PROBES[@]}" -- $M6 -smp 2
guest default "$Q6" default.iso usb --faults -1 --io net,hid --irq net --absent '\[IOMMU\] (IR|vt-d)' \
    --expect '\[IOMMU\] DMA remapping off \(default' "${PROBES[@]}" -- $M6 -smp 2
guest lockdep-smp4 "$Q6" lockdep.iso usb --edu --ir --io net,hid --irq net --timeout 900 --expect "$IRX" \
    --expect '\[LDTEST\] all 0x0*A/0x0*A' "${PROBES[@]}" -- $M6 -smp 4

# IPIs, TLB shootdowns and the LAPIC timer with remapping on.
ssd() { # ssd <name> <qemu> <smp> <machine args...>
    local name=$1 q=$2 n=$3; shift 3
    selected "$name" || return 0
    timeout 1500 python3 "$REPO/tools/smp_stress_drive.py" --iso "$WD/ir.iso" --smp "$n" --quick \
        --qemu "$q" --qemu-pre "$*" --log "$WD/$name.log" >"$WD/$name.out" 2>&1
    local rc=$?
    if [ $rc = 0 ] && grep -q '^\[SSD\] RESULT ' "$WD/$name.out" && grep -qE "IR: on: " "$WD/$name.log"; then
        echo "[iommu] $name: PASS"; PASS=$((PASS+1))
    else
        echo "[iommu] $name: FAIL (rc $rc; $WD/$name.out)"; grep -a FAIL "$WD/$name.out" | head -5
        FAILN=$((FAILN+1))
    fi
}
ssd smp-ipi-q6 "$Q6" 4 $M6
if [ -x "$Q9" ]; then
    ssd smp-ipi-q9 "$Q9" 8 $M9
fi

make clean >/dev/null 2>&1
echo "[ir-gate] $PASS passed, $FAILN failed"
[ $FAILN = 0 ]
