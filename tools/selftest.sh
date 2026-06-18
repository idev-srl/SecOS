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
#   A. M4 isolation selftest reports 12/12 PASS.
#   B. M7 ring-3 cooperative scheduling: two ring-3 processes alternate via
#      SYS_YIELD (both 1->2 and 2->1 switches observed, no CPU exception).

set -euo pipefail

TIMEOUT=12
while [[ $# -gt 0 ]]; do
    case "$1" in
        --timeout) shift; TIMEOUT=$1 ;;
        -h|--help) sed -n '2,/^set /p' "$0" | grep '^#' | sed 's/^# \?//'; exit 0 ;;
        *) echo "[selftest] unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

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
if ! tools/smoke.sh --mb2 --timeout "$TIMEOUT" --log "$LOG" >/dev/null 2>&1; then
    echo "[selftest] FAIL — kernel did not stay alive (triple fault / crash)"
    FAIL=$((FAIL+1))
fi

echo "[selftest] Assertions:"

# A. M4 isolation selftest 12/12
grep -q "12/12 PASS" "$LOG"; check "M4 isolation selftest 12/12" $?

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
    set +e
    timeout "$((TIMEOUT+6))" qemu-system-x86_64 \
        -cdrom myos.iso \
        -drive file=disk.img,if=virtio,format=raw -boot d \
        -debugcon file:"$log" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -no-shutdown -display none -m 256M
    set -e
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
set +e
timeout "$TIMEOUT" qemu-system-x86_64 -cdrom myos.iso \
    -debugcon file:"$M12LOG" -global isa-debugcon.iobase=0xe9 \
    -no-reboot -display none -m 2G
set -e

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

echo "[selftest] ---"
echo "[selftest] RESULT: $PASS passed, $FAIL failed"
if [[ "$FAIL" -eq 0 ]]; then echo "[selftest] DONE: ALL PASS"; exit 0; fi
echo "[selftest] DONE: FAILURES PRESENT (log: $LOG)"
exit 1
