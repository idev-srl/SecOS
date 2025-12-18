#!/usr/bin/env bash
# SecOS - Boot UEFI Diretto (Standalone)
# Script che avvia direttamente UEFI boot senza dipendenze da run_uefi.sh
# Usa QEMU 9.2 + macchina pc + immagine MBR+ESP

set -euo pipefail

QEMU=/home/luigi/qemu-local/bin/qemu-system-x86_64
OVMF_CODE=/usr/share/OVMF/OVMF_CODE.fd
DIST_DIR=dist
ESP_IMG="${DIST_DIR}/esp_raw.img"
BOOTX64="${DIST_DIR}/EFI/BOOT/BOOTX64.EFI"
KERNEL_ELF="${DIST_DIR}/kernel.elf"

echo "=== SecOS UEFI Boot (Direct) ==="
echo "QEMU: $($QEMU --version | head -1)"
echo "================================="

# Build bootloader
echo "[1/4] Building UEFI bootloader..."
make -s uefi || { echo "Build failed!"; exit 1; }

# Verifica files
if [ ! -f "$BOOTX64" ]; then
    echo "ERROR: $BOOTX64 not found!"
    exit 1
fi

if [ ! -f "$KERNEL_ELF" ]; then
    echo "WARN: $KERNEL_ELF not found (continuing anyway)"
fi

# Crea immagine MBR+ESP se non esiste
if [ ! -f "$ESP_IMG" ] || [ "${FORCE_REBUILD:-0}" -eq 1 ]; then
    echo "[2/4] Creating MBR+ESP image (256MB)..."
    
    # Crea immagine vuota
    dd if=/dev/zero of="$ESP_IMG" bs=1M count=256 2>/dev/null
    
    # Crea partizione EF (EFI System Partition)
    echo -e "n\np\n1\n\n\nt\nef\nw\n" | fdisk "$ESP_IMG" >/dev/null 2>&1
    
    # Formatta FAT32
    mformat -i "${ESP_IMG}@@1M" -F -v SECOS :: 2>/dev/null
    
    echo "[3/4] Populating ESP..."
    # Crea directories
    mmd -i "${ESP_IMG}@@1M" ::EFI ::EFI/BOOT 2>/dev/null || true
    
    # Copia bootloader
    mcopy -o -i "${ESP_IMG}@@1M" "$BOOTX64" ::EFI/BOOT/ || { echo "ERROR: mcopy BOOTX64 failed!"; exit 1; }
    
    # Copia kernel se presente
    if [ -f "$KERNEL_ELF" ]; then
        mcopy -o -i "${ESP_IMG}@@1M" "$KERNEL_ELF" :: || echo "WARN: kernel.elf copy failed"
    fi
    
    # Crea startup.nsh (UEFI boot script)
    printf 'FS0:\\EFI\\BOOT\\BOOTX64.EFI\r\n' > /tmp/startup.nsh
    mcopy -o -i "${ESP_IMG}@@1M" /tmp/startup.nsh ::startup.nsh 2>/dev/null || true
    
    echo "ESP image created: $ESP_IMG"
else
    echo "[2/4] Using existing ESP image: $ESP_IMG"
fi

# Crea copia OVMF_VARS locale
mkdir -p "$DIST_DIR"
VARS_COPY="${DIST_DIR}/OVMF_VARS.fd"
if [ -f "/usr/share/OVMF/OVMF_VARS.fd" ]; then
    cp -f /usr/share/OVMF/OVMF_VARS.fd "$VARS_COPY" 2>/dev/null || touch "$VARS_COPY"
else
    touch "$VARS_COPY"
fi

# Launch QEMU
echo "[4/4] Launching QEMU..."
echo "Press Ctrl+C to stop"
echo "Serial output in: ${DIST_DIR}/serial.log"
echo "================================="

"$QEMU" \
    -machine pc \
    -cpu qemu64 \
    -m 512M \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$VARS_COPY" \
    -drive id=disk,format=raw,file="$ESP_IMG",if=none \
    -device ahci,id=ahci \
    -device ide-hd,drive=disk,bus=ahci.0 \
    -serial file:"${DIST_DIR}/serial.log" \
    -display none \
    -no-reboot \
    "$@"

echo ""
echo "QEMU terminated"
echo "Check ${DIST_DIR}/serial.log for output"
