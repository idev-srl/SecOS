#!/usr/bin/env bash
# Test manuale boot UEFI con shell interattiva
# Istruzioni a schermo per l'utente

cat << 'EOF'
╔═══════════════════════════════════════════════════════════╗
║         SecOS UEFI Interactive Boot Test                  ║
╚═══════════════════════════════════════════════════════════╝

Questo script avvia QEMU con OVMF e la shell UEFI interattiva.

📋 ISTRUZIONI:
   1. Attendi che appaia la shell UEFI
   2. Digita questi comandi:
      
      FS0:
      cd EFI\BOOT
      BOOTX64.EFI

   3. Il bootloader partirà e vedrai l'output

🔑 Comandi utili nella shell UEFI:
   - map            Mostra dispositivi (cerca FS0:)
   - ls             Lista files
   - help           Aiuto comandi

⚠️  NOTA: QEMU è in modalità stdio, quindi:
    - L'output apparirà nel terminale
    - Ctrl+A X per uscire da QEMU
    - Oppure digita 'reset' nella shell UEFI

Premi INVIO per continuare...
EOF

read -r

cd /home/luigi/secos

QEMU=/home/luigi/qemu-local/bin/qemu-system-x86_64
ESP_IMG=dist/esp_mbr.img
OVMF_CODE=/usr/share/OVMF/OVMF_CODE.fd
OVMF_VARS=dist/OVMF_VARS.fd

# Verifica e prepara VARS
if [ ! -f "$OVMF_VARS" ]; then
    if [ -f /usr/share/OVMF/OVMF_VARS.fd ]; then
        cp /usr/share/OVMF/OVMF_VARS.fd "$OVMF_VARS"
    else
        touch "$OVMF_VARS"
    fi
fi

echo
echo "Avvio QEMU UEFI..."
echo "================================="
echo

"$QEMU" \
    -machine pc \
    -cpu qemu64 \
    -m 512M \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$OVMF_VARS" \
    -drive if=ide,format=raw,file="$ESP_IMG" \
    -serial mon:stdio \
    -no-reboot

echo
echo "QEMU terminato"
