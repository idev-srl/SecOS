#!/usr/bin/env bash
#
# mkuefidisk.sh — build a bootable UEFI GPT+ESP disk image for SECoS.
#
# Produces a raw disk image with a single EFI System Partition (FAT32) holding:
#   /EFI/BOOT/BOOTX64.EFI   (the SECoS UEFI loader)
#   /kernel.elf             (the kernel, read from the ESP root by the loader)
#
# Run `make uefi` first (it builds dist/EFI/BOOT/BOOTX64.EFI + dist/kernel.elf).
# No root required (uses mtools + sgdisk + dd on plain files).
#
# Usage:   tools/mkuefidisk.sh [output.img]
# Env:     ESP_MB=64   (size of the ESP in MiB)
#
# SPDX-License-Identifier: MIT
set -euo pipefail

ESP_MB="${ESP_MB:-64}"
OUT="${1:-secos-uefi.img}"
DIST="dist"
LOADER="$DIST/EFI/BOOT/BOOTX64.EFI"
KERNEL="$DIST/kernel.elf"

for f in "$LOADER" "$KERNEL"; do
    [ -f "$f" ] || { echo "[mkuefidisk] missing $f — run 'make uefi' first" >&2; exit 1; }
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
ESP="$TMP/esp.img"

# --- 1. FAT32 EFI System Partition image ---
dd if=/dev/zero of="$ESP" bs=1M count="$ESP_MB" status=none
mkfs.fat -F 32 -n SECOS "$ESP" >/dev/null
mmd   -i "$ESP" ::/EFI ::/EFI/BOOT
mcopy -i "$ESP" "$LOADER" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP" "$KERNEL" ::/kernel.elf

# --- 2. GPT disk: 1 MiB pre-gap (sector 2048) + ESP + GPT backup ---
DISK_MB=$((ESP_MB + 3))
dd if=/dev/zero of="$OUT" bs=1M count="$DISK_MB" status=none
sgdisk -Z "$OUT" >/dev/null 2>&1 || true
sgdisk -n 1:2048:+"${ESP_MB}"M -t 1:ef00 -c 1:"ESP" "$OUT" >/dev/null

# --- 3. Write the ESP filesystem into partition 1 (sector 2048 = 1 MiB) ---
dd if="$ESP" of="$OUT" bs=512 seek=2048 conv=notrunc status=none

echo "[mkuefidisk] OK: $OUT ($(ls -lh "$OUT" | awk '{print $5}')) — GPT + ESP(FAT32), BOOTX64.EFI + kernel.elf"
