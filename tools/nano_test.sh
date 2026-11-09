#!/usr/bin/env bash
# tools/nano_test.sh — U8 NANO gate: GNU nano (musl + ncursesw) on SecOS.
#
#   image: make clean + make iso CONSOLE=root (or --iso FILE), no root disk;
#   guest: tools/nano_drive.py boots it once (-smp $NANO_SMP, default 2) with a
#          user-net e1000 (a free host port 23000-23999 -> guest :22; never
#          2222/8000), PS/2 + usb-tablet and a private QMP socket, and edits a
#          file with /usr/bin/nano three ways, checking the saved bytes:
#          - over SSH on a host pty (Dropbear pty, TERM=xterm-256color): type
#            UTF-8 (à è), ^W search, ^K cut + ^U paste, resize the pty 30x90 ->
#            40x100 (SIGWINCH: nano repaints for 40 rows), ^O save, ^X exit;
#          - on the kernel console (TERM=linux), the same edit;
#          - in a secwm terminal (TERM=xterm): à è typed with the Italian
#            layout (loadkeys it), the window shrunk with the resize grip while
#            nano runs (nano repaints its help bar on the new last rows,
#            checked on a screendump), save, exit, `od` in the terminal;
#          plus: no ring-3 fault, no panic, no ENOSYS from the musl layer.
# Options: --iso FILE (skip the image build), --smp N, --keep, --only ssh,con,gui.
# Output: "  [PASS] NANO ..." / "  [FAIL] NANO ..." and "RESULT:".
# QEMU runs with a private /tmp (unshare -rm); only the QEMU started here is
# killed (the driver owns it).
set -uo pipefail
ISO=""; SMP=${NANO_SMP:-2}; KEEP=0; ONLY=ssh,con,gui
while [[ $# -gt 0 ]]; do
    case "$1" in
        --iso) shift; ISO=$1 ;;
        --smp) shift; SMP=$1 ;;
        --keep) KEEP=1 ;;
        --only) shift; ONLY=$1 ;;
        *) echo "nano_test: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
export PYENV_VERSION=${PYENV_VERSION:-3.11.8}
mkdir -p "$REPO/build"
W=$(mktemp -d "$REPO/build/nano_test.XXXXXX")   # not /tmp: QEMU gets a private one
PASSN=0; FAILN=0
ok() { if [[ "$2" -eq 0 ]]; then echo "  [PASS] NANO $1"; PASSN=$((PASSN+1));
       else echo "  [FAIL] NANO $1"; FAILN=$((FAILN+1)); fi; }
cleanup() { if [[ $KEEP -eq 0 ]]; then rm -rf "$W"; else echo "nano_test: artifacts kept in $W"; fi; }
trap cleanup EXIT
for t in qemu-system-x86_64 ssh ssh-keygen unshare; do
    command -v $t >/dev/null || { echo "  [SKIP] NANO gate ($t missing)"; echo "RESULT: 0 passed, 0 failed"; exit 0; }
done
if [[ -z "$ISO" ]]; then
    echo "[nano] building the image (make iso CONSOLE=root)..."
    make clean >/dev/null 2>&1
    if ! make iso CONSOLE=root >"$W/build.log" 2>&1; then
        tail -20 "$W/build.log"; ok "image builds" 1; echo "RESULT: $PASSN passed, $FAILN failed"; exit 1
    fi
    ISO=myos.iso
fi
cp "$ISO" "$W/nano.iso"
PORT=$((23000 + $$ % 1000))
[[ $PORT -eq 2222 || $PORT -eq 8000 ]] && PORT=23999
echo "[nano] guest session (ssh on host port $PORT, console, secwm)..."
unshare -rm bash -c 'mount -t tmpfs tmpfs /tmp && exec "$@"' _ \
    timeout 900 python3 tools/nano_drive.py --iso "$W/nano.iso" --log "$W/guest.log" --out "$W/drv.out" \
        --port "$PORT" --smp "$SMP" --only "$ONLY" --shots "$W" >"$W/drv.stdout" 2>&1
rc=$?
while IFS= read -r l; do
    case "$l" in
        "[NANO-DRV] DONE "*) ;;
        "[NANO-DRV] "*" OK"*)   n=${l#\[NANO-DRV\] }; ok "${n%% OK*}${n#* OK}" 0 ;;
        "[NANO-DRV] "*" FAIL"*) n=${l#\[NANO-DRV\] }; ok "${n%% FAIL*}${n#* FAIL}" 1 ;;
        "[NANO-DRV] "*) ok "${l#\[NANO-DRV\] }" 1 ;;
    esac
done <"$W/drv.out" 2>/dev/null
grep -q "^\[NANO-DRV\] DONE" "$W/drv.out" 2>/dev/null || { ok "driver ran to completion (rc=$rc)" 1; tail -20 "$W/drv.stdout"; }
echo "RESULT: $PASSN passed, $FAILN failed"
[[ $FAILN -eq 0 ]]
