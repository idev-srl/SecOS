#!/bin/bash
# tools/dma_scan.sh — [M55] static check of the DMA API rule (kernel/dma.h):
# no code outside kernel/dma.c turns a kernel pointer into a bus address.
#   1. kvirt_to_phys / virt_to_phys / vmm_translate / vmm_kvirt_to_phys_slow /
#      vmm_walk_translate calls: only in mm/ (the VMM/PMM themselves),
#      kernel/dma.c, and the allowlist below (each one is not a DMA address);
#   2. every driver that enables PCI bus mastering takes a DMA handle
#      (dma_dev / dma_dev_legacy);
#   3. no driver allocates frames straight from the PMM (DMA memory comes
#      from __dma buffers or dma_alloc_coherent).
# Comments are ignored. Prints "[dma-scan] PASS" or the offending lines and
# "[dma-scan] FAIL". SPDX-License-Identifier: MIT
set -u
cd "$(dirname "$0")/.." || exit 2
# file:reason — calls that are not DMA addresses.
ALLOW=(
    "kernel/smp.c:AP trampoline page (CPU real-mode code, not a device)"
    "kernel/kernel.c:debug print of a heap address (selftest log)"
)
SRC=$(git ls-files '*.c' '*.h' | grep -vE '^(edk2|edk2_bootloader|third_party|user|uefi|tools)/')
strip() { # strip comments (// and /* */, line-preserving) from a C file
    python3 -I - "$1" <<'EOF'
import re, sys
s = open(sys.argv[1], encoding='latin1').read()
def blank(m): return re.sub(r'[^\n]', ' ', m.group(0))
s = re.sub(r'/\*.*?\*/', blank, s, flags=re.S)
s = re.sub(r'//[^\n]*', '', s)
sys.stdout.write(s)
EOF
}
bad=0
for f in $SRC; do
    case "$f" in mm/*|kernel/dma.c|kernel/dma.h) continue ;; esac
    hits=$(strip "$f" | grep -nE '\b(kvirt_to_phys|virt_to_phys|vmm_translate|vmm_kvirt_to_phys_slow|vmm_walk_translate)[[:space:]]*\(')
    [ -n "$hits" ] || continue
    ok=0
    for a in "${ALLOW[@]}"; do [ "${a%%:*}" = "$f" ] && { ok=1; echo "[dma-scan] allowed $f (${a#*:}):"; }; done
    while IFS= read -r l; do echo "    $f:$l"; done <<<"$hits"
    [ $ok = 1 ] || { echo "[dma-scan] FAIL: $f turns a kernel pointer into a physical address outside kernel/dma.c"; bad=1; }
done
for f in $(git ls-files 'drivers/*.c' 'drivers/**/*.c' 'net/*.c'); do
    body=$(strip "$f")
    if grep -qE '^[[:space:]]+pci_enable_(mem|io)_and_busmaster[[:space:]]*\(' <<<"$body" &&
       ! grep -qE '\bdma_dev(_legacy)?[[:space:]]*\(' <<<"$body"; then
        echo "[dma-scan] FAIL: $f enables bus mastering without a DMA handle (dma_dev)"; bad=1
    fi
    if grep -nE '\bpmm_alloc[a-z_]*[[:space:]]*\(' <<<"$body" >/dev/null; then
        echo "[dma-scan] FAIL: $f allocates PMM frames directly (use dma_alloc_coherent)"; bad=1
    fi
done
n=$(git ls-files 'drivers/*.c' 'drivers/**/*.c' | xargs grep -lE '\bdma_static[[:space:]]*\(' | wc -l)
echo "[dma-scan] $n driver files take bus addresses from dma_static()"
if [ $bad = 0 ]; then echo "[dma-scan] PASS"; else echo "[dma-scan] FAIL"; fi
exit $bad
