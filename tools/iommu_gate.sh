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
# [M55] With iommu=on the kernel drivers are in the strict kernel DMA domain
# (kernel/dma.h): every guest above checks it (STRICT_E), plus
#   dma-scan       tools/dma_scan.sh (no kvirt_to_phys for DMA outside dma.c);
#   dma-neg        -DSECOS_DMA_TEST: AHCI PRDs at .text / a heap page fault,
#                  .text unchanged, nothing leaks to disk, I/O still works;
#   kernel-pt      iommu.kernel=pt (the passthrough fallback);
#   e1000-strict   the legacy e1000 in the strict domain;
#   dmadebug-*     -DSECOS_DMA_DEBUG on i440fx (AHCI/NVMe/xHCI/e1000e, and
#                  virtio-blk/e1000) and on q35 strict.
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
         "-netdev user,id=n0 -device ${NIC:-e1000e},netdev=n0"
}
# [M55] i440fx (no IOMMU, -DSECOS_DMA_DEBUG): the same set with an added AHCI.
devs_pc() {
    echo "-device edu -device ahci,id=ahci0" \
         "-drive if=none,id=s1,file=$WD/$1-s.img,format=raw -device ide-hd,bus=ahci0.0,drive=s1" \
         "-drive if=none,id=n1,file=$WD/$1-n.img,format=raw -device nvme,serial=m55,drive=n1" \
         "-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0" \
         "-drive if=none,id=u1,file=$WD/$1-u.img,format=raw -device usb-storage,bus=xhci.0,drive=u1" \
         "-netdev user,id=n0 -device ${NIC:-e1000e},netdev=n0"
}
# [M55] i440fx with the legacy virtio-blk disk (AHCI/NVMe are then not probed).
devs_vblk() {
    echo "-drive if=none,id=s1,file=$WD/$1-s.img,format=raw -device virtio-blk-pci,drive=s1" \
         "-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0" \
         "-drive if=none,id=u1,file=$WD/$1-u.img,format=raw -device usb-storage,bus=xhci.0,drive=u1" \
         "-netdev user,id=n0 -device ${NIC:-e1000},netdev=n0"
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
    local dk=(--disk "${SATA:-sata}=$WD/$name-s.img")
    [ "${NONVME:-0}" = 1 ] || dk+=(--disk "nvme=$WD/$name-n.img")
    dk+=(--disk "usb=$WD/$name-u.img")
    if python3 "$REPO/tools/iommu_check.py" --name "$name" --workdir "$WD" --qemu "$q" \
            "${dk[@]}" "${chk[@]}" -- "$@" -m 1G "${cd[@]}" $(${DEVS:-devs} "$name") >"$WD/$name.out" 2>&1; then
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
# [M55] The strict kernel DMA domain (the default with iommu=on): built, its
# self-test passed, every driven function in it (AHCI 8086:2922, NVMe
# 1b36:0010, xHCI 1b36:000d; e1000e 8086:10d3 / e1000 8086:100e), no driver
# handed a non-DMA address to its device, /proc/iommu agrees. The guests'
# fault count (0, or exactly the edu tests' faults) is checked as before.
DMADEV='\[DMA\] [0-9a-f]{2}:[0-9a-f]{2}\.[0-7] '
STRICT=(--expect '\[DMA\] kernel DMA domain: strict' --expect '\[DMA\] strict domain self-test PASS'
        --expect "${DMADEV}8086:2922: strict kernel DMA domain" --expect "${DMADEV}1b36:0010: strict kernel DMA domain"
        --expect "${DMADEV}1b36:000d: strict kernel DMA domain" --absent '\[DMA\] BUG'
        --absent 'attach to the strict domain FAILED' --proc-expect 'dma mode strict .* violations 0')
STRICT_E=("${STRICT[@]}" --expect "${DMADEV}8086:10d3: strict kernel DMA domain")

# [M55] Static check: no driver turns a kernel pointer into a bus address
# outside kernel/dma.c (tools/dma_scan.sh).
if selected dma-scan; then
    if bash "$REPO/tools/dma_scan.sh" >"$WD/dma-scan.out" 2>&1; then
        echo "[iommu] dma-scan: PASS"; PASS=$((PASS+1))
    else
        echo "[iommu] dma-scan: FAIL ($WD/dma-scan.out)"; grep FAIL "$WD/dma-scan.out" | head -5; FAILN=$((FAILN+1))
    fi
fi

# ---------------------------------------------------------------- images
build iommu "iommu=on" "-DSECOS_IOMMU_TEST"
build iommu100 "iommu=on iommu.testloops=100" "-DSECOS_IOMMU_TEST"
if [ $QUICK = 0 ]; then
    build ptoff "iommu=on iommu.pt=off iommu.testloops=100" "-DSECOS_IOMMU_TEST"
    build qioff "iommu=on iommu.qi=off iommu.testloops=100" "-DSECOS_IOMMU_TEST"
    build blkahci "iommu=on iommu.block=00:1f.2" "-DSECOS_IOMMU_TEST"
    build lockdep "iommu=on iommu.testloops=100" "-DSECOS_IOMMU_TEST -DSECOS_LOCKDEP"
    build kpt "iommu=on iommu.kernel=pt iommu.testloops=100" "-DSECOS_IOMMU_TEST"     # [M55]
    build dmadbg "" "-DSECOS_DMA_DEBUG"                                                # [M55]
    build dmadbgs "iommu=on iommu.testloops=10" "-DSECOS_DMA_DEBUG -DSECOS_IOMMU_TEST" # [M55]
fi
build dmaneg "iommu=on" "-DSECOS_DMA_TEST"                                             # [M55]
build noflush "iommu=on iommu.testloops=10" "-DSECOS_IOMMU_TEST -DSECOS_IOMMU_TEST_NOFLUSH"
build default "" ""

# ---------------------------------------------------------------- guests
guest default "$Q6" default.iso $IO --proc-off --faults -1 --expect '\[IOMMU\] DMA remapping off \(default' \
    --absent '\[IOMMU\] vt-d' "${PROBES[@]}" -- -machine q35 -device intel-iommu -smp 2
guest main "$Q6" iommu.iso --edu $IO --expect "$ON" "${PROBES[@]}" "${STRICT_E[@]}" -- \
    -machine q35 -device intel-iommu -smp 2
# [M55] Negative: an AHCI PRD aimed at .text, then at a heap page (both outside
# .dmabss + coherent memory) must fault in the strict domain and fail the
# command (QEMU's AHCI then completes it with the data dropped); .text stays
# bit-identical (boot measurement), the heap bytes never reach the disk, and
# every driver still does its I/O afterwards (exactly those 2 faults).
guest dma-neg "$Q6" dmaneg.iso $IO --faults 2 "${PROBES[@]}" "${STRICT_E[@]}" \
    --expect '\[DMA-TEST\] ahci read into \.text phys=0x[0-9A-Fa-f]+ \(command [a-z]+\), IOMMU write fault seen, \.text hash unchanged' \
    --expect '\[DMA-TEST\] ahci write from a heap page phys=0x[0-9A-Fa-f]+ \(command [a-z]+\), IOMMU read fault seen, heap bytes not on disk' \
    --expect '\[DMA-TEST\] ahci read after the faults ok' --expect '\[DMA-TEST\] RESULT pass=0x0*3 fail=0x0*0( |\n|$)' \
    --expect '\[IOMMU\] DMA fault bdf=00:1f.2 iova=0x[0-9a-f]+ reason=0x[0-9a-f]+ \([^)]*\) W' \
    --expect '\[IOMMU\] DMA fault bdf=00:1f.2 iova=0x[0-9a-f]+ reason=0x[0-9a-f]+ \([^)]*\) R' -- \
    -machine q35 -device intel-iommu -smp 2
# Negative control: without the IOTLB flush the "unmap then DMA" test fails.
guest noflush "$Q6" noflush.iso --faults -1 --no-prompt --timeout 240 \
    --expect '\[IOMMU-TEST\] FAIL dma-after-unmap-faults' --expect '\[IOMMU-TEST\] RESULT pass=[0-9]+ fail=[1-9]' -- \
    -machine q35 -device intel-iommu -smp 2
if [ $QUICK = 0 ]; then
    guest aw48-cm "$Q6" iommu100.iso --edu $IO --expect "$ON" --expect 'vt-d0: sagaw 0x6 .* CM' \
        --expect '4-level tables, iova bits 48' "${PROBES[@]}" "${STRICT_E[@]}" -- \
        -machine q35 -device intel-iommu,aw-bits=48,caching-mode=on -smp 2
    guest cm-qemu-ptoff "$Q6" iommu100.iso --edu $IO --expect 'no-PT' \
        --expect 'kernel domain identity tables' "${PROBES[@]}" "${STRICT_E[@]}" -- \
        -machine q35 -device intel-iommu,caching-mode=on,pt=off -smp 2
    guest kernel-ptoff "$Q6" ptoff.iso --edu $IO --expect 'kernel domain identity tables \(iommu.pt=off\)' \
        "${PROBES[@]}" "${STRICT_E[@]}" -- -machine q35 -device intel-iommu,aw-bits=48 -smp 2
    guest qioff-cm "$Q6" qioff.iso --edu $IO --expect 'register invalidation' "${PROBES[@]}" "${STRICT_E[@]}" -- \
        -machine q35 -device intel-iommu,caching-mode=on -smp 2
    # [M55] The fallback boot word: kernel drivers stay in passthrough.
    guest kernel-pt "$Q6" kpt.iso --edu $IO --expect "$ON" "${PROBES[@]}" \
        --expect '\[DMA\] kernel DMA domain: passthrough \(iommu.kernel=pt\)' --absent 'strict kernel DMA domain' \
        --proc-expect 'dma mode pt ' -- -machine q35 -device intel-iommu -smp 2
    # [M55] The legacy e1000 (8086:100e, no MSI) in the strict domain.
    NIC=e1000 guest e1000-strict "$Q6" iommu100.iso --edu $IO --expect "$ON" "${STRICT[@]}" \
        --expect "${DMADEV}8086:100e: strict kernel DMA domain" --expect '\[E1000\] ready' \
        --expect '\[AHCI\] ready' --expect '\[NVME\] ready' --expect '\[MSC\] usb0 ready' \
        --expect '\[DHCP\] lease ip=' -- -machine q35 -device intel-iommu -smp 2
    # [M55] -DSECOS_DMA_DEBUG with no IOMMU (i440fx): any address outside
    # .dmabss/coherent memory handed to a device panics. Every driver does
    # its I/O: AHCI + NVMe + xHCI/HID/MSC + e1000e, then virtio-blk + e1000.
    DEVS=devs_pc guest dmadebug-pc "$Q6" dmadbg.iso $IO --proc-off --faults -1 "${PROBES[@]}" \
        --expect '\(SECOS_DMA_DEBUG: violations panic\)' --absent '\[DMA\] BUG' -- -machine pc -smp 2
    DEVS=devs_vblk NONVME=1 SATA=sata:/dev/vda guest dmadebug-vblk "$Q6" dmadbg.iso $IO --proc-off --faults -1 \
        --expect '\(SECOS_DMA_DEBUG: violations panic\)' --absent '\[DMA\] BUG' --expect '\[VIRTIO-BLK\] ready' \
        --expect '\[E1000\] ready' --expect '\[MSC\] usb0 ready' --expect '\[HID\] boot keyboard ready' \
        --expect '\[DHCP\] lease ip=' -- -machine pc -smp 2
    # [M55] -DSECOS_DMA_DEBUG and the strict domain together.
    guest dmadebug-strict "$Q6" dmadbgs.iso --edu $IO --expect "$ON" "${PROBES[@]}" "${STRICT_E[@]}" \
        --expect '\(SECOS_DMA_DEBUG: violations panic\)' -- -machine q35 -device intel-iommu,caching-mode=on -smp 2
    # The built-in q35 AHCI (00:1f.2) left blocked: its DMA must fault (so it
    # really is behind the IOMMU) and the kernel must survive it.
    guest block-ahci "$Q6" blkahci.iso --faults -1 --no-prompt --timeout 240 \
        --expect 'iommu.block: leaving 00:1f.2 blocked' \
        --expect '\[IOMMU\] DMA fault bdf=00:1f.2 iova=0x[0-9a-f]+ reason=0x2' \
        --expect "${DMADEV}8086:2922: not in the kernel passthrough domain" \
        --expect '\[NVME\] ready' -- -machine q35 -device intel-iommu -smp 2
    guest no-iommu "$Q6" iommu.iso --faults -1 $IO --proc-off \
        --expect 'iommu=on: off \(no DMA-remapping hardware\), continuing' "${PROBES[@]}" -- -machine q35 -smp 2
    guest amd-iommu "$Q6" iommu.iso --faults -1 $IO --proc-off \
        --expect 'iommu=on: off \(AMD-Vi: not supported before M64\)' "${PROBES[@]}" -- \
        -machine q35 -device amd-iommu -smp 2
    guest lockdep-smp4 "$Q6" lockdep.iso --edu $IO --timeout 900 --expect "$ON" --expect '\[LDTEST\] all 0x0*A/0x0*A' \
        "${PROBES[@]}" "${STRICT_E[@]}" -- -machine q35 -device intel-iommu,caching-mode=on -smp 4
    if [ -x "$Q9" ]; then
        guest q9-x2apic "$Q9" iommu100.iso --edu $IO --expect "$ON" --expect 'mode=x2apic' "${PROBES[@]}" "${STRICT_E[@]}" -- \
            -machine q35,kernel-irqchip=split -cpu max,+x2apic -device intel-iommu,intremap=on,eim=on,aw-bits=48,caching-mode=on -smp 4
        guest q9-aw39 "$Q9" iommu100.iso --edu $IO --expect "$ON" "${PROBES[@]}" "${STRICT_E[@]}" -- \
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
            SATA=sata:/dev/sdb guest uefi "$Q6" - --edu $IO --timeout 900 --expect "$ON" --expect 'by UEFI loader' "${PROBES[@]}" "${STRICT_E[@]}" -- \
                -machine q35 -bios "$OVMF" -drive format=raw,file=fat:rw:"$WD/esp/" -device intel-iommu -smp 2
        else
            echo "[iommu] uefi: FAIL (build, see $WD/build-uefi.log)"; FAILN=$((FAILN+1))
        fi
    else
        echo "[iommu-gate] SKIP UEFI guest (no OVMF)"
    fi
    # ext4 root on the q35 AHCI disk (translated; [M55] strict kernel DMA
    # domain) + FAT32 data disk under fsstress; host e2fsck / fsck.vfat clean.
    selected fsstress-ahci-root && {
    python3 "$REPO/tools/fs_stress_drive.py" --iso "$WD/iommu100.iso" --smp 2 --data vfat \
        --rounds 1 --qemu-pre "-device intel-iommu,caching-mode=on" --work "$WD/fsd" >"$WD/fsd.out" 2>&1
    fsd_rc=$?
    nf=$(cat "$WD"/fsd/*.log 2>/dev/null | grep -ac '\[IOMMU\] DMA fault')
    on=$(cat "$WD"/fsd/*.log 2>/dev/null | grep -acE "$ON")
    st=$(cat "$WD"/fsd/*.log 2>/dev/null | grep -acE "${DMADEV}8086:2922: strict kernel DMA domain")
    bug=$(cat "$WD"/fsd/*.log 2>/dev/null | grep -ac '\[DMA\] BUG')
    if [ $fsd_rc = 0 ] && [ "$nf" = 0 ] && [ "$on" -ge 1 ] && [ "$st" -ge 1 ] && [ "$bug" = 0 ]; then
        echo "[iommu] fsstress-ahci-root: PASS"; PASS=$((PASS+1))
    else
        echo "[iommu] fsstress-ahci-root: FAIL (rc $fsd_rc, $nf DMA faults, iommu-on lines $on, strict AHCI lines $st, DMA BUG lines $bug; $WD/fsd.out)"
        grep -a "FAIL" "$WD/fsd.out" | head -5
        FAILN=$((FAILN+1))
    fi
    }
fi
make clean >/dev/null 2>&1
echo "[iommu-gate] $PASS passed, $FAILN failed"
[ $FAILN = 0 ]
