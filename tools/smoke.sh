#!/usr/bin/env bash
# tools/smoke.sh — SECoS deterministic smoke test runner.
#
# Usage:
#   tools/smoke.sh --mb2 [--timeout N] [--log /tmp/foo.log]
#   tools/smoke.sh --uefi [--timeout N] [--log /tmp/foo.log]
#
# Exit codes:
#   0  PASS — QEMU was killed by timeout (kernel alive after N seconds)
#   1  FAIL — QEMU exited before timeout (triple fault / reset / crash)
#   2  BUILD ERROR or missing dependency
#
# PASS/FAIL criterion:
#   A triple fault triggers a CPU reset. With -no-reboot, QEMU exits
#   immediately (exit 0 before timeout). A live kernel keeps running
#   until timeout(1) kills it (exit 124, remapped to 0 here).
#
# Debugcon log:
#   Kernel writes crash-signature markers to ISA port 0xE9.
#   QEMU forwards those bytes to -debugcon file:$LOG.
#   Expect to see:
#       SECoS build <BUILD_TS> git:<GIT_HASH>
#       [M2] Stack switch ok. RSP=0xFFFFFF80000049xx

set -euo pipefail

# ── defaults ──────────────────────────────────────────────────────────────────
MODE=""
TIMEOUT=20
LOG=""

# ── argument parsing ───────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mb2)    MODE=mb2 ;;
        --uefi)   MODE=uefi ;;
        --timeout) shift; TIMEOUT=$1 ;;
        --log)     shift; LOG=$1 ;;
        -h|--help)
            sed -n '2,/^set /p' "$0" | grep '^#' | sed 's/^# \?//'
            exit 0 ;;
        *) echo "[smoke] Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

if [[ -z "$MODE" ]]; then
    echo "[smoke] Error: specify --mb2 or --uefi" >&2
    exit 2
fi

if [[ -z "$LOG" ]]; then
    LOG="/tmp/secos_${MODE}.log"
fi

# Resolve repo root (parent of tools/)
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

echo "[smoke] Mode    : $MODE"
echo "[smoke] Timeout : ${TIMEOUT}s"
echo "[smoke] Log     : $LOG"
echo ""

# ── dependency checks ──────────────────────────────────────────────────────────
check_cmd() {
    if ! command -v "$1" &>/dev/null; then
        echo "[smoke] ERROR: '$1' not found in PATH" >&2
        exit 2
    fi
}
check_cmd qemu-system-x86_64
check_cmd timeout

# ── build ─────────────────────────────────────────────────────────────────────
build_mb2() {
    echo "[smoke] Building Multiboot2 ISO..."
    if ! make iso 2>&1; then
        echo "[smoke] BUILD FAILED (make iso)" >&2
        exit 2
    fi
    if [[ ! -f myos.iso ]]; then
        echo "[smoke] ERROR: myos.iso not found after build" >&2
        exit 2
    fi
    echo "[smoke] Build OK: myos.iso"
}

build_uefi() {
    echo "[smoke] Building kernel + UEFI loader..."
    # 'make uefi' depends on 'make all', so one invocation is enough.
    # It also copies kernel.bin → dist/kernel.elf (Makefile line 153).
    if ! make uefi 2>&1; then
        echo "[smoke] BUILD FAILED (make uefi)" >&2
        exit 2
    fi
    if [[ ! -f dist/EFI/BOOT/BOOTX64.EFI ]]; then
        echo "[smoke] ERROR: dist/EFI/BOOT/BOOTX64.EFI not found after build" >&2
        exit 2
    fi
    if [[ ! -f dist/kernel.elf ]]; then
        echo "[smoke] ERROR: dist/kernel.elf not found after build" >&2
        exit 2
    fi
    echo "[smoke] Build OK: dist/EFI/BOOT/BOOTX64.EFI + dist/kernel.elf"
}

# ── OVMF detection ─────────────────────────────────────────────────────────────
find_ovmf() {
    local candidates=(
        /usr/share/ovmf/OVMF.fd
        /usr/share/OVMF/OVMF.fd
        /usr/share/OVMF/x64/OVMF.fd
        /usr/share/edk2/ovmf/OVMF_CODE.fd
        /usr/share/edk2-ovmf/OVMF_CODE.fd
    )
    for f in "${candidates[@]}"; do
        if [[ -f "$f" ]]; then
            echo "$f"
            return 0
        fi
    done
    echo "" ; return 1
}

# ── run ───────────────────────────────────────────────────────────────────────
run_mb2() {
    echo "[smoke] Launching QEMU (Multiboot2 / GRUB)..."
    # Clear previous log
    : > "$LOG"
    # 'timeout' returns 124 when it kills the child (= PASS for us).
    # Disable set -e here so the non-zero exit propagates to QEMU_EXIT below.
    set +e
    timeout "$TIMEOUT" qemu-system-x86_64 \
        -cdrom myos.iso \
        -debugcon file:"$LOG" \
        -global isa-debugcon.iobase=0xe9 \
        -no-reboot -no-shutdown \
        -display none \
        -m 256M
    QEMU_EXIT=$?
    set -e
}

run_uefi() {
    local ovmf
    ovmf="$(find_ovmf)" || true
    if [[ -z "$ovmf" ]]; then
        echo "[smoke] ERROR: OVMF firmware not found. Install ovmf package." >&2
        echo "  Tried: /usr/share/ovmf/OVMF.fd, /usr/share/OVMF/OVMF.fd, ..." >&2
        exit 2
    fi
    echo "[smoke] OVMF : $ovmf"
    echo "[smoke] Launching QEMU (UEFI / q35 / OVMF)..."
    # Clear previous log
    : > "$LOG"
    # 'timeout' returns 124 when it kills the child (= PASS for us).
    # Disable set -e here so the non-zero exit propagates to QEMU_EXIT below.
    set +e
    timeout "$TIMEOUT" qemu-system-x86_64 \
        -machine q35,accel=tcg \
        -m 256M \
        -bios "$ovmf" \
        -drive format=raw,file=fat:rw:dist/ \
        -debugcon file:"$LOG" \
        -global isa-debugcon.iobase=0xe9 \
        -no-reboot -no-shutdown \
        -display none \
        -net none
    QEMU_EXIT=$?
    set -e
}

# QEMU_EXIT is set inside run_mb2() / run_uefi() after the timeout call.
QEMU_EXIT=0

# ── main ──────────────────────────────────────────────────────────────────────
case "$MODE" in
    mb2)
        build_mb2
        run_mb2
        ;;
    uefi)
        build_uefi
        run_uefi
        ;;
esac

echo ""
echo "[smoke] QEMU exit code: $QEMU_EXIT"

if [[ $QEMU_EXIT -eq 124 ]]; then
    # timeout(1) sends SIGTERM to QEMU after N seconds and returns 124.
    # This means the kernel was alive for the full test window.
    echo "[smoke] RESULT: PASS — kernel alive for ${TIMEOUT}s (no triple fault)"
    echo ""
    echo "[smoke] Debugcon log ($LOG):"
    if [[ -s "$LOG" ]]; then
        cat "$LOG"
    else
        echo "  (empty — kernel may not write to debugcon port 0xE9 yet)"
    fi
    exit 0
else
    # QEMU exited before timeout:
    #   exit 0  → guest reset (triple fault + -no-reboot → QEMU exits cleanly)
    #   exit >0 → QEMU error or signal
    echo "[smoke] RESULT: FAIL — QEMU exited before timeout (crash / triple fault)"
    echo ""
    echo "[smoke] Debugcon log ($LOG, last 40 lines):"
    if [[ -s "$LOG" ]]; then
        tail -40 "$LOG"
    else
        echo "  (empty)"
    fi
    exit 1
fi
