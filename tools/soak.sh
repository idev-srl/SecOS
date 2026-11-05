#!/usr/bin/env bash
# tools/soak.sh — SECoS randomized "realistic session" soak + bug-finding harness.
#
# Boots the default image into the signed GNU bash login shell and drives a long,
# seeded, realistic interactive session over the serial console (coreutils,
# loops, background jobs, file I/O in ramfs, /proc and /dev reads, long/wrapping
# output, and optionally the ring-3 syscall fuzzer). Detects: CPU exceptions,
# kernel panics, heap-corruption markers, triple faults (QEMU exits early),
# stalls (no progress), and missing expected output (sentinels). This is the
# complement to the demo-based gates in tools/selftest.sh: it exercises the
# system the way a human at the shell would, which is how the heap-corruption
# bug of 2026-11 was found.
#
# Usage:
#   tools/soak.sh                 # short run (CI-sized): fixed seed, ~2-3 min
#   tools/soak.sh --long          # long run: many iterations, ~10-15 min
#   tools/soak.sh --seed N        # reproduce a specific session
#   tools/soak.sh --iters N       # number of commands to drive
#   tools/soak.sh --mem 512M|1G|4G
#   tools/soak.sh --smp 1|2
#   tools/soak.sh --disk          # attach an ext4 virtio-blk data disk at /mnt
#   tools/soak.sh --fuzz          # also run the signed syscall fuzzer (safe mode)
#   tools/soak.sh --cmdsub        # enable $(...) command substitution (known-hung
#                                 # on main until fix/bash-cmdsubst lands)
#   tools/soak.sh --log PATH      # debugcon+serial log path (default /tmp/soak.log)
#
# Exit: 0 = PASS (session completed, no kernel fault), 1 = FAIL (see log + .repro).
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

LONG=0; SEED=$RANDOM; ITERS=250; MEM=256M; SMP=1; DISK=0; FUZZ=0; CMDSUB=0
LOG=/tmp/soak.log; SETTLE=28; STALL=25; PIPES=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --long)   LONG=1 ;;
        --seed)   shift; SEED=$1 ;;
        --iters)  shift; ITERS=$1 ;;
        --mem)    shift; MEM=$1 ;;
        --smp)    shift; SMP=$1 ;;
        --disk)   DISK=1 ;;
        --fuzz)   FUZZ=1 ;;
        --cmdsub) CMDSUB=1 ;;
        --pipes)  PIPES=1 ;;
        --log)    shift; LOG=$1 ;;
        --settle) shift; SETTLE=$1 ;;
        --stall)  shift; STALL=$1 ;;
        -h|--help) sed -n '2,/^set /p' "$0" | grep '^#' | sed 's/^# \?//'; exit 0 ;;
        *) echo "[soak] unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
[[ $LONG -eq 1 ]] && ITERS=1200

echo "[soak] building ISO (default image)..."
if ! make iso >/tmp/soak_build.log 2>&1; then
    echo "[soak] BUILD ERROR — see /tmp/soak_build.log" >&2; tail -20 /tmp/soak_build.log >&2; exit 2
fi

QEMU=(qemu-system-x86_64 -cdrom myos.iso -boot d -m "$MEM" -smp "$SMP"
      -debugcon "file:$LOG" -global isa-debugcon.iobase=0xe9
      -no-reboot -display none -serial stdio)

if [[ $DISK -eq 1 ]]; then
    echo "[soak] building ext4 data disk..."
    make disk-ext4 >/tmp/soak_disk.log 2>&1 || { echo "[soak] disk build failed"; exit 2; }
    QEMU+=(-drive file=disk.img,if=virtio,format=raw)
fi

echo "[soak] seed=$SEED iters=$ITERS mem=$MEM smp=$SMP disk=$DISK fuzz=$FUZZ cmdsub=$CMDSUB"
: > "$LOG"

OPTS=(--seed "$SEED" --iters "$ITERS" --settle "$SETTLE" --stall "$STALL"
      --smp "$SMP" --log "$LOG")
[[ $DISK   -eq 1 ]] && OPTS+=(--disk)
[[ $FUZZ   -eq 1 ]] && OPTS+=(--fuzz)
[[ $CMDSUB -eq 1 ]] && OPTS+=(--cmdsub)
export SOAK_PIPES=$PIPES

python3 tools/soak_drive.py "${OPTS[@]}" -- "${QEMU[@]}"
rc=$?
echo "[soak] debugcon log: $LOG"
[[ -f "$LOG.repro" ]] && echo "[soak] reproducer: $LOG.repro"
exit $rc
