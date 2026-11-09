#!/bin/bash
# tools/udrv_gate.sh — the M57 Driver Space v2 gate, positive checks
# (docs/PHASE5_PLAN.md §3 "M57", docs/devlog/M57-udrv-core.md).
#
# Builds CONSOLE=root images and boots them on q35 with QEMU's edu device at
# 00:04.0, driven by /bin/udrv_edu (user/udrv_edu.c) and checked by
# tools/udrv_check.py:
#   run          q35 + intel-iommu, `iommu=on udrv=00:04.0 udrv.noir`
#                (interrupt remapping is M56): udrv_edu passes (claim, INFO,
#                BAR 0 id/liveness/factorial, 10000 MSI round trips through
#                the claim fd + one through poll(), 2 KiB edu DMA both ways
#                through the device's own IOMMU domain); then 8 more runs,
#                every other one exiting without RELEASE while it holds
#                DMA, the BAR and an armed vector: free IRQ vectors, IOMMU
#                domains, VT-d table pages and ring-3 DMA/BAR pages back to
#                their level exactly, MemFree+Slab and heap in use within the
#                usual slack; /proc/udrv (device free, claims counted, 0
#                faults) and /proc/iommu (device blocked, 0 faults);
#   service      the same image with a fresh ext4 root (tools/mkroot.sh) whose
#                /etc/inittab (written with debugfs) runs
#                `udrv_edu --serve` as a respawn service: it passes and holds
#                a claim; kill -9 from the shell three times, each time the
#                claim is released and init restarts it, the new instance
#                passes again; resources after the last restart equal those
#                after the first;
#   nommu        plain q35, no IOMMU: the claim is MMIO only (DMA_ALLOC
#                -ENODEV, bus mastering and IRQ_SETUP -EPERM), the BAR works;
#   default      an image without udrv= on q35 + intel-iommu + edu: no [UDRV]
#                line, /proc/udrv empty, edu left to nobody;
#   noir         q35 + intel-iommu without udrv.noir (no interrupt
#                remapping before M56): the claim is MMIO only, bus mastering
#                and DMA refused for that reason;
#   unsafe       plain q35 with udrv.unsafe: DMA without an IOMMU works and
#                the kernel reports the taint;
#   lockdep-smp4 the "run" guest on a -DSECOS_LOCKDEP image at -smp 4;
#   bkl-on       the "run" guest with bkl=on.
# Every guest also runs the SYS_UDRV argument-validation table
# (/bin/udrv_negtest --env <environment> and /bin/udrv_user, user/udrv_negtest.c):
# every row must return its expected errno, and the device is free and nothing
# is held afterwards.
#
# usage: tools/udrv_gate.sh [--no-build] [--only g1,g2] [--workdir DIR]
# Runs in a private /tmp (unshare -rm); QEMU is stopped through QMP or its
# pid file; no host port is forwarded. Prints "[udrv] <guest>: PASS|FAIL"
# per guest and "[udrv-gate] N passed, M failed".
# SPDX-License-Identifier: MIT
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ -z "${UDRV_GATE_NS:-}" ]; then
    exec unshare -rm env UDRV_GATE_NS=1 bash "$REPO/tools/udrv_gate.sh" "$@"
fi
mount -t tmpfs tmpfs /tmp

BUILD=1; WD="$REPO/build/udrv-gate"; ONLY=""
Q6=qemu-system-x86_64
while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) BUILD=0 ;;
        --workdir) shift; WD=$1 ;;
        --only) shift; ONLY=" $(echo "$1" | tr , ' ') " ;;
        *) echo "usage: $0 [--no-build] [--only g1,g2] [--workdir DIR]" >&2; exit 2 ;;
    esac
    shift
done
export PYENV_VERSION="${PYENV_VERSION:-3.11.8}"
mkdir -p "$WD"
cd "$REPO"
PASS=0; FAILN=0
selected() { [ -z "$ONLY" ] || [[ "$ONLY" == *" $1 "* ]]; }

build() { # build <tag> <kernel cmdline> <cflags>  -> $WD/<tag>.iso
    local tag=$1 cl=$2 cf=$3
    [ $BUILD = 1 ] || { [ -f "$WD/$tag.iso" ] && return 0; }
    echo "[udrv-gate] build $tag (cmdline '$cl', CFLAGS_EXTRA '$cf')"
    make clean >/dev/null 2>&1
    if make -j8 iso CONSOLE=root KERNEL_CMDLINE="$cl" CFLAGS_EXTRA="$cf" >"$WD/build-$tag.log" 2>&1; then
        cp myos.iso "$WD/$tag.iso"
    else
        echo "[udrv] build-$tag: FAIL (see $WD/build-$tag.log)"; FAILN=$((FAILN+1)); return 1
    fi
}

guest() { # guest <name> <iso> <checker args...> -- <machine args...>
    local name=$1 iso=$2; shift 2
    selected "$name" || return 0
    local chk=()
    while [ "$1" != "--" ]; do chk+=("$1"); shift; done
    shift
    if python3 "$REPO/tools/udrv_check.py" --name "$name" --workdir "$WD" --qemu "$Q6" "${chk[@]}" -- \
            "$@" -m 1G -cdrom "$WD/$iso" -boot d -device edu,addr=04.0 >"$WD/$name.out" 2>&1; then
        PASS=$((PASS+1))
    else
        FAILN=$((FAILN+1))
    fi
    grep -E ": (PASS|FAIL)$| FAIL " "$WD/$name.out"
}

# A fresh persistent root whose /etc/inittab runs udrv_edu as a service.
mkroot_service() {
    local img=$WD/service-root.img
    rm -f "$img"
    tools/mkroot.sh "$img" 256M >"$WD/mkroot.log" 2>&1 || return 1
    printf '%s\n' '# udrv gate (tools/udrv_gate.sh): a root console and the edu driver service' \
        'console:console:/bin/bash -i' \
        'edu:respawn:/bin/udrv_edu -n 1000 --serve' >"$WD/inittab"
    printf 'cd /etc\nwrite %s inittab\nsif /etc/inittab uid 0\nsif /etc/inittab gid 0\nsif /etc/inittab mode 0100644\n' \
        "$WD/inittab" >"$WD/debugfs.cmds"
    debugfs -w -f "$WD/debugfs.cmds" "$img" >>"$WD/mkroot.log" 2>&1 || return 1
    debugfs -R 'cat /etc/inittab' "$img" 2>/dev/null | grep -q 'udrv_edu -n 1000 --serve'
}

UD="iommu=on udrv=00:04.0 udrv.noir"
# ---------------------------------------------------------------- images
if selected run || selected service || selected nommu; then build udrv "$UD" ""; fi
selected default && build default "" ""
selected noir && build noir "iommu=on udrv=00:04.0" ""
selected unsafe && build unsafe "udrv=00:04.0 udrv.unsafe" ""
selected lockdep-smp4 && build lockdep "$UD" "-DSECOS_LOCKDEP"
selected bkl-on && build bklon "$UD bkl=on" ""

# ---------------------------------------------------------------- guests
guest run udrv.iso --mode run --rounds 10000 --leak-runs 8 --negtest translated \
    --expect '\[UDRV\] 1 device\(s\) for ring-3 drivers; iommu=on ir=off udrv\.noir' -- \
    -machine q35 -device intel-iommu -smp 2
if selected service; then
    if mkroot_service; then
        guest service udrv.iso --mode service --kills 3 --expect '\[ROOT\] persistent root fs=ext4 dev=vda' -- \
            -machine q35 -device intel-iommu -smp 2 -drive file="$WD/service-root.img",format=raw,if=virtio
    else
        echo "[udrv] service: FAIL (root image, see $WD/mkroot.log)"; FAILN=$((FAILN+1))
    fi
fi
guest nommu udrv.iso --mode nommu --negtest nommu \
    --expect '\[IOMMU\] iommu=on: off \(no DMA-remapping hardware\)' -- -machine q35 -smp 2
guest default default.iso --mode default --negtest nodev --expect '\[IOMMU\] DMA remapping off \(default' -- \
    -machine q35 -device intel-iommu -smp 2
guest noir noir.iso --mode negtest --negtest noir \
    --expect '\[UDRV\] 1 device\(s\) for ring-3 drivers; iommu=on ir=off\n' -- -machine q35 -device intel-iommu -smp 2
guest unsafe unsafe.iso --mode negtest --negtest unsafe \
    --expect '\[UDRV\] \*\*\* WARNING: udrv\.unsafe' -- -machine q35 -smp 2
guest lockdep-smp4 lockdep.iso --mode run --rounds 2000 --leak-runs 4 --timeout 900 --negtest translated \
    --expect '\[LDTEST\] all 0x0*A/0x0*A' -- -machine q35 -device intel-iommu -smp 4
guest bkl-on bklon.iso --mode run --rounds 10000 --leak-runs 4 --negtest translated \
    --expect '\[BKL\] kernel lock mode: bkl=on' -- -machine q35 -device intel-iommu -smp 2

make clean >/dev/null 2>&1
echo "[udrv-gate] $PASS passed, $FAILN failed"
[ $FAILN = 0 ]
