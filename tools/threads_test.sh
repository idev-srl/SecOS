#!/usr/bin/env bash
# tools/threads_test.sh — U9 THREADS gate: POSIX threads on SecOS.
#
#   host: the musl sysroot (tools/port-musl.sh, cached); tools/secos-cc builds
#         user/thrtest/thrraw.c (raw clone/futex/tgkill: the kernel side) and
#         user/thrtest/thrstress.c (musl pthreads) onto an ext2 test disk;
#   image: an ISO (CONSOLE=root, ROOT=ram), -DSECOS_LOCKDEP with --lockdep;
#   guest, for every -smp in the list (default "1 2 4"), over the serial bash:
#     - /bin/thrstress is installed (embedded, crypto/user_thrstress_elf.h);
#     - `thrraw all`: clone, 40 threads, 2000-thread churn, shared fd table,
#       exit_group, leader exits first, kill -9, fork/execve from a thread,
#       futex (EAGAIN, timeouts MONOTONIC/REALTIME, wake counts, CMP_REQUEUE,
#       bitsets, a 20000-round ping-pong), tgkill, process-signal routing,
#       group stop/continue + SIGINT, /proc/<pid>/task + Threads:;
#     - `thrstress all N` (N = 8, 16 at -smp 4): mutex, condvar, rwlock,
#       barrier, semaphore, pthread_cancel (read, cond_wait, sleep, async,
#       disabled+testcancel), join of 10000 short threads, 500 detached
#       threads (stacks unmapped by the threads themselves), TLS, keys,
#       pthread_once, malloc and stdio from N threads, pthread_kill and
#       routing, kill / SIGINT to a multi-threaded process, execve from a
#       thread, errno per thread;
#     - smptest threads (futex ping-pong pairs + shared mmap/fd churn + fork
#       from a thread, lost-wakeup watchdog) and smptest thrkill;
#     - from the shell: a 9-thread process shows Threads: 9 and dies by
#       kill -9 / SIGINT with the right status;
#     - no heap growth (SUnreclaim) and no frame leak (MemFree + Slab) between
#       a warm-up run and a second full run;
#     - no CPU exception, panic, lockdep report or SMAP violation on debugcon.
#   plus one boot with the `nothreads` boot word: pthread_create fails,
#   single-threaded programs (bash, musl) still run.
# Options: --lockdep, --iso FILE, --smp "LIST", --keep, --no-nothreads.
# Output: "  [PASS] THREADS ..." / "  [FAIL] THREADS ..." and a RESULT line.
# QEMU runs with a private /tmp (unshare -rm), no network; only QEMUs started
# here are killed.
set -uo pipefail
LOCKDEP=0; ISO=""; SMPS="${THREADS_SMP:-1 2 4}"; KEEP=0; NOTHR=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --lockdep) LOCKDEP=1 ;;
        --iso) shift; ISO=$1 ;;
        --smp) shift; SMPS=$1 ;;
        --keep) KEEP=1 ;;
        --no-nothreads) NOTHR=0 ;;
        *) echo "threads_test: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
export PYENV_VERSION=${PYENV_VERSION:-3.11.8}
mkdir -p build
W=$(mktemp -d "$REPO/build/threads_test.XXXXXX")
PASSN=0; FAILN=0
ok() { if [[ "$2" -eq 0 ]]; then echo "  [PASS] THREADS $1"; PASSN=$((PASSN+1));
       else echo "  [FAIL] THREADS $1"; FAILN=$((FAILN+1)); fi; }
QPID=
cleanup() {
    [[ -n "$QPID" ]] && kill -9 "$QPID" 2>/dev/null
    if [[ $KEEP -eq 0 ]]; then rm -rf "$W"; else echo "threads_test: artifacts kept in $W"; fi
}
trap cleanup EXIT
for t in mke2fs qemu-system-x86_64 unshare; do
    command -v $t >/dev/null || { echo "threads_test: $t missing"; echo "RESULT: 0 passed, 1 failed"; exit 2; }
done

# ---- host --------------------------------------------------------------------------
need=0
[[ -f build/sysroot/lib/libc.a ]] || need=1
for f in $(find user/musl-secos tools/port-musl.sh -type f); do [[ $f -nt build/sysroot/lib/libc.a ]] && need=1; done
if [[ $need -eq 1 ]]; then bash tools/port-musl.sh >"$W/port.log" 2>&1; rc=$?; else rc=0; fi
if [[ $rc -eq 3 ]]; then
    echo "  [SKIP] THREADS (musl sources not cached and not downloadable)"
    echo "[threads] RESULT: 0 passed, 0 failed (skipped)"; exit 0
fi
ok "musl sysroot (pthreads: __clone, cancellation points)" $rc
[[ $rc -eq 0 ]] || { tail -20 "$W/port.log"; echo "[threads] RESULT: $PASSN passed, $FAILN failed"; exit 1; }
D=$W/disk; mkdir -p "$D"
tools/secos-cc -O2 -Wall -Wextra -o "$D/thrraw" user/thrtest/thrraw.c -lsecos >"$W/cc.log" 2>&1 \
  && tools/secos-cc -O2 -Wall -Wextra -o "$D/thrstress" user/thrtest/thrstress.c -lsecos >>"$W/cc.log" 2>&1
rc=$?; ok "secos-cc builds thrraw + thrstress" $rc
[[ $rc -eq 0 ]] || { cat "$W/cc.log"; echo "[threads] RESULT: $PASSN passed, $FAILN failed"; exit 1; }
! grep -q "warning:" "$W/cc.log"; ok "test programs build without warnings" $?
mke2fs -q -t ext2 -d "$D" "$W/t.img" 64M >/dev/null 2>&1; ok "ext2 test disk" $?

build_iso() { # build_iso <out> <make args...>
    local out=$1; shift
    make clean >/dev/null 2>&1
    if ! make iso CONSOLE=root "$@" >"$W/build.log" 2>&1; then
        echo "threads_test: BUILD ERROR (see $W/build.log)"; tail -20 "$W/build.log"; KEEP=1; return 1
    fi
    cp myos.iso "$out"; make clean >/dev/null 2>&1
}
if [[ -z "$ISO" ]]; then
    echo "[threads] building the image (CONSOLE=root$([[ $LOCKDEP -eq 1 ]] && echo ', -DSECOS_LOCKDEP'))..."
    extra=(); [[ $LOCKDEP -eq 1 ]] && extra=(CFLAGS_EXTRA=-DSECOS_LOCKDEP)
    build_iso "$W/os.iso" "${extra[@]}" || { echo "[threads] RESULT: $PASSN passed, $((FAILN+1)) failed"; exit 1; }
    ISO=$W/os.iso
    ! grep -q "warning:" "$W/build.log"; ok "kernel image builds without warnings" $?
fi

# ---- guest ---------------------------------------------------------------------------
boot() { # boot <iso> <smp> <tag>
    local tag=$3
    rm -f "$W/in"; mkfifo "$W/in"; : >"$W/$tag.ser"; : >"$W/$tag.dbg"
    unshare -rm bash -c 'mount -t tmpfs tmpfs /tmp && exec qemu-system-x86_64 -cdrom "$1" -boot d -m 1G -cpu max \
        -smp "$2" -drive file="$3",if=virtio,format=raw \
        -debugcon file:"$4" -global isa-debugcon.iobase=0xe9 -no-reboot -display none -nic none \
        -serial stdio' _ "$1" "$2" "$W/t.img" "$W/$tag.dbg" <"$W/in" >"$W/$tag.ser" 2>&1 &
    QPID=$!
    exec 3>"$W/in"
    SER=$W/$tag.ser; DBG=$W/$tag.dbg
}
stop() { exec 3>&-; [[ -n "$QPID" ]] && kill -9 "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=; }
send() { printf '%s\r' "$1" >&3; }
waitfor() { # waitfor <regex> <seconds>
    local n=$(( $2 * 5 ))
    while (( n-- > 0 )); do
        grep -aEq -- "$1" "$SER" && return 0
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.2
    done
    grep -aEq -- "$1" "$SER"
}
run() { send "$3; echo $1-RC-\$?"; waitfor "^$1-RC-[0-9]+" "$2"; }
rcof() { grep -a "^$1-RC-" "$SER" | tail -1 | sed -E "s/^$1-RC-([0-9]+).*/\1/"; }
lines() { # lines <prefix> <check-prefix> <from-marker>: every "<prefix> PASS/FAIL" after the marker
    tr -d '\r' < "$SER" | sed -n "/$3/,\$p" | grep -a "^$1 PASS \|^$1 FAIL " | while IFS= read -r l; do
        case "$l" in "$1 PASS "*) echo "0 $2 ${l#$1 PASS }";; *) echo "1 $2 ${l#$1 FAIL }";; esac; done
}
meminfo() { # meminfo <tag> -> "MemFree Slab SUnreclaim" (kB) of the last MEM-<tag> dump
    tr -d '\r' < "$SER" | sed -n "/^MEMB-$1/,/^MEME-$1/p" | awk '/^MemFree:/{f=$2} /^Slab:/{s=$2} /^SUnreclaim:/{u=$2} END{print f+0, s+0, u+0}'
}

for SMP in $SMPS; do
    N=8; [[ $SMP -ge 4 ]] && N=16
    T="smp$SMP"
    echo "[threads] booting (-smp $SMP, N=$N)..."
    boot "$ISO" "$SMP" "$T"
    waitfor 'bash-[0-9.]+# ' 240; ok "$T boot to the root console" $?
    run MNT 30 "grep -q ' /mnt ' /proc/mounts"; [[ "$(rcof MNT)" == 0 ]]; ok "$T test disk at /mnt" $?
    run BIN 30 "test -x /bin/thrstress"; [[ "$(rcof BIN)" == 0 ]]; ok "$T /bin/thrstress installed" $?
    # warm-up: the first run grows the heap to its working size
    run WARM 600 "/bin/thrstress all $N >/dev/null 2>&1"
    send "echo MEMB-a; cat /proc/meminfo; echo MEME-a"; waitfor '^MEME-a' 30
    echo "[threads]   thrraw all..."
    run RAW 600 "echo THRRAW-BEGIN; /mnt/thrraw all"
    while read -r rc name; do ok "$T $name" "$rc"; done < <(lines "TR" "raw" "THRRAW-BEGIN")
    grep -aq "^TR DONE pass=[0-9]* fail=0" "$SER" && [[ "$(rcof RAW)" == 0 ]]; ok "$T thrraw all completed (rc $(rcof RAW))" $?
    echo "[threads]   thrstress all $N..."
    run STR 900 "echo THRSTRESS-BEGIN; /mnt/thrstress all $N"
    while read -r rc name; do ok "$T $name" "$rc"; done < <(lines "TS" "pthread" "THRSTRESS-BEGIN")
    grep -aEq "^TS DONE pass=[0-9]+ fail=0 N=$N" "$SER" && [[ "$(rcof STR)" == 0 ]]; ok "$T thrstress all $N completed (rc $(rcof STR))" $?
    echo "[threads]   smptest threads / thrkill..."
    run SMT 120 "smptest threads $N 10"
    grep -aq "^\[smptest\] threads PASS" "$SER" && [[ "$(rcof SMT)" == 0 ]]
    ok "$T smptest threads ($(grep -ao '\[smptest\] threads [A-Z]* threads=[0-9]* pingpongs=[0-9]*' "$SER" | tail -1 | sed 's/.*pingpongs=//') ping-pongs)" $?
    run SMK 180 "smptest thrkill 30 $N"
    grep -aq "^\[smptest\] thrkill PASS" "$SER" && [[ "$(rcof SMK)" == 0 ]]; ok "$T smptest thrkill 30 rounds" $?
    # from the shell: Threads: in /proc, kill -9 and SIGINT of a 9-thread process
    run SPN 60 "/bin/thrstress spin 8 > /dev/null & sleep 1; grep -E '^Threads:' /proc/\$!/status | tr -d '\t'; ls /proc/\$!/task | wc -l | sed 's/^/TASKS=/'; kill -9 \$!; wait \$!"
    [[ "$(rcof SPN)" == 137 ]] && grep -aq "^Threads:9" "$SER" && grep -aq "^TASKS=9" "$SER"
    ok "$T shell: Threads: 9, 9 task entries, kill -9 -> 137" $?
    run SPI 60 "/bin/thrstress spin 8 > /dev/null & sleep 1; kill -INT \$!; wait \$!"
    [[ "$(rcof SPI)" == 130 ]]; ok "$T shell: SIGINT to a 9-thread process -> 130 (rc $(rcof SPI))" $?
    run PS 30 "ls /proc | grep -c '^[0-9]' | sed 's/^/NPROC=/'"
    # second full run: no heap / frame growth
    run AGAIN 900 "/bin/thrstress all $N >/dev/null 2>&1; /mnt/thrraw all >/dev/null 2>&1"
    [[ "$(rcof AGAIN)" == 0 ]]; ok "$T second thrstress + thrraw run passes" $?
    sleep 2
    send "echo MEMB-b; cat /proc/meminfo; echo MEME-b"; waitfor '^MEME-b' 30
    read -r f1 s1 u1 < <(meminfo a); read -r f2 s2 u2 < <(meminfo b)
    du=$(( u2 - u1 )); dfs=$(( (f2 + s2) - (f1 + s1) ))
    [[ $u1 -gt 0 && $du -le 512 && $dfs -ge -2048 ]]
    ok "$T no growth: SUnreclaim $u1 -> $u2 kB (+$du), MemFree+Slab $((f1+s1)) -> $((f2+s2)) kB" $?
    ! grep -aEq "\[EXC\]|PANIC|panic|\[LOCKDEP\]|\[LOCK\] BUG|\[SMAP\] violation|\[TLB\].*(BUG|timeout)|\[PROC\] BUG" "$DBG"
    ok "$T debugcon: no CPU exception, panic, lockdep report or SMAP violation" $?
    stop
done

if [[ $NOTHR -eq 1 ]]; then
    echo "[threads] building the nothreads image..."
    if build_iso "$W/nothr.iso" KERNEL_CMDLINE=nothreads; then
        boot "$W/nothr.iso" 2 nothr
        waitfor 'bash-[0-9.]+# ' 240; ok "nothreads: boot to the root console" $?
        grep -aq "\[THREAD\] nothreads: clone(CLONE_THREAD) refused" "$DBG"; ok "nothreads: boot word seen" $?
        run NT 60 "/mnt/thrstress mutex 4"
        [[ "$(rcof NT)" != 0 ]] && grep -aq "^TS FAIL mutex" "$SER"; ok "nothreads: pthread_create fails" $?
        run NF 60 "/mnt/thrraw fork"; [[ "$(rcof NF)" != 0 ]]; ok "nothreads: raw clone refused" $?
        run NB 60 "echo \$((6*7)) | grep -q 42 && /mnt/thrstress execd 5"
        [[ "$(rcof NB)" == 5 ]]; ok "nothreads: bash and a musl program still run" $?
        stop
    else
        ok "nothreads image builds" 1
    fi
fi
echo "[threads] RESULT: $PASSN passed, $FAILN failed"
[[ $FAILN -eq 0 ]]
