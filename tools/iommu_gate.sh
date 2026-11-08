#!/bin/bash
# tools/iommu_gate.sh — the M54 VT-d DMA-remapping gate (docs/PHASE5_PLAN.md
# §3 "M54", docs/devlog/M54-vtd-core.md).
#
# Builds CONSOLE=root images and boots them on q35 with QEMU's intel-iommu
# (every guest also gets an AHCI data disk, NVMe, xHCI + usb-kbd +
# usb-storage and an e1000e on user networking), checked by
# tools/iommu_check.py:
#   default    iommu=off image on q35 + intel-iommu: remapping stays off,
#              drivers and I/O as before;
#   main       iommu=on + the in-kernel edu tests (positive, unmapped,
#              unmap-then-DMA, read-only, DMA mask, blocked, passthrough,
#              a 1000-round map/unmap loop with PMM free frames steady, a
#              rate-limited fault storm), every driver does real I/O in the
#              passthrough domain (disk read/write checked from the host,
#              1 MiB each way over TCP, USB keyboard input), /proc/iommu
#              shows only the edu faults;
#   matrix     aw-bits=48, caching-mode=on, QEMU pt=off (identity tables),
#              kernel iommu.pt=off, iommu.qi=off (register invalidation);
#   negative   -DSECOS_IOMMU_TEST_NOFLUSH (unmap without IOTLB flush) must
#              be caught by the edu test; iommu.block=00:1f.2 (the q35
#              built-in AHCI left blocked) faults instead of reading;
#   no IOMMU   iommu=on on plain q35 and on q35 + amd-iommu: logged, the
#              system boots as usual;
#   lockdep    -DSECOS_LOCKDEP, -smp 4;
#   UEFI       OVMF q35 + intel-iommu, `iommu=on` in the load options;
#   QEMU 9.2   (if --qemu9 or ~/qemu-local exists) x2APIC + intremap/eim,
#              aw-bits 48 + caching mode;
#   fs         ext4 root on the q35 AHCI disk + FAT32 data disk under
#              fsstress (tools/fs_stress_drive.py), e2fsck clean, 0 faults.
#
# usage: tools/iommu_gate.sh [--quick] [--no-build] [--only g1,g2] [--qemu9 BIN] [--workdir DIR]
#   --quick: main + default + one negative (for a fast check).
#   --no-build: reuse the images in the work directory; --only: just these
#   guests (names as printed; the fsstress run is "fsstress-ahci-root").
# Runs in a private /tmp (unshare -rm); QEMU is stopped through QMP or its pid
# file; no host port is forwarded (the network peers are random 127.0.0.1
# ports the guest reaches as 10.0.2.2). Prints "[iommu] <guest>: PASS|FAIL"
# per guest and "[iommu-gate] N passed, M failed".
# SPDX-License-Identifier: MIT
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ -z "${IOMMU_GATE_NS:-}" ]; then
    exec unshare -rm env IOMMU_GATE_NS=1 bash "$REPO/tools/iommu_gate.sh" "$@"
fi
mount -t tmpfs tmpfs /tmp

QUICK=0; BUILD=1; WD="$REPO/build/iommu-gate"; ONLY=""
Q6=qemu-system-x86_64
Q9="${SECOS_QEMU_X2APIC:-$HOME/qemu-local/bin/qemu-system-x86_64}"
while [ $# -gt 0 ]; do
    case "$1" in
        --quick) QUICK=1 ;;
        --no-build) BUILD=0 ;;
        --qemu9) shift; Q9=$1 ;;
        --workdir) shift; WD=$1 ;;
        --only) shift; ONLY=" $(echo "$1" | tr , ' ') " ;;
        *) echo "usage: $0 [--quick] [--no-build] [--only g1,g2] [--qemu9 BIN] [--workdir DIR]" >&2; exit 2 ;;
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
    echo "[iommu-gate] build $tag (cmdline '$cl', CFLAGS_EXTRA '$cf')"
    make clean >/dev/null 2>&1
    if make -j8 iso CONSOLE=root KERNEL_CMDLINE="$cl" CFLAGS_EXTRA="$cf" >"$WD/build-$tag.log" 2>&1; then
        cp myos.iso "$WD/$tag.iso"
    else
        echo "[iommu] build-$tag: FAIL (see $WD/build-$tag.log)"; FAILN=$((FAILN+1)); return 1
    fi
}

disks() { for d in s n u; do rm -f "$WD/$1-$d.img"; truncate -s 64M "$WD/$1-$d.img"; done; }
devs() { # devs <name>: the device set every guest gets
    echo "-device edu" \
         "-drive if=none,id=s1,file=$WD/$1-s.img,format=raw -device ide-hd,bus=ide.3,drive=s1" \
         "-drive if=none,id=n1,file=$WD/$1-n.img,format=raw -device nvme,serial=m54,drive=n1" \
         "-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0" \
         "-drive if=none,id=u1,file=$WD/$1-u.img,format=raw -device usb-storage,bus=xhci.0,drive=u1" \
         "-netdev user,id=n0 -device e1000e,netdev=n0"
}
IO="--io net,hid,proc"
selected() { [ -z "$ONLY" ] || [[ "$ONLY" == *" $1 "* ]]; }
guest() { # guest <name> <qemu> <iso|-> <checker args...> -- <machine args...>
    local name=$1 q=$2 iso=$3; shift 3
    selected "$name" || return 0
    local chk=()
    while [ "$1" != "--" ]; do chk+=("$1"); shift; done
    shift
    disks "$name"
    local cd=()
    [ "$iso" != "-" ] && cd=(-cdrom "$WD/$iso" -boot d)
    if python3 "$REPO/tools/iommu_check.py" --name "$name" --workdir "$WD" --qemu "$q" \
            --disk "${SATA:-sata}=$WD/$name-s.img" --disk "nvme=$WD/$name-n.img" --disk "usb=$WD/$name-u.img" \
            "${chk[@]}" -- "$@" -m 1G "${cd[@]}" $(devs "$name") >"$WD/$name.out" 2>&1; then
        PASS=$((PASS+1))
    else
        FAILN=$((FAILN+1))
    fi
    grep -E ": (PASS|FAIL)$| FAIL " "$WD/$name.out"
}
ON='\[IOMMU\] on: vt-d, [0-9]+ function\(s\) in the kernel passthrough domain, 0 not translated, 0 failed'
PROBES=(--expect '\[AHCI\] ready' --expect '\[NVME\] ready' --expect '\[XHCI\] running'
        --expect '\[MSC\] usb0 ready' --expect '\[HID\] boot keyboard ready' --expect '\[E1000E\] ready'
        --expect '\[DHCP\] lease ip=')

# ---------------------------------------------------------------- images
build iommu "iommu=on" "-DSECOS_IOMMU_TEST"
build iommu100 "iommu=on iommu.testloops=100" "-DSECOS_IOMMU_TEST"
if [ $QUICK = 0 ]; then
    build ptoff "iommu=on iommu.pt=off iommu.testloops=100" "-DSECOS_IOMMU_TEST"
    build qioff "iommu=on iommu.qi=off iommu.testloops=100" "-DSECOS_IOMMU_TEST"
    build blkahci "iommu=on iommu.block=00:1f.2" "-DSECOS_IOMMU_TEST"
    build lockdep "iommu=on iommu.testloops=100" "-DSECOS_IOMMU_TEST -DSECOS_LOCKDEP"
fi
build noflush "iommu=on iommu.testloops=10" "-DSECOS_IOMMU_TEST -DSECOS_IOMMU_TEST_NOFLUSH"
build default "" ""

# ---------------------------------------------------------------- guests
guest default "$Q6" default.iso $IO --proc-off --faults -1 --expect '\[IOMMU\] DMA remapping off \(default' \
    --absent '\[IOMMU\] vt-d' "${PROBES[@]}" -- -machine q35 -device intel-iommu -smp 2
guest main "$Q6" iommu.iso --edu $IO --expect "$ON" "${PROBES[@]}" -- \
    -machine q35 -device intel-iommu -smp 2
# Negative control: without the IOTLB flush the "unmap then DMA" test fails.
guest noflush "$Q6" noflush.iso --faults -1 --no-prompt --timeout 240 \
    --expect '\[IOMMU-TEST\] FAIL dma-after-unmap-faults' --expect '\[IOMMU-TEST\] RESULT pass=[0-9]+ fail=[1-9]' -- \
    -machine q35 -device intel-iommu -smp 2
if [ $QUICK = 0 ]; then
    guest aw48-cm "$Q6" iommu100.iso --edu $IO --expect "$ON" --expect 'vt-d0: sagaw 0x6 .* CM' \
        --expect '4-level tables, iova bits 48' "${PROBES[@]}" -- \
        -machine q35 -device intel-iommu,aw-bits=48,caching-mode=on -smp 2
    guest cm-qemu-ptoff "$Q6" iommu100.iso --edu $IO --expect 'no-PT' \
        --expect 'kernel domain identity tables' "${PROBES[@]}" -- \
        -machine q35 -device intel-iommu,caching-mode=on,pt=off -smp 2
    guest kernel-ptoff "$Q6" ptoff.iso --edu $IO --expect 'kernel domain identity tables \(iommu.pt=off\)' \
        "${PROBES[@]}" -- -machine q35 -device intel-iommu,aw-bits=48 -smp 2
    guest qioff-cm "$Q6" qioff.iso --edu $IO --expect 'register invalidation' "${PROBES[@]}" -- \
        -machine q35 -device intel-iommu,caching-mode=on -smp 2
    # The built-in q35 AHCI (00:1f.2) left blocked: its DMA must fault (so it
    # really is behind the IOMMU) and the kernel must survive it.
    guest block-ahci "$Q6" blkahci.iso --faults -1 --no-prompt --timeout 240 \
        --expect 'iommu.block: leaving 00:1f.2 blocked' \
        --expect '\[IOMMU\] DMA fault bdf=00:1f.2 iova=0x[0-9a-f]+ reason=0x2' \
        --expect '\[NVME\] ready' -- -machine q35 -device intel-iommu -smp 2
    guest no-iommu "$Q6" iommu.iso --faults -1 $IO --proc-off \
        --expect 'iommu=on: off \(no DMA-remapping hardware\), continuing' "${PROBES[@]}" -- -machine q35 -smp 2
    guest amd-iommu "$Q6" iommu.iso --faults -1 $IO --proc-off \
        --expect 'iommu=on: off \(AMD-Vi: not supported before M64\)' "${PROBES[@]}" -- \
        -machine q35 -device amd-iommu -smp 2
    guest lockdep-smp4 "$Q6" lockdep.iso --edu $IO --timeout 900 --expect "$ON" --expect '\[LDTEST\] all 0x0*A/0x0*A' \
        "${PROBES[@]}" -- -machine q35 -device intel-iommu,caching-mode=on -smp 4
    if [ -x "$Q9" ]; then
        guest q9-x2apic "$Q9" iommu100.iso --edu $IO --expect "$ON" --expect 'mode=x2apic' "${PROBES[@]}" -- \
            -machine q35,kernel-irqchip=split -cpu max,+x2apic -device intel-iommu,intremap=on,eim=on,aw-bits=48,caching-mode=on -smp 4
        guest q9-aw39 "$Q9" iommu100.iso --edu $IO --expect "$ON" "${PROBES[@]}" -- \
            -machine q35 -device intel-iommu -smp 2
    else
        echo "[iommu-gate] SKIP QEMU 9.2 guests ($Q9 not found)"
    fi
    # UEFI/OVMF: the loader passes the RSDP; iommu=on comes from the load
    # options (startup.nsh). The test image's kernel goes on the ESP.
    OVMF=""
    for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd; do [ -f "$f" ] && { OVMF=$f; break; }; done
    if [ -n "$OVMF" ] && selected uefi; then
        make clean >/dev/null 2>&1
        if make -j8 kernel.bin CONSOLE=root CFLAGS_EXTRA="-DSECOS_IOMMU_TEST" >"$WD/build-uefi.log" 2>&1 &&
           make uefi >>"$WD/build-uefi.log" 2>&1; then
            rm -rf "$WD/esp"; mkdir -p "$WD/esp/EFI/SECOS"
            cp dist/EFI/BOOT/BOOTX64.EFI "$WD/esp/EFI/SECOS/SECOS.EFI"; cp dist/kernel.elf "$WD/esp/"
            printf 'fs0:\r\n\\EFI\\SECOS\\SECOS.EFI iommu=on iommu.testloops=100\r\n' > "$WD/esp/startup.nsh"
            # The ESP (vvfat, AHCI port 0) is sda, so the data disk is sdb.
            SATA=sata:/dev/sdb guest uefi "$Q6" - --edu $IO --timeout 900 --expect "$ON" --expect 'by UEFI loader' "${PROBES[@]}" -- \
                -machine q35 -bios "$OVMF" -drive format=raw,file=fat:rw:"$WD/esp/" -device intel-iommu -smp 2
        else
            echo "[iommu] uefi: FAIL (build, see $WD/build-uefi.log)"; FAILN=$((FAILN+1))
        fi
    else
        echo "[iommu-gate] SKIP UEFI guest (no OVMF)"
    fi
    # ext4 root on the q35 AHCI disk (translated, passthrough domain) +
    # FAT32 data disk under fsstress; host e2fsck / fsck.vfat clean.
    selected fsstress-ahci-root && {
    python3 "$REPO/tools/fs_stress_drive.py" --iso "$WD/iommu100.iso" --smp 2 --data vfat \
        --rounds 1 --qemu-pre "-device intel-iommu,caching-mode=on" --work "$WD/fsd" >"$WD/fsd.out" 2>&1
    fsd_rc=$?
    nf=$(cat "$WD"/fsd/*.log 2>/dev/null | grep -ac '\[IOMMU\] DMA fault')
    on=$(cat "$WD"/fsd/*.log 2>/dev/null | grep -acE "$ON")
    if [ $fsd_rc = 0 ] && [ "$nf" = 0 ] && [ "$on" -ge 1 ]; then
        echo "[iommu] fsstress-ahci-root: PASS"; PASS=$((PASS+1))
    else
        echo "[iommu] fsstress-ahci-root: FAIL (rc $fsd_rc, $nf DMA faults, iommu-on lines $on; $WD/fsd.out)"
        grep -a "FAIL" "$WD/fsd.out" | head -5
        FAILN=$((FAILN+1))
    fi
    }
fi
make clean >/dev/null 2>&1
echo "[iommu-gate] $PASS passed, $FAILN failed"
[ $FAILN = 0 ]
