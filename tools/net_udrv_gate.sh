#!/bin/bash
# tools/net_udrv_gate.sh — the M58 gate: the e1000e NIC driven from ring 3
# (/sbin/udrv-e1000e, user/udrv/e1000e.c) through the kernel network proxy
# (net/net_udrv.c). docs/PHASE5_PLAN.md §3 "M58", docs/devlog/M58-udrv-e1000e.md.
#
# Images (CONSOLE=root, a throwaway SSH key baked in):
#   udrv     make iso UDRV=8086:10d3   (cmdline iommu=on udrv=8086:10d3)
#   kern     KERNEL_CMDLINE=iommu=on   (the same kernel, no udrv=: fallback)
#   lockdep  udrv + -DSECOS_LOCKDEP
# Guests (q35 + intel-iommu,intremap=on + e1000e, QEMU user networking;
# tools/net_udrv_check.py unless noted):
#   udrv-smp2      DHCP, 8 MiB download + 4 MiB upload with MD5, DNS, SSH,
#                  perf numbers, 20 x kill -9 of the driver during a
#                  download loop (respawn, traffic resumes, MemFree+Slab and
#                  the ring-3 page counters steady);
#   udrv-smp4      the same at -smp 4 with 5 kills;
#   kern-smp2      fallback: the in-kernel e1000e, same data checks + perf;
#   netstress-smp2 / netstress-smp4  tools/net_stress_drive.py (the NETSMP
#                  workload: TCP echo/firehose, UDP floods, DNS, a server
#                  with resets, parallel ssh) on the ring-3 driver;
#   lockdep-smp4   the udrv-smp4 checks on the lockdep image, plus
#                  netstress on it.
# usage: tools/net_udrv_gate.sh [--no-build] [--only g1,g2] [--workdir DIR]
# Private /tmp (unshare -rm); host ports are kernel-assigned (never
# 2222/8000); QEMU stopped through QMP / its pid file / its own parent.
# Prints "[nud] <guest>: PASS|FAIL" per guest and "[nud-gate] N passed, M failed".
# SPDX-License-Identifier: MIT
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ -z "${NUD_GATE_NS:-}" ]; then
    exec unshare -rm env NUD_GATE_NS=1 bash "$REPO/tools/net_udrv_gate.sh" "$@"
fi
mount -t tmpfs tmpfs /tmp

BUILD=1; WD="$REPO/build/net-udrv-gate"; ONLY=""
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
[ -f "$WD/id" ] || ssh-keygen -q -t ed25519 -N '' -C secos-m58-gate -f "$WD/id"

build() { # build <tag> <make args...>
    local tag=$1; shift
    [ $BUILD = 1 ] || { [ -f "$WD/$tag.iso" ] && return 0; }
    echo "[nud-gate] build $tag ($*)"
    make clean >/dev/null 2>&1
    if make -j8 iso CONSOLE=root SSH_AUTHORIZED_KEYS="$WD/id.pub" "$@" >"$WD/build-$tag.log" 2>&1; then
        cp myos.iso "$WD/$tag.iso"
        echo "[nud-gate] build $tag: $(grep -c 'warning:' "$WD/build-$tag.log") warning(s)"
    else
        echo "[nud] build-$tag: FAIL (see $WD/build-$tag.log)"; FAILN=$((FAILN+1)); return 1
    fi
}

M="-machine q35,kernel-irqchip=split -device intel-iommu,intremap=on -m 1G"

guest() { # guest <name> <iso> <smp> <checker args...>
    local name=$1 iso=$2 smp=$3; shift 3
    selected "$name" || return 0
    if timeout 1800 python3 tools/net_udrv_check.py --name "$name" --workdir "$WD" --key "$WD/id" "$@" -- \
            $M -smp "$smp" -cdrom "$WD/$iso" -boot d >"$WD/$name.out" 2>&1; then
        PASS=$((PASS+1))
    else
        FAILN=$((FAILN+1))
    fi
    grep -E ": (PASS|FAIL)$| FAIL |perf |kill-loop" "$WD/$name.out"
}

stress() { # stress <name> <iso> <smp>
    local name=$1 iso=$2 smp=$3
    selected "$name" || return 0
    timeout 900 python3 tools/net_stress_drive.py --iso "$WD/$iso" --smp "$smp" --nic e1000e \
        --qemu-extra "$M" --key "$WD/id" --log "$WD/$name.log" --out "$WD/$name.out" >/dev/null 2>&1
    local bad=0
    grep -q "^\[NSD\] RESULT PASS" "$WD/$name.out" || bad=1
    grep -aq "\[LOCKDEP\]\|PANIC\|\[EXC\]" "$WD/$name.log" && bad=1
    grep -aq "\[UDRV\] eth0 driver attached" "$WD/$name.log" || bad=1
    grep -E "FAIL|RESULT" "$WD/$name.out" | cut -c1-300
    if [ $bad = 0 ]; then echo "[nud] $name: PASS"; PASS=$((PASS+1)); else echo "[nud] $name: FAIL"; FAILN=$((FAILN+1)); fi
}

if selected udrv-smp2 || selected udrv-smp4 || selected netstress-smp2 || selected netstress-smp4; then
    build udrv UDRV=8086:10d3
fi
selected kern-smp2 && build kern KERNEL_CMDLINE=iommu=on
selected lockdep-smp4 && build lockdep UDRV=8086:10d3 CFLAGS_EXTRA=-DSECOS_LOCKDEP

guest udrv-smp2 udrv.iso 2 --mode udrv --kills 20 --perf \
    --expect '\[UDRV\] eth0 driver attached pid=\d+ mac=' \
    --expect '\[init\] started net0 \(/sbin/udrv-e1000e\)'
guest udrv-smp4 udrv.iso 4 --mode udrv --kills 5
guest kern-smp2 kern.iso 2 --mode kernel --perf --expect '\[NET\] MSI-X NAPI armed'
stress netstress-smp2 udrv.iso 2
stress netstress-smp4 udrv.iso 4
if selected lockdep-smp4; then
    guest lockdep-smp4 lockdep.iso 4 --mode udrv --kills 5 --timeout 600 --expect '\[LDTEST\] all 0x0*A/0x0*A'
    ONLY_SAVE=$ONLY; ONLY=""
    stress lockdep-netstress-smp4 lockdep.iso 4
    ONLY=$ONLY_SAVE
fi

make clean >/dev/null 2>&1
echo "[nud-gate] $PASS passed, $FAILN failed"
[ $FAILN = 0 ]
