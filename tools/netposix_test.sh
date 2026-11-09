#!/usr/bin/env bash
# tools/netposix_test.sh — the U10 NETPOSIX gate: the network userland for musl
# programs (docs/devlog/U10-net-posix.md).
#
#   host:  musl sysroot (tools/port-musl.sh, cached); user/netposix/nptest.c
#          built three times with tools/secos-cc: CAP_NET (nptest),
#          CAP_NET|CAP_NET_RAW (nptest_raw), default manifest without CAP_NET
#          (nptest_nonet); an ext2 test disk with them; an ISO (CONSOLE=root);
#   guest: tools/netposix_drive.py inside a private network namespace
#          (unshare -rmn: lo up, resolv.conf -> 127.0.0.1 for slirp's DNS
#          forwarder, tmpfs /tmp), so every host service is local and no real
#          network or host port is touched: TCP/UDP echo, refused ports, ICMP
#          errors, DNS (UDP + TCP fallback) through musl's resolver, NTP,
#          AF_UNIX with file permissions, epoll/eventfd/timerfd/itimer, ping
#          sockets and raw sockets (CAP_NET_RAW), iputils ping as a user and
#          as root, ntpd + hwclock, a guest TCP server, no socket leak.
#
# Options:
#   --lockdep     image built with -DSECOS_LOCKDEP (lockdep boot controls too)
#   --udrv        q35 + intel-iommu,intremap=on + e1000e with the ring-3 driver
#                 (make iso UDRV=8086:10d3, M58)
#   --nic MODEL   QEMU NIC (default e1000; --udrv implies e1000e)
#   --smp N       guest CPUs (default 2)
#   --iso FILE    use this image instead of building one
#   --keep        keep the work directory
# Output: "  [PASS] NETPOSIX ..." / "  [FAIL] NETPOSIX ..." and
# "[netposix] RESULT: N passed, M failed" (tools/selftest.sh counts them).
# SPDX-License-Identifier: MIT
set -uo pipefail
LOCKDEP=0; UDRV=0; NIC=e1000; SMP=${NETPOSIX_SMP:-2}; ISO=""; KEEP=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --lockdep) LOCKDEP=1 ;;
        --udrv) UDRV=1; NIC=e1000e ;;
        --nic) shift; NIC=$1 ;;
        --smp) shift; SMP=$1 ;;
        --iso) shift; ISO=$1 ;;
        --keep) KEEP=1 ;;
        *) echo "netposix_test: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
export PYENV_VERSION=${PYENV_VERSION:-3.11.8}
mkdir -p build
W=$(mktemp -d "$REPO/build/netposix.XXXXXX")
PASSN=0; FAILN=0
ok() { if [[ "$2" -eq 0 ]]; then echo "  [PASS] NETPOSIX $1"; PASSN=$((PASSN+1));
       else echo "  [FAIL] NETPOSIX $1"; FAILN=$((FAILN+1)); fi; }
cleanup() { if [[ $KEEP -eq 0 ]]; then rm -rf "$W"; else echo "netposix_test: artifacts kept in $W"; fi; }
trap cleanup EXIT
for t in mke2fs qemu-system-x86_64 unshare ip python3; do
    command -v $t >/dev/null || { echo "netposix_test: $t missing"; echo "[netposix] RESULT: 0 passed, 1 failed"; exit 2; }
done

# ---- host: sysroot + test programs ------------------------------------------------
need=0
[[ -f build/sysroot/lib/libc.a ]] || need=1
for f in $(find user/musl-secos tools/port-musl.sh -type f); do [[ $f -nt build/sysroot/lib/libc.a ]] && need=1; done
if [[ $need -eq 1 ]]; then bash tools/port-musl.sh >"$W/port.log" 2>&1; rc=$?; else rc=0; fi
ok "musl sysroot" $rc
[[ $rc -eq 0 ]] || { tail -20 "$W/port.log"; echo "[netposix] RESULT: $PASSN passed, $FAILN failed"; exit 1; }
D=$W/disk; mkdir -p "$D"
CC=tools/secos-cc
$CC -O2 -Wall -Wextra --secos-note=user/note_net.S -o "$D/nptest" user/netposix/nptest.c >"$W/cc.log" 2>&1 \
  && $CC -O2 -Wall -Wextra --secos-note=user/note_netraw.S -o "$D/nptest_raw" user/netposix/nptest.c >>"$W/cc.log" 2>&1 \
  && $CC -O2 -Wall -Wextra -o "$D/nptest_nonet" user/netposix/nptest.c >>"$W/cc.log" 2>&1
rc=$?
! grep -q "warning:" "$W/cc.log"; w=$?
ok "nptest builds and signs (CAP_NET, CAP_NET|CAP_NET_RAW, no CAP_NET), no warning" $(( rc | w ))
[[ $rc -eq 0 ]] || { cat "$W/cc.log"; echo "[netposix] RESULT: $PASSN passed, $FAILN failed"; exit 1; }
chmod 0755 "$D"/nptest*
mke2fs -q -t ext2 -d "$D" "$W/t.img" 32M >/dev/null 2>&1
ok "ext2 test disk" $?

# ---- the image ----------------------------------------------------------------------
if [[ -z "$ISO" ]]; then
    extra=(); [[ $LOCKDEP -eq 1 ]] && extra+=(CFLAGS_EXTRA=-DSECOS_LOCKDEP)
    [[ $UDRV -eq 1 ]] && extra+=(UDRV=8086:10d3)
    echo "[netposix] building the image (CONSOLE=root ${extra[*]:-})..."
    make clean >/dev/null 2>&1
    if ! make -j8 iso CONSOLE=root "${extra[@]}" >"$W/build.log" 2>&1; then
        echo "netposix_test: BUILD ERROR (see $W/build.log)"; tail -20 "$W/build.log"; KEEP=1
        echo "[netposix] RESULT: $PASSN passed, $((FAILN+1)) failed"; exit 1
    fi
    nw=$(grep -c "warning:" "$W/build.log")
    ok "image builds ($nw warnings)" $(( nw != 0 ))
    cp myos.iso "$W/os.iso"; ISO=$W/os.iso
    make clean >/dev/null 2>&1
fi

# ---- guest, in a private network namespace -------------------------------------------
QX=""
[[ $UDRV -eq 1 ]] && QX="-machine q35,kernel-irqchip=split -device intel-iommu,intremap=on"
args=(--iso "$ISO" --disk "$W/t.img" --smp "$SMP" --nic "$NIC" --workdir "$W" --qemu-extra "$QX")
[[ $UDRV -eq 1 ]] && args+=(--udrv)
printf 'nameserver 127.0.0.1\n' >"$W/resolv.conf"
echo "[netposix] guest (-smp $SMP, $NIC$([[ $UDRV -eq 1 ]] && echo ', ring-3 driver')$([[ $LOCKDEP -eq 1 ]] && echo ', lockdep'))..."
unshare -rmn bash -c '
    set -e
    ip link set lo up
    mount -t tmpfs tmpfs /tmp
    mount --bind "$1" /etc/resolv.conf
    shift
    exec timeout 1500 python3 -I "$0" "$@"
' "$REPO/tools/netposix_drive.py" "$W/resolv.conf" "${args[@]}" | tee "$W/drive.out"
grep -q "^RESULT: " "$W/drive.out" || { ok "guest driver finished" 1; KEEP=1; }
if [[ $LOCKDEP -eq 1 ]]; then
    grep -aq "LDTEST\] all" "$W/netposix.dbg"; ok "lockdep image: boot negative controls ran" $?
fi
if grep -q "\[FAIL\]" "$W/drive.out"; then KEEP=1; fi
dp=$(sed -nE 's/^RESULT: ([0-9]+) passed, ([0-9]+) failed/\1/p' "$W/drive.out" | tail -1)
df=$(sed -nE 's/^RESULT: ([0-9]+) passed, ([0-9]+) failed/\2/p' "$W/drive.out" | tail -1)
PASSN=$((PASSN + ${dp:-0})); FAILN=$((FAILN + ${df:-1}))
echo "[netposix] RESULT: $PASSN passed, $FAILN failed"
[[ $FAILN -eq 0 ]]
