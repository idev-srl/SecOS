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
#   A. M4 isolation selftest reports 20/20 PASS.
#   B. M7 ring-3 cooperative scheduling: two ring-3 processes alternate via
#      SYS_YIELD (both 1->2 and 2->1 switches observed, no CPU exception).

# No errexit: a failing assertion must be COUNTED by check() (with -e the first
# failing grep/[[ ]] silently aborted the run before reaching check/RESULT).
# Build failures still exit 2 explicitly.
set -uo pipefail

TIMEOUT=12
while [[ $# -gt 0 ]]; do
    case "$1" in
        --timeout) shift; TIMEOUT=$1 ;;
        -h|--help) sed -n '2,/^set /p' "$0" | grep '^#' | sed 's/^# \?//'; exit 0 ;;
        *) echo "[selftest] unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

# [M43] The gates drive a root bash on the console over serial: build every
# image with the pre-login console policy (make CONSOLE=root). A gate that
# tests the console login itself passes CONSOLE=login on its make line.
export CONSOLE=root

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# [M48] SMP is on by default, so the gates run multicore: a qemu-system-x86_64
# shim (first in PATH, so the Python drivers get it too) adds
# -smp $SELFTEST_SMP to every QEMU started without an explicit -smp. A gate
# whose demo needs one CPU (strict ordering of two cooperating tasks) passes
# -smp 1 itself, or sets QEMU_SMP=1 for a helper (tools/smoke.sh) that starts
# QEMU. SELFTEST_SMP=1 runs the whole suite single-core.
SELFTEST_SMP=${SELFTEST_SMP:-2}
if [[ "$SELFTEST_SMP" != 1 ]]; then
    QSHIM=$(mktemp -d /tmp/secos_selftest_qemu.XXXXXX)
    REALQ=$(command -v qemu-system-x86_64)
    cat > "$QSHIM/qemu-system-x86_64" <<EOF
#!/bin/bash
for a in "\$@"; do [[ "\$a" == -smp ]] && exec "$REALQ" "\$@"; done
exec "$REALQ" -smp "\${QEMU_SMP:-$SELFTEST_SMP}" "\$@"
EOF
    chmod +x "$QSHIM/qemu-system-x86_64"
    export PATH="$QSHIM:$PATH"
    trap 'rm -rf "$QSHIM"' EXIT
    echo "[selftest] QEMU guests run with -smp $SELFTEST_SMP unless a gate says otherwise"
fi

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
# [M48] One CPU: the M7 demo is two tasks handing one CPU back and forth with
# SYS_YIELD; with a second CPU each would simply run on its own.
if ! QEMU_SMP=1 tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$LOG" >/dev/null 2>&1; then
    echo "[selftest] FAIL — kernel did not stay alive (triple fault / crash)"
    FAIL=$((FAIL+1))
fi

echo "[selftest] Assertions:"

# A. M4 isolation selftest 20/20 (12 base + 8 identity-alias cases)
grep -q "20/20 PASS" "$LOG"; check "M4 isolation selftest 20/20" $?

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
    timeout "$((TIMEOUT+6))" qemu-system-x86_64 \
        -cdrom myos.iso \
        -drive file=disk.img,if=virtio,format=raw -boot d \
        -debugcon file:"$log" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -no-shutdown -display none -m 256M
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

# ---- M11: Driver Space (signed manifest -> proc_type -> capability boundary) ----
M11LOG=/tmp/secos_selftest_m11.log
echo "[selftest] Building M11 image (M11_DRIVER_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make CFLAGS_EXTRA=-DM11_DRIVER_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M11 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running M11 (mb2, ${TIMEOUT}s)..."
tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$M11LOG" >/dev/null 2>&1 || true

# Driver: the signed manifest marks it PROC_TYPE_DRIVER and grants dev0 caps 0x13.
grep -q "\[M11\] driver bound dev=0x0000000000000000 caps=0x0000000000000013" "$M11LOG"; check "M11 driver bound from signed manifest (dev0 caps=0x13)" $?
grep -q "\[M11\] spawned driver proc_type=0x0000000000000001" "$M11LOG"; check "M11 driver runs as PROC_TYPE_DRIVER" $?
# Granted mediated HW access works: written register value round-trips on read.
grep -q "\[driver\] READ_REG ret=0 val=0xcafef00dd15ea5ed" "$M11LOG"; check "M11 driver mediated reg read/write round-trips (granted)" $?
# Un-granted capability (MAP_MEM) is refused even though the device supports it.
grep -q "\[driver\] MAP_MEM ret=-1" "$M11LOG"; check "M11 un-granted capability (MAP_MEM) refused (DRV_ERR_PERM)" $?
# A plain user process has no driver rights: every SYS_DRIVER call -> DRV_ERR_NOTDRV.
grep -q "\[M11\] spawned userprobe proc_type=0x0000000000000000" "$M11LOG"; check "M11 user probe runs as PROC_TYPE_USER" $?
grep -q "\[user\] GET_INFO ret=-7" "$M11LOG"; check "M11 user process denied SYS_DRIVER (DRV_ERR_NOTDRV)" $?
n_audit=$(grep -c "\[DRV-AUDIT\]" "$M11LOG" || true)
[[ "$n_audit" -ge 8 ]]; check "M11 driver calls audited (audit=$n_audit)" $?
grep -q "\[M11\] DONE" "$M11LOG"; check "M11 demo completed ([M11] DONE)" $?
! grep -q "\[EXC\]" "$M11LOG"; check "no CPU exception ([EXC]) during M11 run" $?

# ---- M12: memory scalability + W^X hard gate ----
M12LOG=/tmp/secos_selftest_m12.log
echo "[selftest] Building M12 image (default; W^X + heap selftests run at boot)..."
make clean >/dev/null 2>&1 || true
if ! make iso >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M12 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running M12 (mb2, -m 2G, ${TIMEOUT}s)..."
: > "$M12LOG"
timeout "$TIMEOUT" qemu-system-x86_64 -cdrom myos.iso \
    -debugcon file:"$M12LOG" -global isa-debugcon.iobase=0xe9 \
    -no-reboot -display none -m 2G
# PMM manages all RAM on a 2 GB VM (was clamped to ~512 MB before M12).
freehex=$(grep -oE "free_MB=0x[0-9A-Fa-f]+" "$M12LOG" | head -1 | sed 's/free_MB=0x//')
freedec=$(( 16#${freehex:-0} ))
[[ "$freedec" -gt 512 ]]; check "M12 PMM reports >512 MB free on a 2 GB VM (free=${freedec} MB)" $?
# W^X hard gate: a W+X mapping request is refused by vmm_map.
grep -q "\[WX\] W+X mapping rejected (good)" "$M12LOG"; check "M12 W^X gate rejects a W+X mapping" $?
# Heap serves a multi-frame allocation correctly (the M11 >4 KB gotcha).
grep -q "\[HEAP\] large kmalloc(64K) OK" "$M12LOG"; check "M12 heap serves large (64K) kmalloc" $?
! grep -q "\[EXC\]" "$M12LOG"; check "no CPU exception ([EXC]) during M12 2G boot" $?

# ---- M13: usability & policy (manifest max_mem, IPC channels, getticks) ----
M13LOG=/tmp/secos_selftest_m13.log
echo "[selftest] Building M13 image (M13_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make CFLAGS_EXTRA=-DM13_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M13 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running M13 (mb2, ${TIMEOUT}s)..."
tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$M13LOG" >/dev/null 2>&1 || true

# Manifest memory-limit enforcement: an over-limit signed program is refused at load.
grep -q "\[M13\] max_mem program REFUSED at load (good)" "$M13LOG"; check "M13 manifest max_mem refused at load (headline)" $?
# New syscall SYS_GETTICKS returns a non-zero uptime (the producer prints it).
grep -qE "\[ipc_send\] sent=10 ticks=[1-9]" "$M13LOG"; check "M13 SYS_GETTICKS returns uptime + IPC send" $?
# Minimal IPC: the consumer receives the producer's message over kernel channel 0.
grep -q "M13-IPC-OK" "$M13LOG"; check "M13 IPC message delivered consumer<-producer (channel 0)" $?
grep -q "\[M13\] DONE" "$M13LOG"; check "M13 demo completed ([M13] DONE)" $?
! grep -q "\[EXC\]" "$M13LOG"; check "no CPU exception ([EXC]) during M13 run" $?

# ---- M14: demand paging (lazy page materialization on #PF) ----
M14LOG=/tmp/secos_selftest_m14.log
echo "[selftest] Building M14 image (M14_DEMAND_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make CFLAGS_EXTRA=-DM14_DEMAND_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M14 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running M14 (mb2, ${TIMEOUT}s)..."
tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$M14LOG" >/dev/null 2>&1 || true

# Headline: a non-zero reserved footprint but ZERO pages mapped at load (no eager mapping).
grep -qE "reserved footprint=0x0*[1-9A-F][0-9A-F]* mapped at load=0x0000000000000000" "$M14LOG"; check "M14 lazy load: footprint reserved, 0 pages mapped at load (headline)" $?
# Pages materialize on first touch via the #PF handler.
n_pf=$(grep -c "\[PF\] demand page" "$M14LOG"); [[ "$n_pf" -ge 1 ]]; check "M14 pages materialize on demand ([PF] demand page x$n_pf)" $?
# The lazily-paged program actually ran (code+stack faulted in) and printed.
grep -q "signed SecOS user program running in ring 3" "$M14LOG"; check "M14 demand-paged program runs ring-3" $?
grep -q "\[M14\] DONE" "$M14LOG"; check "M14 demo completed ([M14] DONE)" $?
! grep -q "\[EXC\]" "$M14LOG"; check "no CPU exception ([EXC]) during M14 run" $?

# ---- M15: fault-driven process kill (kernel survives a ring-3 fault) ----
M15LOG=/tmp/secos_selftest_m15.log
echo "[selftest] Building M15 image (M15_KILL_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make CFLAGS_EXTRA=-DM15_KILL_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M15 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running M15 (mb2, ${TIMEOUT}s)..."
tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$M15LOG" >/dev/null 2>&1 || true

# The faulting program ran up to the wild write...
grep -q "\[crash\] about to fault" "$M15LOG"; check "M15 faulting program reached the fault" $?
# ...and was terminated BEFORE its post-fault line (the fault killed it).
! grep -q "STILL ALIVE" "$M15LOG"; check "M15 killed process never continued past the fault" $?
# The kernel reports the kill of the offending ring-3 process.
grep -q "\[KILL\] pid=" "$M15LOG"; check "M15 kernel killed the faulting ring-3 process ([KILL])" $?
# And the kernel SURVIVED: a subsequently-spawned program runs normally.
grep -q "signed SecOS user program running in ring 3" "$M15LOG"; check "M15 kernel survives: later program runs ring-3" $?
grep -q "\[M15\] DONE" "$M15LOG"; check "M15 demo completed ([M15] DONE)" $?

# ---- M16: exec model (argv + blocking wait + exit status) ----
M16LOG=/tmp/secos_selftest_m16.log
echo "[selftest] Building M16 image (M16_EXEC_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make CFLAGS_EXTRA=-DM16_EXEC_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M16 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running M16 (mb2, ${TIMEOUT}s)..."
tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$M16LOG" >/dev/null 2>&1 || true

# argv reached the child (argv[0]+alpha+beta => argc=3).
grep -q "\[child\] argc=3" "$M16LOG"; check "M16 argv delivered to child (argc=3)" $?
# Blocking SYS_WAIT returned the child's exit status (== argc == 3).
grep -q "\[parent\] child status=3" "$M16LOG"; check "M16 blocking wait returns child exit status (3)" $?
grep -q "\[M16\] DONE" "$M16LOG"; check "M16 demo completed ([M16] DONE)" $?
! grep -q "\[EXC\]" "$M16LOG"; check "no CPU exception ([EXC]) during M16 run" $?

# ---- M17: blocking primitives (sleep + blocking recv) ----
M17LOG=/tmp/secos_selftest_m17.log
echo "[selftest] Building M17 image (M17_BLOCK_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make CFLAGS_EXTRA=-DM17_BLOCK_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M17 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running M17 (mb2, ${TIMEOUT}s)..."
tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$M17LOG" >/dev/null 2>&1 || true

# The receiver actually BLOCKED on the empty channel (state PROC_BLOCKED)...
grep -q "\[M17\] consumer is BLOCKED on recv" "$M17LOG"; check "M17 SYS_MSG_RECV blocks on empty channel" $?
# ...and a later send WOKE it, delivering the message.
grep -q "\[ipc_recv\] got: " "$M17LOG"; check "M17 send wakes the blocked receiver" $?
# Blocking SYS_SLEEP really slept (uptime advanced by >= the requested ticks).
grep -q "\[sleep\] OK slept >=10" "$M17LOG"; check "M17 SYS_SLEEP blocks for the requested ticks" $?
grep -q "\[M17\] DONE" "$M17LOG"; check "M17 demo completed ([M17] DONE)" $?
! grep -q "\[EXC\]" "$M17LOG"; check "no CPU exception ([EXC]) during M17 run" $?

# ---- M18: dynamic memory (malloc/free over sbrk, mmap, mprotect, W^X) ----
M18LOG=/tmp/secos_selftest_m18.log
echo "[selftest] Building M18 image (M18_MEM_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make CFLAGS_EXTRA=-DM18_MEM_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M18 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running M18 (mb2, ${TIMEOUT}s)..."
tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$M18LOG" >/dev/null 2>&1 || true

# malloc/free over sbrk (multi-page, demand-faulted) + free-list reuse.
grep -q "\[m18\] malloc rw OK" "$M18LOG"; check "M18 malloc/sbrk multi-page read-write" $?
grep -q "\[m18\] free+reuse OK" "$M18LOG"; check "M18 free + malloc reuses the block" $?
# anonymous mmap read-write.
grep -q "\[m18\] mmap rw OK" "$M18LOG"; check "M18 anonymous mmap read-write" $?
# W^X enforced on both mmap and mprotect.
grep -q "\[m18\] mmap W+X refused OK" "$M18LOG"; check "M18 mmap W+X rejected (W^X)" $?
grep -q "\[m18\] mprotect W+X refused OK" "$M18LOG"; check "M18 mprotect W+X rejected (W^X)" $?
# mprotect to read-only is enforced: read still works, write is fatal.
grep -q "\[m18\] read-after-RO OK" "$M18LOG"; check "M18 read works after mprotect read-only" $?
! grep -q "STILL ALIVE after RO write" "$M18LOG"; check "M18 write to RO page is fatal (mprotect enforced)" $?
grep -q "\[KILL\] pid=" "$M18LOG"; check "M18 RO write terminates the process ([KILL])" $?
grep -q "\[M18\] DONE" "$M18LOG"; check "M18 demo completed ([M18] DONE)" $?

# ---- M19: copy-on-write fork ----
M19LOG=/tmp/secos_selftest_m19.log
echo "[selftest] Building M19 image (M19_FORK_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make CFLAGS_EXTRA=-DM19_FORK_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M19 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running M19 (mb2, ${TIMEOUT}s)..."
tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$M19LOG" >/dev/null 2>&1 || true

# fork() created a child...
grep -q "\[FORK\] parent=" "$M19LOG"; check "M19 fork creates a child process" $?
# ...the child inherited the parent's memory (COW read) ...
grep -q "\[m19\] child inherited P OK" "$M19LOG"; check "M19 child inherits parent memory" $?
# ...and a write triggered a private copy.
grep -q "\[m19\] child wrote C OK" "$M19LOG"; check "M19 child write copies-on-write" $?
# COW isolation: the parent's buffer is untouched by the child's write.
grep -q "\[m19\] parent buf isolated OK" "$M19LOG"; check "M19 COW isolation (parent unchanged)" $?
# Blocking wait returns the child's exit status (7).
grep -q "\[m19\] parent: child status=7" "$M19LOG"; check "M19 parent reads child exit status (7)" $?
grep -q "\[M19\] DONE" "$M19LOG"; check "M19 demo completed ([M19] DONE)" $?
! grep -q "\[EXC\]" "$M19LOG"; check "no CPU exception ([EXC]) during M19 run" $?

# ---- execve: in-place image replacement (SYS_EXECVE) ----
# user/exectest.c (installed as /bin/exectest with tampered/unsigned/script
# twins) checks the failure paths (caller continues), what a successful exec
# keeps (pid, parent link, argv/envp, plain fds, mask, pending signals) and
# drops (FD_CLOEXEC fds, caught handlers), and 200 fork+exec cycles with a
# stable PMM free count.
EXLOG=/tmp/secos_selftest_execve.log
echo "[selftest] Building execve image (EXECVE_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make CFLAGS_EXTRA=-DEXECVE_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] EXECVE BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running execve (mb2, 45s)..."
tools/smoke.sh --mb2 --timeout 45 --log "$EXLOG" >/dev/null 2>&1 || true
for t in "nonexistent path -> ENOENT, caller continues" \
         "tampered ELF refused (EACCES), caller continues" \
         "unsigned ELF refused (EACCES), caller continues" \
         "non-ELF file -> ENOEXEC, caller continues" \
         "directory -> EACCES" \
         "bad path pointer -> EFAULT" "bad argv pointer -> EFAULT" \
         "bad argv\[1\] string -> EFAULT" "kernel envp pointer -> EFAULT" \
         "pid unchanged after failed execs" \
         "fcntl F_SETFD/F_GETFD" "fcntl F_DUPFD honours the minimum fd" "pipe2(O_CLOEXEC)" \
         "argv delivered to new image" "pid unchanged across exec" \
         "envp delivered (getenv)" "envp delivered to main" \
         "plain fd survives exec" "O_CLOEXEC fd closed by exec" \
         "FD_CLOEXEC (F_SETFD) fd closed by exec" "pipe2(O_CLOEXEC) end closed by exec" \
         "pre-exec child collected by the new image" \
         "caught handler reset to SIG_DFL" "ignored signal stays ignored" \
         "signal mask preserved" "pending signal preserved across exec" \
         "200 fork+exec cycles all exit 0" \
         "no memory leak across 200 fork+exec (|delta| <= 64 kB)"; do
    grep -q "\[EXECVE\] $t OK" "$EXLOG"; check "execve: $t" $?
done
grep -q "\[EXEC\] pid=0x0000000000000001 entry=" "$EXLOG"; check "execve: in-place exec kept pid 1 ([EXEC] marker)" $?
! grep -q "\[EXECVE\] .* FAIL" "$EXLOG"; check "execve: no FAIL marker" $?
grep -q "\[EXECVE\] DONE-USER" "$EXLOG" && grep -q "\[EXECVE\] DONE$" "$EXLOG"
check "execve demo completed ([EXECVE] DONE)" $?
! grep -q "\[EXC\]" "$EXLOG"; check "no CPU exception ([EXC]) during execve run" $?

# ---- M20: unified page cache + file-backed mmap ----
M20LOG=/tmp/secos_selftest_m20.log
echo "[selftest] Building M20 image (M20_MMAP_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make CFLAGS_EXTRA=-DM20_MMAP_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M20 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running M20 (mb2, ${TIMEOUT}s)..."
tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$M20LOG" >/dev/null 2>&1 || true

# File-backed mmap returns the file's bytes (sourced from the page cache).
grep -q "\[m20\] mmap content OK" "$M20LOG"; check "M20 file-backed mmap returns file content" $?
# read() returns the same bytes (also via the cache).
grep -q "\[m20\] read content OK" "$M20LOG"; check "M20 read() returns file content via cache" $?
# read() and mmap are coherent (shared cache pages).
grep -q "\[m20\] read/mmap coherent OK" "$M20LOG"; check "M20 read() and mmap are coherent" $?
grep -q "\[M20\] DONE" "$M20LOG"; check "M20 demo completed ([M20] DONE)" $?
! grep -q "\[EXC\]" "$M20LOG"; check "no CPU exception ([EXC]) during M20 run" $?

# ---- M21: AHCI (SATA) storage — the disk path for VMware / physical PCs ----
M21LOG=/tmp/secos_selftest_m21.log
echo "[selftest] Building M21 image (default; AHCI probe runs at boot)..."
make clean >/dev/null 2>&1 || true
if ! make iso >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M21 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
make disk-fat32 >/dev/null 2>&1 || { echo "  [FAIL] M21 disk image build"; FAIL=$((FAIL+1)); }
echo "[selftest] Running M21 (q35 AHCI/SATA data disk, ${TIMEOUT}s)..."
: > "$M21LOG"
# q35 routes `if=ide` to its built-in AHCI controller -> our driver drives it.
timeout "$((TIMEOUT+4))" qemu-system-x86_64 -machine q35 -m 256M \
    -cdrom myos.iso -boot d -drive file=disk.img,format=raw,if=ide \
    -debugcon file:"$M21LOG" -global isa-debugcon.iobase=0xe9 \
    -no-reboot -no-shutdown -display none
grep -q "\[AHCI\] SATA disk sda" "$M21LOG"; check "M21 AHCI finds a SATA disk (sda)" $?
grep -q "\[AHCI\] ready" "$M21LOG"; check "M21 AHCI driver ready" $?
grep -q "\[M10\] disk mounted at /mnt fs=FAT32" "$M21LOG"; check "M21 FAT32 mounts on the AHCI disk" $?
grep -q "\[M10\] read /mnt hello" "$M21LOG"; check "M21 reads a file from the AHCI disk" $?
grep -q "\[M10\] disk write+readback: OK" "$M21LOG"; check "M21 AHCI write+readback persists" $?
! grep -q "\[EXC\]" "$M21LOG"; check "no CPU exception ([EXC]) during M21 run" $?

# ---- M22: NVMe storage + USB stack (xHCI + HID keyboard + Mass Storage) ----
# Reuses the default myos.iso (NVMe/xHCI probes run at boot) + the FAT32 disk.img.
M22NLOG=/tmp/secos_selftest_m22n.log
M22ULOG=/tmp/secos_selftest_m22u.log
echo "[selftest] Running M22 NVMe (q35 + NVMe data disk, ${TIMEOUT}s)..."
: > "$M22NLOG"
# q35 ISO boot needs -boot d (else SeaBIOS hangs trying to boot the NVMe disk).
timeout "$((TIMEOUT+4))" qemu-system-x86_64 -machine q35 -m 256M \
    -cdrom myos.iso -boot d \
    -drive file=disk.img,if=none,id=nvm,format=raw -device nvme,serial=secos1,drive=nvm \
    -debugcon file:"$M22NLOG" -global isa-debugcon.iobase=0xe9 \
    -no-reboot -no-shutdown -display none
grep -q "\[NVME\] ready nvme0n1" "$M22NLOG"; check "M22 NVMe namespace ready (nvme0n1)" $?
grep -q "\[M10\] disk mounted at /mnt fs=FAT32" "$M22NLOG"; check "M22 FAT32 mounts on the NVMe disk" $?
grep -q "\[M10\] disk write+readback: OK" "$M22NLOG"; check "M22 NVMe write+readback persists" $?
! grep -q "\[EXC\]" "$M22NLOG"; check "no CPU exception ([EXC]) during M22 NVMe run" $?

echo "[selftest] Running M22 USB (xHCI + usb-kbd + usb-storage, ${TIMEOUT}s)..."
: > "$M22ULOG"
timeout "$((TIMEOUT+4))" qemu-system-x86_64 -m 256M \
    -cdrom myos.iso -boot d \
    -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
    -drive file=disk.img,if=none,id=us,format=raw -device usb-storage,bus=xhci.0,drive=us \
    -debugcon file:"$M22ULOG" -global isa-debugcon.iobase=0xe9 \
    -no-reboot -no-shutdown -display none
grep -q "\[XHCI\] running" "$M22ULOG"; check "M22 xHCI controller running" $?
grep -q "\[HID\] boot keyboard ready" "$M22ULOG"; check "M22 USB HID boot keyboard enumerated" $?
grep -q "\[MSC\] usb0 ready" "$M22ULOG"; check "M22 USB Mass Storage ready (usb0)" $?
grep -q "\[M10\] disk mounted at /mnt fs=FAT32" "$M22ULOG"; check "M22 FAT32 mounts on the USB disk" $?
grep -q "\[M10\] disk write+readback: OK" "$M22ULOG"; check "M22 USB write+readback persists" $?
! grep -q "\[EXC\]" "$M22ULOG"; check "no CPU exception ([EXC]) during M22 USB run" $?

# ---- M50 B3: per-disk block locks + xHCI/HID locks under real concurrency ----
# tools/blk_stress_drive.py on -smp 4 q35 with sda+sdb (AHCI, sdb1 = MBR
# partition), nvme0n1 and usb0 (usb-storage next to a usb-kbd on qemu-xhci).
# Phase 1: fs/blkstress.c (-DSECOS_B3_BLKSTRESS) runs disk hashes, writes and
# devfs byte-stripe RMW on every CPU at once outside the BKL while the host
# types on the USB keyboard; phase 2: parallel dd from bash plus typing.
# Digests/md5s and every byte the guest wrote are compared with the images.
echo "[selftest] Building M50 B3 block-stress image (-DSECOS_B3_BLKSTRESS)..."
make clean >/dev/null 2>&1 || true
if ! make iso CFLAGS_EXTRA=-DSECOS_B3_BLKSTRESS >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M50 B3 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
BSDDIR=$(mktemp -d /tmp/secos_selftest_bsd.XXXXXX)
echo "[selftest] Running M50 B3 block/USB lock stress (-smp 4)..."
timeout 1200 python3 tools/blk_stress_drive.py --iso myos.iso --smp 4 --out "$BSDDIR" \
    --log "$BSDDIR/debugcon.log" >"$BSDDIR/drive.out" 2>&1
grep -q "^\[BSD\] RESULT " "$BSDDIR/drive.out"; check "M50 B3 block stress driver ran to completion" $?
while IFS= read -r l; do
    case "$l" in
        "[BSD] RESULT "*) ;;
        "[BSD] "*" OK"*)   x=${l#\[BSD\] }; check "M50 B3 ${x%% OK*}" 0 ;;
        "[BSD] "*" FAIL"*) x=${l#\[BSD\] }; check "M50 B3 ${x%% FAIL*}" 1; echo "    $l" | cut -c1-200 ;;
    esac
done < "$BSDDIR/drive.out"
rm -f "$BSDDIR"/*.img

# ---- M23: POSIX FS personality (/dev, /proc, /sys + persistent ext2 root) ----
M23LOG=/tmp/secos_selftest_m23.log
M23RLOG=/tmp/secos_selftest_m23root.log
echo "[selftest] Building M23 image (M23_FS_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make iso CFLAGS_EXTRA=-DM23_FS_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M23 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
make disk-fat32 >/dev/null 2>&1 || { echo "  [FAIL] M23 disk image build"; FAIL=$((FAIL+1)); }
echo "[selftest] Running M23 demo (/dev + /proc + signed ring-3 program)..."
: > "$M23LOG"
timeout "$((TIMEOUT+4))" qemu-system-x86_64 -cdrom myos.iso -boot d \
    -drive file=disk.img,if=virtio,format=raw \
    -debugcon file:"$M23LOG" -global isa-debugcon.iobase=0xe9 \
    -no-reboot -no-shutdown -display none -m 256M
grep -q "devfs(/dev)=0x0000000000000001" "$M23LOG"; check "M23 devfs mounted at /dev" $?
grep -q "procfs(/proc)=0x0000000000000001" "$M23LOG"; check "M23 procfs mounted at /proc" $?
grep -q "sysfs(/sys)=0x0000000000000001" "$M23LOG"; check "M23 sysfs mounted at /sys" $?
grep -q "\[m23fs\] /dev/zero read OK" "$M23LOG"; check "M23 signed prog reads /dev/zero (ring-3)" $?
grep -q "\[m23fs\] /dev/null write OK" "$M23LOG"; check "M23 signed prog writes /dev/null (ring-3)" $?
grep -q "\[m23fs\] DONE" "$M23LOG"; check "M23 signed prog completes (/proc, stat, lseek)" $?
! grep -q "\[EXC\]" "$M23LOG"; check "no CPU exception ([EXC]) during M23 demo" $?

# Persistent ext2 root: build a system disk (marker /.secosroot) and confirm the
# kernel adopts it as the VFS root. (Default build; no demo gate.)
echo "[selftest] Running M23 persistent ext2 root..."
make iso >/tmp/secos_selftest_build.log 2>&1
make sysdisk-ext2 >/dev/null 2>&1 || { echo "  [FAIL] M23 sysdisk build"; FAIL=$((FAIL+1)); }
: > "$M23RLOG"
timeout "$((TIMEOUT+4))" qemu-system-x86_64 -cdrom myos.iso -boot d \
    -drive file=sysdisk.img,if=virtio,format=raw \
    -debugcon file:"$M23RLOG" -global isa-debugcon.iobase=0xe9 \
    -no-reboot -no-shutdown -display none -m 256M
grep -q "\[M23\] persistent ext2/ext4 root on" "$M23RLOG"; check "M23 persistent ext2/ext4 root adopted" $?
! grep -q "\[VFS\] root RAMFS mounted" "$M23RLOG"; check "M23 RAMFS root skipped when ext2 root present" $?
! grep -q "\[EXC\]" "$M23RLOG"; check "no CPU exception ([EXC]) during M23 persistent-root boot" $?

# ---- M33: full ext4 root WITH metadata_csum stays e2fsck-clean after writes ----
# Boot a default-mkfs.ext4 (metadata_csum) sysdisk as the root; SecOS installs
# coreutils + writes /VERSION; the volume must remain e2fsck-clean (crc32c
# superblock/gd/inode/dir/bitmap checksums all refreshed).
M33LOG=/tmp/secos_selftest_m33.log
if command -v e2fsck >/dev/null 2>&1; then
  echo "[selftest] Running M33 ext4 metadata_csum root..."
  make sysdisk-ext4-csum >/dev/null 2>&1 || { echo "  [FAIL] M33 csum sysdisk build"; FAIL=$((FAIL+1)); }
  cp sysdisk.img /tmp/secos_m33csum.img
  : > "$M33LOG"
  timeout "$((TIMEOUT+4))" qemu-system-x86_64 -cdrom myos.iso -boot d \
      -drive file=/tmp/secos_m33csum.img,if=virtio,format=raw \
      -debugcon file:"$M33LOG" -global isa-debugcon.iobase=0xe9 \
      -no-reboot -no-shutdown -display none -m 512M
  grep -q "\[M23\] persistent ext2/ext4 root on" "$M33LOG"; check "M33 metadata_csum ext4 root adopted" $?
  e2fsck -fn /tmp/secos_m33csum.img >/tmp/secos_m33fsck.log 2>&1; check "M33 metadata_csum volume e2fsck-clean after SecOS writes" $?
  ! grep -q "\[EXC\]" "$M33LOG"; check "no CPU exception ([EXC]) during M33 csum-root boot" $?
else
  echo "[selftest] (skip M33 ext4 csum: e2fsck not installed)"
fi

# ---- M27a: JBD2 journal replay (crash recovery) ----
M27LOG=/tmp/secos_selftest_m27.log
echo "[selftest] Building M27 image (M27_RECOVER_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make iso CFLAGS_EXTRA=-DM27_RECOVER_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M27 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
# ext4 image with a dirty JBD2 journal (e2fsck-validated synthetic transaction).
if make disk-journal >/tmp/secos_selftest_build.log 2>&1; then
    # Cross-check: e2fsck on a COPY must itself recover the journal to "NEW"
    # (proves the synthetic journal is a genuine JBD2 journal, not self-consistency).
    cp disk.img /tmp/secos_m27_e2fsck.img
    e2fsck -fy /tmp/secos_m27_e2fsck.img >/dev/null 2>&1
    if debugfs -R "cat /target.bin" /tmp/secos_m27_e2fsck.img 2>/dev/null | grep -q NEW; then
        check "M27 synthetic journal is e2fsck-recoverable (ground truth)" 0
    else
        check "M27 synthetic journal is e2fsck-recoverable (ground truth)" 1
    fi
    echo "[selftest] Running M27 journal replay (ext4 dirty journal, ${TIMEOUT}s)..."
    : > "$M27LOG"
    timeout "$TIMEOUT" qemu-system-x86_64 -cdrom myos.iso -drive file=disk.img,if=virtio,format=raw -boot d \
        -debugcon file:"$M27LOG" -global isa-debugcon.iobase=0xe9 -no-reboot -display none -m 256M
    grep -q "\[M27\] journal recover: txns=0x0000000000000001" "$M27LOG"; check "M27 detects + counts the committed transaction" $?
    grep -q '\[M27\] target.bin n=.* data="NEW"' "$M27LOG"; check "M27 replay rewrites the in-place block (OLD->NEW)" $?
    grep -q "\[M27\] REPLAY OK" "$M27LOG"; check "M27 journal replay OK (matches e2fsck)" $?
    ! grep -q "\[EXC\]" "$M27LOG"; check "no CPU exception ([EXC]) during M27 run" $?
    # After SecOS replayed, the image must be e2fsck-clean (journal consumed).
    e2fsck -fn disk.img >/dev/null 2>&1; check "M27 post-replay image is e2fsck-clean" $?
else
    echo "  [SKIP] M27 disk-journal build failed (mke2fs/debugfs/python missing?)"
fi

# ---- M26: VFS maturity (metadata, symlinks, mount control) on ext2 /mnt ----
M26LOG=/tmp/secos_selftest_m26.log
echo "[selftest] Building M26 image (M26_FS_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make iso CFLAGS_EXTRA=-DM26_FS_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M26 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
make disk-ext2 >/dev/null 2>&1 || { echo "  [FAIL] M26 ext2 disk build"; FAIL=$((FAIL+1)); }
echo "[selftest] Running M26 (ext2 /mnt, ${TIMEOUT}s)..."
: > "$M26LOG"
timeout "$TIMEOUT" qemu-system-x86_64 -cdrom myos.iso -drive file=disk.img,if=virtio,format=raw -boot d \
    -debugcon file:"$M26LOG" -global isa-debugcon.iobase=0xe9 -no-reboot -display none -m 256M
grep -q "\[M26\] chmod 0640 -> mode=0x00000000000001A0" "$M26LOG"; check "M26 chmod persists + stat reads mode (0640)" $?
grep -q "\[M26\] chown 1000:1000 -> uid=0x00000000000003E8 gid=0x00000000000003E8" "$M26LOG"; check "M26 chown persists uid/gid" $?
grep -q 'target="m26.txt"' "$M26LOG"; check "M26 symlink create + readlink target" $?
grep -q "lstat type=0x0000000000000003 stat-follow type=0x0000000000000001" "$M26LOG"; check "M26 lstat=symlink, stat follows to file" $?
grep -q "\[M26\] umount rc=0x0000000000000000 remount rc=0x0000000000000000" "$M26LOG"; check "M26 umount + remount succeed" $?
grep -q "\[M26\] DONE" "$M26LOG"; check "M26 demo completed ([M26] DONE)" $?
! grep -q "\[EXC\]" "$M26LOG"; check "no CPU exception ([EXC]) during M26 run" $?

# ---- M25: anonymous pipes across fork (blocking read + EOF) ----
M25LOG=/tmp/secos_selftest_m25.log
echo "[selftest] Building M25 image (M25_PIPE_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make CFLAGS_EXTRA=-DM25_PIPE_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M25 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running M25 (mb2, ${TIMEOUT}s)..."
tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$M25LOG" >/dev/null 2>&1 || true
grep -q "\[m25\] child read: " "$M25LOG"; check "M25 child reads parent's pipe data (blocking)" $?
grep -q "M25-PIPE-OK" "$M25LOG"; check "M25 pipe payload delivered intact" $?
grep -q "\[m25\] child got EOF OK" "$M25LOG"; check "M25 pipe EOF after write end closed" $?
grep -q "\[m25\] DONE-USER" "$M25LOG"; check "M25 pipe demo program completed" $?
grep -q "\[M25\] DONE" "$M25LOG"; check "M25 demo completed ([M25] DONE)" $?
! grep -q "\[EXC\]" "$M25LOG"; check "no CPU exception ([EXC]) during M25 run" $?

# ---- M30: signals (handler + raise + sigreturn, SIG_IGN of SIGPIPE) ----
M30LOG=/tmp/secos_selftest_m30.log
echo "[selftest] Building M30 image (M30_SIGNAL_DEMO=1, signing enforced)..."
make clean >/dev/null 2>&1 || true
if ! make CFLAGS_EXTRA=-DM30_SIGNAL_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M30 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running M30 (mb2, ${TIMEOUT}s)..."
tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$M30LOG" >/dev/null 2>&1 || true
grep -q "\[SIG\] deliver sig=0x000000000000000A" "$M30LOG"; check "M30 SIGUSR1 delivered to a custom handler" $?
grep -q "\[m30\] caught SIGUSR1" "$M30LOG"; check "M30 handler ran in ring 3" $?
grep -q "\[m30\] sigreturn ok" "$M30LOG"; check "M30 sigreturn restored the interrupted context" $?
grep -q "\[m30\] sigpipe epipe ok" "$M30LOG"; check "M30 SIG_IGN of SIGPIPE: EPIPE without termination" $?
grep -q "\[M30\] DONE" "$M30LOG"; check "M30 demo completed ([M30] DONE)" $?
! grep -q "\[EXC\]" "$M30LOG"; check "no CPU exception ([EXC]) during M30 run" $?

# ---- M27b: write-side journaling (crash atomicity) ----
# Build a verify image + two crash images, then for each crash point: run1 cuts
# power mid-commit, run2 recovers and reports. A write-journalled op must be
# ATOMIC across the crash (absent if cut before commit, present if cut after the
# journal is published) and the volume must stay e2fsck-clean.
if command -v mke2fs >/dev/null 2>&1; then
  mkj(){ rm -rf /tmp/secos_jc && mkdir -p /tmp/secos_jc; printf 'hi\n' > /tmp/secos_jc/hello.txt;
         mke2fs -F -q -t ext4 -O has_journal,^metadata_csum,^64bit -b 1024 -d /tmp/secos_jc "$1" 16384 2>/dev/null; }
  runimg(){ : > "$2"; timeout "$TIMEOUT" qemu-system-x86_64 -cdrom "$1" -drive file="$3",if=virtio,format=raw -boot d \
            -debugcon file:"$2" -global isa-debugcon.iobase=0xe9 -no-reboot -display none -m 256M >/dev/null 2>&1 || true; }
  echo "[selftest] Building M27b images (verify + crash1 + crash2)..."
  make clean >/dev/null 2>&1 || true
  make iso CFLAGS_EXTRA=-DM27B_VERIFY=1 >/tmp/secos_selftest_build.log 2>&1 && cp myos.iso /tmp/secos_iso_verify.iso
  make clean >/dev/null 2>&1 || true
  make iso CFLAGS_EXTRA=-DM27B_CRASH=1 >/tmp/secos_selftest_build.log 2>&1 && cp myos.iso /tmp/secos_iso_c1.iso
  make clean >/dev/null 2>&1 || true
  make iso CFLAGS_EXTRA=-DM27B_CRASH=2 >/tmp/secos_selftest_build.log 2>&1 && cp myos.iso /tmp/secos_iso_c2.iso
  # baseline: journalled writes (default M10 test) leave the volume e2fsck-clean
  mkj /tmp/secos_jbase.img
  runimg /tmp/secos_iso_verify.iso /tmp/secos_m27b_base.log /tmp/secos_jbase.img
  grep -q "\[M10\] disk write+readback: OK" /tmp/secos_m27b_base.log; check "M27b journalled write+readback OK" $?
  e2fsck -fn /tmp/secos_jbase.img >/dev/null 2>&1; check "M27b journalled write leaves volume e2fsck-clean" $?
  # scenario 1: crash BEFORE commit -> op atomically absent
  mkj /tmp/secos_jc1.img
  runimg /tmp/secos_iso_c1.iso     /tmp/secos_m27b_c1r1.log /tmp/secos_jc1.img
  grep -q "\[M27B\] CRASH before commit" /tmp/secos_m27b_c1r1.log; check "M27b run1 cut power before commit" $?
  runimg /tmp/secos_iso_verify.iso /tmp/secos_m27b_c1r2.log /tmp/secos_jc1.img
  grep -q "\[M27B\] verify: newf ABSENT" /tmp/secos_m27b_c1r2.log; check "M27b crash-before-commit: op atomically absent" $?
  e2fsck -fn /tmp/secos_jc1.img >/dev/null 2>&1; check "M27b crash-before-commit: e2fsck-clean" $?
  # scenario 2: crash AFTER publish -> recovery replays -> op present
  mkj /tmp/secos_jc2.img
  runimg /tmp/secos_iso_c2.iso     /tmp/secos_m27b_c2r1.log /tmp/secos_jc2.img
  grep -q "\[M27B\] CRASH after publish" /tmp/secos_m27b_c2r1.log; check "M27b run1 cut power after publish" $?
  runimg /tmp/secos_iso_verify.iso /tmp/secos_m27b_c2r2.log /tmp/secos_jc2.img
  grep -q "\[M27\] journal recover: txns=0x0000000000000001" /tmp/secos_m27b_c2r2.log; check "M27b crash-after-publish: recovery replays the txn" $?
  grep -q "\[M27B\] verify: newf PRESENT" /tmp/secos_m27b_c2r2.log; check "M27b crash-after-publish: op atomically present" $?
  e2fsck -fn /tmp/secos_jc2.img >/dev/null 2>&1; check "M27b crash-after-publish: e2fsck-clean" $?
else
  echo "  [SKIP] M27b (mke2fs unavailable)"
fi

# ---- M28-1: ACPI topology discovery (read-only; scales with -smp) ----
M28LOG=/tmp/secos_selftest_m28.log
echo "[selftest] Building M28 image (ACPI discovery + AP bring-up at boot)..."
make clean >/dev/null 2>&1 || true
if ! make iso >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M28 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running M28 (mb2, -smp 2, ${TIMEOUT}s)..."
: > "$M28LOG"
timeout "$TIMEOUT" qemu-system-x86_64 -cdrom myos.iso -smp 2 -boot d \
    -debugcon file:"$M28LOG" -global isa-debugcon.iobase=0xe9 -no-reboot -display none -m 256M
grep -q "\[ACPI\] CPUs=0x0000000000000002" "$M28LOG"; check "M28 ACPI discovers all CPUs (-smp 2)" $?
grep -q "lapic=0x00000000FEE00000" "$M28LOG"; check "M28 ACPI reports the LAPIC base" $?
grep -q "ioapic0=0x00000000FEC00000" "$M28LOG"; check "M28 ACPI reports the IOAPIC base" $?
# M28-2: APIC switchover (LAPIC timer + IOAPIC replace the PIC/PIT).
grep -q "\[APIC\] IOAPIC route irq=0x0000000000000001 .* vec=0x0000000000000021" "$M28LOG"; check "M28-2 IOAPIC routes keyboard IRQ1 -> vec 0x21" $?
grep -q "\[APIC\] LAPIC timer hz=0x00000000000003E8" "$M28LOG"; check "M28-2 LAPIC timer programmed at 1 kHz" $?
grep -q "\[APIC\] mode active" "$M28LOG"; check "M28-2 APIC mode active (PIC masked)" $?
grep -q "\[APIC\] timer tick verified" "$M28LOG"; check "M28-2 LAPIC timer actually ticks (IRQ delivery OK)" $?
# M28-3: TSC monotonic timekeeping.
grep -q "\[TSC\] calibrated hz=" "$M28LOG"; check "M28-3 TSC calibrated against PIT ch2" $?
grep -q "\[TSC\] monotonic OK" "$M28LOG"; check "M28-3 ktime_ns() monotonic + sane frequency" $?
! grep -q "\[EXC\]" "$M28LOG"; check "no CPU exception ([EXC]) during M28 run" $?
# M28-2 also brings up the AP (M29-1/2): it must come online with no fault.
grep -q "\[SMP\] online cpus=0x0000000000000002" "$M28LOG"; check "M29-2 AP comes online (-smp 2)" $?

# ---- M29-3: multicore scheduling (user tasks run on an application processor) ----
M29LOG=/tmp/secos_selftest_m29.log
echo "[selftest] Building M29 image (M8 workload, multicore)..."
make clean >/dev/null 2>&1 || true
if ! make iso CFLAGS_EXTRA="-DM8_SCHED_DEMO=1 -DDEV_ALLOW_UNSIGNED" >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M29 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running M29 (mb2, -smp 2, ${TIMEOUT}s)..."
: > "$M29LOG"
timeout "$TIMEOUT" qemu-system-x86_64 -cdrom myos.iso -smp 2 -boot d \
    -debugcon file:"$M29LOG" -global isa-debugcon.iobase=0xe9 -no-reboot -display none -m 256M
# A ring-3 user task (pid != 0) scheduled on CPU index 1 = it ran on the AP.
grep -q "\[SMP\] cpu=0x0000000000000001 run pid=" "$M29LOG"; check "M29-3 user task runs on an application processor" $?
grep -q "\[M8\] PMM stable across rounds: NO LEAK" "$M29LOG"; check "M29-3 no PMM leak under multicore scheduling" $?
grep -q "\[M8\] DONE" "$M29LOG"; check "M29-3 multicore workload completes" $?
! grep -q "\[EXC\]" "$M29LOG"; check "no CPU exception ([EXC]) during M29 multicore run" $?

# ---- Phase K: M31 libc + coreutils, M32 signed packages (one build/run) ----
# Each signed spawn pays an Ed25519 verify (slow under TCG), so this variant runs
# only 2-3 spawns and gets a generous timeout instead of the default 12s.
M31LOG=/tmp/secos_selftest_m31.log
PK_TIMEOUT=45
echo "[selftest] Building Phase K image (M31 libc + coreutils + M32 packages)..."
make clean >/dev/null 2>&1 || true
if ! make iso CFLAGS_EXTRA="-DM31_LIBC_DEMO=1 -DM32_PKG_DEMO=1" >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] PhaseK BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
echo "[selftest] Running Phase K (mb2, ${PK_TIMEOUT}s)..."
: > "$M31LOG"
timeout "$PK_TIMEOUT" qemu-system-x86_64 -cdrom myos.iso -boot d \
    -debugcon file:"$M31LOG" -global isa-debugcon.iobase=0xe9 -no-reboot -display none -m 256M
grep -q "\[m31\] DONE fails=0" "$M31LOG"; check "M31 libc: printf/string/stdlib/ctype tests pass in ring 3" $?
! grep -q "\[m31\] FAIL" "$M31LOG"; check "M31 libc: no failing test" $?
grep -q "\[M31\] coreutils installed to /bin" "$M31LOG"; check "M31 coreutils installed to /bin" $?
grep -q "CU-ECHO-OK" "$M31LOG"; check "M31 coreutils: echo runs in ring 3" $?
grep -q "\[M32\] signed package installed" "$M31LOG"; check "M32 signed package installs" $?
grep -q "\[M32\] tampered package REFUSED" "$M31LOG"; check "M32 tampered package refused" $?
grep -q "SECOS-PKG-INSTALLED" "$M31LOG"; check "M32 package-delivered file is readable" $?
! grep -q "\[EXC\]" "$M31LOG"; check "no CPU exception ([EXC]) during Phase K run" $?

# ---- M34-M39 shared helper: boot an ISO under mb2 with a given timeout ----
run_iso() { # run_iso <log> <timeout> [extra qemu args...]
    local log="$1" t="$2"; shift 2
    : > "$log"
    timeout "$t" qemu-system-x86_64 -cdrom myos.iso -boot d \
        -debugcon file:"$log" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -m 256M "$@" >/dev/null 2>&1
}
# Drop scheduler trace lines that can interleave mid-line with ring-3 output.
declutter() { # declutter <log>  (in place)
    sed -z -E -i 's/\[SCHED\] (preempt|exit|switch) [0-9A-Fx> -]*\n//g' "$1"
}
build_demo() { # build_demo <label> <CFLAGS_EXTRA>
    make clean >/dev/null 2>&1 || true
    if ! make iso CFLAGS_EXTRA="$2" >/tmp/secos_selftest_build.log 2>&1; then
        echo "[selftest] $1 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
        tail -20 /tmp/secos_selftest_build.log >&2; exit 2
    fi
}

# ---- M34: lua 5.4.7 compiled from source, signed, runs in ring 3 ----
# Uses the committed signed crypto/user_lua_elf.h (regenerate with `make port-lua`).
M34LOG=/tmp/secos_selftest_m34.log
echo "[selftest] Building M34 image (M34_LUA_DEMO=1, signing enforced)..."
build_demo M34 "-DM34_LUA_DEMO=1"
echo "[selftest] Running M34 (mb2, 30s)..."
run_iso "$M34LOG" 30; declutter "$M34LOG"
grep -q "\[M34\] lua spawned" "$M34LOG"; check "M34 signed lua interpreter passes the load gate" $?
grep -q "hello from lua Lua 5.4 on SecOS" "$M34LOG"; check "M34 lua runs its script in ring 3" $?
grep -q "math.sqrt(2) =.1.4142135623731" "$M34LOG"; check "M34 lua floats: math.sqrt(2) (%.14g)" $?
grep -q "math.pi =.3.1415926535898" "$M34LOG"; check "M34 lua floats: math.pi" $?
grep -q "fib(20) =.6765" "$M34LOG"; check "M34 lua recursion: fib(20)=6765" $?
grep -q "upper =.SECOS ROCKS" "$M34LOG"; check "M34 lua string library" $?
grep -q "\[M34\] DONE" "$M34LOG"; check "M34 demo completed ([M34] DONE)" $?
! grep -q "\[EXC\]" "$M34LOG"; check "no CPU exception ([EXC]) during M34 run" $?

# ---- M35: generalized capability model (confined signed program + [AUDIT]) ----
M35LOG=/tmp/secos_selftest_m35.log
echo "[selftest] Building M35 image (M35_CAP_DEMO=1, signing enforced)..."
build_demo M35 "-DM35_CAP_DEMO=1"
echo "[selftest] Running M35 (mb2, ${TIMEOUT}s)..."
run_iso "$M35LOG" "$TIMEOUT"; declutter "$M35LOG"
grep -q "\[M35\] confined process pid loaded, caps=0x0000000000000840" "$M35LOG"; check "M35 manifest confines the process (CAP_ENFORCE|FS_READ|TIME)" $?
grep -q "\[m35\] getticks() = [0-9]* (CAP_TIME granted" "$M35LOG"; check "M35 granted capability allowed (CAP_TIME)" $?
grep -q "\[m35\] open(/proc/version, RDONLY) = 3" "$M35LOG"; check "M35 granted capability allowed (CAP_FS_READ)" $?
grep -q "\[AUDIT\] pid=.* DENY  FS_WRITE (create)" "$M35LOG"; check "M35 create denied + audited (no CAP_FS_WRITE)" $?
grep -q "\[AUDIT\] pid=.* DENY  PROC (spawn)" "$M35LOG"; check "M35 spawn denied + audited (no CAP_PROC)" $?
grep -q "\[AUDIT\] pid=.* DENY  IPC (pipe)" "$M35LOG"; check "M35 pipe denied + audited (no CAP_IPC)" $?
grep -q "\[m35\] socket() = -1" "$M35LOG"; check "M35 socket denied (no CAP_NET)" $?
grep -q "\[M35\] audit log total events: 0x0000000000000004" "$M35LOG"; check "M35 audit ring recorded the denials (4 events)" $?
grep -q "\[M35\] DONE" "$M35LOG"; check "M35 demo completed ([M35] DONE)" $?
! grep -q "\[EXC\]" "$M35LOG"; check "no CPU exception ([EXC]) during M35 run" $?

# ---- M46: exec confinement (no_new_privs; caps only shrink across exec) ----
# user/m46_confine.c in three signed builds: m46conf (confined), m46probe
# (ambient + explicit NET/INPUT/DISPLAY), m46setid (SETID, root 04755). The
# kernel spawns the ambient control first, then the confined driver. (fb0 does
# not exist yet when the demo runs - the framebuffer comes up later in boot -
# so the explicit-cap check uses /dev/input/event0, the same gate.)
M46LOG=/tmp/secos_selftest_m46.log
echo "[selftest] Building M46 image (M46_CONFINE_DEMO=1, signing enforced)..."
build_demo M46 "-DM46_CONFINE_DEMO=1"
echo "[selftest] Running M46 (mb2, 60s)..."
run_iso "$M46LOG" 60; declutter "$M46LOG"
m46() { grep -aq "^\[m46\] $1" "$M46LOG"; check "M46 $2" $?; }
m46 "ctl socket=OK"   "ambient control: probe gets CAP_NET"
m46 "ctl input=OK"    "ambient control: probe gets CAP_INPUT"
m46 "ctl write=OK"    "ambient control: probe writes files"
m46 "ctl-setid caps=.* uid=1000 euid=0" "ambient control: SETID program run as uid 1000 gets euid 0 (spawn)"
m46 "ctl-setid-exec caps=.* uid=1000 euid=0" "ambient control: SETID program gets euid 0 (execve)"
m46 "set nnp rc=0 nnp=1" "prctl(PR_SET_NO_NEW_PRIVS) on an ambient process"
m46 "nnp-setid caps=.* euid=1000" "voluntary no_new_privs: SETID program keeps euid 1000 (spawn)"
m46 "nnp-setid-exec caps=.* euid=1000" "voluntary no_new_privs: SETID program keeps euid 1000 (execve)"
m46 "conf caps=0x60000f40 nnp=1" "confined driver: caps 0xF40, confined, no_new_privs automatic"
m46 "spawn caps=0x60000f40 nnp=1" "spawned ambient probe is confined to the parent's caps"
m46 "spawn socket=DENIED" "spawned ambient probe: no CAP_NET"
m46 "spawn input=DENIED"  "spawned ambient probe: no CAP_INPUT"
m46 "spawn write=DENIED"  "spawned ambient probe: no file write"
m46 "fork caps=0x60000f40 nnp=1" "fork child keeps the parent's confinement"
m46 "exec caps=0x60000f40 nnp=1" "execve of the ambient probe stays confined"
m46 "exec socket=DENIED"  "execve'd probe: no CAP_NET"
m46 "exec input=DENIED"   "execve'd probe: no CAP_INPUT"
m46 "exec write=DENIED"   "execve'd probe: no file write"
m46 "busybox touch rc=1 file=ABSENT" "spawned BusyBox cannot create a file"
m46 "bash redirect rc=1 file=ABSENT" "spawned bash cannot create a file"
m46 "conf-setid caps=.* uid=1000 euid=1000" "confined uid 1000 + SETID program: no euid 0 (spawn)"
m46 "conf-setid-exec caps=.* uid=1000 euid=1000" "confined uid 1000 + SETID program: no euid 0 (execve)"
m46 "clear nnp rc=-22 nnp=1" "no_new_privs cannot be cleared"
grep -aq "\[AUDIT\] pid=.* DENY  NET (spawn)" "$M46LOG"; check "M46 lost capability audited ([AUDIT] DENY NET (spawn))" $?
grep -aq "\[AUDIT\] pid=.* DENY  SETID (setid-nnp)" "$M46LOG"; check "M46 refused set-id exec audited" $?
grep -aq "\[M46\] DONE" "$M46LOG"; check "M46 demo completed ([M46] DONE)" $?
! grep -aq "\[EXC\]" "$M46LOG"; check "no CPU exception ([EXC]) during M46 run" $?

# ---- M39: POSIX shell-from-source foundation (cwd/env/dup2/termios/...) ----
M39PLOG=/tmp/secos_selftest_m39posix.log
echo "[selftest] Building M39 POSIX image (M39_POSIX_DEMO=1, signing enforced)..."
build_demo M39-POSIX "-DM39_POSIX_DEMO=1"
echo "[selftest] Running M39 POSIX (mb2, ${TIMEOUT}s)..."
run_iso "$M39PLOG" "$TIMEOUT"; declutter "$M39PLOG"
for t in "getcwd" "chdir+getcwd" "setenv/getenv" "setenv overwrite" "getenv-missing" \
         "getppid" "dup2 redirect" "termios get/set" "getuid/geteuid" "gettimeofday" "fcntl F_DUPFD"; do
    grep -qF "[M39] $t OK" "$M39PLOG"; check "M39 POSIX: $t" $?
done
grep -q "\[M39\] POSIX-FOUNDATION DONE" "$M39PLOG"; check "M39 POSIX foundation program completed" $?
! grep -q "\[M39\].*FAIL" "$M39PLOG"; check "M39 POSIX: no failing check" $?
grep -q "\[M39\] DONE" "$M39PLOG"; check "M39 POSIX demo completed ([M39] DONE)" $?
! grep -q "\[EXC\]" "$M39PLOG"; check "no CPU exception ([EXC]) during M39 POSIX run" $?

# ---- M39: user-space ASLR (per-process stack/heap/mmap slides) ----
# Every created process logs its randomized layout; across the multi-process
# M16 + Phase K runs the slides must actually differ between processes.
cat "$M16LOG" "$M31LOG" 2>/dev/null | grep -o "\[ASLR\] stack_top=0x[0-9A-F]* brk=0x[0-9A-F]* mmap=0x[0-9A-F]*" > /tmp/secos_selftest_aslr.txt || true
n_aslr=$(wc -l < /tmp/secos_selftest_aslr.txt)
n_st=$(awk '{print $2}' /tmp/secos_selftest_aslr.txt | sort -u | wc -l)
n_brk=$(awk '{print $3}' /tmp/secos_selftest_aslr.txt | sort -u | wc -l)
n_mm=$(awk '{print $4}' /tmp/secos_selftest_aslr.txt | sort -u | wc -l)
[[ "$n_aslr" -ge 3 ]]; check "M39 ASLR layout logged per process (n=$n_aslr)" $?
[[ "$n_st" -ge 2 && "$n_brk" -ge 2 && "$n_mm" -ge 2 ]]; check "M39 ASLR slides differ across processes (stack=$n_st brk=$n_brk mmap=$n_mm distinct)" $?

# ---- M45: kernel KASLR (random 2 MiB-aligned slide of the high-half base) ----
# A -DSECOS_KASLR_DEBUG image prints the slide on raw debugcon only (never the
# kernel log). Boot it 5 times under MB2: the slides must differ (>= 4 distinct
# of 5; 768 slots), every boot must reach the W^X audit clean, and the measured-
# boot hash must stay the sha256 of .text+.rodata of kernel.bin whatever the
# slide. Then: -smp 4 -cpu max (RDRAND, APs online), the `nokaslr` MB2 option,
# UEFI (2 boots, EFI_RNG via virtio-rng, `nokaslr` in the load options) and a
# -DSECOS_NO_KASLR build.
kaslr_host_meas() { # sha256 of [.text start, _rodata_end) of kernel.bin
    local syms ts re toff
    syms=$(nm kernel.bin 2>/dev/null || true)
    ts=$(awk '$3=="_text_start"{print $1}' <<<"$syms")
    re=$(awk '$3=="_rodata_end"{print $1}' <<<"$syms")
    toff=$(readelf -SW kernel.bin | awk '{for(i=1;i<NF;i++) if($i==".text") print $(i+3)}' || true)
    [[ -n "$ts" && -n "$re" && -n "$toff" ]] || return 0
    tail -c +$(( 16#$toff + 1 )) kernel.bin | head -c $(( 16#$re - 16#$ts )) | sha256sum | cut -d' ' -f1
}
K45=/tmp/secos_selftest_m45
rm -rf "$K45"; mkdir -p "$K45"
echo "[selftest] Building M45 KASLR image (SECOS_KASLR_DEBUG, SECOS_SMP_AT_BOOT)..."
build_demo M45 "-DSECOS_KASLR_DEBUG -DSECOS_SMP_AT_BOOT"
k45_meas=$(kaslr_host_meas)
grep -q "^\[kaslr\] [0-9]* relocations" /tmp/secos_selftest_build.log; check "M45 relocation table extracted at link time" $?
[[ -n "$(nm kernel.bin | awk '$3=="_kaslr_start"')" ]]; check "M45 kernel.bin carries the .kaslr table" $?
slides=""; canaries=""; n_ok=0; n_meas=0; n_wx=0; n_exc=0; n_ssp=0; n_leak=0
for i in 1 2 3 4 5; do
    echo "[selftest] Running M45 KASLR boot $i/5 (mb2, 15s)..."
    run_iso "$K45/mb2_$i.log" 15
    s=$(grep -ao "\[KASLR-DEBUG\] slide=0x[0-9a-f]*" "$K45/mb2_$i.log" | head -1 | sed 's/.*=//')
    slides="$slides $s"
    canaries="$canaries $(grep -ao "\[KASLR-DEBUG\] canary=0x[0-9a-f]*" "$K45/mb2_$i.log" | head -1 | sed 's/.*=//')"
    grep -q "\[SSP\] stack canary randomized at boot" "$K45/mb2_$i.log" && n_ssp=$((n_ssp+1))
    # Kernel-image addresses in the log must be link-time ones (0xFFFFFFFF80..83..):
    # a slid address would reveal the slide through dmesg.
    grep -aoiE "0xFFFFFFFF[0-9A-F]{8}" "$K45/mb2_$i.log" | grep -viE "^0xFFFFFFFF8[0-3]|^0xFFFFFFFFFFFFFFFF$" | grep -q . && n_leak=$((n_leak+1))
    grep -q "\[KASLR\] on: kernel base randomized, 768 slots x 2 MiB (~9.6 bits), entropy=.*by MB2 entry" "$K45/mb2_$i.log" && n_ok=$((n_ok+1))
    m=$(grep -ao "\[M37\] kernel code measurement (sha256 .text+.rodata): [0-9a-f]*" "$K45/mb2_$i.log" | awk '{print $NF}')
    [[ -n "$m" && "$m" == "$k45_meas" ]] && n_meas=$((n_meas+1))
    grep -q "\[WX\] kernel image W^X verified (0 violations)" "$K45/mb2_$i.log" && n_wx=$((n_wx+1))
    grep -q "\[EXC\]" "$K45/mb2_$i.log" && n_exc=$((n_exc+1))
done
n_distinct=$(tr ' ' '\n' <<<"$slides" | grep -c . || true)
n_uniq=$(tr ' ' '\n' <<<"$slides" | grep . | sort -u | wc -l)
echo "  slides:$slides"
[[ "$n_ok" -eq 5 ]]; check "M45 MB2: KASLR on in 5/5 boots (n=$n_ok)" $?
[[ "$n_distinct" -eq 5 && "$n_uniq" -ge 4 ]]; check "M45 MB2: slides differ across boots ($n_uniq distinct of $n_distinct)" $?
bad_slide=0
for s in $slides; do (( s % 0x200000 == 0 && s < 768 * 0x200000 )) || bad_slide=1; done
[[ "$bad_slide" -eq 0 ]]; check "M45 MB2: every slide 2 MiB-aligned and below 1.5 GiB" $?
[[ "$n_meas" -eq 5 ]]; check "M45 measured boot slide-independent: 5/5 equal to sha256 of kernel.bin (n=$n_meas)" $?
[[ "$n_wx" -eq 5 ]]; check "M45 W^X audit clean at the slid base in 5/5 boots (n=$n_wx)" $?
[[ "$n_exc" -eq 0 ]]; check "M45 no CPU exception in the 5 KASLR boots" $?
c_uniq=$(tr ' ' '\n' <<<"$canaries" | grep . | sort -u | wc -l)
[[ "$n_ssp" -eq 5 && "$c_uniq" -ge 4 ]]; check "M45 stack canary randomized at boot ($c_uniq distinct of 5)" $?
[[ "$n_leak" -eq 0 ]]; check "M45 no slid kernel address in the kernel log ($n_leak boots leaked)" $?
# SMP + RDRAND: the APs come up on the relocated image.
echo "[selftest] Running M45 KASLR (mb2, -smp 4 -cpu max, 25s)..."
run_iso "$K45/smp.log" 25 -smp 4 -cpu max
grep -q "\[KASLR\] on: .*entropy=RDRAND" "$K45/smp.log"; check "M45 entropy from RDRAND when the CPU has it (-cpu max)" $?
grep -q "\[SMP\] online cpus=0x0000000000000004" "$K45/smp.log"; check "M45 all 4 CPUs online on the slid kernel (-smp 4)" $?
! grep -q "\[EXC\]" "$K45/smp.log"; check "no CPU exception ([EXC]) during M45 -smp 4 run" $?
# `nokaslr` on the multiboot2 command line.
make iso CFLAGS_EXTRA="-DSECOS_KASLR_DEBUG -DSECOS_SMP_AT_BOOT" KERNEL_CMDLINE=nokaslr >/tmp/secos_selftest_build.log 2>&1
echo "[selftest] Running M45 nokaslr (mb2, 15s)..."
run_iso "$K45/nok.log" 15
grep -q "\[KASLR\] off (nokaslr boot option)" "$K45/nok.log"; check "M45 MB2 'nokaslr' keeps the link base" $?
grep -q "\[WX\] kernel image W^X verified (0 violations)" "$K45/nok.log"; check "M45 MB2 nokaslr boot reaches the W^X audit" $?
make iso CFLAGS_EXTRA="-DSECOS_KASLR_DEBUG -DSECOS_SMP_AT_BOOT" >/tmp/secos_selftest_build.log 2>&1
# UEFI: the loader relocates. Two boots (slides differ), one with virtio-rng
# (EFI_RNG_PROTOCOL), one with `nokaslr` in the load options (startup.nsh).
OVMF45=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/OVMF/x64/OVMF.fd \
         /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/edk2-ovmf/OVMF_CODE.fd; do
    [[ -f "$f" ]] && { OVMF45="$f"; break; }
done
if [[ -n "$OVMF45" ]] && make uefi >/tmp/secos_selftest_build.log 2>&1; then
    run_uefi45() { # run_uefi45 <esp> <tag> [qemu args]
        local esp="$1" tag="$2"; shift 2
        : > "$K45/u_$tag.ser"; : > "$K45/u_$tag.dbg"
        timeout 35 qemu-system-x86_64 -machine q35,accel=tcg -m 256M -bios "$OVMF45" \
            -drive format=raw,file=fat:rw:"$esp"/ \
            -serial file:"$K45/u_$tag.ser" -debugcon file:"$K45/u_$tag.dbg" \
            -global isa-debugcon.iobase=0xe9 -no-reboot -no-shutdown -display none -net none "$@" >/dev/null 2>&1
    }
    # One fresh ESP per boot: OVMF stores NvVars (boot order, device paths) on
    # a writable ESP, and a stale entry sends a boot with a different PCI
    # layout (virtio-rng) to the UEFI shell.
    for t in a b rng; do cp -r dist "$K45/esp_$t"; done
    mkdir -p "$K45/esp_nok/EFI/SECOS"
    cp dist/EFI/BOOT/BOOTX64.EFI "$K45/esp_nok/EFI/SECOS/SECOS.EFI"; cp dist/kernel.elf "$K45/esp_nok/"
    printf 'fs0:\r\n\\EFI\\SECOS\\SECOS.EFI nokaslr\r\n' > "$K45/esp_nok/startup.nsh"
    echo "[selftest] Running M45 KASLR under UEFI (3 boots + nokaslr, 35s each)..."
    run_uefi45 "$K45/esp_a" a
    run_uefi45 "$K45/esp_b" b
    run_uefi45 "$K45/esp_rng" rng -device virtio-rng-pci
    run_uefi45 "$K45/esp_nok" nok
    ua=$(grep -ao "\[KASLR-DEBUG\] slide=0x[0-9a-f]*" "$K45/u_a.dbg" | sed 's/.*=//')
    ub=$(grep -ao "\[KASLR-DEBUG\] slide=0x[0-9a-f]*" "$K45/u_b.dbg" | sed 's/.*=//')
    ur=$(grep -ao "\[KASLR-DEBUG\] slide=0x[0-9a-f]*" "$K45/u_rng.dbg" | sed 's/.*=//')
    echo "  UEFI slides: $ua $ub $ur"
    grep -q "\[KASLR\] on: .*by UEFI loader" "$K45/u_a.dbg" && grep -q "\[KASLR\] on: .*by UEFI loader" "$K45/u_b.dbg"
    check "M45 UEFI: the loader randomizes the kernel base" $?
    [[ -n "$ua" && -n "$ub" && -n "$ur" ]] && [[ $(printf '%s\n' "$ua" "$ub" "$ur" | sort -u | wc -l) -ge 2 ]]
    check "M45 UEFI: slides differ across boots" $?
    grep -aq "\[SECURE BOOT\] kernel signature OK" "$K45/u_a.ser"; check "M45 UEFI: secure boot still verifies the KASLR kernel" $?
    m=$(grep -ao "\[M37\] kernel code measurement (sha256 .text+.rodata): [0-9a-f]*" "$K45/u_a.dbg" | awk '{print $NF}')
    [[ -n "$m" && "$m" == "$k45_meas" ]]; check "M45 UEFI: measurement equals the MB2 one and the file hash" $?
    grep -q "\[WX\] kernel image W^X verified (0 violations)" "$K45/u_a.dbg"; check "M45 UEFI: W^X audit clean at the slid base" $?
    grep -q "\[SSP\] stack canary randomized at boot" "$K45/u_a.dbg"; check "M45 UEFI: stack canary randomized by the loader" $?
    grep -q "\[KASLR\] on: .*entropy=EFI_RNG" "$K45/u_rng.dbg"; check "M45 UEFI: entropy from EFI_RNG_PROTOCOL (virtio-rng)" $?
    grep -q "\[KASLR\] off (nokaslr boot option)" "$K45/u_nok.dbg"; check "M45 UEFI: 'nokaslr' load option keeps the link base" $?
    ! grep -q "\[EXC\]" "$K45/u_a.dbg" "$K45/u_b.dbg" "$K45/u_rng.dbg" "$K45/u_nok.dbg"; check "no CPU exception ([EXC]) during the M45 UEFI runs" $?
else
    echo "  [SKIP] M45 UEFI KASLR (OVMF missing or make uefi failed)"
fi
# Build-time opt-out.
echo "[selftest] Building M45 opt-out image (SECOS_NO_KASLR)..."
build_demo M45-off "-DSECOS_NO_KASLR"
run_iso "$K45/off.log" 15
grep -q "\[KASLR\] off (built with SECOS_NO_KASLR)" "$K45/off.log"; check "M45 -DSECOS_NO_KASLR boots at the link base" $?
grep -q "\[SSP\] stack canary randomized at boot" "$K45/off.log"; check "M45 SECOS_NO_KASLR still randomizes the stack canary" $?
grep -q "\[WX\] kernel image W^X verified (0 violations)" "$K45/off.log"; check "M45 SECOS_NO_KASLR boot reaches the W^X audit" $?

# ---- Phase L (M36/M37) + M38/M39 boot hardening on the DEFAULT image ----
# Reproducible build (M37): the default image is built twice from clean with a
# fixed SOURCE_DATE_EPOCH; kernel.bin must be byte-identical.
MLLOG=/tmp/secos_selftest_ml.log
export SOURCE_DATE_EPOCH=1700000000
echo "[selftest] Building default image twice (SOURCE_DATE_EPOCH, reproducibility)..."
build_demo M37-repro-1 ""
k1=$(sha256sum kernel.bin | cut -d' ' -f1)
build_demo M37-repro-2 ""
k2=$(sha256sum kernel.bin | cut -d' ' -f1)
unset SOURCE_DATE_EPOCH
[[ -n "$k1" && "$k1" == "$k2" ]]; check "M37 reproducible build: kernel.bin byte-identical (${k1:0:16})" $?
echo "[selftest] Running default image (mb2, -cpu Haswell for SMEP, ${TIMEOUT}s)..."
run_iso "$MLLOG" "$TIMEOUT" -cpu Haswell
# M36 exploit mitigations.
grep -q "\[M36\] mitigations: SMEP=on" "$MLLOG"; check "M36 SMEP enabled (-cpu Haswell)" $?
grep -q "\[M36\] mitigations: .*stack-canary=on" "$MLLOG"; check "M36 stack canaries reported on" $?
n_chk=$(objdump -d kernel.bin 2>/dev/null | grep -c "call.*<__stack_chk_fail>" || true)
ksyms=$(nm kernel.bin 2>/dev/null || true)
grep -q " __stack_chk_guard$" <<<"$ksyms" && [[ "$n_chk" -ge 10 ]]; check "M36 kernel instrumented with stack canaries (__stack_chk_fail calls=$n_chk)" $?
grep -q "\[WX\] low identity map: checked=0x00000000000000FF NX verified" "$MLLOG"; check "M36 low identity map NX (exec only 0-2MB)" $?
# M39 high-half kernel image W^X auditor.
grep -q "\[WX\] kernel image W^X verified (0 violations)" "$MLLOG"; check "M39 kernel image W^X audit: 0 violations" $?
# M37 measured boot: the logged SHA-256 of .text+.rodata must equal a host-side
# recomputation over the same bytes of kernel.bin.
meas=$(grep -o "\[M37\] kernel code measurement (sha256 .text+.rodata): [0-9a-f]*" "$MLLOG" | awk '{print $NF}' || true)
ts=$(awk '$3=="_text_start"{print $1}' <<<"$ksyms")
re=$(awk '$3=="_rodata_end"{print $1}' <<<"$ksyms")
toff=$(readelf -SW kernel.bin | awk '{for(i=1;i<NF;i++) if($i==".text") print $(i+3)}' || true)
host_meas=""
if [[ -n "$ts" && -n "$re" && -n "$toff" ]]; then
    host_meas=$(tail -c +$(( 16#$toff + 1 )) kernel.bin | head -c $(( 16#$re - 16#$ts )) | sha256sum | cut -d' ' -f1 || true)
fi
[[ ${#meas} -eq 64 ]]; check "M37 boot measurement logged (sha256 .text+.rodata)" $?
[[ -n "$meas" && "$meas" == "$host_meas" ]]; check "M37 boot measurement matches host-side sha256 of kernel.bin" $?
grep -q "\[M37\] measured boot OK" "$MLLOG"; check "M37 measured boot OK" $?
# M45 KASLR is on in the default image, and the slide is never logged there.
grep -q "\[KASLR\] on: kernel base randomized" "$MLLOG"; check "M45 default image: KASLR on" $?
grep -q "\[SSP\] stack canary randomized at boot" "$MLLOG"; check "M45 default image: stack canary randomized at boot" $?
! grep -aq "KASLR-DEBUG" "$MLLOG" kernel.bin; check "M45 default image never logs the slide" $?
# M38 WPA2 crypto KATs + M39 association-core KATs (run at boot in crypto selftest).
grep -q "\[WPA2\] PBKDF2 PMK KAT PASS" "$MLLOG"; check "M38 WPA2 PBKDF2-SHA1 PMK KAT (IEEE 802.11i)" $?
grep -q "\[WPA2\] AES-128 KAT PASS" "$MLLOG"; check "M38 AES-128 KAT (FIPS-197)" $?
grep -q "\[WPA2\] RFC3394 key-wrap KAT PASS" "$MLLOG"; check "M39 RFC3394 key-wrap KAT" $?
grep -q "\[WPA2\] RFC3394 key-unwrap KAT PASS" "$MLLOG"; check "M39 RFC3394 key-unwrap KAT" $?
grep -q "\[WPA2\] PTK derivation (KCK|KEK|TK) OK" "$MLLOG"; check "M39 WPA2 PTK derivation (PRF)" $?
grep -q "\[WPA2\] 4-way handshake supplicant KAT PASS" "$MLLOG"; check "M39 synthetic WPA2 4-way handshake KAT" $?
grep -q "\[CRYPTO\] --- all PASS ---" "$MLLOG"; check "M38/M39 boot crypto suite all PASS" $?
! grep -q "\[EXC\]" "$MLLOG"; check "no CPU exception ([EXC]) during default -cpu Haswell boot" $?
# [M44] Haswell has SMEP but no SMAP: the kernel must fall back cleanly. The
# low identity map is retired after early boot and absent from user CR3s.
grep -q "\[SMAP\] not supported by this CPU (fallback: no SMAP)" "$MLLOG"; check "M44 SMAP fallback on a CPU without SMAP (-cpu Haswell)" $?
grep -q "\[IDMAP\] low identity map removed from the kernel page tables" "$MLLOG"; check "M44 low identity map removed from the kernel page tables" $?
grep -q "\[IDMAP\] user CR3 audit: no low kernel mappings (OK)" "$MLLOG"; check "M44 user CR3 carries no low kernel mapping" $?
! objdump -d kernel.bin 2>/dev/null | grep -q "%gs:0x28"; check "M44 stack canary not read from %gs:0x28 (global guard)" $?
# [M44] SMAP on by default: the same image on -cpu max (SMAP present), single
# and multi core (-smp 4 brings the APs up only with SECOS_SMP_AT_BOOT; the
# M29 gates cover that build).
SMAPLOG=/tmp/secos_selftest_smap.log
echo "[selftest] Running default image (mb2, -cpu max for SMAP, 35s)..."
run_iso "$SMAPLOG" 35 -cpu max
grep -q "\[SMAP\] enabled: kernel user access only via uaccess" "$SMAPLOG"; check "M44 SMAP enabled by default (-cpu max)" $?
grep -q "\[M36\] mitigations: SMEP=on SMAP=on" "$SMAPLOG"; check "M44 SMEP+SMAP both on (-cpu max)" $?
grep -q "\[IDMAP\] user CR3 audit: no low kernel mappings (OK)" "$SMAPLOG"; check "M44 user CR3 audit OK under SMAP" $?
grep -q "\[INIT\]\|\[init\]" "$SMAPLOG"; check "M44 boot reaches init under SMAP" $?
! grep -q "\[EXC\]\|\[PANIC\]\|\[SMAP\] violation" "$SMAPLOG"; check "no exception / SMAP violation during the -cpu max boot" $?

# ---- M39: boot into GNU bash (built from source) + run a command over serial ----
# The default image installs the signed bash to /bin/bash and launches it as the
# login shell. Feed one line over the serial console; bash's output (console fd 1)
# is mirrored to debugcon. The arithmetic expansion proves bash evaluated it (TTY
# echo alone would show the unexpanded text). Serial input drops the first char
# of a line under QEMU, hence the leading spaces.
BASHLOG=/tmp/secos_selftest_bash.log
echo "[selftest] Running boot-into-bash (mb2, serial input, 40s)..."
: > "$BASHLOG"
( sleep 25; printf '  echo BASH-$((6*7))-OK\r'; sleep 10 ) | \
    timeout 40 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -debugcon file:"$BASHLOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
grep -q "\[M39\] installed GNU bash to /bin/bash" "$BASHLOG"; check "M39 signed bash installed to /bin/bash at boot" $?
grep -q "BASH-42-OK" "$BASHLOG"; check "M39 bash login shell evaluates a command (\$((6*7)) -> 42)" $?
! grep -q "\[EXC\]" "$BASHLOG"; check "no CPU exception ([EXC]) during boot-into-bash" $?

# ---- Regression: fb console cursor overflow -> heap/PML4 corruption ----
# Lines longer than one screen row, printed at the bottom of the screen, used to
# make the blinking-cursor draw write past the end of the RAM back buffer (cursor
# drawn at cursor_x == cells_per_row before the wrap), corrupting the next heap
# block; a later fork's kmalloc then overwrote the parent's live PML4 and the
# kernel froze after a few bash commands. Print 60 over-long lines (fills the
# 48-row screen, wrapping on the last row), then run 24 fork+exec commands and a
# final arithmetic echo: it must be evaluated with no CPU exception.
BURSTLOG=/tmp/secos_selftest_bash_burst.log
echo "[selftest] Running bash long-line + fork burst (mb2, serial input, 60s)..."
: > "$BURSTLOG"
LONGLINE=$(printf 'W%.0s' $(seq 1 150))
( sleep 25
  printf '  for i in {1..60}; do echo %s; done\r' "$LONGLINE"; sleep 4
  for i in $(seq 1 12); do printf '  help\r'; sleep 0.8; printf '  ls\r'; sleep 0.8; done
  printf '  echo BURST-$((6*7))-OK\r'; sleep 4 ) | \
    timeout 60 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -debugcon file:"$BURSTLOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
grep -q "BURST-42-OK" "$BURSTLOG"; check "bash survives long wrapped lines + 24 fork/exec commands (fb cursor overflow regression)" $?
! grep -q "\[EXC\]" "$BURSTLOG"; check "no CPU exception ([EXC]) during long-line + fork burst" $?

# ---- Regression: bash command substitution, pipelines, background wait ----
# `$(...)` and plain pipelines used to hang forever under bash:
#  - an exiting child kept its pipe write end until its zombie was reaped, and
#    bash reads a substitution pipe to EOF *before* wait()ing -> no EOF;
#  - an exec'd (spawned) program didn't inherit bash's dup2'd stdio, so
#    `$(uname)` / `seq | wc` wrote to the console instead of the pipe;
#  - zombies were only reaped from idle, so a busy loop piled them up until the
#    27th process's kernel stack overlapped IST3 and the kernel halted;
#  - waitpid(-1) counted collected zombies as children -> `cmd & wait` hung.
# Each CSn marker only appears once expanded (the TTY echo shows the raw text);
# output may follow the "# " prompt on the same line, so markers are unanchored.
# The two 50-iteration loops (builtin + exec'd substitutions) would exhaust the
# 16-entry pipe table / 32-entry process table on any per-use leak, and MemFree
# must not drift between them.
CSLOG=/tmp/secos_selftest_cmdsubst.log
echo "[selftest] Running bash command substitution + pipelines (mb2, serial input, 120s)..."
: > "$CSLOG"
( sleep 25
  printf '  echo CS1-$(echo SUB)\r'; sleep 3
  printf '  x=$(echo hi); echo CS2-$x\r'; sleep 3
  printf '  echo CS3-$(echo $(echo deep))\r'; sleep 3
  printf '  echo CS4-`uname`\r'; sleep 3
  printf '  echo CS5-$(seq 1 3 | wc -l)\r'; sleep 3
  printf '  for i in $(seq 1 3); do echo CS6-$i; done\r'; sleep 3
  printf '  seq 1 50 | wc\r'; sleep 3
  printf '  echo CS10-$(ls /bin | wc -l)\r'; sleep 3
  printf '  seq 1 100 | head -5 | wc -l; echo CS11-$(seq 1 100 | head -5 | wc -l)\r'; sleep 4
  printf '  for i in $(seq 1 6); do sleep 1 & done; wait; echo CS12-$((6*7))\r'; sleep 8
  printf '  for i in $(seq 1 50); do x=$(echo s$i); y=$(uname); done; echo CS7-$x-$y\r'; sleep 10
  printf '  cat /proc/meminfo\r'; sleep 3
  printf '  for i in $(seq 1 50); do x=$(echo t$i); y=$(uname); done; echo CS8-$x-$y\r'; sleep 10
  printf '  cat /proc/meminfo\r'; sleep 3
  printf '  echo CS9-$((6*7))\r'; sleep 3 ) | \
    timeout 120 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -debugcon file:"$CSLOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
grep -q "CS1-SUB" "$CSLOG"; check "bash \$(echo SUB) expands (no pipe-EOF hang)" $?
grep -q "CS2-hi" "$CSLOG"; check "bash x=\$(echo hi); echo \$x" $?
grep -q "CS3-deep" "$CSLOG"; check "bash nested \$(echo \$(echo deep))" $?
grep -q "CS4-SecOS" "$CSLOG"; check "bash backticks capture an exec'd program (\`uname\`)" $?
grep -q "CS5-3$" "$CSLOG"; check "bash \$(seq 1 3 | wc -l) -> 3" $?
[[ "$(grep -Ec '(^|# )CS6-[123]$' "$CSLOG")" -eq 3 ]]; check "bash for i in \$(seq 1 3) iterates 3 times" $?
grep -Eq "(^|# ) +50 +50 +141$" "$CSLOG"; check "bash pipeline seq 1 50 | wc -> 50 50 141" $?
grep -Eq "CS10-[1-9][0-9]*$" "$CSLOG"; check "bash \$(ls /bin | wc -l) counts /bin entries" $?
grep -q "CS11-5$" "$CSLOG"; check "bash 3-stage seq 1 100 | head -5 | wc -l -> 5 (early reader exit)" $?
grep -q "CS12-42" "$CSLOG"; check "bash 6 background jobs + wait returns (no ECHILD hang)" $?
grep -q "CS7-s50-SecOS" "$CSLOG"; check "bash 50x builtin + exec'd substitutions (no pipe/process-table leak)" $?
grep -q "CS8-t50-SecOS" "$CSLOG"; check "bash another 50x substitutions" $?
mf=( $(grep -o "^MemFree: *[0-9]*" "$CSLOG" | awk '{print $2}') )
mf_drift=999999
[[ ${#mf[@]} -ge 2 ]] && mf_drift=$(( mf[0] - mf[1] ))
[[ $mf_drift -lt 256 ]]; check "no memory leak across 50 substitutions (MemFree ${mf[0]:-?} -> ${mf[1]:-?} kB)" $?
grep -q "CS9-42" "$CSLOG"; check "bash still responsive after the substitution loops" $?
! grep -q "\[EXC\]" "$CSLOG"; check "no CPU exception ([EXC]) during command substitution / pipelines" $?

# ============================================================================
# STRESS/FUZZ GATES (short, fixed-seed versions of tools/soak.sh + the signed
# ring-3 syscall fuzzer user/sysfuzz.c). The LONG versions stay manual:
#   tools/soak.sh --long            # ~10-15 min randomized bash session
#   /bin/sysfuzz -s <seed> -b N -c M        (from the shell)
# These gates reuse the already-built myos.iso. The fuzzer runs in FULL mode:
# every pointer class (NULL, kernel half, non-canonical, unmapped hole,
# PROT_NONE, buffers running off a mapping) reaches every syscall, and must come
# back as an error (uaccess exception-table fixups -> -EFAULT), never a ring-0
# fault. Its arguments are shaped only to stay non-blocking (see user/sysfuzz.c).
# ============================================================================
# ---- stress: ring-3 syscall fuzzer (full mode, fixed seed) ----
FUZZLOG=/tmp/secos_selftest_sysfuzz.log
echo "[selftest] Running ring-3 syscall fuzzer (full mode, mb2, serial, 80s)..."
: > "$FUZZLOG"
( sleep 25; printf '  /bin/sysfuzz -s 777 -b 12 -c 150; echo FUZZEND\r'; sleep 28
  printf '  /bin/sysfuzz -s 33 -b 24 -c 120 > /dev/null; /bin/echo FZALIVE""-42\r'; sleep 15 ) | \
    timeout 80 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -debugcon file:"$FUZZLOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
grep -q "\[sysfuzz\] DONE ok" "$FUZZLOG"; check "syscall fuzzer (full mode) ran every syscall to completion" $?
# seed 33 batch 23 once umount()ed "/" -> empty namespace, every later exec failed.
grep -q "^FZALIVE-42" "$FUZZLOG"; check "system still execs /bin programs after fuzzing (root not unmountable)" $?
! grep -q "ring0" "$FUZZLOG"; check "no ring-0 CPU fault during full-mode syscall fuzzing" $?
! grep -q "\[PANIC\]" "$FUZZLOG"; check "no kernel panic during full-mode syscall fuzzing" $?

# ---- stress: short randomized soak (fixed seed) via tools/soak_drive.py ----
SOAKLOG=/tmp/secos_selftest_soak.log
echo "[selftest] Running short randomized bash soak (mb2, serial, fixed seed)..."
: > "$SOAKLOG"
if python3 tools/soak_drive.py --seed 4242 --iters 60 --settle 28 --stall 25 \
        --log "$SOAKLOG" -- \
        qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -debugcon "file:$SOAKLOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/tmp/secos_selftest_soak_run.log 2>&1; then
    check "randomized bash soak (60 commands) completed with all sentinels" 0
else
    check "randomized bash soak (60 commands) completed with all sentinels" 1
    tail -5 /tmp/secos_selftest_soak_run.log | sed 's/^/    /'
fi
! grep -q "ring0\|\[PANIC\]" "$SOAKLOG"; check "no kernel panic/ring-0 fault during bash soak" $?

# ---- Regression: bash child exit >= 128 / killed by signal; stale stat ABI ----
# (a) strsignal() was implicitly declared in the bash build, so bash truncated
#     its returned pointer to 32 bits and crashed in strlen() reporting a child
#     that exited >= 128 or died by a signal.
# (b) a coreutils `stat` built against the old 56-byte struct stat had its
#     return address overwritten by the kernel's 88-byte SYS_STAT copy-out.
# Markers only appear once expanded (the TTY echo shows the raw `$?` text).
UBLOG=/tmp/secos_selftest_userland.log
echo "[selftest] Running bash exit-status/signal + stat regressions (mb2, serial, 85s)..."
: > "$UBLOG"
( sleep 25
  printf "  bash -c 'exit 142'; echo UB1-\$?\r"; sleep 4
  printf '  sleep 10 & kill -9 $!; wait $!; echo UB2-$?\r'; sleep 4
  printf '  sleep 10 & kill -TERM $!; wait $!; echo UB3-$?\r'; sleep 4
  printf '  stat /bin/bash && echo UB4-$((2+2))\r'; sleep 5
  printf '  echo UB6-$(head -c 32 /dev/zero | wc -c)\r'; sleep 4
  printf '  /bin/abi_test segv; echo UB7-$?\r'; sleep 4
  printf '  for i in 1 2 3; do sleep 1 & done\r'; sleep 6
  printf '  echo UB5-$((6*7))\r'; sleep 4 ) | \
    timeout 85 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -debugcon file:"$UBLOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
grep -q "UB1-142" "$UBLOG"; check "bash survives bash -c 'exit 142' and reports \$? = 142" $?
! grep -q "Signal 14" "$UBLOG"; check "exit 142 is reported as an exit, not as a signal (POSIX wait status)" $?
grep -q "UB7-139" "$UBLOG"; check "a ring-3 #PF kill is reported as SIGSEGV (\$? = 139)" $?
grep -q "UB2-137" "$UBLOG"; check "bash survives kill -9 of a child (\$? = 137)" $?
grep -q "UB3-143" "$UBLOG"; check "bash survives SIGTERM of a child (\$? = 143)" $?
grep -q "File: /bin/bash" "$UBLOG" && grep -q "UB4-4" "$UBLOG"; check "coreutils stat /bin/bash works (stat ABI)" $?
grep -q "UB6-32" "$UBLOG"; check "stdio flushed at exit (head -c 32 /dev/zero | wc -c -> 32)" $?
grep -q "UB5-42" "$UBLOG"; check "bash survives SIGCHLDs while idle at the prompt (tty -EINTR)" $?
# Exactly one CPU exception: the deliberate ring-3 #PF of `abi_test segv` (UB7).
[[ "$(grep -c "\[EXC\] int=" "$UBLOG")" -eq 1 ]] && ! grep -q "\[EXC\].*ring0" "$UBLOG"
check "no unexpected CPU exception ([EXC]) during exit-status/stat regressions" $?

# ============================================================================
# ---- lua in /bin + bash job control (jobs/fg/bg/%n/wait/kill/disown) ----
# ============================================================================
# lua 5.4.7 is installed to /bin/lua at boot; bash is built with job control.
# Async Ctrl-Z/Ctrl-C can't be sent over the polled serial line, so the job is
# stopped/continued with kill -STOP/-CONT %1. Markers only appear once
# expanded (the TTY echo shows the raw text). Also guards two kernel fixes:
# a console reader blocks instead of spinning in ring 0 (else background jobs
# never ran while bash sat at its prompt: LJ6 needs `sleep 2 &` to finish on
# its own), and a background TTY reader gets SIGTTIN (stops, LJ7).
LJLOG=/tmp/secos_selftest_luajobs.log
echo "[selftest] Running lua + bash job control (mb2, serial input, 95s)..."
: > "$LJLOG"
( sleep 25
  printf "  echo LJ1-\$(lua -e 'print(6*7)')\r"; sleep 4
  printf '  echo LJ2-$(type -t jobs fg bg)\r'; sleep 4
  printf '  sleep 30 &\r'; sleep 3
  printf '  j=$(jobs); [[ $j == *Running* ]] && echo LJ3-run\r'; sleep 3
  printf '  kill -STOP %%1; sleep 1; j=$(jobs); [[ $j == *Stopped* ]] && echo LJ4-stopped\r'; sleep 4
  printf '  kill -CONT %%1; j=$(jobs); [[ $j == *Running* ]] && echo LJ5-run\r'; sleep 3
  printf '  kill %%1; wait %%1; echo LJW-$?\r'; sleep 4
  printf '  sleep 2 &\r'; sleep 6
  printf '  j=$(jobs); [[ $j != *Running* ]] && echo LJ6-finished\r'; sleep 3
  printf '  cat &\r'; sleep 3
  printf '  j=$(jobs); [[ $j == *Stopped* ]] && echo LJ7-ttin; kill -9 %%1; sleep 1; wait\r'; sleep 5
  printf '  sleep 3 & kill -STOP %%%%; sleep 1; fg; echo LJ8-$?\r'; sleep 9
  printf "  echo 'print(io.read(\"l\"))' > /tmp/lj.lua; echo LJ9-\$(echo hi | lua /tmp/lj.lua)\r"; sleep 4
  printf '  echo LJ-$((6*7))-END\r'; sleep 3 ) | \
    timeout 95 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -debugcon file:"$LJLOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
grep -q "\[M34\] installed lua to /bin/lua" "$LJLOG"; check "lua installed to /bin/lua at boot" $?
grep -q "LJ1-42" "$LJLOG"; check "lua -e 'print(6*7)' -> 42 from bash" $?
grep -q "LJ2-builtin builtin builtin" "$LJLOG"; check "bash: jobs/fg/bg are shell builtins (type)" $?
grep -q "LJ3-run" "$LJLOG"; check "bash: jobs lists the background job (Running)" $?
grep -q "LJ4-stopped" "$LJLOG"; check "bash: kill -STOP %1 -> jobs shows Stopped" $?
grep -q "LJ5-run" "$LJLOG"; check "bash: kill -CONT %1 -> job Running again" $?
grep -q "LJW-143" "$LJLOG"; check "bash: kill %1; wait %1 returns (\$? = 143)" $?
grep -q "LJ6-finished" "$LJLOG"; check "background job finishes while bash idles at the prompt" $?
grep -q "LJ7-ttin" "$LJLOG"; check "background TTY reader stops on SIGTTIN" $?
grep -q "LJ8-0" "$LJLOG"; check "bash: fg resumes a stopped job and waits for it" $?
grep -q "LJ9-hi" "$LJLOG"; check "lua script reads stdin through io.read" $?
grep -q "LJ-42-END" "$LJLOG"; check "bash responsive after the job-control sequence" $?
! grep -q "\[EXC\]" "$LJLOG"; check "no CPU exception ([EXC]) during lua/job-control run" $?

# ============================================================================
# ---- bash over the real execve: pids, exec replacing the shell, scripts ----
# ============================================================================
# bash's fork+execve now replaces the forked child in place, so $! is the pid
# of the program itself (the kernel logs "[EXEC] execve pid=<p> path=..."),
# `exec cmd` replaces bash (init respawns the console shell when cmd
# exits), a script without an ELF header gets ENOEXEC and bash runs it, and the
# environment crosses exec. Markers only appear once expanded.
BXLOG=/tmp/secos_selftest_bashexec.log
BXSER=/tmp/secos_selftest_bashexec.serial
echo "[selftest] Running bash over the real execve (mb2, serial input, 70s)..."
: > "$BXLOG"
( sleep 25
  printf "  bash -c 'exec echo BX1-\$((40+2))'\r"; sleep 4
  printf "  (exec bash -c 'echo \$\$ > /tmp/bxpid') & wait; [[ \$(cat /tmp/bxpid) == \$! ]] && echo BX2-same-\$((1+1))\r"; sleep 5
  printf '  sleep 30 & p=$!; printf -v h %%016X $p; sleep 1; kill -STOP $p; sleep 1; j=$(jobs -l); [[ $j == *$p*Stopped* ]] && echo BX3-$h-stopped; kill -9 $p; wait\r'; sleep 5
  printf '  bash -c "exec /bin/nonexist" 2>/dev/null; echo BX4-rc-$?\r'; sleep 4
  printf '  printf "echo BX5-\\$((2+3))\\n" > /tmp/bx.sh; chmod +x /tmp/bx.sh; bash -c /tmp/bx.sh\r'; sleep 4
  printf '  BXV=bar$((40+2)) bash -c "echo BX6-\\$BXV"\r'; sleep 4
  printf '  exec /bin/echo BX7-$((3+4))\r'; sleep 5
  printf '  uptime\r'; sleep 3 ) | \
    timeout 70 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -debugcon file:"$BXLOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio > "$BXSER" 2>&1
grep -q "BX1-42" "$BXLOG"; check "bash -c 'exec echo' runs the exec'd program" $?
grep -q "BX2-same-2" "$BXLOG"; check "bash: \$! is the exec'd program's own pid" $?
BXH=$(grep -o "BX3-[0-9A-F]\{16\}-stopped" "$BXLOG" | head -1 | cut -d- -f2)
[[ -n "$BXH" ]] && grep -q "\[EXEC\] execve pid=0x$BXH path=/bin/sleep" "$BXLOG"
check "bash: kill -STOP \$! stops the real program (pid exec'd /bin/sleep)" $?
grep -q "BX4-rc-127" "$BXLOG"; check "bash: exec of a missing file fails with 127" $?
grep -q "BX5-5" "$BXLOG"; check "bash: a script (ENOEXEC) runs through bash" $?
grep -q "BX6-bar42" "$BXLOG"; check "bash: environment crosses execve" $?
grep -q "BX7-7" "$BXLOG"; check "bash: exec replaces the login shell" $?
sed -n '/BX7-7/,$p' "$BXSER" | grep -q "\[init\] started console"
check "init respawns the console shell after the exec'd program exits" $?
! grep -q "\[EXC\]" "$BXLOG"; check "no CPU exception ([EXC]) during bash execve run" $?

# ============================================================================
# ---- Line editing: console termios (raw mode) + bash readline over serial ----
# ============================================================================
# (a) /bin/ttytest drives the console TTY through raw mode: three VMIN=1 single
#     byte reads, ESC [ A arriving whole (FIONREAD), Ctrl-C as data with ISIG
#     off, a VMIN=0/VTIME timeout, TIOCGWINSZ, restore, and a canonical read
#     with an erase. Each input below answers one TT-*-READY prompt.
# (b) bash's GNU readline: Up arrow re-runs the previous line, Left arrow /
#     Home / Delete / Ctrl-A/D/K/W/U edit the line, Tab completes a file name
#     (and double Tab lists the candidates), Ctrl-R searches the history, and
#     the bind builtin exists. Every marker only appears once bash evaluated
#     the EDITED line (the typed text never contains the expanded value).
LELOG=/tmp/secos_selftest_lineedit.log
echo "[selftest] Running console termios + bash readline editing (mb2, serial, 110s)..."
: > "$LELOG"
( sleep 25
  printf '  /bin/ttytest\r'; sleep 3
  printf 'abc'; sleep 2
  printf '\033[A'; sleep 2
  printf '\003'; sleep 3
  printf 'xy\177z\r'; sleep 3
  printf '  n=0\r'; sleep 2
  printf '  n=$((n+1)); echo LE1-$n\r'; sleep 3
  printf '\033[A\r'; sleep 3
  printf '  echo LE2-$((6*))\033[D\033[D7\r'; sleep 3
  printf '  echo LE3-$(/bin/unam\t -s)\r'; sleep 3
  printf '  /bin/ba\t\t\t'; sleep 3
  printf '\025  echo LE4-$((2*2))\r'; sleep 3
  printf 'XX  echo LE5-$((5*5))\001\004\004\r'; sleep 3
  printf '\022n+1'; sleep 2
  printf '\r'; sleep 3
  printf '   LE7-$((3+4))\033[Hecho\r'; sleep 3
  printf '  echo LE8-$((9+1))X\033[D\033[3~\r'; sleep 3
  printf '  bind -q beginning-of-line; echo LE9-$((4*2))\r'; sleep 3
  printf '  echo LE10-$((5+5))JUNK\033[D\033[D\033[D\033[D\013\r'; sleep 3
  printf '  echo LE11-$((2+9)) junkword\027\r'; sleep 3
  printf '  echo LE12-$TERM\r'; sleep 3
  printf '  echo LE-$((6*7))-END\r'; sleep 4 ) | \
    timeout 110 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -debugcon file:"$LELOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
grep -q "TT-WINSZ-OK" "$LELOG"; check "tty: TIOCGWINSZ reports the console size" $?
grep -q "TT-RAW-OK" "$LELOG"; check "tty: raw mode (ICANON/ECHO off, VMIN=1) single-byte reads" $?
grep -q "TT-ESC-OK" "$LELOG"; check "tty: ESC [ A arrives whole (FIONREAD after the ESC)" $?
grep -q "TT-ISIG-OK" "$LELOG"; check "tty: ISIG off delivers Ctrl-C as a byte" $?
grep -q "TT-VTIME-OK" "$LELOG"; check "tty: VMIN=0/VTIME read times out with 0" $?
grep -q "TT-RESTORE-OK" "$LELOG"; check "tty: saved termios restored (ICANON|ECHO|ISIG)" $?
grep -q "TT-CANON-OK" "$LELOG"; check "tty: canonical read with DEL erase -> one edited line" $?
grep -q "TT-DONE 8/8" "$LELOG"; check "tty: ttytest 8/8" $?
grep -q "LE1-1" "$LELOG" && grep -q "LE1-2" "$LELOG"; check "readline: Up arrow + Enter re-runs the previous command" $?
grep -q "LE2-42" "$LELOG"; check "readline: Left arrow moves the cursor for a mid-line insert" $?
grep -q "LE3-SecOS" "$LELOG"; check "readline: Tab completes /bin/unam -> /bin/uname" $?
grep -Eq "basename +bash" "$LELOG"; check "readline: repeated Tab lists /bin/ba* (bash, basename)" $?
grep -q "LE4-4" "$LELOG"; check "readline: Ctrl-U kills the line" $?
grep -q "LE5-25" "$LELOG"; check "readline: Ctrl-A + Ctrl-D edit the start of the line" $?
grep -q "LE1-3" "$LELOG"; check "readline: Ctrl-R reverse search finds and runs a history line" $?
grep -q "LE7-7" "$LELOG"; check "readline: Home key (ESC [ H) moves to the start" $?
grep -q "LE8-10" "$LELOG"; check "readline: Delete key (ESC [ 3 ~) deletes under the cursor" $?
grep -q "can be invoked via" "$LELOG" && grep -q "LE9-8" "$LELOG"; check "readline: bind builtin available" $?
grep -q "LE10-10" "$LELOG"; check "readline: Ctrl-K kills to end of line" $?
grep -q "LE11-11" "$LELOG"; check "readline: Ctrl-W kills the previous word" $?
grep -q "LE12-linux" "$LELOG"; check "bash: TERM set for the console (/etc/bash.bashrc)" $?
grep -q "LE-42-END" "$LELOG"; check "bash responsive after the line-editing session" $?
! grep -q "\[EXC\]" "$LELOG"; check "no CPU exception ([EXC]) during the line-editing session" $?

# ============================================================================
# ---- Pseudo-terminals: /dev/ptmx + /dev/pts, sessions, ctty, bash on a pty ----
# ============================================================================
# (a) /bin/ptytest (signed): posix_openpt/unlockpt/ptsname, a locked slave is
#     refused; forkpty child = new session with the slave as controlling tty
#     (ttyname, /dev/tty, tcgetpgrp); canonical I/O through the master with
#     ICRNL/ONLCR/echo; ^C written to the master -> SIGINT; TIOCSWINSZ ->
#     exactly one SIGWINCH; raw mode on the slave; master close -> SIGHUP and
#     slave EIO; child exit -> master EIO; 100 openpty/close + 20 forkpty
#     cycles reuse slot 0 (no leak); bash -c 'tty' prints /dev/pts/N; an
#     interactive bash on a pty runs a line, ^C ends `sleep 100`, exits.
# (b) /bin/script runs an interactive bash on a pty and relays the console to
#     it (raw console, both directions); the inner bash's `tty` is /dev/pts/0,
#     the outer one is back on /dev/console afterwards.
# (c) the fuzzer also hammers pty ends (open/read/write/ioctl, setsid/getsid).
PTYLOG=/tmp/secos_selftest_pty.log
echo "[selftest] Running pseudo-terminal tests + script relay (mb2, serial, 110s)..."
: > "$PTYLOG"
( sleep 25
  printf '  /bin/ptytest\r'; sleep 40
  printf '  script\r'; sleep 6
  printf 'echo SCRIPT-$((6*7)); tty\r'; sleep 3
  printf 'exit\r'; sleep 4
  printf '  echo PTY-AFTER-$((1+1)); tty\r'; sleep 3
  printf '  /bin/sysfuzz -s 4242 -b 30 -c 200 > /dev/null; /bin/echo FZPTY""-42\r'; sleep 25 ) | \
    timeout 110 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -debugcon file:"$PTYLOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
for t in OPEN SESSION CANON SIGINT WINCH RAW HUP EOF CYCLE BASH IBASH; do
    grep -q "PT-$t-OK" "$PTYLOG"; check "pty: $t" $?
done
grep -q "PT-DONE 11/11" "$PTYLOG"; check "pty: ptytest 11/11" $?
grep -q "^SCRIPT-42" "$PTYLOG" && grep -q "^/dev/pts/0" "$PTYLOG"; check "pty: script runs bash on /dev/pts/0 and relays the console" $?
grep -q "Script done (shell status 0)" "$PTYLOG"; check "pty: script ends when its shell exits" $?
grep -q "^PTY-AFTER-2" "$PTYLOG" && grep -q "^/dev/console" "$PTYLOG"; check "pty: console bash responsive on /dev/console afterwards" $?
grep -q "^FZPTY-42" "$PTYLOG"; check "pty: fuzzing pty ends + setsid/getsid leaves the system usable" $?
! grep -q "ring0\|\[PANIC\]" "$PTYLOG"; check "no kernel panic/ring-0 fault during the pty tests" $?
! grep -q "\[EXC\].*ring0" "$PTYLOG"; check "no ring-0 CPU exception during the pty tests" $?

# ============================================================================
# ---- BusyBox 1.36.1 userland (built from source, signed, applet stubs) ----
# ============================================================================
# /bin/busybox + one signed stub per applet are installed at boot; bash runs
# them like any program. One representative command per applet family; each
# BBn marker only appears once expanded (the TTY echo shows the raw text). vi
# and less are driven in command mode only: the console TTY is cooked and drops
# control bytes (ESC), see docs/BUSYBOX_PORT.md.
BBLOG=/tmp/secos_selftest_busybox.log
echo "[selftest] Running BusyBox userland (mb2, serial input, 150s)..."
: > "$BBLOG"
( sleep 25
  printf '  echo BB1-$(echo hello world | busybox sed s/world/secos/)\r'; sleep 4
  printf "  echo BB2-\$(printf 'b\\\\na\\\\nc\\\\n' | sort | head -1)\r"; sleep 4
  printf "  echo BB3-\$(seq 1 10 | awk '{s+=\$1} END{print s}')\r"; sleep 4
  printf "  echo BB4-\$(find /bin -name 'b*' | wc -l)\r"; sleep 5
  printf "  echo BB5-\$(printf 'x\\\\ny\\\\nx\\\\n' | grep -c x)\r"; sleep 4
  printf '  cd /tmp && echo tarok > t1 && tar cf t.tar t1 && gzip t.tar && rm t1 && gunzip t.tar.gz && tar xf t.tar && echo BB6-$(cat t1)\r'; sleep 10
  printf '  echo BB7-$(echo -n abc | sha256sum | cut -c1-16)\r'; sleep 4
  printf '  echo H4sIAAAAAAAAA3MqLa50yq9QSE8sSVUoSKzMyU9M4QIAm1kjZBUAAAA= | base64 -d | zcat | sed s/^/BB8-/\r'; sleep 5
  printf '  echo BB9-$(echo bzok | bzip2 | bzcat)\r'; sleep 5
  printf '  echo hello > /tmp/v.txt; vi /tmp/v.txt\r'; sleep 4
  printf ':s/hello/edited by vi/\r'; sleep 2
  printf ':wq\r'; sleep 3
  printf '  echo BB10-$(cat /tmp/v.txt)\r'; sleep 4
  printf '  seq 1 100 > /tmp/s; less /tmp/s\r'; sleep 4
  printf 'q\r'; sleep 3
  printf '  echo BB11-$?\r'; sleep 3
  printf '  echo BB12-$(busybox --list | wc -l)-$(uname)\r'; sleep 4
  printf '  mkdir -p /tmp/d/e && touch /tmp/d/e/x && echo BB13-$(ls /tmp/d/e) && rm -r /tmp/d && test ! -e /tmp/d && echo BB14-gone\r'; sleep 8
  printf '  timeout 2 sleep 9; echo BB15-$?\r'; sleep 6
  printf '  echo BB16-$(cp -r /etc /tmp/e2 2>&1 | wc -l)-$(ls /tmp/e2 | wc -l)-$(ls /etc | wc -l)\r'; sleep 6
  printf '  echo BB-$((6*7))-END\r'; sleep 3 ) | \
    timeout 150 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -debugcon file:"$BBLOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
grep -q "\[BB\] installed BusyBox to /bin/busybox" "$BBLOG"; check "BusyBox installed to /bin/busybox with its applet stubs" $?
grep -q "BB1-hello secos" "$BBLOG"; check "busybox sed s/world/secos/" $?
grep -q "BB2-a$" "$BBLOG"; check "sort | head -1 (BusyBox sort/head)" $?
grep -q "BB3-55" "$BBLOG"; check "awk sums seq 1 10 -> 55" $?
grep -Eq "BB4-[5-9]$" "$BBLOG"; check "find /bin -name 'b*' | wc -l" $?
grep -q "BB5-2$" "$BBLOG"; check "grep -c" $?
grep -q "BB6-tarok" "$BBLOG"; check "tar + gzip round trip in /tmp (cwd-relative, unlink while open)" $?
grep -q "BB7-ba7816bf8f01cfea" "$BBLOG"; check "sha256sum of 'abc' matches the host value" $?
grep -q "BB8-BusyBox gate payload" "$BBLOG"; check "base64 -d | zcat of a host-made gzip stream" $?
grep -q "BB9-bzok" "$BBLOG"; check "bzip2 | bzcat round trip" $?
grep -q "BB10-edited by vi" "$BBLOG"; check "vi starts, edits (ex :s), writes and quits (:wq)" $?
grep -q "BB11-0" "$BBLOG"; check "less starts and quits with q" $?
grep -Eq "BB12-1[0-9][0-9]-SecOS" "$BBLOG"; check "busybox --list + uname reports SecOS" $?
grep -q "BB13-x" "$BBLOG" && grep -q "BB14-gone" "$BBLOG"; check "mkdir -p / touch / ls / rm -r" $?
grep -q "BB15-143" "$BBLOG"; check "timeout 2 sleep 9 -> 143 (SIGTERM forwarded through the stubs)" $?
grep -Eq "BB16-0-([0-9]+)-\1\b" "$BBLOG"; check "cp -r /etc on the ramfs root: no 'can't preserve' warnings" $?
grep -q "BB-42-END" "$BBLOG"; check "bash responsive after the BusyBox sequence" $?
! grep -q "\[EXC\]" "$BBLOG"; check "no CPU exception ([EXC]) during the BusyBox run" $?

# ============================================================================
# ---- Orderly poweroff / reboot from bash (SYS_REBOOT) ----
# ============================================================================
# (a) poweroff: write a file to an ext4 data disk, start a background job,
#     poweroff. QEMU must exit by itself (ACPI S5, rc 0, not killed by timeout
#     -> 124), the kernel must log TERM -> sync -> unmount -> poweroff, and on
#     the host the file must be there and the volume e2fsck-clean.
# (b) reboot: QEMU runs WITHOUT -no-reboot; the boot banner must appear twice.
# (c) poweroff under UEFI/OVMF (q35): QEMU must exit by itself.
# The feeder exits right after the command, so a QEMU that does not power off
# is caught by `timeout` (124).
PWLOG=/tmp/secos_selftest_power.log
PWDISK=/tmp/secos_selftest_power_disk.img
PWSTAGE=/tmp/secos_selftest_power_stage
echo "[selftest] Running orderly poweroff from bash with an ext4 disk (mb2, serial, 60s)..."
rm -rf "$PWSTAGE"; mkdir -p "$PWSTAGE"; echo hi > "$PWSTAGE/hello.txt"
dd if=/dev/zero of="$PWDISK" bs=1M count=64 status=none
mkfs.ext4 -q -F -O ^metadata_csum -L SECOSDATA -d "$PWSTAGE" "$PWDISK"
: > "$PWLOG"
( sleep 25; printf '  echo POWER-SYNC-OK > /mnt/pw.txt; sleep 30 &\r'; sleep 3
  printf '  poweroff\r'; sleep 1 ) | \
    timeout 60 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -drive file="$PWDISK",if=virtio,format=raw \
        -debugcon file:"$PWLOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
pw_rc=$?
[[ $pw_rc -eq 0 ]]; check "poweroff: QEMU exits by itself via ACPI S5 (rc=$pw_rc, 124 = hung)" $?
grep -q "\[POWER\] poweroff/reboot/halt/shutdown installed to /bin" "$PWLOG"; check "poweroff/reboot/halt/shutdown installed to /bin" $?
grep -q "\[POWER\] poweroff requested by pid=" "$PWLOG"; check "poweroff: SYS_REBOOT reached from bash" $?
grep -q "\[POWER\] synced 0x0000000000000001 filesystem(s)" "$PWLOG"; check "poweroff: the ext4 /mnt was synced" $?
grep -q "\[POWER\] unmounted" "$PWLOG" && grep -q "\[POWER\] powering off" "$PWLOG"; check "poweroff: unmount, then ACPI power-off" $?
[[ "$(debugfs -R 'cat /pw.txt' "$PWDISK" 2>/dev/null)" == "POWER-SYNC-OK" ]]; check "poweroff: file written just before poweroff is on the ext4 disk" $?
e2fsck -fn "$PWDISK" >/dev/null 2>&1; check "poweroff: ext4 data disk e2fsck-clean afterwards" $?
! grep -q "\[EXC\]" "$PWLOG"; check "no CPU exception ([EXC]) during poweroff" $?

RBLOG=/tmp/secos_selftest_reboot.log
echo "[selftest] Running reboot from bash (mb2, serial, reboot allowed, 75s)..."
: > "$RBLOG"
( sleep 25; printf '  reboot\r'; sleep 45 ) | \
    timeout 75 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -debugcon file:"$RBLOG" -global isa-debugcon.iobase=0xe9 \
        -display none -serial stdio >/dev/null 2>&1
grep -q "\[POWER\] restarting" "$RBLOG"; check "reboot: orderly restart path taken" $?
[[ "$(grep -c "SECoS build" "$RBLOG")" -ge 2 ]]; check "reboot: the system came back up (boot banner twice)" $?

OVMF_PW=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/OVMF/x64/OVMF.fd \
         /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/edk2-ovmf/OVMF_CODE.fd; do
    [[ -f "$f" ]] && { OVMF_PW="$f"; break; }
done
if [[ -n "$OVMF_PW" ]] && make uefi >/tmp/secos_selftest_build.log 2>&1; then
    UPLOG=/tmp/secos_selftest_power_uefi.log
    echo "[selftest] Running poweroff under UEFI/OVMF (q35, serial, 75s)..."
    : > "$UPLOG"
    # vvfat (fat:rw:) writes back into its directory: boot a copy of dist/.
    UPESP=$(mktemp -d /tmp/secos_power_esp.XXXXXX); cp -r dist/. "$UPESP"/; rm -f "$UPESP/NvVars"
    ( sleep 45; printf '  poweroff\r'; sleep 1 ) | \
        timeout 75 qemu-system-x86_64 -machine q35,accel=tcg -m 256M -bios "$OVMF_PW" \
            -drive format=raw,file=fat:rw:"$UPESP"/ \
            -debugcon file:"$UPLOG" -global isa-debugcon.iobase=0xe9 \
            -no-reboot -display none -net none -serial stdio >/dev/null 2>&1
    up_rc=$?
    rm -rf "$UPESP"
    grep -q "\[POWER\] powering off" "$UPLOG"; check "UEFI: poweroff from bash reaches ACPI power-off" $?
    [[ $up_rc -eq 0 ]]; check "UEFI: QEMU exits by itself after poweroff (rc=$up_rc)" $?
    ! grep -q "ACPI poweroff unavailable" "$UPLOG"; check "UEFI: S5 powered off before the kernel gave up (no \"unavailable\" message)" $?
else
    echo "  [SKIP] UEFI poweroff (OVMF missing or make uefi failed)"
fi

# ---- M39: UEFI secure boot (loader Ed25519-verifies kernel.elf before jumping) ----
OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/OVMF/x64/OVMF.fd \
         /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/edk2-ovmf/OVMF_CODE.fd; do
    [[ -f "$f" ]] && { OVMF="$f"; break; }
done
if [[ -n "$OVMF" ]] && make uefi >/tmp/secos_selftest_build.log 2>&1; then
    SB=/tmp/secos_selftest_sb
    rm -rf "$SB"; mkdir -p "$SB"
    cp -r dist "$SB/esp_good"; cp -r dist "$SB/esp_bad"
    # Flip one byte inside .text of the tampered copy.
    toff=$(readelf -SW dist/kernel.elf | awk '{for(i=1;i<NF;i++) if($i==".text") print $(i+3)}')
    printf '\xff' | dd of="$SB/esp_bad/kernel.elf" bs=1 seek=$(( 16#$toff + 256 )) conv=notrunc 2>/dev/null
    for v in good bad; do
        echo "[selftest] Running UEFI secure boot ($v kernel, 25s)..."
        : > "$SB/ser_$v.log"; : > "$SB/dbg_$v.log"
        timeout 25 qemu-system-x86_64 -machine q35,accel=tcg -m 256M -bios "$OVMF" \
            -drive format=raw,file=fat:rw:"$SB/esp_$v"/ \
            -serial file:"$SB/ser_$v.log" -debugcon file:"$SB/dbg_$v.log" \
            -global isa-debugcon.iobase=0xe9 -no-reboot -no-shutdown -display none -net none >/dev/null 2>&1
    done
    ! cmp -s dist/kernel.elf "$SB/esp_bad/kernel.elf"; check "M39 secure boot: tampered kernel differs from the signed one" $?
    grep -aq "\[SECURE BOOT\] kernel signature OK" "$SB/ser_good.log"; check "M39 secure boot: signed kernel verified by the UEFI loader" $?
    grep -q "SECoS build" "$SB/dbg_good.log"; check "M39 secure boot: verified kernel boots" $?
    # The ESP is QEMU's vvfat (FAT16): it must not be mistaken for a FAT32 /mnt.
    ! grep -q "\[M10\] disk write+readback: FAIL" "$SB/dbg_good.log"; check "UEFI boot: FAT16 ESP not mounted as FAT32 (no M10 readback FAIL)" $?
    grep -aq "\[SECURE BOOT\] kernel signature INVALID - refusing to boot" "$SB/ser_bad.log"; check "M39 secure boot: one flipped byte -> kernel refused" $?
    ! grep -q "SECoS build" "$SB/dbg_bad.log"; check "M39 secure boot: tampered kernel never runs" $?
else
    echo "  [SKIP] M39 secure boot (OVMF missing or make uefi failed)"
fi

# ======================================================================
# ---- Heap hardening: integrity checks catch corruption at the source ----
# ======================================================================
# (1) Default image: the boot heap selftest (alloc patterns, aligned frees,
#     every detector in non-panicking expect mode) passes.
# (2) HEAP_CORRUPT_DEMO=1: a 1-byte overflow past kmalloc(100) must panic at
#     kfree with the tail-canary message; =2: a double free must panic as such.
# (3) -DSECOS_HEAP_DEBUG (poison-on-free + periodic full check): boot into
#     bash, run a command burst, kill PID 1 (init) to drop to the recovery shell and `heapcheck` —
#     no false positive, heap sound.
HEAPLOG=/tmp/secos_selftest_heap.log
echo "[selftest] Building heap-hardening image (default)..."
make clean >/dev/null 2>&1 || true
if ! make >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] HEAP BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    exit 2
fi
tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$HEAPLOG" >/dev/null 2>&1 || true
grep -q "\[HEAP\]\[SELFTEST\] all PASS" "$HEAPLOG"; check "HEAP boot selftest all PASS (patterns, aligned kfree, detectors)" $?
! grep -q "\[HEAP\]\[SELFTEST\] FAIL" "$HEAPLOG"; check "HEAP no selftest case failed" $?
! grep -q "\[HEAP\] CORRUPTION DETECTED" "$HEAPLOG"; check "HEAP no corruption reported on a clean boot" $?
for demo in 1 2; do
    DLOG=/tmp/secos_selftest_heap_demo$demo.log
    echo "[selftest] Building HEAP_CORRUPT_DEMO=$demo image..."
    make clean >/dev/null 2>&1 || true
    if ! make CFLAGS_EXTRA=-DHEAP_CORRUPT_DEMO=$demo >/tmp/secos_selftest_build.log 2>&1; then
        echo "[selftest] HEAP DEMO BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
        exit 2
    fi
    tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$DLOG" >/dev/null 2>&1 || true
    grep -q "\[HEAP-DEMO\] start" "$DLOG"; check "HEAP demo $demo reached" $?
    ! grep -q "\[HEAP-DEMO\] FAIL" "$DLOG"; check "HEAP demo $demo: corruption did not go unnoticed" $?
done
grep -q "\[PANIC\] HEAP: heap buffer overflow (tail canary smashed)" /tmp/secos_selftest_heap_demo1.log
check "HEAP 1-byte overflow -> panic at kfree (tail canary)" $?
grep -q "\[PANIC\] HEAP: double free" /tmp/secos_selftest_heap_demo2.log
check "HEAP double free -> panic (double free)" $?
HDBGLOG=/tmp/secos_selftest_heap_debug.log
echo "[selftest] Building SECOS_HEAP_DEBUG image + bash burst (serial input, 90s)..."
make clean >/dev/null 2>&1 || true
if ! make iso CFLAGS_EXTRA=-DSECOS_HEAP_DEBUG >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] HEAP DEBUG BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    exit 2
fi
: > "$HDBGLOG"
( sleep 25
  for i in $(seq 1 15); do printf '  ls /\r'; sleep 0.6; printf '  cat /proc/meminfo\r'; sleep 0.6; done
  printf '  echo HEAPDBG-$((6*7))-OK\r'; sleep 3
  printf '  kill -9 1\r'; sleep 6
  printf '  heapcheck\r'; sleep 4 ) | \
    timeout 90 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -debugcon file:"$HDBGLOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
grep -q "\[HEAP\] hardened heap (debug" "$HDBGLOG"; check "HEAP debug build active (poison + periodic check)" $?
grep -q "HEAPDBG-42-OK" "$HDBGLOG"; check "HEAP debug: bash command burst completes" $?
grep -q "\[HEAP\] heapcheck: OK" "$HDBGLOG"; check "HEAP debug: shell heapcheck reports a sound heap" $?
! grep -q "\[HEAP\] CORRUPTION DETECTED" "$HDBGLOG"; check "HEAP debug: no false-positive corruption report" $?
! grep -q "\[EXC\]" "$HDBGLOG"; check "no CPU exception ([EXC]) during heap debug run" $?

# ============================================================================
# UACCESS GATE: bad user pointers in syscalls return -EFAULT, never a ring-0
# fault. The signed ring-3 program user/uaccess_test.c (/bin/uaccess_test)
# hands ~25 syscalls an in-range but unmapped (munmap'd hole, 0xDEADBEEF) or
# read-only (PROT_NONE) pointer — including the three fuzzer-found triggers
# (spawn wild path, lstat into PROT_NONE, write(1, p, 1 MB) off the mapping) —
# and asserts the error code. It also checks the paths the fixups must NOT
# break: kernel-mode demand paging of a fresh buffer (M14), a kernel write into
# a copy-on-write page after fork (M19, needs CR0.WP), and a sigframe pushed
# from the #PF return path onto a fresh stack page (nested #PF, needs #PF off
# IST). Rebuilds a plain image: the heap section above left a debug build.
# ============================================================================
# [M44] SMAP violation: an image that deliberately reads a user page from ring 0
# with RFLAGS.AC=0 (outside the uaccess primitives) must panic with a clear
# [SMAP] line on a CPU with SMAP, and skip the probe on one without.
SVLOG=/tmp/secos_selftest_smapviol.log
echo "[selftest] Building SMAP violation image (SMAP_VIOLATION_DEMO)..."
build_demo SMAPVIOL "-DSMAP_VIOLATION_DEMO"
echo "[selftest] Running SMAP violation image (mb2, -cpu max, 20s)..."
run_iso "$SVLOG" 20 -cpu max
grep -q "\[SMAP-DEMO\] direct kernel read of a user page" "$SVLOG"; check "M44 SMAP violation probe ran (-cpu max)" $?
grep -q "\[SMAP\] violation: ring-0 read of user address cr2=0x0000000100000000" "$SVLOG"; check "M44 direct kernel read of a user page -> [SMAP] violation" $?
grep -q "\[PANIC\] SMAP violation" "$SVLOG"; check "M44 SMAP violation panics the kernel (no silent retry)" $?
! grep -q "\[SMAP-DEMO\] FAIL" "$SVLOG"; check "M44 the stray access never completed" $?
run_iso "$SVLOG.nosmap" 20 -cpu qemu64
grep -q "\[SMAP-DEMO\] skipped: SMAP not enabled" "$SVLOG.nosmap"; check "M44 no SMAP CPU: probe skipped, kernel keeps running" $?
! grep -q "\[PANIC\]" "$SVLOG.nosmap"; check "M44 no panic on a CPU without SMAP" $?

UALOG=/tmp/secos_selftest_uaccess.log
echo "[selftest] Building plain image for the uaccess gate..."
make clean >/dev/null 2>&1 || true
if ! make iso >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] UACCESS BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    exit 2
fi
echo "[selftest] Running uaccess + ABI gate (mb2, serial, 100s)..."
: > "$UALOG"
UASER=/tmp/secos_selftest_uaccess_serial.log
: > "$UASER"
( sleep 25; printf '  /bin/uaccess_test; echo UA-$((6*7))-END\r'; sleep 20
  printf '  cat /proc/version; echo VER-$((6*7))-END\r'; sleep 5
  printf '  /bin/abi_test; echo ABI-$((6*7))-END\r'; sleep 30 ) | \
    timeout 100 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M -cpu max \
        -debugcon file:"$UALOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >"$UASER" 2>&1
grep -q "\[UA-TEST\] DONE pass=[0-9]* fail=0" "$UALOG"; check "UACCESS every bad-pointer case returns an error (DONE fail=0)" $?
grep -q "UA-42-END" "$UALOG"; check "UACCESS shell survives the test (sentinel reached)" $?
# [identity-map hole] low user pointers that alias the kernel-only identity map
# must be refused; the kernel image (/proc/version lives in .rodata) stays intact.
grep -q "\[UA-TEST\] ident-read-0x100000 OK" "$UALOG"; check "UACCESS read via identity alias 0x100000 -> -EFAULT" $?
grep -q "\[UA-TEST\] ident-write-0x100000 OK" "$UALOG"; check "UACCESS WRITE via identity alias 0x100000 -> -EFAULT (no kernel overwrite)" $?
grep -q "\[UA-TEST\] ident-straddle-min OK" "$UALOG"; check "UACCESS range straddling the user-window base -> -EFAULT" $?
# [M44] (gate runs on -cpu max: SMAP on) no identity map in user CR3s, kernel-half
# pointers refused, no sigframe written through a low rsp, AC=1 on entry harmless.
grep -q "\[SMAP\] enabled" "$UALOG"; check "UACCESS gate runs with SMAP enabled (-cpu max)" $?
grep -q "\[UA-TEST\] noident-mmap-hint-low-ignored OK" "$UALOG"; check "UACCESS mmap hint in the old identity window is ignored" $?
grep -q "\[UA-TEST\] noident-read-physmap-ptr OK" "$UALOG" && grep -q "\[UA-TEST\] noident-write-physmap-ptr OK" "$UALOG"
check "UACCESS physmap pointers -> -EFAULT (read + write)" $?
grep -q "\[UA-TEST\] noident-read-kimage-ptr OK" "$UALOG" && grep -q "\[UA-TEST\] noident-write-kimage-ptr OK" "$UALOG"
check "UACCESS kernel-image pointers -> -EFAULT (read + write)" $?
grep -q "\[UA-TEST\] noident-ring3-load-0x100000-sigsegv OK" "$UALOG"; check "UACCESS ring-3 load from 0x100000 -> SIGSEGV (not present)" $?
grep -q "\[UA-TEST\] noident-sigframe-low-rsp-killed OK" "$UALOG"; check "UACCESS sigframe aimed at the old identity window is refused" $?
grep -q "\[UA-TEST\] smap-entry-with-ac-set OK" "$UALOG"; check "UACCESS syscall entered with RFLAGS.AC=1 works (entry clac)" $?
! grep -q "\[SMAP\] violation" "$UALOG"; check "UACCESS no SMAP violation" $?
grep -q "SecOS version" "$UASER" && grep -q "VER-42-END" "$UASER"
check "UACCESS kernel .rodata intact after the test (/proc/version readable)" $?
grep -q "\[UA-TEST\] spawn-wild-path OK" "$UALOG"; check "UACCESS spawn(0xDEADBEEF) -> -EFAULT" $?
grep -q "\[UA-TEST\] lstat-protnone-statbuf OK" "$UALOG"; check "UACCESS lstat into a PROT_NONE page -> -EFAULT" $?
grep -q "\[UA-TEST\] write-console-overrun OK" "$UALOG"; check "UACCESS write(1, p, 1 MB) off the mapping -> -EFAULT/short" $?
grep -q "\[UA-TEST\] read-into-demand-page OK" "$UALOG"; check "UACCESS kernel-mode demand paging still serviced (M14)" $?
grep -q "\[UA-TEST\] kernel-write-to-cow-page OK" "$UALOG"; check "UACCESS kernel write into a COW page copies it (M19, CR0.WP)" $?
grep -q "\[UA-TEST\] sigframe-from-pf-path OK" "$UALOG"; check "UACCESS sigframe from the #PF return path (nested #PF)" $?
! grep -q "\[UA-TEST\] .* FAIL" "$UALOG"; check "UACCESS no case failed" $?
! grep -q "ring0" "$UALOG"; check "UACCESS no ring-0 CPU fault" $?
# ABI regression test (user/abi_test.c), same boot: POSIX wait status, DF=1 on
# kernel entry (every entry stub must cld), PROT_NONE.
grep -q "\[ABI-TEST\] DONE pass=[0-9]* fail=0" "$UALOG"; check "ABI every case passes (DONE fail=0)" $?
grep -q "ABI-42-END" "$UALOG"; check "ABI shell survives the test (sentinel reached)" $?
grep -q "\[ABI-TEST\] wait-exit-142-not-signal OK" "$UALOG"; check "ABI exit(142) -> WIFEXITED, WEXITSTATUS=142" $?
grep -q "\[ABI-TEST\] wait-pf-is-sigsegv OK" "$UALOG"; check "ABI ring-3 #PF -> WIFSIGNALED, WTERMSIG=SIGSEGV" $?
grep -q "\[ABI-TEST\] wait-de-is-sigfpe OK" "$UALOG"; check "ABI ring-3 #DE -> SIGFPE" $?
grep -q "\[ABI-TEST\] df-fork-child-ok OK" "$UALOG"; check "ABI fork() entered with DF=1 copies correctly (cld on entry)" $?
grep -q "\[ABI-TEST\] df-pipe-write-read OK" "$UALOG" && grep -q "\[ABI-TEST\] df-stat OK" "$UALOG"
check "ABI write/read/stat entered with DF=1 return correct data" $?
grep -q "\[ABI-TEST\] df-signal-handler-entry-df0 OK" "$UALOG"; check "ABI signal handler starts with DF=0" $?
grep -q "\[ABI-TEST\] pn-user-read-sigsegv OK" "$UALOG"; check "ABI ring-3 read of a PROT_NONE page -> SIGSEGV" $?
grep -q "\[ABI-TEST\] pn-fresh-read-sigsegv OK" "$UALOG"; check "ABI PROT_NONE page is never demand-faulted in" $?
grep -q "\[ABI-TEST\] pn-kernel-copy-from-efault OK" "$UALOG"; check "ABI kernel copy from a PROT_NONE page -> -EFAULT" $?
grep -q "\[ABI-TEST\] pn-mprotect-back-keeps-data OK" "$UALOG"; check "ABI mprotect PROT_NONE -> RW restores the page + contents" $?
grep -q "\[ABI-TEST\] pn-split-guard-sigsegv OK" "$UALOG"; check "ABI partial mprotect splits the VMA (PROT_NONE guard page)" $?
grep -q "\[ABI-TEST\] pn-fork-parent-copy-isolated OK" "$UALOG"; check "ABI PROT_NONE page shared by fork stays COW-isolated" $?
! grep -q "\[ABI-TEST\] .* FAIL" "$UALOG"; check "ABI no case failed" $?
! grep -q "\[PANIC\]" "$UALOG"; check "UACCESS no kernel panic" $?

# ============================================================================
# KERNEL INTERFACES GATE (kernel/kiface.c, fs/procfs.c, kernel/klog.c,
# kernel/kclock.c): the signed ring-3 program user/kitest.c (/bin/kitest)
# exercises per-pid /proc (+ a forked child appearing/disappearing), /proc/stat,
# loadavg, meminfo, mounts, the kernel log (klogctl + /dev/kmsg, dmesg -c),
# RTC wall clock + nanosleep (sub-tick, EINTR) + strftime, uname/sysinfo/umask/
# access, statfs, rename/link/truncate/rmdir, poll/select (pipe, tty, timeouts,
# EINTR) and symlinks in the middle of a path. Boot 1 runs it on the RAMFS root
# and on a journalled metadata_csum ext4 data disk (/mnt), then stages an atomic
# rename-over-existing and powers off; boot 2 checks the rename survived and
# reports statfs, which must equal dumpe2fs; the disk must stay e2fsck-clean.
# Reuses the plain myos.iso built for the uaccess gate.
# ============================================================================
KILOG=/tmp/secos_selftest_kiface.log
KILOG2=/tmp/secos_selftest_kiface2.log
KIFLOG=/tmp/secos_selftest_kiface_fat.log
KIDISK=/tmp/secos_selftest_kiface_ext4.img
KIFAT=/tmp/secos_selftest_kiface_fat.img
echo "[selftest] Running kernel-interfaces gate (mb2, serial, ext4 data disk, 2 boots)..."
dd if=/dev/zero of="$KIDISK" bs=1M count=64 status=none
mkfs.ext4 -q -F -L KIDATA "$KIDISK"
: > "$KILOG"
( sleep 25; printf '  /bin/kitest; echo KI-$((6*7))-END\r'; sleep 15
  printf '  /bin/kitest disk /mnt; echo KID-$((6*7))-END\r'; sleep 12
  printf '  /bin/kitest persist1 /mnt; echo KIP-$((6*7))-END\r'; sleep 5
  printf '  poweroff\r'; sleep 2 ) | \
    timeout 90 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -drive file="$KIDISK",if=virtio,format=raw \
        -debugcon file:"$KILOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
host_now=$(date +%s)
grep -q "KI-42-END" "$KILOG"; check "KIFACE shell survives /bin/kitest (sentinel reached)" $?
grep -Eq "\[KI-TEST\] DONE pass=[0-9]+ fail=0" "$KILOG"; check "KIFACE RAMFS run: every case passes (DONE fail=0)" $?
for c in proc-self-symlink proc-pid-stat-parses proc-child-appears proc-child-disappears proc-fd-pipe-link \
         proc-stat proc-meminfo proc-mounts dmesg-has-boot-line dmesg-clear dev-kmsg-record \
         time-is-unix-epoch nanosleep-sub-tick-300us nanosleep-eintr-rem strftime-1700000000 \
         poll-blocks-until-pipe-data poll-tty-no-input-timeout poll-eintr select-readable \
         midpath-symlink-via-proc-cwd rename-replaces-target rename-exdev rmdir-enotempty; do
    grep -q "\[KI-TEST\] $c OK" "$KILOG"; check "KIFACE $c" $?
done
kt=$(grep -ao "\[KI-TEST\] time=[0-9]*" "$KILOG" | head -1 | sed 's/.*=//')
[[ -n "$kt" && $(( host_now - kt )) -ge 0 && $(( host_now - kt )) -lt 300 ]]
check "KIFACE time() is the host's wall clock (guest=${kt:-?} host=$host_now)" $?
grep -q "KID-42-END" "$KILOG"; check "KIFACE ext4 /mnt run completed" $?
[[ "$(grep -Ec '\[KI-TEST\] DONE pass=[0-9]+ fail=0' "$KILOG")" -ge 3 ]]; check "KIFACE ext4 + persist runs: every case passes" $?
for c in link-nlink-2 link-shares-data unlink-one-link-keeps-file rename-dir-moves-children truncate-shrink \
         ftruncate-grow-zero-fill mtime-is-wall-clock umask-applied-on-create midpath-relative-symlink \
         midpath-absolute-symlink symlink-loop-eloop midpath-loop-eloop statfs-tracks-allocation persist1-rename; do
    grep -q "\[KI-TEST\] $c OK" "$KILOG"; check "KIFACE ext4 $c" $?
done
e2fsck -fn "$KIDISK" >/dev/null 2>&1; check "KIFACE ext4 data disk e2fsck-clean after boot 1" $?
: > "$KILOG2"
( sleep 25; printf '  /bin/kitest persist2 /mnt; echo KIP2-$((6*7))-END\r'; sleep 6
  printf '  poweroff\r'; sleep 2 ) | \
    timeout 60 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -drive file="$KIDISK",if=virtio,format=raw \
        -debugcon file:"$KILOG2" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
grep -q "\[KI-TEST\] persist2-rename-survived-reboot OK" "$KILOG2"; check "KIFACE ext4 rename-over-existing survives a reboot" $?
e2fsck -fn "$KIDISK" >/dev/null 2>&1; check "KIFACE ext4 data disk e2fsck-clean after boot 2" $?
sf=$(grep -ao "statfs-final blocks=[0-9]* bfree=[0-9]* files=[0-9]* ffree=[0-9]*" "$KILOG2" | head -1)
de=$(dumpe2fs -h "$KIDISK" 2>/dev/null | awk -F: '/^Block count/{b=$2}/^Free blocks/{f=$2}/^Inode count/{i=$2}/^Free inodes/{fi=$2}
     END{gsub(/ /,"",b);gsub(/ /,"",f);gsub(/ /,"",i);gsub(/ /,"",fi); printf "statfs-final blocks=%s bfree=%s files=%s ffree=%s", b,f,i,fi}')
[[ -n "$sf" && "$sf" == "$de" ]]; check "KIFACE statfs == dumpe2fs ($sf | $de)" $?
# FAT32 data disk: rename/truncate/rmdir/statfs (no hard links / symlinks).
dd if=/dev/zero of="$KIFAT" bs=1M count=64 status=none
mkfs.fat -F 32 -n KIFAT "$KIFAT" >/dev/null
: > "$KIFLOG"
( sleep 25; printf '  /bin/kitest disk /mnt; echo KIF-$((6*7))-END\r'; sleep 12 ) | \
    timeout 50 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -drive file="$KIFAT",if=virtio,format=raw \
        -debugcon file:"$KIFLOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
grep -Eq "\[KI-TEST\] DONE pass=[0-9]+ fail=0" "$KIFLOG"; check "KIFACE FAT32 /mnt run: every case passes" $?
grep -q "\[KI-TEST\] statfs /mnt type=0x4d44" "$KIFLOG"; check "KIFACE FAT32 statfs reports MSDOS magic" $?
fsck.fat -n "$KIFAT" >/dev/null 2>&1; check "KIFACE FAT32 data disk fsck.fat-clean" $?
! grep -q "\[EXC\]" "$KILOG" "$KILOG2" "$KIFLOG"; check "KIFACE no CPU exception ([EXC])" $?
! grep -q "\[PANIC\]" "$KILOG" "$KILOG2" "$KIFLOG"; check "KIFACE no kernel panic" $?

# ============================================================================
# ---- SSH: Dropbear (from source) — non-interactive exec over QEMU hostfwd ----
# ============================================================================
# Boots the default image with a user-net NIC forwarding a host port to guest
# :22. init has already taken a DHCP lease and started dropbear (-F -R); the
# console bash installs a throwaway test key (generated here, never committed)
# as /root/.ssh/authorized_keys by hand, and ssh runs from the host: an exec
# channel with output + exit status, stderr, a wrong key, an unknown user and a
# password-only client (root has no password login; the multi-user password
# logins are covered by the MUPORTS gate). Needs host ssh +
# ssh-keygen, and a kernel with SYS_POLL (78); otherwise SKIP. See docs/SSH.md.
# (The INIT gate at the end covers the baked-key, zero-step boot.)
SSHLOG=/tmp/secos_selftest_ssh.log
if ! command -v ssh >/dev/null 2>&1 || ! command -v ssh-keygen >/dev/null 2>&1; then
    echo "  [SKIP] SSH gate (host has no ssh / ssh-keygen)"
else
    echo "[selftest] Running Dropbear SSH exec gate (mb2, user-net hostfwd, ~110s)..."
    SSHDIR=$(mktemp -d /tmp/secos_selftest_ssh.XXXXXX)
    ssh-keygen -q -t ed25519 -N '' -C secos-selftest -f "$SSHDIR/id"
    ssh-keygen -q -t ed25519 -N '' -C secos-wrong -f "$SSHDIR/bad"
    SSHPUB=$(cat "$SSHDIR/id.pub")
    SSHPORT=$((22000 + $$ % 1000))
    sshq() { # sshq <out> <ssh args...>
        local out="$1"; shift
        timeout 40 ssh -p "$SSHPORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
            -o BatchMode=yes -o ConnectTimeout=20 -o LogLevel=ERROR "$@" </dev/null >"$out" 2>&1
        echo "RC=$?" >>"$out"
    }
    : > "$SSHLOG"
    ( sleep 25
      printf '  mkdir -p /root/.ssh\r'; sleep 2
      printf "  echo '%s' > /root/.ssh/authorized_keys\r" "$SSHPUB"; sleep 2
      sshq "$SSHDIR/o1" -i "$SSHDIR/id" -o IdentitiesOnly=yes root@localhost 'echo SSH-$((6*7))-OK; uname -s'
      sshq "$SSHDIR/o9" -i "$SSHDIR/id" -o IdentitiesOnly=yes root@localhost 'cat /var/log/sshd.log'
      sshq "$SSHDIR/o2" -i "$SSHDIR/id" -o IdentitiesOnly=yes root@localhost 'echo SSH-ERR >&2; exit 3'
      sshq "$SSHDIR/o3" -i "$SSHDIR/bad" -o IdentitiesOnly=yes root@localhost 'echo SSH-BADKEY-RAN'
      sshq "$SSHDIR/o4" -i "$SSHDIR/id" -o IdentitiesOnly=yes nobody@localhost 'echo SSH-NOUSER-RAN'
      sshq "$SSHDIR/o5" -o PubkeyAuthentication=no -o PreferredAuthentications=password,keyboard-interactive root@localhost true
      sshq "$SSHDIR/o6" -i "$SSHDIR/id" -o IdentitiesOnly=yes root@localhost 'seq 1 500 > /tmp/s; wc -l < /tmp/s'
      # A pty session needs an open stdin: on client EOF Dropbear closes the
      # pty master (upstream behaviour), hanging the shell up. Drive it from a
      # host pty (tools/ssh_winch.py) like an interactive user.
      timeout 40 python3 tools/ssh_winch.py 25 -p "$SSHPORT" -tt -i "$SSHDIR/id" -o IdentitiesOnly=yes \
          -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o LogLevel=ERROR \
          root@localhost 'tty; stty size; echo PTY-$((6*7))' >"$SSHDIR/o7" 2>&1; echo "RC=$?" >>"$SSHDIR/o7"
      timeout 40 python3 tools/ssh_winch.py 30 -p "$SSHPORT" -tt -i "$SSHDIR/id" -o IdentitiesOnly=yes \
          -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o LogLevel=ERROR \
          root@localhost 'stty size; trap "stty size; echo WINCH-GOT" WINCH; echo WINCH-READY; sleep 4; echo WINCH-END' \
          >"$SSHDIR/o8" 2>&1; echo "RC=$?" >>"$SSHDIR/o8"
      sleep 2 ) | \
        timeout 220 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
            -netdev user,id=n0,hostfwd=tcp:127.0.0.1:"$SSHPORT"-:22 -device e1000,netdev=n0 \
            -debugcon file:"$SSHLOG" -global isa-debugcon.iobase=0xe9 \
            -no-reboot -display none -serial stdio >/dev/null 2>&1
    for f in o1 o2 o3 o4 o5 o6 o7 o8 o9; do { echo "--- $f"; cat "$SSHDIR/$f" 2>/dev/null; } >>"$SSHLOG"; done
    if grep -q "has no poll() (SYS_POLL 78)" "$SSHLOG"; then
        echo "  [SKIP] SSH gate (kernel lacks SYS_POLL 78; Dropbear needs poll/select)"
    else
        grep -q "\[SSH\] installed dropbear to /bin/dropbearmulti" "$SSHLOG"; check "SSH dropbearmulti installed to /bin at boot" $?
        grep -q "Generated hostkey /etc/dropbear/dropbear_ed25519_host_key" "$SSHDIR/o9"; check "SSH ed25519 host key generated on first connection (-R, /var/log/sshd.log)" $?
        grep -q "^SSH-42-OK" "$SSHDIR/o1" && grep -q "^SecOS" "$SSHDIR/o1" && grep -q "^RC=0" "$SSHDIR/o1"
        check "SSH exec channel: output + uname, exit status 0" $?
        grep -q "^SSH-ERR" "$SSHDIR/o2" && grep -q "^RC=3" "$SSHDIR/o2"; check "SSH exec channel: stderr forwarded, exit status 3 propagated" $?
        ! grep -q "SSH-BADKEY-RAN" "$SSHDIR/o3" && grep -q "Permission denied (publickey" "$SSHDIR/o3"
        check "SSH unauthorized key refused" $?
        ! grep -q "SSH-NOUSER-RAN" "$SSHDIR/o4" && grep -q "Permission denied" "$SSHDIR/o4"; check "SSH unknown user refused" $?
        grep -q "Permission denied (publickey,password)" "$SSHDIR/o5" && ! grep -q "^RC=0" "$SSHDIR/o5"
        check "SSH root without a key is refused (no password login for root)" $?
        grep -q "^500" "$SSHDIR/o6"; check "SSH sequential sessions keep working (6th session, file I/O)" $?
        grep -q "^/dev/pts/[0-9]" "$SSHDIR/o7" && grep -q "^30 90" "$SSHDIR/o7" && grep -q "^PTY-42" "$SSHDIR/o7"
        check "SSH -tt: the session runs on a pty (tty = /dev/pts/N, stty size)" $?
        grep -q "^30 90" "$SSHDIR/o8"; check "SSH pty-req carries the client window size (30x90)" $?
        grep -q "^40 100" "$SSHDIR/o8" && grep -q "WINCH-GOT" "$SSHDIR/o8"
        check "SSH window-change resizes the pty and raises SIGWINCH (40x100)" $?
        ! grep -q "\[EXC\]" "$SSHLOG"; check "SSH no CPU exception ([EXC])" $?
        ! grep -q "\[PANIC\]" "$SSHLOG"; check "SSH no kernel panic" $?
    fi
    rm -rf "$SSHDIR"
fi

# ============================================================================
# PIPE GATE: blocking-pipe data integrity. Pipes used to lose data once more
# than the 4 KB ring was in flight: a write into a nearly-full pipe stored what
# fitted and returned a short count that libc's fflush() dropped, so
# `seq 1 2000 | wc` reported ~1997 lines. The signed ring-3 /bin/pipe_test
# (user/pipe_test.c) checks pattern+checksum transfers (1 B .. 1 MB) across
# fork, PIPE_BUF atomicity with two writers, a signal interrupting a blocked
# writer, EPIPE/SIGPIPE and stdio through a pipe; then bash pipelines with
# exact counts. Reuses the plain image built for the uaccess gate.
# ============================================================================
PIPELOG=/tmp/secos_selftest_pipe.log
echo "[selftest] Running pipe integrity gate (mb2, serial, 170s)..."
: > "$PIPELOG"
( sleep 25; printf '  /bin/pipe_test; echo PT-$((6*7))-END\r'; sleep 45
  printf '  echo PL1-$(seq 1 20000 | wc -l)-E\r'; sleep 10
  printf '  seq 1 30000 > /tmp/big; echo PL2-$(wc -c < /tmp/big)-$(cat /tmp/big | cat | cat | wc -c)-E\r'; sleep 15
  printf '  echo PL3-$(yes | head -n 100000 | wc -l)-E\r'; sleep 35
  printf '  echo PL4-$(seq 1 5000 | wc)-E\r'; sleep 8
  printf '  echo PL5-$((6*7))-END\r'; sleep 5 ) | \
    timeout 170 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -debugcon file:"$PIPELOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
grep -q "\[PIPE-TEST\] DONE pass=[0-9]* fail=0" "$PIPELOG"; check "PIPE every pipe_test case passes (DONE fail=0)" $?
for t in xfer-1B xfer-4095 xfer-4096 xfer-4097 xfer-64K-one-write xfer-1M-64K-writes \
         xfer-64K-small-writes-5B xfer-256K-slow-reader xfer-256K-slow-writer \
         atomic-1000B-records-2-writers atomic-4096B-records-2-writers \
         signal-partial-write-returns-count signal-empty-write-returns-eintr \
         epipe-ignored-returns-epipe epipe-blocked-writer-returns-partial stdio-printf-20000-lines; do
    grep -q "\[PIPE-TEST\] $t OK" "$PIPELOG"; check "PIPE $t" $?
done
! grep -q "\[PIPE-TEST\] .* FAIL" "$PIPELOG"; check "PIPE no pipe_test case failed" $?
grep -q "PT-42-END" "$PIPELOG"; check "PIPE shell survives pipe_test (sentinel reached)" $?
grep -q "PL1-20000-E" "$PIPELOG"; check "PIPE bash seq 1 20000 | wc -l -> 20000" $?
grep -q "PL2-168894-168894-E" "$PIPELOG"; check "PIPE bash cat f | cat | cat | wc -c == file size (168894)" $?
grep -q "PL3-100000-E" "$PIPELOG"; check "PIPE bash yes | head -n 100000 | wc -l -> 100000 (SIGPIPE ends yes)" $?
grep -Eq "PL4- +5000 +5000 +23893-E" "$PIPELOG"; check "PIPE bash seq 1 5000 | wc -> 5000 5000 23893" $?
grep -q "PL5-42-END" "$PIPELOG"; check "PIPE bash still responsive after the pipelines" $?
! grep -q "\[EXC\]\|\[PANIC\]" "$PIPELOG"; check "PIPE no CPU exception ([EXC]) or panic" $?

# ============================================================================
# M40 GUI GATE: /dev/fb0 (fbdev), /dev/input/eventN (evdev), console handoff,
# keymaps. tools/gui_drive.py boots the plain image (built for the uaccess
# gate) with qemu-xhci + usb-tablet + usb-mouse next to the PS/2 keyboard and
# mouse, types commands into bash over serial and injects input over QMP
# (sendkey / mouse_move / mouse_button / input-send-event); screendump checks
# what is really on screen. In-guest markers come from the signed /bin/guitest
# (user/guitest.c) and its capability-less twin /bin/guitest_nocap.
# A second run on a KEYMAP=it image adds usb-kbd (USB HID keyboard path, and
# the build-time default layout).
# ============================================================================
GUILOG=/tmp/secos_selftest_gui.log
GUIDRV=/tmp/secos_selftest_gui.drv
echo "[selftest] Running M40 GUI gate (fb0/evdev/KDSETMODE/keymap, mb2, serial+QMP, <=300s)..."
: > "$GUIDRV"
timeout 330 python3 tools/gui_drive.py --log "$GUILOG" --out "$GUIDRV" --scenario main >/dev/null 2>&1
for t in guitest-fb-ran guitest-nocap-ran flood-sent guitest-input-ran loadkeys-it guitest-keys-ran \
         gfx-exit-pixels gfx-exit-console-suppressed gfx-exit-restored-log gfx-exit-text-back gfx-exit-text-drawn \
         gfx-crash-pixels gfx-crash-console-suppressed gfx-crash-restored-log gfx-crash-text-back gfx-crash-text-drawn \
         bash-responsive; do
    grep -q "\[GUI-DRV\] $t OK" "$GUIDRV"; check "M40 driver: $t" $?
done
for t in OPEN IOCTL PUTVAR NOTTY MMAP MMAP-RW WRITE-MMAP MMAP-RANGE MMAP-OFFSET MPROTECT-EXEC MPROTECT-RO FORK-SHARED NOLEAK; do
    grep -q "^GF-$t-OK" "$GUILOG"; check "M40 fb0: $t" $?
done
grep -q "G-END fb fails=0" "$GUILOG"; check "M40 fb0: guitest fb completed with 0 failures" $?
grep -q "^GN-FB-DENIED-OK" "$GUILOG" && grep -q "\[AUDIT\] pid=.* DENY  DISPLAY (fb0-open)" "$GUILOG"
check "M40 caps: /dev/fb0 refused (EACCES + audit) without CAP_DISPLAY" $?
grep -q "^GN-INPUT-DENIED-OK" "$GUILOG" && grep -q "\[AUDIT\] pid=.* DENY  INPUT (input-open)" "$GUILOG"
check "M40 caps: /dev/input/event* refused (EACCES + audit) without CAP_INPUT" $?
grep -q "^GN-KD-DENIED-OK" "$GUILOG"; check "M40 caps: KD_GRAPHICS refused (EPERM) without CAP_DISPLAY" $?
ps2='"ImExPS/2 Generic Explorer Mouse"'; um='"QEMU USB Mouse"'; tab='"QEMU USB Tablet"'; kb='"AT Translated Set 2 keyboard"'
grep -qF "GI-EV $ps2 type=2 code=0 value=10" "$GUILOG" && grep -qF "GI-EV $ps2 type=2 code=1 value=5" "$GUILOG" &&
grep -qF "GI-EV $ps2 type=2 code=0 value=-3" "$GUILOG" && grep -qF "GI-EV $ps2 type=2 code=1 value=-7" "$GUILOG"
check "M40 evdev: PS/2 mouse REL_X/REL_Y (+10,+5 / -3,-7) on IRQ12" $?
grep -qF "GI-EV $ps2 type=1 code=272 value=1" "$GUILOG" && grep -qF "GI-EV $ps2 type=1 code=272 value=0" "$GUILOG" &&
grep -qF "GI-EV $ps2 type=2 code=8 value=1" "$GUILOG"
check "M40 evdev: PS/2 mouse BTN_LEFT press/release + REL_WHEEL +1" $?
grep -qF "GI-EV $um type=2 code=0 value=12" "$GUILOG" && grep -qF "GI-EV $um type=2 code=1 value=-6" "$GUILOG" &&
grep -qF "GI-EV $um type=1 code=273 value=1" "$GUILOG" && grep -qF "GI-EV $um type=2 code=8 value=-1" "$GUILOG"
check "M40 evdev: USB HID mouse REL_X/REL_Y, BTN_RIGHT, REL_WHEEL -1" $?
grep -qF "GI-EV $tab type=3 code=0 value=16384" "$GUILOG" && grep -qF "GI-EV $tab type=3 code=1 value=8192" "$GUILOG" &&
grep -qF "GI-EV $tab type=1 code=272 value=1" "$GUILOG" && grep -q "GI-ABS event[0-9] r=0 min=0 max=32767" "$GUILOG"
check "M40 evdev: usb-tablet ABS_X/ABS_Y absolute (16384, 8192) + EVIOCGABS range + button" $?
grep -qF "GI-EV $kb type=1 code=30 value=1" "$GUILOG" && grep -qF "GI-EV $kb type=1 code=30 value=0" "$GUILOG"
check "M40 evdev: PS/2 keyboard KEY_A press/release (console tty still fed)" $?
for t in OPEN SHORTREAD SYN SYN-DROPPED GRAB; do
    grep -q "^GI-$t-OK" "$GUILOG"; check "M40 evdev: $t" $?
done
grep -q "G-END input fails=0" "$GUILOG"; check "M40 evdev: guitest input completed with 0 failures" $?
KT_IT='3c3e2f3a5f40235b5d7b7dc3a0c3a8c3a9c3acc3b2c3b9607e4022c2a3e282ac'
grep -q "^GK-BYTES $KT_IT\$" "$GUILOG"; check "M40 keymap it (PS/2): < > / : _ @ # [ ] { } à è é ì ò ù \` ~ @(Ctrl+AltGr) \" £ €" $?
[ "$(grep -c '\[VT\] text console restored (graphics owner exited' "$GUILOG")" -ge 2 ]
check "M40 console: text console restored after the graphics owner exits and after it crashes" $?
grep -q "^GG-CRASHING" "$GUILOG" && grep -q "\[KILL\] pid=" "$GUILOG"; check "M40 console: the crashing graphics client was killed (SIGSEGV)" $?
! grep -q "\[PANIC\]\|\[EXC\].*ring0" "$GUILOG"; check "M40 no kernel panic / ring-0 fault during the GUI gate" $?

# fb0 under UEFI/OVMF (GOP framebuffer, q35): the same fb + handoff checks.
# The ESP is a private copy (vvfat is read-write: the guest may write to it).
OVMF_G=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/OVMF/x64/OVMF.fd \
         /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/edk2-ovmf/OVMF_CODE.fd; do
    [[ -f "$f" ]] && { OVMF_G="$f"; break; }
done
if [[ -n "$OVMF_G" ]] && make uefi >/tmp/secos_selftest_build.log 2>&1; then
    GUIULOG=/tmp/secos_selftest_gui_uefi.log
    GUIUDIST=$(mktemp -d /tmp/secos_gui_esp.XXXXXX); cp -r dist/. "$GUIUDIST"/
    echo "[selftest] Running M40 fb0 under UEFI/OVMF (q35, serial, 115s)..."
    : > "$GUIULOG"
    ( sleep 55; printf '\025  /bin/guitest fb\r'; sleep 20; printf '\025  /bin/guitest gfx\r'; sleep 25
      printf '\025  echo UEFIGUI-$((6*7))-END\r'; sleep 5 ) | \
        timeout 115 qemu-system-x86_64 -machine q35,accel=tcg -m 512M -bios "$OVMF_G" \
            -drive format=raw,file=fat:rw:"$GUIUDIST"/ \
            -debugcon file:"$GUIULOG" -global isa-debugcon.iobase=0xe9 \
            -no-reboot -display none -net none -serial stdio >/dev/null 2>&1
    rm -rf "$GUIUDIST"
    grep -q "G-END fb fails=0" "$GUIULOG"; check "M40 UEFI: guitest fb (GOP fb0 ioctls/mmap/refusals/no leak) 0 failures" $?
    grep -q "^GG-KDGRAPHICS-OK" "$GUIULOG" && grep -q "\[VT\] text console restored (graphics owner exited" "$GUIULOG"
    check "M40 UEFI: KD_GRAPHICS + text console restored on exit" $?
    grep -q "UEFIGUI-42-END" "$GUIULOG"; check "M40 UEFI: bash responsive afterwards" $?
else
    echo "  [SKIP] M40 UEFI fb0 gate (OVMF missing or make uefi failed)"
fi

# ============================================================================
# GUI GATE: secwm compositor + graphical terminal + SWP client isolation
# (docs/GUI.md). tools/gui_wm_drive.py boots the plain image with a PS/2
# keyboard and a usb-tablet, starts `startgui --trace` from bash over serial and
# drives it over QMP: screendump checks the desktop/panel/client pixels, a click
# focuses the terminal, sendkey types `echo GUI-OK-$((6*7))` and the pty output
# must come back as GUI-OK-42 (secwm --trace mirrors terminal lines to
# debugcon), job control on the pty (Ctrl+C / Ctrl+Z / fg), launcher button +
# Ctrl+Alt+T, Italian AltGr symbols, window drag, Ctrl+Alt+Backspace back to
# the text console. A second session runs five hostile SWP clients (out of
# bounds PUT, lying length, unknown "read pixels" request, PUT before CREATE,
# oversized request) next to a good one; a third kills the compositor with
# SIGSEGV and checks the console comes back.
# ============================================================================
WMLOG=/tmp/secos_selftest_wm.log
WMDRV=/tmp/secos_selftest_wm.drv
echo "[selftest] Running GUI compositor gate (secwm + terminal + SWP isolation, mb2, serial+QMP, <=450s)..."
: > "$WMDRV"
timeout 480 python3 tools/gui_wm_drive.py --log "$WMLOG" --out "$WMDRV" --timeout 450 >/dev/null 2>&1
for t in wm-ready term-open client-window client-sandbox-fds client-sandbox-nocap client-sandbox-pts \
         client-sandbox-confined screen-size desktop-pixel panel-pixel not-text-console client-pixels \
         term-focus-click client-focus-out pty-echo-42 no-key-leak-to-client job-ctrl-c job-ctrl-z job-fg \
         pty-winsize shortcut-next-window client-focus-in client-key client-button no-key-leak-to-term \
         launcher-terminal shortcut-terminal layout-it layout-it-symbols layout-us lvgl-sysmon-ready lvgl-sysmon-pixels \
         lvgl-sysmon-click close-shortcut drag-move resize-grip resize-winsize \
         exit-combo exit-message text-console-back bash-after-exit \
         wm2-ready hostile-oob hostile-len hostile-read hostile-early hostile-big hostile-no-oob-pixels \
         hostile-good-client-intact hostile-wm-alive exit-button \
         crash-ready crash-restored crash-bash crash-text-console bash-responsive no-panic; do
    grep -q "\[WM-DRV\] $t OK" "$WMDRV"; check "GUI wm: $t" $?
done
[ "$(grep -c 'SW-VIOLATION client' "$WMLOG")" -eq 5 ]; check "GUI wm: exactly the 5 hostile clients were disconnected" $?
! grep -q "SWE-STILL-CONNECTED" "$WMLOG"; check "GUI wm: no hostile client stayed connected" $?

GUIKLOG=/tmp/secos_selftest_gui_it.log
GUIKDRV=/tmp/secos_selftest_gui_it.drv
echo "[selftest] Building KEYMAP=it image + USB HID keyboard gate (<=300s)..."
make clean >/dev/null 2>&1 || true
if make iso KEYMAP=it >/tmp/secos_selftest_build.log 2>&1; then
    : > "$GUIKDRV"
    timeout 330 python3 tools/gui_drive.py --log "$GUIKLOG" --out "$GUIKDRV" --scenario usbkbd >/dev/null 2>&1
    grep -q "\[KBD\] keymap it" "$GUIKLOG" && grep -q "\[GUI-DRV\] boot-keymap-it OK" "$GUIKDRV"
    check "M40 keymap: make KEYMAP=it boots with the Italian layout" $?
    grep -q "^GK-BYTES $KT_IT\$" "$GUIKLOG"; check "M40 keymap it (USB HID keyboard): same bytes as PS/2" $?
    grep -qF 'GI-EV "QEMU USB Keyboard" type=1 code=30 value=1' "$GUIKLOG" && grep -qF 'GI-EV "QEMU USB Keyboard" type=1 code=30 value=0' "$GUIKLOG"
    check "M40 evdev: USB HID keyboard KEY_A press/release" $?
    grep -q "G-END input fails=0" "$GUIKLOG"; check "M40 evdev: guitest input (with usb-kbd) completed with 0 failures" $?
    grep -q "\[GUI-DRV\] bash-responsive OK" "$GUIKDRV"; check "M40 bash responsive after the USB keyboard gate" $?
    ! grep -q "\[PANIC\]\|\[EXC\].*ring0" "$GUIKLOG"; check "M40 no kernel panic / ring-0 fault (KEYMAP=it run)" $?
else
    echo "[selftest] KEYMAP=it BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    check "M40 keymap: make KEYMAP=it builds" 1
fi

# ============================================================================
# M41 PERSISTENT ROOT GATE: the default `make run*` system disk (tools/mkroot.sh,
# ext4 + journal + metadata_csum) across boots, a kernel upgrade, hard kills and
# corrupt disks. tools/persist_test.sh does the work in a private mktemp dir
# (host port 24000-24999) and prints [PASS]/[FAIL] lines, counted here.
# ============================================================================
if command -v mkfs.ext4 >/dev/null 2>&1 && command -v debugfs >/dev/null 2>&1 && command -v ssh-keygen >/dev/null 2>&1; then
    echo "[selftest] M41 persistent root gate (2 builds, ~10 boots)..."
    PERSIST_OUT=$(mktemp /tmp/secos_selftest_persist.XXXXXX)
    tools/persist_test.sh --crash-runs "${PERSIST_CRASH_RUNS:-3}" >"$PERSIST_OUT" 2>&1
    while IFS= read -r l; do
        case "$l" in
            "  [PASS] "*) check "${l#  \[PASS\] }" 0 ;;
            "  [FAIL] "*) check "${l#  \[FAIL\] }" 1 ;;
            "    "*) echo "$l" ;;
        esac
    done <"$PERSIST_OUT"
    grep -q "RESULT:" "$PERSIST_OUT" || { check "PERSIST gate ran to completion" 1; tail -20 "$PERSIST_OUT"; }
    rm -f "$PERSIST_OUT"
else
    echo "  [SKIP] M41 persistent root gate (needs mkfs.ext4, debugfs, ssh-keygen)"
fi

# ============================================================================
# INIT GATE: /bin/init as PID 1 — a ready system with no manual steps.
# (a) An image built with SSH_AUTHORIZED_KEYS=<throwaway key generated here>
#     boots with a user-net NIC: init must be PID 1, ifup must take a DHCP
#     lease on its own, dropbear must be supervised, and ssh from the host must
#     work straight away (uname, ps, /proc/1/cmdline, ifconfig). The console
#     bash answers over serial, respawns after `exit`, an orphan is reaped by
#     init, and `poweroff` goes through init and makes QEMU exit by itself.
# (b) Same image, NIC without a DHCP server: ifup times out, the boot goes on;
#     then `kill -9 1` -> the kernel logs PID 1's death and the kernel shell
#     takes the console.
# (c) -DSECOS_INIT_TAMPER_TEST: /bin/init fails its signature check -> boot
#     falls back to the bash login shell.
# The host port is per run (23000-23999), never the user's 2222.
# ============================================================================
INITLOG=/tmp/secos_selftest_init.log
INITSER=/tmp/secos_selftest_init.serial
if ! command -v ssh >/dev/null 2>&1 || ! command -v ssh-keygen >/dev/null 2>&1; then
    echo "  [SKIP] INIT gate (host has no ssh / ssh-keygen)"
else
    INITDIR=$(mktemp -d /tmp/secos_selftest_init.XXXXXX)
    ssh-keygen -q -t ed25519 -N '' -C secos-init-selftest -f "$INITDIR/id"
    INITPORT=$((23000 + $$ % 1000))
    echo "[selftest] Building the default image with a baked throwaway SSH key..."
    make clean >/dev/null 2>&1 || true
    if ! make iso SSH_AUTHORIZED_KEYS="$INITDIR/id.pub" >/tmp/secos_selftest_build.log 2>&1; then
        echo "[selftest] INIT BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
        exit 2
    fi
    initssh() { # initssh <out> <command>
        timeout 40 ssh -p "$INITPORT" -i "$INITDIR/id" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no \
            -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o ConnectTimeout=20 -o LogLevel=ERROR \
            root@localhost "$2" </dev/null >"$1" 2>&1
        echo "RC=$?" >>"$1"
    }
    echo "[selftest] Running init PID 1 boot: DHCP + sshd + console + poweroff (mb2, ~90s)..."
    : > "$INITLOG"
    ( sleep 30
      initssh "$INITDIR/s1" 'uname -a; ps; echo CMDLINE-$(cat /proc/1/cmdline); ifconfig'
      printf '  echo INITCON-$((6*7))\r'; sleep 3
      printf '  exit\r'; sleep 4
      printf '  echo RESPAWN-$((1+1))\r'; sleep 3
      printf '  bash -c "sleep 1 &"; sleep 3; echo ORPHAN-$((2+3))\r'; sleep 6
      printf '  poweroff\r'; sleep 1 ) | \
        timeout 90 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
            -netdev user,id=n0,hostfwd=tcp:127.0.0.1:"$INITPORT"-:22 -device e1000,netdev=n0 \
            -debugcon file:"$INITLOG" -global isa-debugcon.iobase=0xe9 \
            -no-reboot -display none -serial stdio >"$INITSER" 2>&1
    init_rc=$?
    { echo "--- s1"; cat "$INITDIR/s1"; } >>"$INITLOG"
    grep -q "\[INIT\] /bin/init started, pid=0x0000000000000001 (PID 1)" "$INITLOG"; check "INIT /bin/init started by the kernel as PID 1" $?
    grep -q "\[init\] SecOS init (pid 1): 4 entries from /etc/inittab" "$INITSER"; check "INIT reads the default /etc/inittab" $?
    grep -q "\[SSH\] baked authorized_keys installed to /root/.ssh" "$INITLOG"; check "INIT baked SSH key installed as /root/.ssh/authorized_keys" $?
    grep -q "\[DHCP\] lease ip=" "$INITLOG" && grep -q "ifup: eth0: lease 10.0.2.15/24 gw 10.0.2.2" "$INITSER"
    check "INIT networking came up by itself (ifup took a DHCP lease)" $?
    grep -q "\[init\] started sshd (/bin/dropbearmulti) pid" "$INITSER"; check "INIT sshd started as a supervised service" $?
    grep -q "^SecOS secos" "$INITDIR/s1" && grep -q "^RC=0" "$INITDIR/s1"; check "INIT ssh with the baked key works at once (uname -a, rc 0)" $?
    grep -Eq "^ +1 root .* init$" "$INITDIR/s1" && grep -q "^CMDLINE-init" "$INITDIR/s1"; check "INIT ps / /proc/1/cmdline show init as PID 1" $?
    grep -Eq "dropbearmulti dropbear -F -R -p 22$" "$INITDIR/s1" && grep -Eq "/bin/bash -i$" "$INITDIR/s1"
    check "INIT ps shows the supervised sshd and the console bash" $?
    grep -q "UP RUNNING dhcp" "$INITDIR/s1"; check "INIT ifconfig reports the DHCP lease" $?
    grep -q "^INITCON-42" "$INITSER"; check "INIT console bash works over serial" $?
    grep -q "^RESPAWN-2" "$INITSER" && [[ "$(grep -c "\[init\] started console" "$INITSER")" -ge 2 ]]
    check "INIT console bash respawns after exit" $?
    grep -q "\[init\] reaped orphan pid" "$INITSER" && grep -q "^ORPHAN-5" "$INITSER"; check "INIT reaps an orphan re-parented to PID 1" $?
    grep -q "\[init\] shutdown requested (power off)" "$INITSER" && grep -q "\[POWER\] poweroff requested by pid=0x0000000000000001" "$INITLOG"
    check "INIT poweroff goes through init (stop services, sync, SYS_REBOOT)" $?
    [[ $init_rc -eq 0 ]]; check "INIT poweroff makes QEMU exit by itself (rc=$init_rc, 124 = hung)" $?
    ! grep -q "\[EXC\]\|\[PANIC\]" "$INITLOG"; check "INIT no CPU exception or panic" $?

    INITLOG2=/tmp/secos_selftest_init_nodhcp.log
    echo "[selftest] Running init with no DHCP server, then kill -9 1 (mb2, ~60s)..."
    : > "$INITLOG2"
    ( sleep 32; printf '  echo NODHCP-$((4+4))\r'; sleep 3
      printf '  kill -9 1\r'; sleep 6
      printf '  vls /etc\r'; sleep 3 ) | \
        timeout 60 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
            -netdev socket,id=n0,mcast=230.0.0.1:$INITPORT -device e1000,netdev=n0 \
            -debugcon file:"$INITLOG2" -global isa-debugcon.iobase=0xe9 \
            -no-reboot -display none -serial stdio >"$INITSER.2" 2>&1
    grep -q "ifup: eth0: no DHCP lease" "$INITSER.2" && grep -q "^NODHCP-8" "$INITSER.2"
    check "INIT no DHCP server: ifup times out and the boot goes on" $?
    grep -q "\[INIT\] PID 1 (/bin/init) exited" "$INITLOG2" && grep -q "\[INIT\] PID 1 died; fallback to the kernel shell" "$INITLOG2"
    check "INIT death of PID 1 is logged" $?
    grep -q "inittab" "$INITSER.2" && grep -q "secos:/" "$INITSER.2"; check "INIT kernel shell takes the console after PID 1 dies" $?
    ! grep -q "\[EXC\]\|\[PANIC\]" "$INITLOG2"; check "INIT no CPU exception or panic (no DHCP, PID 1 killed)" $?

    INITLOG3=/tmp/secos_selftest_init_tamper.log
    echo "[selftest] Building -DSECOS_INIT_TAMPER_TEST and running the fallback (mb2, ~40s)..."
    make clean >/dev/null 2>&1 || true
    if ! make iso CFLAGS_EXTRA=-DSECOS_INIT_TAMPER_TEST >/tmp/secos_selftest_build.log 2>&1; then
        echo "[selftest] INIT TAMPER BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
        exit 2
    fi
    : > "$INITLOG3"
    ( sleep 25; printf '  echo FALLBACK-$((5+5))\r'; sleep 4 ) | \
        timeout 40 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
            -debugcon file:"$INITLOG3" -global isa-debugcon.iobase=0xe9 \
            -no-reboot -display none -serial stdio >"$INITSER.3" 2>&1
    grep -q "\[INIT\] FAIL: /bin/init refused (signature/load); fallback to bash" "$INITLOG3"
    check "INIT unsigned /bin/init is refused" $?
    grep -q "falling back to bash" "$INITSER.3" && grep -q "^FALLBACK-10" "$INITSER.3"; check "INIT fallback: the bash login shell works" $?
    # [M43] Same tampered init with the default console policy (CONSOLE=login):
    # the kernel must offer /bin/login, never an unauthenticated root bash.
    make clean >/dev/null 2>&1 || true
    if make iso CONSOLE=login CFLAGS_EXTRA=-DSECOS_INIT_TAMPER_TEST >/tmp/secos_selftest_build.log 2>&1; then
        : > "$INITLOG3"
        ( sleep 25; printf '  echo FALLBACK-$((5+5))\r'; sleep 4 ) | \
            timeout 40 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
                -debugcon file:"$INITLOG3" -global isa-debugcon.iobase=0xe9 \
                -no-reboot -display none -serial stdio >"$INITSER.4" 2>&1
        grep -q "\[INIT\] console fallback: /bin/login" "$INITLOG3" && grep -aq "Welcome to SecOS\| login: " "$INITSER.4"
        check "INIT fallback (CONSOLE=login): unsigned init -> the console gets /bin/login" $?
        ! grep -aq "^FALLBACK-10" "$INITSER.4"; check "INIT fallback (CONSOLE=login): no unauthenticated root shell" $?
    else
        check "INIT fallback (CONSOLE=login) image build" 1
    fi
    rm -rf "$INITDIR"
    make clean >/dev/null 2>&1 || true
    make iso >/tmp/secos_selftest_build.log 2>&1   # leave a plain default image behind
fi

# ---- M41: exFAT read-write (host-made image, SecOS writes, corrupt images) ----
# The image is made by the real host tools (mkfs.exfat + exfat-fuse, fetched on
# first use without sudo — tools/exfat-host.sh). SecOS reads the host files
# (long Unicode names, 3 MiB + fragmented files), writes/mkdirs/unlinks/renames,
# unmounts cleanly; then the host must find it fsck.exfat-clean, deepcheck-clean
# and read every file back identically. A second boot attaches six damaged
# images (AHCI x4 + NVMe + USB): each is refused or fails per object, no fault.
M41LOG=/tmp/secos_selftest_m41.log
M41CLOG=/tmp/secos_selftest_m41_corrupt.log
M41ULOG=/tmp/secos_selftest_m41_user.log
M41USER=/tmp/secos_selftest_m41_user.ser
echo "[selftest] Building M41 image (M41_EXFAT_DEMO=1)..."
make clean >/dev/null 2>&1 || true
if ! make iso CFLAGS_EXTRA=-DM41_EXFAT_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M41 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
if make disk-exfat >/tmp/secos_selftest_m41_disk.log 2>&1; then
    echo "[selftest] Running M41 exFAT good-image suite (virtio, 25s)..."
    : > "$M41LOG"
    timeout 25 qemu-system-x86_64 -cdrom myos.iso -drive file=disk.img,if=virtio,format=raw -boot d \
        -debugcon file:"$M41LOG" -global isa-debugcon.iobase=0xe9 -no-reboot -display none -m 256M
    grep -q "\[EXFAT\] mounted vda at /mnt (rw)" "$M41LOG"; check "M41 boot auto-mounts the exFAT disk at /mnt" $?
    for t in "longname read" "case-insensitive lookup" "emoji (surrogate pair) name read" \
             "big.bin 3MiB host file verify" "frag_a.bin fragmented host file verify" \
             "create unicode file" "VolumeDirty set while mounted rw" "mkdir" "mkdir+rmdir" \
             "rmdir non-empty refused" "unlink host file" "rename across dirs" "rename over existing target" \
             "truncate shrink+grow (zero tail)" "write 2MiB file" "interleaved (fragmenting) writes" \
             "write beyond VDL zero-fills gap" "directory grows past one cluster (80 creates, 40 unlinks)" \
             "/proc/mounts lists exfat" "statfs type=exfat" "VolumeDirty cleared on clean unmount" \
             "persistence after remount"; do
        grep -qF "[M41] $t: OK" "$M41LOG"; check "M41 $t" $?
    done
    n_ok=$(grep -c "^\[M41\] .*: OK$" "$M41LOG" || true); n_bad=$(grep -c "^\[M41\] .*: FAIL$" "$M41LOG" || true)
    [[ "$n_ok" -ge 38 && "$n_bad" -eq 0 ]]; check "M41 all in-kernel exFAT checks OK (ok=$n_ok fail=$n_bad)" $?
    grep -q "\[M41\] DONE" "$M41LOG"; check "M41 good-image suite completed" $?
    ! grep -q "\[EXC\]" "$M41LOG"; check "no CPU exception ([EXC]) during M41 good-image run" $?
    tools/exfat-host.sh verify disk.img >/tmp/secos_selftest_m41_verify.log 2>&1
    check "M41 host: fsck.exfat -n + deepcheck clean, VolumeDirty clear, all files read back identically" $?
    if make disk-exfat-corrupt >/tmp/secos_selftest_m41_disk.log 2>&1; then
        echo "[selftest] Running M41 exFAT corrupt-image suite (q35, 6 disks, 30s)..."
        : > "$M41CLOG"
        C=exfat_corrupt
        timeout 30 qemu-system-x86_64 -machine q35 -m 256M -cdrom myos.iso -boot d \
            -drive file=$C/files.img,format=raw,if=ide,index=0 \
            -drive file=$C/csum1.img,format=raw,if=ide,index=1 \
            -drive file=$C/csum2.img,format=raw,if=ide,index=3 \
            -drive file=$C/upcase.img,format=raw,if=ide,index=4 \
            -drive file=$C/rootcycle.img,if=none,id=nvm,format=raw -device nvme,serial=secos1,drive=nvm \
            -device qemu-xhci,id=xhci -drive file=$C/geo.img,if=none,id=us,format=raw -device usb-storage,bus=xhci.0,drive=us \
            -debugcon file:"$M41CLOG" -global isa-debugcon.iobase=0xe9 -no-reboot -no-shutdown -display none
        sed -n '/dev=sda mount rc=0x0000000000000000/,/dev=sdb/p' "$M41CLOG" | grep -q "ok.txt read=0x0000000000000005"
        check "M41 corrupt/files: volume mounts, intact file still reads" $?
        grep -q "oob.txt read=0xFFFFFFFFFFFFFFFF" "$M41CLOG"; check "M41 corrupt/files: out-of-range cluster -> read error" $?
        grep -q "cycle.bin read=0xFFFFFFFFFFFFFFFF" "$M41CLOG" && grep -q "cycle.bin write=0xFFFFFFFFFFFFFFFF" "$M41CLOG"
        check "M41 corrupt/files: FAT cycle -> read/write error (no hang)" $?
        grep -q "badsum.txt visible=0x0000000000000000" "$M41CLOG" && grep -q "badcnt.txt visible=0x0000000000000000" "$M41CLOG"
        check "M41 corrupt/files: bad SetChecksum / SecondaryCount sets are skipped" $?
        grep -q "loopdir readdir=0xFFFFFFFFFFFFFFFF" "$M41CLOG"; check "M41 corrupt/files: cyclic directory chain -> readdir error" $?
        grep -q "main boot region invalid; using backup (read-only)" "$M41CLOG" && \
            sed -n '/dev=sdb mount rc=0x0000000000000000/,/dev=sdc/p' "$M41CLOG" | grep -q "create rc=0xFFFFFFFFFFFFFFFF"
        check "M41 corrupt/csum1: main checksum bad -> backup region, read-only" $?
        grep -q "dev=sdc mount rc=0xFFFFFFFFFFFFFFFF" "$M41CLOG"; check "M41 corrupt/csum2: both boot checksums bad -> refused" $?
        grep -q "dev=sdd mount rc=0xFFFFFFFFFFFFFFFF" "$M41CLOG" && grep -q "up-case table checksum mismatch" "$M41CLOG"
        check "M41 corrupt/upcase: up-case checksum bad -> refused" $?
        grep -q "dev=nvme0n1 mount rc=0xFFFFFFFFFFFFFFFF" "$M41CLOG" && grep -q "root directory chain corrupt or cyclic" "$M41CLOG"
        check "M41 corrupt/rootcycle: cyclic root chain -> refused" $?
        grep -q "dev=usb0 mount rc=0xFFFFFFFFFFFFFFFF" "$M41CLOG"; check "M41 corrupt/geo: impossible geometry -> refused" $?
        grep -q "\[M41C\] DONE (kernel alive)" "$M41CLOG" && ! grep -q "\[EXC\]\|\[PANIC\]" "$M41CLOG"
        check "M41 corrupt suite: kernel alive, no CPU exception or panic" $?
        if make -C tools/exfat_fuzz >/dev/null 2>&1; then
            echo "[selftest] Running exFAT host fuzz (ASan+UBSan, 3000 mutated images)..."
            timeout 300 tools/exfat_fuzz/exfat_fuzz exfat_corrupt/base.img 3000 41 >/tmp/secos_selftest_m41_fuzz.log 2>&1
            check "M41 host fuzz: 3000 mutated images, no crash/UB/hang" $?
        else
            echo "  [SKIP] M41 host fuzz (gcc with ASan/UBSan unavailable)"
        fi
    else
        echo "  [FAIL] M41 corrupt image build (see /tmp/secos_selftest_m41_disk.log)"; FAIL=$((FAIL+1))
    fi
    # Userland: BusyBox mount/df/umount over the serial bash, default image.
    make clean >/dev/null 2>&1 || true
    if make iso >/tmp/secos_selftest_build.log 2>&1 && make disk-exfat >/tmp/secos_selftest_m41_disk.log 2>&1; then
        echo "[selftest] Running M41 userland mount -t exfat over serial bash (60s)..."
        : > "$M41ULOG"
        ( sleep 28
          printf '  umount /mnt; mkdir -p /x; mount -t exfat /dev/vda /x && echo MX1-$(cat /x/hello.txt)\r'; sleep 4
          printf '  echo MX2-$(grep -c " /x exfat " /proc/mounts)\r'; sleep 3
          printf '  df | grep /x | sed s/^/MX3-/\r'; sleep 3
          printf '  cat /x/Relazione* | sed s/^/MX4-/\r'; sleep 3
          printf '  echo from-bash > /x/bash_wrote.txt; mkdir /x/bdir && mv /x/bash_wrote.txt /x/bdir/ && echo MX5-$(cat /x/bdir/bash_wrote.txt)\r'; sleep 4
          printf '  umount /x && echo MX6-$(grep -c exfat /proc/mounts)\r'; sleep 3
          printf '  mount -t exfat /dev/vda /x && echo MX7-$(cat /x/bdir/bash_wrote.txt) && umount /x\r'; sleep 4
        ) | timeout 60 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
            -drive file=disk.img,if=virtio,format=raw \
            -debugcon file:"$M41ULOG" -global isa-debugcon.iobase=0xe9 \
            -no-reboot -display none -serial stdio >"$M41USER" 2>&1
        grep -q "^MX1-hello from disk" "$M41USER"; check "M41 user: mount -t exfat /dev/vda /x (BusyBox, SYS_MOUNT)" $?
        grep -q "^MX2-1" "$M41USER"; check "M41 user: /proc/mounts shows the exfat mount" $?
        grep -q "^MX3-/dev/vda .* /x" "$M41USER"; check "M41 user: df lists the exfat mount" $?
        grep -q "^MX4-Il documento" "$M41USER"; check "M41 user: bash reads a long Unicode name" $?
        grep -q "^MX5-from-bash" "$M41USER" && grep -q "^MX6-0" "$M41USER" && grep -q "^MX7-from-bash" "$M41USER"
        check "M41 user: write + mkdir + mv, umount, remount persists" $?
        tools/exfat-host.sh fsck disk.img >/dev/null 2>&1 && python3 -I tools/exfat_img.py deepcheck disk.img >/dev/null 2>&1 \
            && [[ "$(python3 -I tools/exfat_img.py dirty disk.img)" == "0" ]]
        check "M41 user: image fsck/deepcheck-clean and VolumeDirty clear after umount" $?
    else
        echo "  [FAIL] M41 userland image build"; FAIL=$((FAIL+1))
    fi
else
    echo "  [FAIL] M41 disk-exfat build (exfatprogs/exfat-fuse fetch failed? see /tmp/secos_selftest_m41_disk.log)"
    FAIL=$((FAIL+1))
fi

# ============================================================================
# MOUNT HARDENING GATE (fix/mount-hardening). Three bugs found by the M41 exFAT
# work, all outside its driver:
# (1) vfs_mount() stored the caller's mount-point pointer — a kernel-stack copy
#     for SYS_MOUNT — so a userland `mount -t vfat|ext2` left a dangling entry
#     (/proc/mounts garbage; a garbage mount point could shadow /bin). The VFS
#     now owns the string. Also: a second fat32/ext2 instance is refused (it
#     hijacked the attached one) and umount is -EBUSY while a file is open.
# (2) fat32 page-cache staleness on inode-slot reuse (remount, >32 names,
#     "f9" vs "F9" aliases); the VFS also drops an FS's pages on (un)mount.
# (3) st_dev was one constant: per-mount ids now (block-backed = Linux dev_t).
# One -DSECOS_HEAP_DEBUG run on q35/AHCI: vfat sda (boot /mnt), ext2 sdb,
# exFAT sdc. BusyBox over the serial bash; heapcheck from the recovery shell.
# ============================================================================
MHLOG=/tmp/secos_selftest_mnt.log
MHSER=/tmp/secos_selftest_mnt.ser
echo "[selftest] Building mount-hardening disks + SECOS_HEAP_DEBUG image..."
make clean >/dev/null 2>&1 || true
MHD=mnt_hardening_disks; rm -rf "$MHD"; mkdir -p "$MHD/stage"
printf 'hello from disk\n' > "$MHD/stage/hello.txt"
if dd if=/dev/zero of="$MHD/fat.img" bs=1M count=64 status=none && mkfs.fat -F 32 -n SECOSDATA "$MHD/fat.img" >/dev/null \
   && mcopy -i "$MHD/fat.img" "$MHD/stage/hello.txt" ::HELLO.TXT \
   && dd if=/dev/zero of="$MHD/ext2.img" bs=1M count=64 status=none && mkfs.ext2 -q -L SECOSEXT -d "$MHD/stage" "$MHD/ext2.img" \
   && bash tools/exfat-host.sh mkgood "$MHD/exfat.img" >/tmp/secos_selftest_mnt_disk.log 2>&1 \
   && make iso CFLAGS_EXTRA=-DSECOS_HEAP_DEBUG >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] Running mount-hardening gate (q35, 3 SATA disks, serial bash, 200s)..."
    : > "$MHLOG"
    ( sleep 30
      printf '  echo MH1-$(stat -c %%d /)-$(stat -c %%d /mnt)-$(stat -c %%d /proc)\r'; sleep 3
      printf '  df /mnt | tail -1 | sed s/^/MH2-/\r'; sleep 3
      printf '  mkdir -p /mnt/SUB /x /y /z /w && mount -t ext2 /dev/sdb /mnt/SUB && echo MH3-$(find /mnt -xdev | grep -ic hello)-$(find /mnt | grep -ic hello)-$(stat -c %%d /mnt/SUB)-$(df /mnt/SUB | tail -1 | cut -d" " -f1); umount /mnt/SUB\r'; sleep 6
      printf '  umount /mnt; echo MH4-$?\r'; sleep 3
      printf '  ok=0; for i in $(seq 50); do mount -t vfat /dev/sda /x; mount -t ext2 /dev/sdb /y; mount -t exfat /dev/sdc /z; m=$(cat /proc/mounts); case "$m" in *"/dev/sda /x vfat "*"/dev/sdb /y ext2 "*"/dev/sdc /z exfat "*) ok=$((ok+1));; esac; umount /x; umount /y; umount /z; case "$(cat /proc/mounts)" in *" /x "*|*" /y "*|*" /z "*) ok=$((ok-100));; esac; done; echo MH5-$ok-$(grep -vc "^[a-z/]* /[a-z]* [a-z0-9]* rw" /proc/mounts)\r'; sleep 45
      printf '  mount -t vfat /dev/sda /x && for i in $(seq 40); do echo content-$i > /x/f$i; done; bad=0; for i in $(seq 40); do [ "$(cat /x/f$i)" = "content-$i" ] || bad=$((bad+1)); done; echo MH6-$bad\r'; sleep 12
      printf '  umount /x; mount -t vfat /dev/sda /x; cat /x/f7 >/dev/null; umount /x; mount -t vfat /dev/sda /x; echo MH7-$(cat /x/f8)\r'; sleep 5
      printf '  cat /x/F9 >/dev/null; echo changed-9 > /x/f9; echo MH8-$(cat /x/F9); umount /x\r'; sleep 4
      printf '  mount -t ext2 /dev/sdb /y; exec 3</y/hello.txt; umount /y; echo MH9-$?; exec 3<&-; umount /y; echo MH10-$?\r'; sleep 4
      printf '  mount -t ext2 /dev/sdb /y; mount -t ext2 /dev/sdb /w; echo MH11-$?-$(grep -c " /w " /proc/mounts)-$(cat /y/hello.txt); umount /y\r'; sleep 4
      printf '  mount -t vfat /dev/sda /x/; echo MH12-$?-$(grep -c "^/dev/sda /x vfat" /proc/mounts); umount /x/; echo MH13-$?\r'; sleep 4
      printf '  echo MHEND-$(ls /bin | wc -l)\r'; sleep 3
      printf '  kill -9 1\r'; sleep 6
      printf '  heapcheck\r'; sleep 4
    ) | timeout 200 qemu-system-x86_64 -machine q35 -cdrom myos.iso -boot d -m 256M \
        -drive file="$MHD/fat.img",format=raw,if=ide,index=0 \
        -drive file="$MHD/ext2.img",format=raw,if=ide,index=1 \
        -drive file="$MHD/exfat.img",format=raw,if=ide,index=3 \
        -debugcon file:"$MHLOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >"$MHSER" 2>&1
    tr -d '\r' < "$MHSER" > "$MHSER.txt"; MHSER="$MHSER.txt"
    # sda = 8:0 = 2048, sdb = 8:16 = 2064 (glibc makedev encoding)
    read -r mh_r mh_m mh_p <<<"$(grep -a '^MH1-' "$MHSER" | head -1 | tr -d '\r' | cut -d- -f2- | tr - ' ')"
    [[ -n "$mh_r" && "$mh_m" == "2048" && "$mh_r" != "$mh_m" && "$mh_p" != "$mh_r" && "$mh_p" != "$mh_m" ]]
    check "MNT st_dev: / , /mnt (sda=8:0) and /proc differ (stat -c %d)" $?
    grep -aq "^MH2-/dev/sda .* /mnt$" "$MHSER"; check "MNT df /mnt names /dev/sda, not /" $?
    grep -aq "^MH3-1-2-2064-/dev/sdb$" "$MHSER"; check "MNT find -xdev stops at a nested mount; df/stat see ext2 on sdb" $?
    grep -aq "^MH5-50-0$" "$MHSER"; check "MNT 50x mount/umount vfat+ext2+exfat from userland: /proc/mounts exact every time" $?
    grep -aq "^MH6-0$" "$MHSER"; check "MNT fat32: 40 files written then read back (no stale page past the old 32-slot pool)" $?
    grep -aq "^MH7-content-8$" "$MHSER"; check "MNT fat32: remount does not serve the previous file's cached pages" $?
    grep -aq "^MH8-changed-9$" "$MHSER"; check "MNT fat32: f9/F9 share one inode (coherent cache)" $?
    grep -aq "^MH9-1$" "$MHSER" && grep -aq "^MH10-0$" "$MHSER"; check "MNT umount -EBUSY with an open fd, succeeds once closed" $?
    grep -aq "^MH11-[1-9][0-9]*-0-hello from disk$" "$MHSER"; check "MNT second ext2 mount refused, first one intact" $?
    grep -aq "^MH12-0-1$" "$MHSER" && grep -aq "^MH13-0$" "$MHSER"; check "MNT mount/umount with a trailing slash" $?
    grep -aq "^MHEND-[1-9][0-9]" "$MHSER"; check "MNT /bin still intact after the loop" $?
    grep -aq "\[HEAP\] heapcheck: OK" "$MHLOG" && ! grep -aq "\[HEAP\] CORRUPTION DETECTED" "$MHLOG"
    check "MNT heap-debug: no heap corruption, heapcheck OK" $?
    ! grep -aq "\[EXC\]\|\[PANIC\]" "$MHLOG"; check "MNT no CPU exception or panic" $?
else
    echo "  [FAIL] mount-hardening image/disk build (see /tmp/secos_selftest_mnt_disk.log, /tmp/secos_selftest_build.log)"
    FAIL=$((FAIL+1))
fi
rm -rf "$MHD"
# ============================================================================
# READ-ONLY MOUNT GATE ([M46], docs/devlog/M46-exec-confinement.md): `mount -o
# ro` for FAT32 and ext2/ext4 (EROFS on every change, nothing written to the
# disk), `mount -o remount,ro|rw`, and ext4 `ro,noload` (no journal replay; a
# plain ro mount replays a dirty journal, as Linux does). q35/AHCI: vfat sda
# (boot /mnt), clean ext4 sdb (must stay byte-identical), a dirty-journal ext4
# sdc (the boot root probe must not replay it) and an ext4 sdd for remount.
# ============================================================================
ROD=/tmp/secos_selftest_ro
ROLOG=/tmp/secos_selftest_ro.log
ROSER=/tmp/secos_selftest_ro.ser
echo "[selftest] Building read-only mount disks + default image..."
make clean >/dev/null 2>&1 || true
rm -rf "$ROD"; mkdir -p "$ROD/stage" "$ROD/jstage"
printf 'hello from disk\n' > "$ROD/stage/hello.txt"
printf 'OLDOLDOLDOLD\n' > "$ROD/jstage/target.bin"
if dd if=/dev/zero of="$ROD/fat.img" bs=1M count=64 status=none && mkfs.fat -F 32 -n SECOSDATA "$ROD/fat.img" >/dev/null \
   && mcopy -i "$ROD/fat.img" "$ROD/stage/hello.txt" ::HELLO.TXT \
   && dd if=/dev/zero of="$ROD/ext4.img" bs=1M count=64 status=none && mkfs.ext4 -q -F -L SECOSRO -d "$ROD/stage" "$ROD/ext4.img" \
   && cp "$ROD/ext4.img" "$ROD/ext4rw.img" \
   && mke2fs -F -q -t ext4 -O has_journal,^metadata_csum,^64bit -b 1024 -d "$ROD/jstage" "$ROD/jrnl.img" 16384 >/dev/null \
   && python3 tools/mk_dirty_journal.py "$ROD/jrnl.img" /target.bin NEW >/dev/null 2>&1 \
   && make iso >/tmp/secos_selftest_build.log 2>&1; then
    ro_md5=$(md5sum < "$ROD/ext4.img")
    echo "[selftest] Running read-only mount gate (q35, 4 SATA disks, serial bash, 120s)..."
    : > "$ROLOG"
    ( sleep 30
      printf '  umount /mnt; mkdir -p /x /y /z /w; echo RO0-$?\r'; sleep 3
      printf '  mount -t vfat -o ro /dev/sda /x && echo RO1-$(cat /x/hello.txt)-$(grep -c "^/dev/sda /x vfat ro" /proc/mounts)\r'; sleep 3
      printf '  touch /x/new 2>/tmp/e; echo RO2-$?-$(ls /x | grep -ci new)-$(grep -c "Read-only file system" /tmp/e)\r'; sleep 3
      printf '  rm -f /x/hello.txt; mkdir /x/d 2>/dev/null; mv /x/hello.txt /x/h2 2>/dev/null; echo RO3-$(cat /x/hello.txt)-$(ls /x | grep -c "^d$")\r'; sleep 3
      printf '  mount -o remount,rw /x && echo hi > /x/new && echo RO4-$(cat /x/new)-$(grep -c "^/dev/sda /x vfat rw" /proc/mounts)\r'; sleep 3
      printf '  mount -o remount,ro /x && (echo z > /x/new) 2>/dev/null; echo RO5-$?-$(cat /x/new)-$(grep -c "^/dev/sda /x vfat ro" /proc/mounts); umount /x\r'; sleep 3
      printf '  mount -t ext4 -o ro /dev/sdb /y && echo RO6-$(cat /y/hello.txt)-$(grep -c "^/dev/sdb /y ext4 ro" /proc/mounts)\r'; sleep 3
      printf '  mkdir /y/d 2>/dev/null; echo RO7-$?; (echo x > /y/hello.txt) 2>/dev/null; echo RO8-$?-$(cat /y/hello.txt)\r'; sleep 3
      printf '  chmod 600 /y/hello.txt 2>/dev/null; echo RO9-$?; ln -s a /y/l 2>/dev/null; echo RO10-$?; umount /y; echo RO11-$?\r'; sleep 3
      printf '  mount -t ext4 -o ro /dev/sdd /w && mount -o remount,rw /w && echo w > /w/w.txt && echo RO12-$(cat /w/w.txt)-$(grep -c "^/dev/sdd /w ext4 rw" /proc/mounts)\r'; sleep 3
      printf '  mount -o remount,ro /w && touch /w/w2 2>/dev/null; echo RO13-$?-$(grep -c "^/dev/sdd /w ext4 ro" /proc/mounts); umount /w\r'; sleep 3
      printf '  mount -t ext4 -o noload /dev/sdc /z 2>/dev/null && echo RO14-0 || echo RO14-1\r'; sleep 3
      printf '  mount -t ext4 -o ro,noload /dev/sdc /z && echo RO15-$(head -c 3 /z/target.bin)\r'; sleep 3
      printf '  mount -o remount,rw /z 2>/dev/null && echo RO16-0 || echo RO16-1; umount /z\r'; sleep 3
      printf '  mount -t ext4 -o ro /dev/sdc /z && echo RO17-$(head -c 3 /z/target.bin); umount /z; echo ROEND\r'; sleep 4
    ) | timeout 120 qemu-system-x86_64 -machine q35 -cdrom myos.iso -boot d -m 256M \
        -drive file="$ROD/fat.img",format=raw,if=ide,index=0 \
        -drive file="$ROD/ext4.img",format=raw,if=ide,index=1 \
        -drive file="$ROD/jrnl.img",format=raw,if=ide,index=3 \
        -drive file="$ROD/ext4rw.img",format=raw,if=ide,index=4 \
        -debugcon file:"$ROLOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >"$ROSER" 2>&1
    tr -d '\r' < "$ROSER" > "$ROSER.txt"; ROSER="$ROSER.txt"
    ro() { grep -aq "^$1\$" "$ROSER"; check "RO $2" $?; }
    ro "RO1-hello from disk-1" "vfat: mount -o ro, /proc/mounts says ro"
    ro "RO2-1-0-1"             "vfat ro: touch fails with EROFS, no file created"
    ro "RO3-hello from disk-0" "vfat ro: rm/mkdir/mv refused, file intact"
    ro "RO4-hi-1"              "vfat: mount -o remount,rw, then a write works"
    ro "RO5-1-hi-1"            "vfat: mount -o remount,ro, then writes fail again"
    ro "RO6-hello from disk-1" "ext4: mount -o ro, /proc/mounts says ro"
    ro "RO7-1"                 "ext4 ro: mkdir refused"
    ro "RO8-1-hello from disk" "ext4 ro: write refused, file intact"
    ro "RO9-1"                 "ext4 ro: chmod refused"
    ro "RO10-1"                "ext4 ro: symlink refused"
    ro "RO11-0"                "ext4 ro: umount"
    [[ "$(md5sum < "$ROD/ext4.img")" == "$ro_md5" ]]; check "RO ext4 ro mount wrote nothing (image byte-identical)" $?
    ro "RO12-w-1"              "ext4: ro then remount,rw, a write works"
    ro "RO13-1-1"              "ext4: remount,ro refuses writes again"
    [[ "$(debugfs -R 'cat /w.txt' "$ROD/ext4rw.img" 2>/dev/null)" == "w" ]] && e2fsck -fn "$ROD/ext4rw.img" >/dev/null 2>&1
    check "RO ext4 remounted volume: the rw write is on disk, e2fsck-clean" $?
    ro "RO14-1"                "ext4: noload without ro is refused (EINVAL)"
    ro "RO15-OLD"              "ext4 ro,noload: the dirty journal is not replayed (boot probe left it too)"
    ro "RO16-1"                "ext4: remount,rw of a noload mount with a dirty journal is refused"
    ro "RO17-NEW"              "ext4 ro: a dirty journal is replayed (Linux semantics)"
    e2fsck -fn "$ROD/jrnl.img" >/dev/null 2>&1; check "RO replayed journal volume e2fsck-clean" $?
    grep -aq "^ROEND" "$ROSER"; check "RO script completed" $?
    ! grep -aq "\[EXC\]\|\[PANIC\]" "$ROLOG"; check "RO no CPU exception or panic" $?
else
    echo "  [FAIL] read-only mount image/disk build (see /tmp/secos_selftest_build.log)"
    FAIL=$((FAIL+1))
fi
rm -rf "$ROD"
# ---- M42: NTFS read-only (host-made image, listing vs ntfs-3g, corrupt images) ----
# The image is made by the real host tools (mkntfs + ntfs-3g in a user
# namespace, tools/ntfs-host.sh); ntfs-3g's listing of it (type, size and
# content hash of every object) is the reference. SecOS walks the whole volume
# in the kernel and its listing must match exactly; targeted checks cover
# Unicode / case-insensitive names, the fragmented ($ATTRIBUTE_LIST), sparse
# and LZNT1 files, EFS -> EACCES, symlinks/junctions and EROFS on every write.
# A second boot attaches six damaged images (AHCI x4 + NVMe + USB); the host
# harness checks two more and fuzzes the driver under ASan/UBSan.
M42LOG=/tmp/secos_selftest_m42.log
M42CLOG=/tmp/secos_selftest_m42_corrupt.log
M42ULOG=/tmp/secos_selftest_m42_user.log
M42USER=/tmp/secos_selftest_m42_user.ser
echo "[selftest] Building M42 image (M42_NTFS_DEMO=1)..."
make clean >/dev/null 2>&1 || true
if ! make iso CFLAGS_EXTRA=-DM42_NTFS_DEMO=1 >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] M42 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    tail -20 /tmp/secos_selftest_build.log >&2; exit 2
fi
if make disk-ntfs >/tmp/secos_selftest_m42_disk.log 2>&1; then
    echo "[selftest] Running M42 NTFS good-image suite (virtio, 45s)..."
    : > "$M42LOG"
    timeout 45 qemu-system-x86_64 -cdrom myos.iso -drive file=disk.img,if=virtio,format=raw -boot d \
        -debugcon file:"$M42LOG" -global isa-debugcon.iobase=0xe9 -no-reboot -display none -m 256M
    grep -q "\[NTFS\] mounted vda at /mnt (ro)" "$M42LOG"; check "M42 boot auto-mounts the NTFS disk at /mnt (read-only)" $?
    for t in "full tree walk without readdir/lookup errors" "case-insensitive lookup (\$UpCase)" \
             "unicode long name read" "case-insensitive non-ASCII lookup" "CJK name read" \
             "DOS 8.3 alias hidden from readdir" "metafiles hidden (\$MFT, \$Volume, ...)" \
             "2000-entry multi-level directory lists everything" \
             "fragmented.bin (\$ATTRIBUTE_LIST, ~1300 runs) verify" "big.bin 5MiB verify" \
             "sparse.bin 48MiB (holes read as zero) verify" "compressed (LZNT1) file readable" \
             "EFS-encrypted file -> EACCES" "hard link: nlink=2" "Windows relative symlink" \
             "junction -> path under the mount point" "WSL + Interix symlinks" \
             "every write op refused with EROFS (create/mkdir/unlink/rename/truncate/write/symlink/link/rmdir/chmod)" \
             "read-write mount request refused" "/proc/mounts lists /mnt ntfs ro" "/proc/filesystems lists ntfs" \
             "statfs type=NTFS, read-only, sane counts" "second simultaneous NTFS instance" \
             "stale inode after unmount fails cleanly" "remount + read"; do
        grep -qF "[M42] $t: OK" "$M42LOG"; check "M42 $t" $?
    done
    n_ok=$(grep -c "^\[M42\] .*: OK$" "$M42LOG" || true); n_bad=$(grep -c "^\[M42\] .*: FAIL$" "$M42LOG" || true)
    [[ "$n_ok" -ge 37 && "$n_bad" -eq 0 ]]; check "M42 all in-kernel NTFS checks OK (ok=$n_ok fail=$n_bad)" $?
    grep -q "\[M42\] DONE" "$M42LOG"; check "M42 good-image suite completed" $?
    ! grep -q "\[EXC\]\|\[PANIC\]" "$M42LOG"; check "no CPU exception during M42 good-image run" $?
    grep "^\[M42L\] " "$M42LOG" | sed 's/^\[M42L\] //' > /tmp/secos_selftest_m42.list
    python3 -I tools/ntfs_img.py compare disk.img.list /tmp/secos_selftest_m42.list /mnt >/tmp/secos_selftest_m42_cmp.log 2>&1
    rc=$?; check "M42 kernel listing == ntfs-3g listing (size + content hash of every file; $(tail -1 /tmp/secos_selftest_m42_cmp.log))" $rc
    if make disk-ntfs-corrupt >/tmp/secos_selftest_m42_disk.log 2>&1; then
        echo "[selftest] Running M42 NTFS corrupt-image suite (q35, 6 disks, 40s)..."
        : > "$M42CLOG"
        C=ntfs_corrupt
        timeout 40 qemu-system-x86_64 -machine q35 -m 256M -cdrom myos.iso -boot d \
            -drive file=$C/files.img,format=raw,if=ide,index=0 \
            -drive file=$C/bootmain.img,format=raw,if=ide,index=1 \
            -drive file=$C/mft0.img,format=raw,if=ide,index=3 \
            -drive file=$C/mftboth.img,format=raw,if=ide,index=4 \
            -drive file=$C/upcase.img,if=none,id=nvm,format=raw -device nvme,serial=secos1,drive=nvm \
            -device qemu-xhci,id=xhci -drive file=$C/rootbad.img,if=none,id=us,format=raw -device usb-storage,bus=xhci.0,drive=us \
            -debugcon file:"$M42CLOG" -global isa-debugcon.iobase=0xe9 -no-reboot -no-shutdown -display none
        SDA="$(sed -n '/dev=sda mount rc=0x0000000000000000/,/dev=sdb/p' "$M42CLOG")"
        grep -q "/mnt/ok.txt read=0x0000000000000007" <<<"$SDA"; check "M42 corrupt/files: volume mounts, intact file reads" $?
        grep -q "/mnt/runbad.bin read=0xFFFFFFFFFFFFFFFB" <<<"$SDA"; check "M42 corrupt/files: run list LCN outside the volume -> EIO" $?
        grep -q "/mnt/attrbad.bin lookup=NULL" <<<"$SDA"; check "M42 corrupt/files: attribute length overflowing the record -> refused" $?
        grep -q "/mnt/compressed/c.txt read=0xFFFFFFFFFFFFFFFB" <<<"$SDA"; check "M42 corrupt/files: bad LZNT1 stream -> EIO" $?
        grep -q "/mnt/secret.efs read=0xFFFFFFFFFFFFFFF3" <<<"$SDA"; check "M42 corrupt/files: EFS file -> EACCES" $?
        grep -q "/mnt/loopdir readdir=0xFFFFFFFFFFFFFFFF" <<<"$SDA"; check "M42 corrupt/files: index node looping onto itself -> readdir error (no hang)" $?
        grep -q "/mnt/indxbad readdir=0xFFFFFFFFFFFFFFFF" <<<"$SDA"; check "M42 corrupt/files: torn INDX block (fixup) -> readdir error" $?
        grep -q "volume is marked dirty" "$M42CLOG" && grep -q "Windows is hibernated" "$M42CLOG"
        check "M42 corrupt/files: dirty flag + hiberfil.sys (Fast Startup) -> warnings, read-only" $?
        grep -q "main boot sector invalid; using the backup copy" "$M42CLOG" && grep -q "dev=sdb mount rc=0x0000000000000000" "$M42CLOG"
        check "M42 corrupt/bootmain: main boot sector bad -> backup copy" $?
        grep -q "\$MFT record 0 damaged, using the \$MFTMirr copy" "$M42CLOG" && grep -q "dev=sdc mount rc=0x0000000000000000" "$M42CLOG"
        check "M42 corrupt/mft0: \$MFT record 0 torn -> \$MFTMirr copy" $?
        grep -q "dev=sdd mount rc=0xFFFFFFFFFFFFFFFF" "$M42CLOG"; check "M42 corrupt/mftboth: record 0 torn in \$MFT and \$MFTMirr -> refused" $?
        grep -q "dev=nvme0n1 mount rc=0xFFFFFFFFFFFFFFFF" "$M42CLOG" && grep -q "\$UpCase invalid" "$M42CLOG"
        check "M42 corrupt/upcase: damaged \$UpCase -> refused" $?
        grep -q "dev=usb0 mount rc=0xFFFFFFFFFFFFFFFF" "$M42CLOG" && grep -q "root directory invalid" "$M42CLOG"
        check "M42 corrupt/rootbad: bad root index -> refused" $?
        grep -q "\[M42C\] DONE (kernel alive)" "$M42CLOG" && ! grep -q "\[EXC\]\|\[PANIC\]" "$M42CLOG"
        check "M42 corrupt suite: kernel alive, no CPU exception or panic" $?
        if make -C tools/ntfs_fuzz >/dev/null 2>&1; then
            ! tools/ntfs_fuzz/ntfs_fuzz ntfs_corrupt/bootboth.img list >/dev/null 2>&1 && \
            ! tools/ntfs_fuzz/ntfs_fuzz ntfs_corrupt/geo.img list >/dev/null 2>&1
            check "M42 host: both boot sectors bad / impossible geometry -> refused" $?
            tools/ntfs_fuzz/ntfs_fuzz disk.img list > /tmp/secos_selftest_m42_host.list 2>/dev/null && \
                python3 -I tools/ntfs_img.py compare disk.img.list /tmp/secos_selftest_m42_host.list >/dev/null 2>&1
            check "M42 host: driver built for the host (ASan/UBSan) lists the image identically" $?
            echo "[selftest] Running NTFS host fuzz (ASan+UBSan, 3000 + 400 mutated images)..."
            timeout 300 tools/ntfs_fuzz/ntfs_fuzz ntfs_corrupt/base.img 3000 42 >/tmp/secos_selftest_m42_fuzz.log 2>&1 && \
            timeout 300 tools/ntfs_fuzz/ntfs_fuzz disk.img 400 4242 >>/tmp/secos_selftest_m42_fuzz.log 2>&1
            check "M42 host fuzz: 3400 mutated images, no crash/UB/hang" $?
        else
            echo "  [SKIP] M42 host harness (gcc with ASan/UBSan unavailable)"
        fi
    else
        echo "  [FAIL] M42 corrupt image build (see /tmp/secos_selftest_m42_disk.log)"; FAIL=$((FAIL+1))
    fi
    # Userland: BusyBox mount/ls/cat/df over the serial bash, default image.
    make clean >/dev/null 2>&1 || true
    if make iso >/tmp/secos_selftest_build.log 2>&1; then
        echo "[selftest] Running M42 userland mount -t ntfs over serial bash (70s)..."
        : > "$M42ULOG"
        ( sleep 28
          printf '  umount /mnt; mkdir -p /x; mount -t ntfs /dev/vda /x && echo MN1-$(cat /x/hello.txt)\r'; sleep 4
          printf '  echo MN2-$(grep -c " /x ntfs ro," /proc/mounts)\r'; sleep 3
          printf '  df | grep /x | sed s/^/MN3-/\r'; sleep 3
          printf '  cat /x/Relazione* | sed s/^/MN4-/\r'; sleep 3
          printf '  (echo hi > /x/new.txt) 2>&1 | sed s/^/MN5-/\r'; sleep 3
          printf '  ln -s hello.txt /x/l 2>&1 | sed s/^/MN6-/; rm -f /x/hello.txt 2>&1 | sed s/^/MN6-/\r'; sleep 3
          printf '  cat /x/secret.efs 2>&1 | sed s/^/MN7-/\r'; sleep 3
          printf '  ls /x/many | wc -l | sed s/^/MN8-/\r'; sleep 4
          printf '  md5sum /x/fragmented.bin /x/compressed/text.txt | sed s/^/MN9-/\r'; sleep 6
          printf '  ls -l /x/winlink_rel | sed s/^/MNA-/; cat /x/winlink_rel | sed s/^/MNA-/; cat /x/junction/c/d/e/f/g/deep.txt | sed s/^/MNJ-/\r'; sleep 3
          printf '  umount /x && echo MNB-$(grep -c ntfs /proc/mounts)\r'; sleep 3
        ) | timeout 70 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
            -drive file=disk.img,if=virtio,format=raw \
            -debugcon file:"$M42ULOG" -global isa-debugcon.iobase=0xe9 \
            -no-reboot -display none -serial stdio >"$M42USER" 2>&1
        FRAG_MD5="$(python3 -I -c 'import hashlib,sys; sys.path.insert(0,"tools"); import ntfs_img as n; print(hashlib.md5(n.pattern(5<<20,9)).hexdigest())' 2>/dev/null)"
        TEXT_MD5="$(python3 -I -c 'import hashlib,sys; sys.path.insert(0,"tools"); import ntfs_img as n; print(hashlib.md5(n.text_blob(300000)).hexdigest())' 2>/dev/null)"
        grep -q "^MN1-hello from ntfs" "$M42USER"; check "M42 user: mount -t ntfs /dev/vda /x (BusyBox, SYS_MOUNT)" $?
        grep -q "^MN2-1" "$M42USER"; check "M42 user: /proc/mounts shows the ntfs mount read-only" $?
        grep -q "^MN3-/dev/vda .* /x" "$M42USER"; check "M42 user: df lists the ntfs mount" $?
        grep -q "^MN4-Il documento" "$M42USER"; check "M42 user: bash reads a long Unicode name" $?
        grep -q "^MN5-.*Read-only file system" "$M42USER" && [[ "$(grep -c '^MN6-.*Read-only file system' "$M42USER")" -ge 2 ]]
        check "M42 user: shell redirection, ln -s and rm fail with EROFS" $?
        grep -q "^MN7-.*Permission denied" "$M42USER"; check "M42 user: reading an EFS file fails with EACCES" $?
        grep -q "^MN8-2000" "$M42USER"; check "M42 user: ls lists all 2000 entries of the big directory" $?
        grep -q "^MN9-$FRAG_MD5 " "$M42USER" && grep -q "^MN9-$TEXT_MD5 " "$M42USER"
        check "M42 user: md5sum of the fragmented and the compressed file match the host" $?
        grep -q "^MNA-.*winlink_rel -> a/b/c/d/e/f/g/deep.txt" "$M42USER" && grep -q "^MNA-deep down" "$M42USER"
        check "M42 user: Windows symlink shown by ls -l and followed by cat" $?
        grep -q "^MNJ-deep down" "$M42USER"; check "M42 user: path through a junction (target mapped under /x)" $?
        grep -q "^MNB-0" "$M42USER"; check "M42 user: umount drops the ntfs mount" $?
    else
        echo "  [FAIL] M42 userland image build"; FAIL=$((FAIL+1))
    fi
else
    echo "  [FAIL] M42 disk-ntfs build (mkntfs/ntfs-3g missing and fetch failed? see /tmp/secos_selftest_m42_disk.log)"
    FAIL=$((FAIL+1))
fi

# ---- libc crypt(): vendored musl SHA-crypt/bcrypt, host + in-guest KATs ----
# Host: the vendored sources built with the host compiler (tools/crypt_kat/).
# Guest: the signed user/crypt_test.elf is put on an ext2 data disk (mode 0755)
# and run from bash at /mnt, so the gate needs no kernel/sysroot change. It
# needs the default image to reach the bash prompt.
CRYPTLOG=/tmp/secos_selftest_crypt.log
CRYPTSER=/tmp/secos_selftest_crypt.serial
CRYPTDIR=/tmp/secos_selftest_crypt
echo "[selftest] crypt(): host KAT + in-guest KAT from /mnt (mb2, serial, ~75s)..."
tools/crypt_kat/run.sh >"$CRYPTDIR.host" 2>&1 && grep -q "\[crypt\] KAT OK" "$CRYPTDIR.host"
check "crypt host KAT (SHA-512/SHA-256-crypt spec vectors, bcrypt, pwhash)" $?
if make crypt-test >/tmp/secos_selftest_build.log 2>&1 && \
   { make clean >/dev/null 2>&1 || true; make iso >>/tmp/secos_selftest_build.log 2>&1; }; then
    rm -rf "$CRYPTDIR"; mkdir -p "$CRYPTDIR/stage"
    install -m 0755 user/crypt_test.elf "$CRYPTDIR/stage/crypt_test"
    dd if=/dev/zero of="$CRYPTDIR/disk.img" bs=1M count=16 status=none
    mkfs.ext2 -q -L SECOSDATA -d "$CRYPTDIR/stage" "$CRYPTDIR/disk.img"
    : > "$CRYPTLOG"
    ( sleep 30; printf '  /mnt/crypt_test; echo CK-$?\r'; sleep 40 ) | \
        timeout 75 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
            -drive file="$CRYPTDIR/disk.img",if=virtio,format=raw \
            -debugcon file:"$CRYPTLOG" -global isa-debugcon.iobase=0xe9 \
            -no-reboot -display none -serial stdio >"$CRYPTSER" 2>&1
    tr -d '\r' < "$CRYPTSER" > "$CRYPTSER.txt"
    grep -q "^\[crypt\] KAT OK: 0 failure(s)" "$CRYPTSER.txt" && grep -q "^CK-0" "$CRYPTSER.txt"
    check "crypt in-guest KAT: signed /mnt/crypt_test passes every vector (exit 0)" $?
    ! grep -q "^\[crypt\] FAIL" "$CRYPTSER.txt"; check "crypt in-guest KAT: no failing check" $?
    ! grep -q "\[EXC\]\|\[PANIC\]" "$CRYPTLOG"; check "crypt no CPU exception or panic" $?
    rm -rf "$CRYPTDIR"
else
    echo "[selftest] CRYPT BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    check "crypt in-guest KAT build" 1
fi

# ---- [M43] account tools: console login, su, sudo, passwd, useradd ----------
# A CONSOLE=login image with a baked ROOT_PASSWORD, driven over serial: a wrong
# then the right root password at the console login, useradd/usermod/chpasswd,
# logout + login as the new wheel user (the console becomes theirs), su with the
# right and a wrong password, sudo as a wheel member, passwd changing the own
# password and a login with the new one. Then, as an unprivileged user, the
# syscall fuzzer (which also fuzzes the id syscalls 100-109) must finish
# without any batch coming out with root ids.
MULOG=/tmp/secos_selftest_users.log
MUSER=/tmp/secos_selftest_users.serial
echo "[selftest] Account tools: console login, su, sudo, passwd, unprivileged fuzzing (mb2, serial, ~210s)..."
if make iso CONSOLE=login ROOT_PASSWORD=Root-pw9 >/tmp/secos_selftest_build.log 2>&1; then
    : > "$MULOG"
    ( sleep 25
      printf 'root\r'; sleep 2; printf 'nope\r'; sleep 6
      printf 'root\r'; sleep 2; printf 'Root-pw9\r'; sleep 4
      printf 'echo MU1-$(id -u)-$HOME\r'; sleep 2
      printf 'useradd carol; useradd dave; useradd -M fz; usermod -aG wheel,video,input carol; printf "carol:Carol-pw1\\ndave:Dave-pw1\\n" | chpasswd; echo MU2-$?\r'; sleep 8
      printf 'echo MU3-$(stat -c %%a-%%U /home/carol /bin/su | tr "\\n" -)\r'; sleep 2
      printf 'exit\r'; sleep 4
      printf 'carol\r'; sleep 2; printf 'Carol-pw1\r'; sleep 4
      printf 'echo MU4-$(id -un)-$(pwd)-$(stat -c %%U /dev/console)\r'; sleep 2
      printf 'su dave -c "echo MU5-\\$(id -un)"\r'; sleep 3; printf 'Dave-pw1\r'; sleep 4
      printf 'su dave -c "echo MU6-BAD"; echo MU6-rc$?\r'; sleep 3; printf 'wrongpw\r'; sleep 5
      printf 'sudo /bin/bash -c "echo MU7-\\$(id -u)"\r'; sleep 3; printf 'Carol-pw1\r'; sleep 4
      printf 'su dave -c "sudo id; echo MU8-rc\\$?"\r'; sleep 3; printf 'Dave-pw1\r'; sleep 5
      printf 'passwd\r'; sleep 3; printf 'Carol-pw1\r'; sleep 3; printf 'Carol-pw2\r'; sleep 2; printf 'Carol-pw2\r'; sleep 5
      printf 'exit\r'; sleep 4
      printf 'carol\r'; sleep 2; printf 'Carol-pw2\r'; sleep 4
      printf 'echo MU9-$(id -un)\r'; sleep 2
      printf 'sudo su fz -c "/bin/sysfuzz -s 4711 -b 12 -c 150"\r'; sleep 3
      printf 'Carol-pw2\r'; sleep 60
      printf 'echo MU10-alive-$(id -un)\r'; sleep 3 ) | \
        timeout 210 qemu-system-x86_64 -cdrom myos.iso -boot d -m 512M \
            -debugcon file:"$MULOG" -global isa-debugcon.iobase=0xe9 \
            -no-reboot -display none -serial stdio > "$MUSER" 2>&1
    tr -d '\r' < "$MUSER" > "$MUSER.txt"
    grep -q "root password set from the build" "$MULOG"; check "USERS ROOT_PASSWORD hash applied to the unset root entry" $?
    grep -q "^Login incorrect" "$MUSER.txt"; check "USERS console login refuses a wrong password" $?
    grep -q "^MU1-0-/root" "$MUSER.txt"; check "USERS console login as root with the baked password" $?
    grep -q "^MU2-0" "$MUSER.txt"; check "USERS useradd + usermod -aG + chpasswd" $?
    grep -q "^MU3-700-carol-4755-root-" "$MUSER.txt"; check "USERS new home 0700 owned by the user; /bin/su root 4755" $?
    grep -q "^MU4-carol-/home/carol-carol" "$MUSER.txt"; check "USERS login as a user: ids, home, console owned by the user" $?
    grep -q "^MU5-dave" "$MUSER.txt"; check "USERS su with the right password" $?
    grep -q "su: Authentication failure" "$MUSER.txt" && grep -q "^MU6-rc1" "$MUSER.txt" && ! grep -q "^MU6-BAD" "$MUSER.txt"
    check "USERS su with a wrong password fails (exit 1)" $?
    grep -q "^MU7-0" "$MUSER.txt"; check "USERS sudo as a wheel member with the own password" $?
    grep -q "not a member of group wheel" "$MUSER.txt" && grep -q "^MU8-rc1" "$MUSER.txt"
    check "USERS sudo refused outside wheel" $?
    grep -q "password updated successfully" "$MUSER.txt" && grep -q "^MU9-carol" "$MUSER.txt"
    check "USERS passwd changes the own password; login with the new one" $?
    ! grep -q "Carol-pw1\|Dave-pw1\|wrongpw\|Root-pw9" <(sed -n '/login:/,$p' "$MUSER.txt" | grep -v "^printf\|chpasswd")
    check "USERS no password ever echoed on the console" $?
    grep -q "^\[sysfuzz\] seed=4711 .* uid=100[0-9]" "$MUSER.txt" && grep -q "^\[sysfuzz\] DONE ok" "$MUSER.txt" && \
        ! grep -q "ESCALAT" "$MUSER.txt"
    check "USERS sysfuzz as an unprivileged user: done, no batch gained root ids" $?
    grep -q "^MU10-alive-carol" "$MUSER.txt"; check "USERS system alive after unprivileged fuzzing" $?
    ! grep -q "ring0\|\[PANIC\]" "$MULOG"; check "USERS no kernel fault or panic" $?
    make iso >/tmp/secos_selftest_build.log 2>&1   # leave the default selftest image behind
else
    echo "[selftest] USERS BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    check "USERS image build" 1
fi

# ---- [M43] PERM gate: permission enforcement, as an unprivileged user ----------
# Regression test for the access rules (kernel/cred.c). Root logs in on the
# console (CONSOLE=login, baked ROOT_PASSWORD), creates eve and bob, mounts an
# ext2 disk holding tools/perm_guest.sh at /opt and an exFAT disk at /media
# with uid=eve,gid=eve,umask=027 and NTFS read-only at /mnt (all on AHCI),
# then runs the script as eve. Each case prints "PT <case> rc=<n> <message>";
# denials must carry the expected errno text, positive controls must pass.
PMLOG=/tmp/secos_selftest_perm.log
PMSER=/tmp/secos_selftest_perm.serial
PMD=/tmp/secos_selftest_perm.d
echo "[selftest] PERM gate: permission enforcement as an unprivileged user (q35, serial, ~150s)..."
rm -rf "$PMD"; mkdir -p "$PMD/stage"
install -m 0755 tools/perm_guest.sh "$PMD/stage/perm_guest.sh"
make clean >/dev/null 2>&1 || true
if make iso CONSOLE=login ROOT_PASSWORD=Root-pw9 >/tmp/secos_selftest_build.log 2>&1 \
   && make disk-ntfs >/tmp/secos_selftest_perm_disk.log 2>&1 \
   && dd if=/dev/zero of="$PMD/ext2.img" bs=1M count=16 status=none \
   && mkfs.ext2 -q -L PERMTEST -d "$PMD/stage" "$PMD/ext2.img" \
   && bash tools/exfat-host.sh mkgood "$PMD/exfat.img" >>/tmp/secos_selftest_perm_disk.log 2>&1; then
    : > "$PMLOG"
    ( sleep 28
      printf 'root\r'; sleep 2; printf 'Root-pw9\r'; sleep 5
      printf 'useradd eve; useradd bob; printf "eve:Eve-pw1\\nbob:Bob-pw1\\n" | chpasswd; echo PS0-$?\r'; sleep 8
      printf 'umount /mnt 2>/dev/null; mkdir -p /opt /media /tmp/rid && mount -t ntfs /dev/sda /mnt && mount -t ext2 /dev/sdb /opt; echo PS1-$?\r'; sleep 5
      printf 'mount -t exfat -o uid=$(id -u eve),gid=$(id -g eve),umask=027 /dev/sdc /media; echo PS1X-$?\r'; sleep 4
      printf 'cp /bin/usertools /tmp/rid/id && chmod 4755 /tmp/rid/id; echo PS2-$(stat -c %%a-%%U /tmp/rid/id)-$(id -u eve)\r'; sleep 3
      printf 'su eve -c "/bin/bash /opt/perm_guest.sh"\r'; sleep 30
      printf 'su bob -c "cat /media/eve.txt" 2>&1 | sed s/^/PX1-/\r'; sleep 4
      printf 'echo PR1-$(head -c 5 /etc/shadow)\r'; sleep 2
      printf 'su eve -c "su bob -c \\"echo PW-BAD\\"; echo PW-rc\\$?"\r'; sleep 4; printf 'wrongpw\r'; sleep 6
      printf 'echo PEND\r'; sleep 3 ) | \
        timeout 150 qemu-system-x86_64 -machine q35 -cdrom myos.iso -boot d -m 512M \
            -drive file=disk.img,format=raw,if=ide,index=0 \
            -drive file="$PMD/ext2.img",format=raw,if=ide,index=1 \
            -drive file="$PMD/exfat.img",format=raw,if=ide,index=3 \
            -debugcon file:"$PMLOG" -global isa-debugcon.iobase=0xe9 \
            -no-reboot -display none -serial stdio > "$PMSER" 2>&1
    tr -d '\r' < "$PMSER" > "$PMSER.txt"; P="$PMSER.txt"
    EVE=$(grep -a '^PS2-' "$P" | head -1 | sed 's/.*-//')
    pt() { grep -a "^PT $1 rc=" "$P" | head -1; }
    deny() { # deny CASE "message regex" description
        local l r; l=$(pt "$1"); [[ -n "$l" && "$l" != *" rc=0 "* && "$l" != *" rc=0" ]] && grep -Eq "$2" <<<"$l"; r=$?
        check "PERM denied: $3 ($l)" $r
    }
    same() { # same CASE "expected line" description
        local l r; l=$(pt "$1"); [[ "$l" == "$2" ]]; r=$?
        check "PERM $3 ($l)" $r
    }
    grep -aq '^PS0-0' "$P" && grep -aq '^PS1-0' "$P" && grep -aq '^PS1X-0' "$P" && grep -aq '^PS2-4755-root-' "$P" && [[ -n "$EVE" && "$EVE" != 0 ]]
    check "PERM setup: users, /opt (ext2), /media (exFAT uid=eve,umask=027), root 4755 copy" $?
    grep -aq "^PT-BEGIN uid=$EVE " "$P"; check "PERM the script runs as eve (uid $EVE)" $?
    deny shadow-read   "Permission denied"          "read /etc/shadow (EACCES)"
    deny passwd-write  "Permission denied"          "write /etc/passwd (EACCES)"
    deny kill-root     "Operation not permitted"    "kill -0 1, a root process (EPERM)"
    deny home-other    "Permission denied"          "list another user's 0700 home (EACCES)"
    deny dev-disk      "Permission denied"          "open /dev/sda without group disk (EACCES)"
    deny dev-fb        "Permission denied"          "open /dev/fb0 without group video (EACCES)"
    deny chown-to-root "Operation not permitted"    "chown own file to root (EPERM)"
    deny sudo-nowheel  "not a member of group wheel" "sudo outside wheel"
    deny ntfs-write    "Read-only file system"      "create on the NTFS mount (EROFS, not EACCES)"
    deny ntfs-rm       "Read-only file system"      "unlink on the NTFS mount (EROFS, not EACCES)"
    same setid-root "PT setid-root rc=0 $EVE" "set-uid root file without a SETID manifest runs with eve's ids"
    same setid-own  "PT setid-own rc=0 $EVE"  "chmod u+s on an own file + exec: still eve"
    grep -aq '^PX1-.*Permission denied' "$P"; check "PERM denied: bob reads eve's file on the umask=027 exFAT mount" $?
    grep -aq "su: Authentication failure" "$P" && grep -aq '^PW-rc1' "$P" && ! grep -aq '^PW-BAD' "$P"
    check "PERM denied: su with a wrong password (exit 1)" $?
    for c in ok-passwd-read ok-home-write ok-ntfs-read ok-own-chmod ok-exfat-write; do
        l=$(pt $c); [[ "$l" == "PT $c rc=0"* ]]; r=$?; check "PERM allowed (control): $c ($l)" $r
    done
    same ok-exfat-owner "PT ok-exfat-owner rc=0 eve 750" "exFAT mount options: eve's new file is eve-owned, mode 750"
    grep -aq '^PR1-root:' "$P"; check "PERM allowed (control): root reads /etc/shadow" $?
    grep -aq '^PT-END' "$P" && grep -aq '^PEND' "$P"; check "PERM script and session completed" $?
    ! grep -aq "ring0\|\[PANIC\]\|\[EXC\]" "$PMLOG"; check "PERM no kernel fault or panic" $?
else
    echo "[selftest] PERM BUILD ERROR — see /tmp/secos_selftest_build.log, /tmp/secos_selftest_perm_disk.log" >&2
    check "PERM image/disk build" 1
fi
rm -rf "$PMD"

# ============================================================================
# MUPORTS GATE: BusyBox, bash and Dropbear on real multi-user credentials.
# tools/mu_ports_test.sh writes a test account straight into /etc/passwd,
# /etc/shadow (an `openssl passwd -6` hash made here) and /etc/group, then
# checks BusyBox owner names / chown / chmod / errno messages (EROFS when the
# kernel has a read-only filesystem), a bash glob over 1000 entries, and SSH
# key + password logins as the user (uid, groups, HOME, pty owner, no
# /etc/shadow), locked/wrong/root passwords refused. Private mktemp dir, host
# port 26000-26999 (never 2222). [PASS]/[FAIL] lines are counted here.
# ============================================================================
echo "[selftest] MUPORTS gate: multi-user BusyBox/bash/Dropbear (mb2, ~150s)..."
make clean >/dev/null 2>&1 || true
if make iso >/tmp/secos_selftest_build.log 2>&1; then
    MUP_OUT=$(mktemp /tmp/secos_selftest_muports.XXXXXX)
    tools/mu_ports_test.sh >"$MUP_OUT" 2>&1
    while IFS= read -r l; do
        case "$l" in
            "  [PASS] "*) check "${l#  \[PASS\] }" 0 ;;
            "  [FAIL] "*) check "${l#  \[FAIL\] }" 1 ;;
            "  [SKIP] "*|"    "*) echo "$l" ;;
        esac
    done <"$MUP_OUT"
    grep -q "RESULT:\|\[SKIP\] MUPORTS gate" "$MUP_OUT" || { check "MUPORTS gate ran to completion" 1; tail -20 "$MUP_OUT"; }
    rm -f "$MUP_OUT"
else
    echo "[selftest] MUPORTS BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    check "MUPORTS gate build" 1
fi

# ============================================================================
# BIGMEM GATE [M49]: large RAM. QEMU backs guest RAM lazily, so a 1 TiB guest
# costs the host only what the kernel touches (PMM metadata ~33 MiB/TiB, page
# tables, the pages the self-test writes). Four boots run in parallel:
#   8g     mb2, -m 8G, -cpu max (1 GiB physmap pages); a root bash over serial
#          runs lua allocating a 512 MiB string (user anon memory far above
#          4 GiB) and /proc/meminfo before/after (MemFree comes back);
#   8g2m   mb2, -m 8G, qemu64 CPU (no pdpe1gb: 2 MiB physmap pages);
#   1t     mb2, -m 1T, phys-bits=46: RAM above 512 GiB (the old physmap limit)
#          is allocated, written and read back through the physmap;
#   uefi   OVMF q35, -m 8G.
# Each boot's kernel self-test ([BIGMEM]) touches one frame in up to 64 RAM
# sections spread over the map plus the highest free frame, checks the zones
# (ordinary frames >= 4 GiB, DMA32 allocations < 4 GiB), COW refcounts, the free
# count and meminfo; the physmap audit proves every leaf supervisor + NX. A run
# whose QEMU cannot start with that much RAM (host limits) is SKIPPED.
# ============================================================================
echo "[selftest] BIGMEM gate: 8G/8G-2M/1T mb2 + 8G UEFI, in parallel (~100s)..."
make clean >/dev/null 2>&1 || true
if make iso >/tmp/secos_selftest_build.log 2>&1 && make uefi >>/tmp/secos_selftest_build.log 2>&1; then
    BMDIR=$(mktemp -d /tmp/secos_selftest_bigmem.XXXXXX)
    cp myos.iso "$BMDIR/bm.iso"; cp -r dist "$BMDIR/esp"
    bm_boot() { # bm_boot <name> <timeout> <qemu args...>   (stdin -> serial)
        local n="$1" t="$2"; shift 2
        : > "$BMDIR/$n.log"
        timeout "$t" qemu-system-x86_64 "$@" -debugcon file:"$BMDIR/$n.log" \
            -global isa-debugcon.iobase=0xe9 -no-reboot -display none -serial stdio \
            >/dev/null 2>"$BMDIR/$n.err"
        echo $? >"$BMDIR/$n.rc"
    }
    ( sleep 30; printf '  head -2 /proc/meminfo\r'; sleep 4
      printf "  lua -e 's=string.rep(\"x\",1<<29) print(\"LUA-BIG\", #s)'\r"; sleep 25
      printf '  head -2 /proc/meminfo\r'; sleep 4; printf '  echo BIGMEM-$((6*7))-DONE\r'; sleep 30 ) | \
        bm_boot 8g 100 -cdrom "$BMDIR/bm.iso" -boot d -m 8G -cpu max &
    bm_boot 8g2m 45 -cdrom "$BMDIR/bm.iso" -boot d -m 8G </dev/null &
    bm_boot 1t 90 -cdrom "$BMDIR/bm.iso" -boot d -m 1T -cpu max,phys-bits=46 </dev/null &
    if [[ -n "$OVMF" ]]; then
        bm_boot uefi 90 -machine q35 -m 8G -cpu max -bios "$OVMF" \
            -drive format=raw,file=fat:rw:"$BMDIR/esp"/ -net none </dev/null &
    fi
    wait
    bm_ok() { grep -aq "$2" "$BMDIR/$1.log"; }
    for n in 8g 8g2m 1t uefi; do
        [[ $n == uefi && -z "$OVMF" ]] && { echo "  [SKIP] BIGMEM uefi (no OVMF)"; continue; }
        if ! grep -aq "SECoS build" "$BMDIR/$n.log" && [[ "$(cat "$BMDIR/$n.rc")" != 124 ]]; then
            echo "  [SKIP] BIGMEM $n: QEMU could not start the guest (host limit): $(head -c 200 "$BMDIR/$n.err")"
            continue
        fi
        bm_ok $n "^\[BIGMEM\] selftest PASS"; check "BIGMEM $n: PMM self-test (sections, zones, refcounts, meminfo)" $?
        bm_ok $n "^\[BIGMEM\] touched=[0-9]* .* pattern OK"; check "BIGMEM $n: frames across the map written + read back via the physmap" $?
        bm_ok $n "^\[PHYSMAP\] audit leaves=[0-9]* all supervisor+NX"; check "BIGMEM $n: every physmap leaf supervisor + NX" $?
        bm_ok $n "^\[HEAP\] kmalloc(4M) phys=0x0000000[1-9A-F]" ; check "BIGMEM $n: 4 MiB kmalloc served from RAM above 4 GiB" $?
        bm_ok $n "started console (/bin/bash)"; check "BIGMEM $n: boot reaches init and the console bash" $?
        ! bm_ok $n "\[EXC\]\|PANIC"; check "BIGMEM $n: no CPU exception or panic" $?
    done
    if grep -aq "SECoS build" "$BMDIR/8g.log"; then
        bm_ok 8g "^\[PHYSMAP\] .* gbpages=1"; check "BIGMEM 8g: physmap uses 1 GiB pages (pdpe1gb)" $?
        bm_ok 8g "^LUA-BIG	536870912"; check "BIGMEM 8g: a ring-3 process allocates and fills 512 MiB" $?
        bm_ok 8g "BIGMEM-42-DONE"; check "BIGMEM 8g: shell alive after the big allocation" $?
        mf=( $(grep -ao "^MemFree: *[0-9]*" "$BMDIR/8g.log" | awk '{print $2}') )
        [[ ${#mf[@]} -ge 2 && $(( ${mf[0]} - ${mf[1]} )) -lt 4096 && $(( ${mf[1]} - ${mf[0]} )) -lt 4096 ]]
        check "BIGMEM 8g: MemFree restored after the 512 MiB process exits (${mf[0]:-?} -> ${mf[1]:-?} kB)" $?
        mt=$(grep -ao "^MemTotal: *[0-9]*" "$BMDIR/8g.log" | head -1 | awk '{print $2}')
        [[ -n "$mt" && $mt -gt 8000000 && $mt -le 8388608 ]]; check "BIGMEM 8g: MemTotal reports the 8 GiB (${mt:-?} kB)" $?
    fi
    if grep -aq "SECoS build" "$BMDIR/8g2m.log"; then
        bm_ok 8g2m "^\[PHYSMAP\] .* pages1g=0 .* gbpages=0"; check "BIGMEM 8g2m: no pdpe1gb -> 2 MiB physmap pages" $?
    fi
    if grep -aq "SECoS build" "$BMDIR/1t.log"; then
        hf=$(grep -ao "^\[BIGMEM\] touched=[0-9]* sections=[0-9]* highest_frame_MiB=[0-9]*" "$BMDIR/1t.log" | sed 's/.*=//')
        [[ -n "$hf" && $hf -gt 524288 ]]; check "BIGMEM 1t: a frame above 512 GiB allocated + verified (highest ${hf:-?} MiB)" $?
        bm_ok 1t "^\[PMM\] sections=1024/"; check "BIGMEM 1t: 1024 RAM sections managed" $?
        bm_ok 1t "^\[PHYSMAP\] .* slots=[1-9][0-9]* "; check "BIGMEM 1t: physmap spans several PML4 slots" $?
    fi
    for n in 8g 8g2m 1t uefi; do cp "$BMDIR/$n.log" "/tmp/secos_selftest_bigmem_$n.log" 2>/dev/null; done
    rm -rf "$BMDIR"
else
    echo "[selftest] BIGMEM BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    check "BIGMEM gate build" 1
fi

# ============================================================================
# NETSMP GATE [M47]: network stack locking under SMP. An image built with
# -DSECOS_SMP_AT_BOOT -DSECOS_LOCKDEP (lock validator on, M50) and a baked
# throwaway SSH key is driven by tools/net_stress_drive.py three times in
# parallel: -smp 4 with e1000e (MSI-X NAPI RX), -smp 4 with e1000 (polled RX)
# and -smp 1 with e1000e. Each run: /bin/netstress client workers (forked,
# spread over the CPUs) doing TCP echo round trips, firehose connections
# closed while data arrives, UDP floods and DNS lookups against host peers;
# /bin/netstress server accepting parallel host connections, a third of them
# reset mid-transfer; two more ssh sessions in parallel. The socket table, TCP
# control blocks, UDP bindings and MemFree are compared before/after each
# round (no leak), and the debugcon log must be free of [LOCKDEP], panics and
# CPU exceptions. Host ports are random (never 2222/8000).
# ============================================================================
if ! command -v ssh >/dev/null 2>&1 || ! command -v ssh-keygen >/dev/null 2>&1; then
    echo "  [SKIP] NETSMP gate (host has no ssh / ssh-keygen)"
else
    echo "[selftest] NETSMP gate: net stack stress, -smp 4/1, lockdep (mb2, ~250s)..."
    NSDIR=$(mktemp -d /tmp/secos_selftest_netsmp.XXXXXX)
    ssh-keygen -q -t ed25519 -N '' -C secos-netsmp-selftest -f "$NSDIR/id"
    make clean >/dev/null 2>&1 || true
    if make iso CFLAGS_EXTRA="-DSECOS_LOCKDEP" \
            SSH_AUTHORIZED_KEYS="$NSDIR/id.pub" >/tmp/secos_selftest_build.log 2>&1; then
        cp myos.iso "$NSDIR/netsmp.iso"
        for cfg in "smp4-msix 4 e1000e" "smp4-poll 4 e1000" "smp1-msix 1 e1000e"; do
            set -- $cfg
            timeout 600 python3 tools/net_stress_drive.py --iso "$NSDIR/netsmp.iso" --smp "$2" --nic "$3" \
                --key "$NSDIR/id" --log "$NSDIR/$1.log" --out "$NSDIR/$1.out" >/dev/null 2>&1 &
        done
        wait
        for run in smp4-msix smp4-poll smp1-msix; do
            while IFS= read -r l; do
                case "$l" in
                    "[NSD] RESULT "*|"[NSD] stats "*|"[NSD] host "*|"[NSD] sshd.log"*) ;;
                    "[NSD] "*" OK"*)   n=${l#\[NSD\] }; check "NETSMP $run ${n%% OK*}" 0 ;;
                    "[NSD] "*" FAIL"*) n=${l#\[NSD\] }; check "NETSMP $run ${n%% FAIL*}" 1; echo "    $l" | cut -c1-300 ;;
                esac
            done <"$NSDIR/$run.out"
            grep -q "^\[NSD\] RESULT " "$NSDIR/$run.out"; check "NETSMP $run driver ran to completion" $?
            ! grep -aq "\[LOCKDEP\]\|PANIC\|\[EXC\]" "$NSDIR/$run.log"
            check "NETSMP $run no lockdep violation, panic or CPU exception" $?
        done
        grep -aq "\[NET\] MSI-X NAPI armed" "$NSDIR/smp4-msix.log"; check "NETSMP e1000e RX runs on MSI-X NAPI" $?
        grep -aq "\[SMP\] online cpus=0x0000000000000004" "$NSDIR/smp4-msix.log"; check "NETSMP four CPUs online" $?
        for f in "$NSDIR"/*.out; do cp "$f" "/tmp/secos_selftest_netsmp_$(basename "$f")"; done
    else
        echo "[selftest] NETSMP BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
        check "NETSMP gate build" 1
    fi
    rm -rf "$NSDIR"
fi

# ---- M48 GATE: SMP for real (TLB shootdown, IPIs, migration, x2APIC) ----
# The default image (SMP on, no define needed) on -smp 4: all CPUs online,
# the boot TLB-shootdown selftest (with its negative control), then the serial
# stress session tools/smp_stress_drive.py on -smp 4 and -smp 8 (CPU-bound
# jobs, pipelines, fork/exec bursts, FPU across migration + signal handlers,
# parallel syscall fuzzers; /proc/smp must show migrations and reschedule
# IPIs, no shootdown timeouts); its first step checks that nproc, sysfs,
# sysconf and sched_getaffinity report every online CPU. The same image
# booted through the GRUB "nosmp" entry keeps a single CPU (and userland
# counts 1). x2APIC mode is exercised only when
# SECOS_QEMU_X2APIC names a QEMU whose TCG supports it (QEMU >= 9).
echo "[selftest] M48 gate: SMP (shootdown selftest, stress -smp 4/8, nosmp)..."
M48LOG=/tmp/secos_selftest_m48.log
make clean >/dev/null 2>&1 || true
if make iso >/tmp/secos_selftest_build.log 2>&1; then
    M48DIR=$(mktemp -d /tmp/secos_selftest_m48.XXXXXX)
    cp myos.iso "$M48DIR/smp.iso"
    run_iso "$M48LOG" 40 -smp 4
    grep -q "\[SMP\] online cpus=0x0000000000000004" "$M48LOG"; check "M48 default image brings all 4 CPUs online" $?
    grep -q "\[M48\] TLB shootdown selftest PASS" "$M48LOG"; check "M48 TLB shootdown reaches every AP (boot selftest)" $?
    grep -q "\[M48\] TLB selftest: .* without stale=0x000000000000000[1-9]" "$M48LOG"
    check "M48 TLB selftest negative control sees stale entries without a shootdown" $?
    grep -q "\[SMP\] cpu=0x000000000000000[1-3] run pid=" "$M48LOG"; check "M48 user tasks run on the APs" $?
    ! grep -q "\[EXC\]\|\[PANIC\]\|\[TLB\] BUG\|shootdown timeout" "$M48LOG"; check "M48 no exception, panic or shootdown timeout at boot (-smp 4)" $?
    for n in 4 8; do
        timeout 1500 python3 tools/smp_stress_drive.py --iso "$M48DIR/smp.iso" --smp "$n" \
            --log "$M48DIR/ssd$n.log" > "$M48DIR/ssd$n.out" 2>&1 &
    done
    wait
    for n in 4 8; do
        while IFS= read -r l; do
            case "$l" in
                "[SSD] RESULT "*) ;;
                "[SSD] "*" OK"*)   x=${l#\[SSD\] }; check "M48 smp$n ${x%% OK*}" 0 ;;
                "[SSD] "*" FAIL"*) x=${l#\[SSD\] }; check "M48 smp$n ${x%% FAIL*}" 1; echo "    $l" | cut -c1-200 ;;
            esac
        done <"$M48DIR/ssd$n.out"
        grep -q "^\[SSD\] RESULT " "$M48DIR/ssd$n.out"; check "M48 smp$n stress driver ran to completion" $?
        cp "$M48DIR/ssd$n.out" "/tmp/secos_selftest_m48_ssd$n.out"; cp "$M48DIR/ssd$n.log" "/tmp/secos_selftest_m48_ssd$n.log"
    done
    # nosmp: the same image through the second GRUB entry (command line "nosmp").
    cp -r isodir "$M48DIR/iso_nosmp"
    sed -i 's/^set default=0/set default=1/; s/^set timeout=3/set timeout=0/' "$M48DIR/iso_nosmp/boot/grub/grub.cfg"
    if grub-mkrescue --output="$M48DIR/nosmp.iso" "$M48DIR/iso_nosmp" >/dev/null 2>&1; then
        : > "$M48LOG.nosmp"
        timeout 30 qemu-system-x86_64 -cdrom "$M48DIR/nosmp.iso" -boot d -m 256M -smp 4 \
            -debugcon file:"$M48LOG.nosmp" -global isa-debugcon.iobase=0xe9 -no-reboot -display none >/dev/null 2>&1
        grep -q "\[SMP\] nosmp on the command line" "$M48LOG.nosmp"; check "M48 nosmp boot word keeps the APs parked" $?
        ! grep -q "\[SMP\] online cpus=" "$M48LOG.nosmp"; check "M48 nosmp: no AP brought up" $?
        grep -q "\[INIT\] /bin/init started" "$M48LOG.nosmp"; check "M48 nosmp: system boots to init on one CPU" $?
        # Userland agrees: nproc, sysfs, Cpus_allowed_list and sysconf all say 1.
        timeout 400 python3 tools/smp_stress_drive.py --iso "$M48DIR/nosmp.iso" --smp 4 --expect-cpus 1 \
            --cpus-only --log "$M48DIR/ssd_nosmp.log" > "$M48DIR/ssd_nosmp.out" 2>&1
        grep -q "^\[SSD\] cpu-count OK" "$M48DIR/ssd_nosmp.out"; check "M48 nosmp: userland CPU count is 1 (nproc, sysfs, sysconf, affinity)" $?
    else
        check "M48 nosmp ISO build" 1
    fi
    if [[ -n "${SECOS_QEMU_X2APIC:-}" && -x "$SECOS_QEMU_X2APIC" ]]; then
        : > "$M48LOG.x2apic"
        timeout 120 "$SECOS_QEMU_X2APIC" -cdrom "$M48DIR/smp.iso" -boot d -m 1G -smp 8 -cpu max,+x2apic \
            -debugcon file:"$M48LOG.x2apic" -global isa-debugcon.iobase=0xe9 -no-reboot -display none >/dev/null 2>&1
        grep -q "mode=x2apic" "$M48LOG.x2apic"; check "M48 x2APIC mode enabled (-cpu max,+x2apic)" $?
        grep -q "\[SMP\] online cpus=0x0000000000000008" "$M48LOG.x2apic"; check "M48 x2APIC: 8 CPUs online" $?
        grep -q "\[M48\] TLB shootdown selftest PASS" "$M48LOG.x2apic"; check "M48 x2APIC: shootdown IPIs delivered" $?
        grep -q "\[INIT\] /bin/init started" "$M48LOG.x2apic"; check "M48 x2APIC: boots to init" $?
    else
        echo "  [SKIP] M48 x2APIC (set SECOS_QEMU_X2APIC to a QEMU >= 9 binary)"
    fi
    rm -rf "$M48DIR"
else
    echo "[selftest] M48 BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    check "M48 gate build" 1
fi

# ---- M50 PAGECACHE GATE: page-cache fill without pc_lock (BKL removal B2) ----
# One image with -DSECOS_PAGECACHE_TEST (boot self-test that forces the fill
# races through a hook between the FS read and publication: a write, an
# invalidate_fs, a recursive miss and a cache flood on the BSP; a remote
# write, a waiter, a timed-out waiter and a remote invalidate_fs on an AP; a
# negative control that ignores the stale bit) and -DSECOS_LOCKDEP, on -smp 4
# with a default mkfs.ext4 disk at /mnt. tools/pc_stress_drive.py then runs
# /bin/pcstress on the ramfs /tmp and the ext4 /mnt at once (writers
# overwrite / O_TRUNC / O_APPEND / ftruncate while hammers read() and mmap
# the same files; lock-step checkers verify every completed write through
# read() and a fresh mmap), unmounts and runs host e2fsck -fn.
echo "[selftest] M50 pagecache gate: fill races (boot self-test) + pcstress -smp 4..."
make clean >/dev/null 2>&1 || true
if make iso CONSOLE=root CFLAGS_EXTRA="-DSECOS_PAGECACHE_TEST -DSECOS_LOCKDEP" >/tmp/secos_selftest_build.log 2>&1; then
    PCDDIR=$(mktemp -d /tmp/secos_selftest_pcd.XXXXXX)
    cp myos.iso "$PCDDIR/pcd.iso"
    timeout 2400 python3 tools/pc_stress_drive.py --iso "$PCDDIR/pcd.iso" --smp 4 --pctest \
        --rounds 200 --log "$PCDDIR/pcd.log" >"$PCDDIR/pcd.out" 2>&1
    while IFS= read -r l; do
        case "$l" in
            "[PCD] "*" OK"*)   x=${l#\[PCD\] }; check "M50 pagecache ${x%% OK*}" 0 ;;
            "[PCD] "*" FAIL"*) x=${l#\[PCD\] }; check "M50 pagecache ${x%% FAIL*}" 1; echo "    $l" | cut -c1-240 ;;
        esac
    done < "$PCDDIR/pcd.out"
    grep -q "^\[PCD\] RESULT " "$PCDDIR/pcd.out"; check "M50 pagecache stress driver ran to completion" $?
    grep -q "\[PCTEST\] negative control: ignoring the stale bit serves a stale page: OK" "$PCDDIR/pcd.log"
    check "M50 pagecache negative control sees the stale page without the stale bit" $?
    rm -rf "$PCDDIR"
else
    echo "[selftest] M50 pagecache BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    check "M50 pagecache gate build" 1
fi

# ---- PROCRACE GATE [M50 B1]: process lifetime + address-space locking ----
# A -DSECOS_LOCKDEP image (lock validator, kfree poisoning with 0x6B) with
# CONSOLE=root, driven by tools/proc_race_drive.py at -smp 4 and -smp 8 in
# parallel: /bin/smptest procrace (8 readers doing kill -0 and reading
# /proc/<pid>/{status,maps,stat} of random pids against 4 churners forking
# workers that mmap/mprotect/munmap/brk, orphan grandchildren and get
# SIGKILLed), a bash-level variant of the same race, MemFree steady across
# the measured runs, and a debugcon log free of [LOCKDEP], [LOCK] BUG, heap
# corruption, panics and CPU exceptions.
echo "[selftest] PROCRACE gate: proc-lifetime race, -smp 4/8, lockdep (mb2, ~300s)..."
make clean >/dev/null 2>&1 || true
if make iso CONSOLE=root CFLAGS_EXTRA="-DSECOS_LOCKDEP" >/tmp/secos_selftest_build.log 2>&1; then
    PRDIR=$(mktemp -d /tmp/secos_selftest_procrace.XXXXXX)
    cp myos.iso "$PRDIR/pr.iso"
    for n in 4 8; do
        timeout 1200 python3 tools/proc_race_drive.py --iso "$PRDIR/pr.iso" --smp "$n" --secs 20 --runs 3 \
            --log "$PRDIR/prd$n.log" > "$PRDIR/prd$n.out" 2>&1 &
    done
    wait
    for n in 4 8; do
        while IFS= read -r l; do
            case "$l" in
                "[PRD] RESULT "*|"[PRD] memfree "*|"[PRD] note "*) ;;
                "[PRD] "*" OK"*)   x=${l#\[PRD\] }; check "PROCRACE smp$n ${x%% OK*}" 0 ;;
                "[PRD] "*" FAIL"*) x=${l#\[PRD\] }; check "PROCRACE smp$n ${x%% FAIL*}" 1; echo "    $l" | cut -c1-200 ;;
            esac
        done <"$PRDIR/prd$n.out"
        grep -q "^\[PRD\] RESULT " "$PRDIR/prd$n.out"; check "PROCRACE smp$n driver ran to completion" $?
        cp "$PRDIR/prd$n.out" "/tmp/secos_selftest_procrace_prd$n.out"; cp "$PRDIR/prd$n.log" "/tmp/secos_selftest_procrace_prd$n.log"
    done
    rm -rf "$PRDIR"
else
    echo "[selftest] PROCRACE BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    check "PROCRACE gate build" 1
fi

# ---- M51 FSVIRT GATE [M51 C2]: ramfs / procfs locks under real concurrency ----
# fs/fsvirt_test.c (-DSECOS_FSVIRT_TEST, lockdep, -smp 4): every CPU runs the
# ramfs ops at once outside the BKL (create/grow/read/truncate/rename/chmod/
# readdir/mkdir/unlink + atomic appends to one log, two passes, heap_check
# and MemFree compared), then procfs readers against a writer rewriting held
# processes' cwd and fd 5 under mm_lock, a negative control (unlocked writer
# must be seen torn), and readers while those processes are destroyed.
echo "[selftest] M51 FSVIRT gate: ramfs/procfs locks, -smp 4, lockdep (mb2, ~60s)..."
make clean >/dev/null 2>&1 || true
if make iso CONSOLE=root CFLAGS_EXTRA="-DSECOS_FSVIRT_TEST -DSECOS_LOCKDEP" >/tmp/secos_selftest_build.log 2>&1; then
    FVDIR=$(mktemp -d /tmp/secos_selftest_fsvt.XXXXXX)
    cp myos.iso "$FVDIR/fsvt.iso"
    timeout 150 qemu-system-x86_64 -cdrom "$FVDIR/fsvt.iso" -boot d -smp 4 -m 1G \
        -debugcon file:"$FVDIR/fsvt.log" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -no-shutdown -display none -serial null >/dev/null 2>&1 &
    FVPID=$!
    for _ in $(seq 1 140); do grep -aq "^\[FSVT\] RESULT " "$FVDIR/fsvt.log" 2>/dev/null && break; sleep 1; done
    kill "$FVPID" 2>/dev/null; wait "$FVPID" 2>/dev/null
    while IFS= read -r l; do
        case "$l" in
            "[FSVT] FAIL "*)   check "M51 FSVIRT ${l#\[FSVT\] }" 1 ;;
            "[FSVT] "*": OK")  x=${l#\[FSVT\] }; check "M51 FSVIRT ${x%: OK}" 0 ;;
            "[FSVT] "*": FAIL") x=${l#\[FSVT\] }; check "M51 FSVIRT ${x%: FAIL}" 1 ;;
        esac
    done < <(grep -a "^\[FSVT\] " "$FVDIR/fsvt.log")
    grep -aq "^\[FSVT\] RESULT PASS" "$FVDIR/fsvt.log"; check "M51 FSVIRT self-test ran to completion and passed" $?
    ! grep -aq "\[LOCKDEP\]\|PANIC\|\[EXC\]" "$FVDIR/fsvt.log"
    check "M51 FSVIRT no lockdep violation, panic or CPU exception" $?
    cp "$FVDIR/fsvt.log" /tmp/secos_selftest_fsvt.log
    rm -rf "$FVDIR"
else
    echo "[selftest] M51 FSVIRT BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    check "M51 FSVIRT gate build" 1
fi

# ---- M51 TTY GATE [C5]: console line discipline + pty locks ----
# One image with -DSECOS_TTY_TEST (boot self-test, kernel/tty_test.c: every
# CPU drives the console ldisc and pty pairs at once outside the BKL — typed
# lines exactly once / in order per typist, raw byte streams complete and in
# order, echo under the lock, the ESC wait dropping the lock, the emulator
# reply ring, pty open/close churn, and a negative control without the
# console lock that must show damage) and -DSECOS_LOCKDEP, on -smp 4.
# tools/tty_stress_drive.py then runs /bin/smptest pty (16 pairs) and pipe,
# types on the PS/2 keyboard (QMP) and the serial line while a background job
# writes to the console, and sends Ctrl-C / Ctrl-Z from the keyboard IRQ.
echo "[selftest] M51 TTY gate: ldisc/pty forced concurrency + pty/pipe stress -smp 4..."
make clean >/dev/null 2>&1 || true
if make iso CONSOLE=root CFLAGS_EXTRA="-DSECOS_TTY_TEST -DSECOS_LOCKDEP" >/tmp/secos_selftest_build.log 2>&1; then
    TSDDIR=$(mktemp -d /tmp/secos_selftest_tsd.XXXXXX)
    cp myos.iso "$TSDDIR/tsd.iso"
    timeout 2400 python3 tools/tty_stress_drive.py --iso "$TSDDIR/tsd.iso" --smp 4 --tttest \
        --log "$TSDDIR/tsd.log" >"$TSDDIR/tsd.out" 2>&1
    while IFS= read -r l; do
        case "$l" in
            "[TSD] RESULT "*) ;;
            "[TSD] "*" OK"*)   x=${l#\[TSD\] }; check "M51 TTY ${x%% OK*}" 0 ;;
            "[TSD] "*" FAIL"*) x=${l#\[TSD\] }; check "M51 TTY ${x%% FAIL*}" 1; echo "    $l" | cut -c1-240 ;;
        esac
    done < "$TSDDIR/tsd.out"
    grep -q "^\[TSD\] RESULT " "$TSDDIR/tsd.out"; check "M51 TTY driver ran to completion" $?
    cp "$TSDDIR/tsd.out" /tmp/secos_selftest_tsd.out; cp "$TSDDIR/tsd.log" /tmp/secos_selftest_tsd.log
    rm -rf "$TSDDIR"
else
    echo "[selftest] M51 TTY BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    check "M51 TTY gate build" 1
fi

# ---- M51 EXEC GATE [C6]: concurrent signed-image loads + userland exec stress ----
# One -DSECOS_EXEC_TEST -DSECOS_LOCKDEP image at -smp 4. At boot,
# kernel/exec_test.c loads signed images (hello, coreutils, bash) and tampered
# copies on every CPU at once, outside the BKL, through both loader entry
# points (copy, and the SYS_SPAWN owned-buffer path): every signed image must
# load with a byte-exact pinned copy, every tampered one be refused, the
# revocation list take concurrent writers, and no frame or kmalloc leak. Then
# tools/exec_stress_drive.py runs `bash -c true` x500 in 4 parallel loops plus
# a loop of a tampered program (refused every time), MemFree steady.
echo "[selftest] M51 exec gate: concurrent loads (boot self-test) + bash -c true x500, -smp 4..."
make clean >/dev/null 2>&1 || true
if make iso CONSOLE=root CFLAGS_EXTRA="-DSECOS_EXEC_TEST -DSECOS_LOCKDEP" >/tmp/secos_selftest_build.log 2>&1; then
    EXDIR=$(mktemp -d /tmp/secos_selftest_exec.XXXXXX)
    cp myos.iso "$EXDIR/ex.iso"
    timeout 1800 python3 tools/exec_stress_drive.py --iso "$EXDIR/ex.iso" --smp 4 --count 500 \
        --log "$EXDIR/ex.log" >"$EXDIR/ex.out" 2>&1
    while IFS= read -r l; do
        case "$l" in
            "[ESD] RESULT "*|"[ESD] memfree "*) ;;
            "[ESD] "*" OK"*)   x=${l#\[ESD\] }; check "M51 exec ${x%% OK*}" 0 ;;
            "[ESD] "*" FAIL"*) x=${l#\[ESD\] }; check "M51 exec ${x%% FAIL*}" 1; echo "    $l" | cut -c1-240 ;;
        esac
    done <"$EXDIR/ex.out"
    grep -q "^\[ESD\] RESULT " "$EXDIR/ex.out"; check "M51 exec stress driver ran to completion" $?
    grep -aq "\[EXECTEST\] DONE ok=[0-9]* fail=0" "$EXDIR/ex.log"; check "M51 exec boot self-test: every check passed" $?
    ! grep -aq "\[EXECTEST\] .*: FAIL" "$EXDIR/ex.log"; check "M51 exec boot self-test: no failing check" $?
    grep -aq "\[EXECTEST\] at least two CPUs loaded images at once: OK" "$EXDIR/ex.log"
    check "M51 exec boot self-test ran on several CPUs at once" $?
    grep -a "\[EXECTEST\] serial \|\[EXECTEST\] pmm " "$EXDIR/ex.log" | sed 's/^/    /'
    cp "$EXDIR/ex.out" /tmp/secos_selftest_exec.out; cp "$EXDIR/ex.log" /tmp/secos_selftest_exec.log
    rm -rf "$EXDIR"
else
    echo "[selftest] M51 exec BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    check "M51 exec gate build" 1
fi

# ---- M53b: ACPI DMAR / IVRS parsing (tools/acpi_dmar_gate.sh) ----
# Read-only: host known answers + 50k-table fuzz, then q35 + intel-iommu /
# amd-iommu, plain q35, i440fx and UEFI guests must log the expected
# [IOMMU] lines and boot normally.
echo "[selftest] Running the M53b DMAR/IVRS gate (~2 min)..."
make clean >/dev/null 2>&1 || true
tools/acpi_dmar_gate.sh >/tmp/secos_selftest_m53b.out 2>&1
while IFS= read -r l; do
    case "$l" in
        "  [PASS] "*) check "${l#  \[PASS\] }" 0 ;;
        "  [FAIL] "*) check "${l#  \[FAIL\] }" 1 ;;
        "  [SKIP] "*) echo "$l" ;;
    esac
done </tmp/secos_selftest_m53b.out
grep -q "^\[m53b\] [0-9]* passed" /tmp/secos_selftest_m53b.out; check "M53b gate ran to completion" $?

echo "[selftest] ---"
echo "[selftest] RESULT: $PASS passed, $FAIL failed"
if [[ "$FAIL" -eq 0 ]]; then echo "[selftest] DONE: ALL PASS"; exit 0; fi
echo "[selftest] DONE: FAILURES PRESENT (log: $LOG)"
exit 1
