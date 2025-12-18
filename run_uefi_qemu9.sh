#!/usr/bin/env bash
# SecOS - Script UEFI con QEMU 9.2 (locale)
# Wrapper che usa la build locale di QEMU 9.2 compilato
# Usa macchina pc (i440fx) per compatibilità OVMF + immagine MBR+ESP

set -euo pipefail

# Override QEMU per usare la build locale
export QEMU=/home/luigi/qemu-local/bin/qemu-system-x86_64

# Verifica che esista
if [ ! -x "$QEMU" ]; then
    echo "ERRORE: QEMU 9.2 non trovato in $QEMU"
    echo "Esegui prima la compilazione di QEMU 9.2"
    exit 1
fi

echo "=== Usando QEMU 9.2 locale ==="
$QEMU --version
echo "================================"
echo

# Non serve più l'override CPU bugcheck con QEMU 9.2!
export OVMF_CPU_BUGCHECK_OVERRIDE=0

# Usa macchina pc (i440fx) invece di q35 per migliore compatibilità OVMF
export USE_PC_MACHINE=1

# Forza IDE standard (funziona bene con pc machine)
export USE_VIRTIO=0
export USE_AHCI=0

# Non usare GPT per ora - usiamo MBR classico che OVMF gestisce meglio
export USE_GPT=0

# Cattura log
export CAPTURE_LOG=1

# Usa l'immagine MBR che abbiamo creato
export DISK_IMG="dist/esp_mbr.img"

# Esegui lo script principale
exec ./run_uefi.sh "$@"
