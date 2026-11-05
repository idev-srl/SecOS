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
if ! make iso CFLAGS_EXTRA="-DSECOS_SMP_AT_BOOT" >/tmp/secos_selftest_build.log 2>&1; then
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
if ! make iso CFLAGS_EXTRA="-DM8_SCHED_DEMO=1 -DDEV_ALLOW_UNSIGNED -DSECOS_SMP_AT_BOOT" >/tmp/secos_selftest_build.log 2>&1; then
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
# M38 WPA2 crypto KATs + M39 association-core KATs (run at boot in crypto selftest).
grep -q "\[WPA2\] PBKDF2 PMK KAT PASS" "$MLLOG"; check "M38 WPA2 PBKDF2-SHA1 PMK KAT (IEEE 802.11i)" $?
grep -q "\[WPA2\] AES-128 KAT PASS" "$MLLOG"; check "M38 AES-128 KAT (FIPS-197)" $?
grep -q "\[WPA2\] RFC3394 key-wrap KAT PASS" "$MLLOG"; check "M39 RFC3394 key-wrap KAT" $?
grep -q "\[WPA2\] RFC3394 key-unwrap KAT PASS" "$MLLOG"; check "M39 RFC3394 key-unwrap KAT" $?
grep -q "\[WPA2\] PTK derivation (KCK|KEK|TK) OK" "$MLLOG"; check "M39 WPA2 PTK derivation (PRF)" $?
grep -q "\[WPA2\] 4-way handshake supplicant KAT PASS" "$MLLOG"; check "M39 synthetic WPA2 4-way handshake KAT" $?
grep -q "\[CRYPTO\] --- all PASS ---" "$MLLOG"; check "M38/M39 boot crypto suite all PASS" $?
! grep -q "\[EXC\]" "$MLLOG"; check "no CPU exception ([EXC]) during default -cpu Haswell boot" $?

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
echo "[selftest] Running ring-3 syscall fuzzer (full mode, mb2, serial, 60s)..."
: > "$FUZZLOG"
( sleep 25; printf '  /bin/sysfuzz -s 777 -b 12 -c 150; echo FUZZEND\r'; sleep 33 ) | \
    timeout 65 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -debugcon file:"$FUZZLOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
grep -q "\[sysfuzz\] DONE ok" "$FUZZLOG"; check "syscall fuzzer (full mode) ran every syscall to completion" $?
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
#     bash, run a command burst, drop to the recovery shell and `heapcheck` —
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
  printf '  exit\r'; sleep 4
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
UALOG=/tmp/secos_selftest_uaccess.log
echo "[selftest] Building plain image for the uaccess gate..."
make clean >/dev/null 2>&1 || true
if ! make iso >/tmp/secos_selftest_build.log 2>&1; then
    echo "[selftest] UACCESS BUILD ERROR — see /tmp/secos_selftest_build.log" >&2
    exit 2
fi
echo "[selftest] Running uaccess + ABI gate (mb2, serial, 60s)..."
: > "$UALOG"
( sleep 25; printf '  /bin/uaccess_test; echo UA-$((6*7))-END\r'; sleep 15
  printf '  /bin/abi_test; echo ABI-$((6*7))-END\r'; sleep 10 ) | \
    timeout 60 qemu-system-x86_64 -cdrom myos.iso -boot d -m 256M \
        -debugcon file:"$UALOG" -global isa-debugcon.iobase=0xe9 \
        -no-reboot -display none -serial stdio >/dev/null 2>&1
grep -q "\[UA-TEST\] DONE pass=[0-9]* fail=0" "$UALOG"; check "UACCESS every bad-pointer case returns an error (DONE fail=0)" $?
grep -q "UA-42-END" "$UALOG"; check "UACCESS shell survives the test (sentinel reached)" $?
grep -q "\[UA-TEST\] spawn-wild-path OK" "$UALOG"; check "UACCESS spawn(0xDEADBEEF) -> -EFAULT" $?
grep -q "\[UA-TEST\] lstat-protnone-statbuf OK" "$UALOG"; check "UACCESS lstat into a PROT_NONE page -> -EFAULT" $?
grep -q "\[UA-TEST\] write-console-overrun OK" "$UALOG"; check "UACCESS write(1, p, 1 MB) off the mapping -> -EFAULT/short" $?
grep -q "\[UA-TEST\] read-into-demand-page OK" "$UALOG"; check "UACCESS kernel-mode demand paging still serviced (M14)" $?
grep -q "\[UA-TEST\] kernel-write-to-cow-page OK" "$UALOG"; check "UACCESS kernel write into a COW page copies it (M19, CR0.WP)" $?
grep -q "\[UA-TEST\] sigframe-from-pf-path OK" "$UALOG"; check "UACCESS sigframe from the #PF return path (nested #PF)" $?
! grep -q "\[UA-TEST\] .* FAIL" "$UALOG"; check "UACCESS no case failed" $?
! grep -q "ring0" "$UALOG"; check "UACCESS no ring-0 CPU fault" $?
# ABI regression test (user/abi_test.c), same boot: POSIX wait status.
grep -q "\[ABI-TEST\] DONE pass=[0-9]* fail=0" "$UALOG"; check "ABI every case passes (DONE fail=0)" $?
grep -q "ABI-42-END" "$UALOG"; check "ABI shell survives the test (sentinel reached)" $?
grep -q "\[ABI-TEST\] wait-exit-142-not-signal OK" "$UALOG"; check "ABI exit(142) -> WIFEXITED, WEXITSTATUS=142" $?
grep -q "\[ABI-TEST\] wait-pf-is-sigsegv OK" "$UALOG"; check "ABI ring-3 #PF -> WIFSIGNALED, WTERMSIG=SIGSEGV" $?
grep -q "\[ABI-TEST\] wait-de-is-sigfpe OK" "$UALOG"; check "ABI ring-3 #DE -> SIGFPE" $?
! grep -q "\[ABI-TEST\] .* FAIL" "$UALOG"; check "ABI no case failed" $?
! grep -q "\[PANIC\]" "$UALOG"; check "UACCESS no kernel panic" $?

echo "[selftest] ---"
echo "[selftest] RESULT: $PASS passed, $FAIL failed"
if [[ "$FAIL" -eq 0 ]]; then echo "[selftest] DONE: ALL PASS"; exit 0; fi
echo "[selftest] DONE: FAILURES PRESENT (log: $LOG)"
exit 1
