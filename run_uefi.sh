#!/usr/bin/env bash
# SecOS - Script di esecuzione UEFI sotto QEMU + OVMF
# Consente di avviare rapidamente il bootloader UEFI (BOOTX64.EFI) e il kernel ELF.
# Variabili d'ambiente opzionali:
#   OVMF_CODE  percorso a OVMF_CODE.fd (default: /usr/share/OVMF/OVMF_CODE.fd)
#   OVMF_VARS  percorso a OVMF_VARS.fd (default: /usr/share/OVMF/OVMF_VARS.fd)
#   QEMU       comando qemu-system-x86_64 (default: qemu-system-x86_64)
#   EXTRA_QEMU argomenti aggiuntivi passati a QEMU
#
# Uso:
#   ./run_uefi.sh            # build automatica + avvio
#   OVMF_CODE=... ./run_uefi.sh
#
set -euo pipefail

QEMU=${QEMU:-qemu-system-x86_64}
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS.fd}
DIST_DIR="dist"
EFI_BOOT="${DIST_DIR}/EFI/BOOT/BOOTX64.EFI"
EFI_BOOT_OVERRIDE=${EFI_BOOT_OVERRIDE:-""} # Se non vuoto, copia questo file su dist/EFI/BOOT/BOOTX64.EFI dopo la build
KERNEL_ELF="${DIST_DIR}/kernel.elf"
DISK_IMG="${DIST_DIR}/esp_fat.img"
GPT_IMG="${DIST_DIR}/esp_gpt.img"
USE_VIRTIO=${USE_VIRTIO:-1}
FORCE_REBUILD=${FORCE_REBUILD:-0}
USE_DIR_FAT=${USE_DIR_FAT:-0}   # Se 1 usa -drive file=fat:rw:dist (QEMU internal FAT) invece di immagine
USE_BIOS_MONO=${USE_BIOS_MONO:-0} # Se 1 prova -bios OVMF.fd monolitico e macchina pc
USE_MINI_FAT=${USE_MINI_FAT:-0}   # Se 1 usa builder Python minimale invece di mtools/mkfs
USE_USB=${USE_USB:-0}             # Se 1 presenta il disco come dispositivo USB rimovibile
USE_PY_GPT=${USE_PY_GPT:-0}       # Se 1 usa builder Python GPT FAT invece di sgdisk (no sudo)
USE_PYFAT=${USE_PYFAT:-0}         # Se 1 crea immagine FAT con pyfatfs (richiede pip install pyfatfs)
USE_GPT_ESP_BUILDER=${USE_GPT_ESP_BUILDER:-0} # Se 1 usa nuovo build_gpt_esp.py (GPT + FAT32 conforme, no sudo)
IMAGE_MB=${IMAGE_MB:-384}          # Dimensione totale immagine quando si usa builder GPT ESP
PARTITION_MB=${PARTITION_MB:-320}  # Dimensione partizione ESP quando si usa builder GPT ESP (>=320 per cluster 4KiB)
NO_VARS=${NO_VARS:-0}              # Se 1 non attacca OVMF_VARS per forzare stato factory
USE_AHCI=${USE_AHCI:-0}            # Se 1 forza controller SATA AHCI esplicito (raccomandato per alcune build OVMF)
USE_PC_MACHINE=${USE_PC_MACHINE:-0} # Se 1 usa -machine pc invece di q35 (diagnostica)
USE_CPU_HOST=${USE_CPU_HOST:-0}     # Se 1 usa -cpu host (più estensioni, talvolta influenza driver)
CAPTURE_LOG=${CAPTURE_LOG:-0}       # Se 1 cattura stdout/stderr QEMU in dist/qemu_run.log
USE_NVME=${USE_NVME:-0}             # Se 1 presenta immagine come disco NVMe (driver spesso incluso in OVMF)
OVMF_CPU_BUGCHECK_OVERRIDE=${OVMF_CPU_BUGCHECK_OVERRIDE:-0} # Se 1 fornisce override bugcheck CPU hotplug (-fw_cfg)
USB_REMOVABLE=${USB_REMOVABLE:-1}   # Se 1 imposta usb-storage come removibile (trigger fallback \\EFI\\BOOT\\BOOTX64.EFI)
FAT_IMG_MB=${FAT_IMG_MB:-64}        # Dimensione immagine FAT superfloppy (MB). 64MB evita edge-case FAT32 con cluster 512B.

# Colori
C_RESET="\033[0m"; C_YEL="\033[33m"; C_RED="\033[31m"; C_GRN="\033[32m"; C_CYN="\033[36m"

info(){ echo -e "${C_CYN}[INFO]${C_RESET} $*"; }
warn(){ echo -e "${C_YEL}[WARN]${C_RESET} $*"; }
err(){ echo -e "${C_RED}[ERR ]${C_RESET} $*"; }
ok(){ echo -e "${C_GRN}[ OK ]${C_RESET} $*"; }

if ! command -v "$QEMU" >/dev/null 2>&1; then
  err "QEMU non trovato nel PATH (${QEMU})"; exit 1
fi

# Verifica OVMF
if [ ! -f "$OVMF_CODE" ]; then
  warn "OVMF_CODE non trovato in $OVMF_CODE (imposta OVMF_CODE=/percorso/OVMF_CODE.fd)"
fi
if [ ! -f "$OVMF_VARS" ]; then
  warn "OVMF_VARS non trovato in $OVMF_VARS (imposta OVMF_VARS=/percorso/OVMF_VARS.fd)"
else
  # Verifica permessi di lettura
  if [ ! -r "$OVMF_VARS" ]; then
    warn "OVMF_VARS esiste ma non leggibile: $OVMF_VARS"
  fi
fi

# Gestione permesso negato: se non possiamo aprire OVMF_VARS per scrittura/lettura, creiamo copia locale
TEMP_VARS="${DIST_DIR}/OVMF_VARS.copy.fd"
USE_VARS="$OVMF_VARS"
if [ -f "$OVMF_VARS" ]; then
  if ! cp "$OVMF_VARS" "$TEMP_VARS" 2>/dev/null; then
    warn "Impossibile copiare OVMF_VARS (permesso negato). Provo ad usare solo OVMF_CODE (modalità stateless)."
    USE_VARS="" # useremo solo il CODE; QEMU senza VARS persisterà meno stato
  else
    USE_VARS="$TEMP_VARS"
  fi
else
  USE_VARS="" # assenza file vars
fi

# Build
info "Eseguo build UEFI (make uefi)"
make -s uefi

if [ -n "$EFI_BOOT_OVERRIDE" ]; then
  if [ ! -f "$EFI_BOOT_OVERRIDE" ]; then
    err "EFI_BOOT_OVERRIDE non esiste: $EFI_BOOT_OVERRIDE"; exit 1
  fi
  info "Sostituisco BOOTX64.EFI con override: $EFI_BOOT_OVERRIDE"
  cp -f "$EFI_BOOT_OVERRIDE" "$EFI_BOOT"
fi

if [ ! -f "$EFI_BOOT" ]; then
  err "File $EFI_BOOT mancante dopo la build"; exit 1
fi
if [ ! -f "$KERNEL_ELF" ]; then
  warn "kernel.elf non trovato (è possibile che la copia sia saltata)"
fi

info "Avvio QEMU con OVMF"

# Semplificato: crea immagine FAT32 "superfloppy" (nessuna GPT) se non esiste
create_fat_image() {
  if [ -f "$DISK_IMG" ]; then
    ok "Immagine FAT già presente: $DISK_IMG"
    # Aggiorna comunque startup.nsh se mancante
    add_startup_nsh
    return
  fi
  info "Creo immagine FAT32 superfloppy"
  truncate -s "${FAT_IMG_MB}M" "$DISK_IMG"
  if command -v mcopy >/dev/null 2>&1; then
    # Usa mtools: mformat per inizializzare l'immagine FAT
    if command -v mformat >/dev/null 2>&1; then
      # Nota: mformat richiede -i per lavorare su un'immagine file.
      if ! mformat -i "$DISK_IMG" -F -v SECOS :: >/dev/null 2>&1; then
        warn "mformat fallito, uso mkfs.vfat"
        # Forza cluster 4KiB (8 settori) per compatibilità/robustezza con alcuni tool.
        mkfs.vfat -F32 -s 8 -n SECOS "$DISK_IMG" >/dev/null 2>&1 || { err "mkfs.vfat fallita"; rm -f "$DISK_IMG"; return 1; }
      fi
    else
      mkfs.vfat -F32 -s 8 -n SECOS "$DISK_IMG" >/dev/null 2>&1 || { err "mkfs.vfat fallita"; rm -f "$DISK_IMG"; return 1; }
    fi
    mmd -i "$DISK_IMG" ::EFI >/dev/null 2>&1 || true
    mmd -i "$DISK_IMG" ::EFI/BOOT >/dev/null 2>&1 || true
    mcopy -i "$DISK_IMG" -o "$EFI_BOOT" ::EFI/BOOT/ || { err "mcopy BOOTX64.EFI fallita"; rm -f "$DISK_IMG"; return 1; }
    [ -f "$KERNEL_ELF" ] && mcopy -i "$DISK_IMG" -o "$KERNEL_ELF" :: || { err "mcopy kernel.elf fallita"; rm -f "$DISK_IMG"; return 1; }
    # Fallback: copia BOOTX64.EFI anche in root
    mcopy -i "$DISK_IMG" -o "$EFI_BOOT" ::BOOTX64.EFI >/dev/null 2>&1 || true
    add_startup_nsh
    # Sanity check: se mtools non riesce a leggere la FAT, meglio fallire qui.
    if command -v minfo >/dev/null 2>&1; then
      minfo -i "$DISK_IMG" :: >/dev/null 2>&1 || { err "Immagine FAT creata ma non leggibile da mtools"; rm -f "$DISK_IMG"; return 1; }
    fi
  else
    mkfs.vfat -F32 -s 8 -n SECOS "$DISK_IMG" >/dev/null 2>&1 || { err "mkfs.vfat fallita"; rm -f "$DISK_IMG"; return 1; }
    if command -v sudo >/dev/null 2>&1; then
      TMPMNT=$(mktemp -d)
      sudo mount -o loop "$DISK_IMG" "$TMPMNT" || { err "Mount loop fallito"; sudo rm -rf "$TMPMNT"; return 1; }
      sudo mkdir -p "$TMPMNT/EFI/BOOT"
      sudo cp "$EFI_BOOT" "$TMPMNT/EFI/BOOT/" || true
      [ -f "$KERNEL_ELF" ] && sudo cp "$KERNEL_ELF" "$TMPMNT/" || true
      # Fallback root-level BOOTX64.EFI
      sudo cp "$EFI_BOOT" "$TMPMNT/BOOTX64.EFI" || true
      printf 'fs0:\nEFI\\BOOT\\BOOTX64.EFI\n\\BOOTX64.EFI\n' | sudo tee "$TMPMNT/startup.nsh" >/dev/null
      sync
      sudo umount "$TMPMNT"
      sudo rm -rf "$TMPMNT"
    else
      err "mtools mancante e niente sudo: impossibile popolare immagine"; return 1;
    fi
  fi
  ok "Immagine FAT creata e popolata"
}

# Crea o aggiorna startup.nsh nell'immagine FAT (richiede mtools oppure sudo montabile)
add_startup_nsh() {
  # Usa mtools se disponibile
  if command -v mcopy >/dev/null 2>&1; then
    TMP_START=$(mktemp)
    printf 'EFI\\BOOT\\BOOTX64.EFI\n\\BOOTX64.EFI\n' > "$TMP_START"
    # Copia sempre (sovrascrive) nella root
    mcopy -i "$DISK_IMG" -o "$TMP_START" ::startup.nsh 2>/dev/null && ok "startup.nsh aggiornato" || warn "Impossibile scrivere startup.nsh (mcopy)"
    rm -f "$TMP_START"
  else
    # Necessario mount loop con sudo
    if command -v sudo >/dev/null 2>&1; then
      TMPMNT=$(mktemp -d)
      if sudo mount -o loop "$DISK_IMG" "$TMPMNT" 2>/dev/null; then
        printf 'EFI\\BOOT\\BOOTX64.EFI\n\\BOOTX64.EFI\n' | sudo tee "$TMPMNT/startup.nsh" >/dev/null && ok "startup.nsh aggiornato (mount)" || warn "Fallita scrittura startup.nsh"
        sync
        sudo umount "$TMPMNT"
      else
        warn "Mount loop fallito, nessun startup.nsh"
      fi
      rm -rf "$TMPMNT"
    else
      warn "Nessun mtools e nessun sudo: skip startup.nsh"
    fi
  fi
}

create_gpt_image() {
  # Richiede sudo + sgdisk + partprobe + mkfs.vfat
  if [ -f "$GPT_IMG" ]; then
    ok "Immagine GPT già presente: $GPT_IMG"
    return
  fi
  if [ -z "${USE_GPT:-}" ]; then
    return
  fi
  if ! command -v sgdisk >/dev/null 2>&1; then
    warn "sgdisk assente, skip GPT (usa superfloppy)"
    return
  fi
  if ! command -v sudo >/dev/null 2>&1; then
    warn "sudo assente, non posso creare GPT, fallback superfloppy"
    return
  fi
  if ! sudo -n true 2>/dev/null; then
    warn "sudo richiede password: esegui manualmente oppure concedi sudo senza password. Fallback superfloppy."
    return
  fi
  info "Creo immagine GPT EFI (${GPT_IMG})"
  truncate -s 64M "$GPT_IMG" || { err "truncate fallito"; return; }
  sgdisk --zap-all "$GPT_IMG" >/dev/null 2>&1 || { err "sgdisk zap fallito"; rm -f "$GPT_IMG"; return; }
  sgdisk -n1:2048:0 -t1:EF00 -c1:"SECOS EFI" "$GPT_IMG" >/dev/null 2>&1 || { err "Creazione partizione EFI fallita"; rm -f "$GPT_IMG"; return; }
  LOOP=$(sudo losetup -f --show "$GPT_IMG") || { err "losetup fallito"; rm -f "$GPT_IMG"; return; }
  sudo partprobe "$LOOP" || warn "partprobe warning"
  # Attendi comparsa partizione
  for i in $(seq 1 10); do
    [ -b "${LOOP}p1" ] && break || sleep 0.2
  done
  if [ ! -b "${LOOP}p1" ]; then
    err "Partizione non visibile (${LOOP}p1)"; sudo losetup -d "$LOOP"; rm -f "$GPT_IMG"; return
  fi
  sudo mkfs.vfat -F32 -n SECOS "${LOOP}p1" >/dev/null 2>&1 || { err "mkfs.vfat fallito"; sudo losetup -d "$LOOP"; rm -f "$GPT_IMG"; return; }
  MNT=$(mktemp -d)
  sudo mount "${LOOP}p1" "$MNT" || { err "mount fallito"; sudo losetup -d "$LOOP"; rm -rf "$MNT"; rm -f "$GPT_IMG"; return; }
  sudo mkdir -p "$MNT/EFI/BOOT"
  sudo cp "$EFI_BOOT" "$MNT/EFI/BOOT/" || warn "Copia BOOTX64.EFI fallita"
  [ -f "$KERNEL_ELF" ] && sudo cp "$KERNEL_ELF" "$MNT/" || true
  # Fallback root-level BOOTX64.EFI
  sudo cp "$EFI_BOOT" "$MNT/BOOTX64.EFI" || warn "Copia root BOOTX64.EFI fallita"
  printf 'EFI\\BOOT\\BOOTX64.EFI\n\\BOOTX64.EFI\n' | sudo tee "$MNT/startup.nsh" >/dev/null || warn "startup.nsh non scritto"
  sync
  sudo umount "$MNT"
  sudo losetup -d "$LOOP"
  rm -rf "$MNT"
  ok "Immagine GPT EFI creata e popolata"
}

# Decide quale immagine usare
if [ "$USE_DIR_FAT" -eq 1 ]; then
  info "Uso directory dist come filesystem FAT (fat:rw:dist)"
  ACTIVE_IMG="DIRFAT"
elif [ "${USE_GPT:-0}" -eq 1 ]; then
  # Usa GPT
  if [ "$FORCE_REBUILD" -eq 1 ]; then
    rm -f "$GPT_IMG" || true
  fi
  if [ "$USE_PY_GPT" -eq 1 ]; then
    info "Creo immagine GPT con builder Python (senza sudo)"
    python3 tools/mkgptfat.py "$EFI_BOOT" "$GPT_IMG" "$KERNEL_ELF" || { err "mkgptfat.py fallito"; exit 1; }
  elif [ "$USE_GPT_ESP_BUILDER" -eq 1 ]; then
    info "Creo immagine GPT ESP con build_gpt_esp.py (senza sudo)"
    python3 tools/build_gpt_esp.py --out "$GPT_IMG" --bootx64 "$EFI_BOOT" --kernel "$KERNEL_ELF" --image-mb "$IMAGE_MB" --partition-mb "$PARTITION_MB" --verify || { err "build_gpt_esp.py fallito"; exit 1; }
  else
    create_gpt_image
  fi
  if [ -f "$GPT_IMG" ]; then
    ACTIVE_IMG="$GPT_IMG"
  else
    warn "Immagine GPT non creata, fallback FAT"
    if [ "$FORCE_REBUILD" -eq 1 ]; then rm -f "$DISK_IMG" || true; fi
    create_fat_image
    ACTIVE_IMG="$DISK_IMG"
  fi
else
  # Forza rimozione GPT se presente per evitare uso accidentale
  if [ -f "$GPT_IMG" ] && [ "$FORCE_REBUILD" -eq 1 ]; then rm -f "$GPT_IMG" || true; fi
  if [ "$FORCE_REBUILD" -eq 1 ]; then rm -f "$DISK_IMG" || true; fi
  if [ "$USE_PYFAT" -eq 1 ]; then
    info "Creo immagine FAT con pyfatfs"
    FAT_MB=${FAT_MB:-32} python3 tools/build_fat_pyfat.py "$EFI_BOOT" "$DISK_IMG" "$KERNEL_ELF" || { err "build_fat_pyfat fallito"; exit 1; }
    ACTIVE_IMG="$DISK_IMG"
  elif [ "$USE_MINI_FAT" -eq 1 ]; then
    info "Creo immagine FAT con builder minimale Python"
    python3 tools/mkfatimg.py "$EFI_BOOT" "$DISK_IMG" "$KERNEL_ELF" || { err "mkfatimg.py fallito"; exit 1; }
    ACTIVE_IMG="$DISK_IMG"
  else
    create_fat_image
    ACTIVE_IMG="$DISK_IMG"
  fi
fi

if [ "$USE_BIOS_MONO" -eq 1 ]; then
  # Tentativo con -bios monolitico e macchina pc (più tradizionale)
  BIOS_CAND="/usr/share/OVMF/OVMF.fd"
  [ -f "$BIOS_CAND" ] || BIOS_CAND="/usr/share/edk2/ovmf/OVMF.fd"
  if [ -f "$BIOS_CAND" ]; then
    info "Avvio in modalità BIOS monolitico: $BIOS_CAND"
    QEMU_ARGS=(
      -machine pc,accel=tcg \
      -cpu qemu64 \
      -m 512M \
      -bios "$BIOS_CAND" \
      -serial stdio \
      -debugcon file:uefi_debug.log -global isa-debugcon.iobase=0x402 \
      -no-reboot
    )
  else
    warn "OVMF.fd monolitico non trovato, uso configurazione pflash"
    USE_BIOS_MONO=0
  fi
fi

if [ "$USE_BIOS_MONO" -eq 0 ]; then
  if [ "$USE_PC_MACHINE" -eq 1 ]; then
    MACHINE_TYPE="pc"
  else
    MACHINE_TYPE="q35"
  fi
  CPU_OPT="qemu64"
  if [ "$USE_CPU_HOST" -eq 1 ]; then
    if [ -e /dev/kvm ]; then
      CPU_OPT="host"
    else
      warn "USE_CPU_HOST=1 ma /dev/kvm assente: fallback a qemu64 (serve KVM per -cpu host)"
    fi
  fi
  QEMU_ARGS=(
    -machine ${MACHINE_TYPE},accel=tcg \
    -cpu ${CPU_OPT} \
    -m 512M \
    -drive if=pflash,format=raw,readonly=on,file="${OVMF_CODE}" \
    -serial stdio \
    -debugcon file:uefi_debug.log -global isa-debugcon.iobase=0x402 \
    -no-reboot
  )
fi

if [ "$USE_USB" -eq 1 ]; then
  # Inserisci controller USB subito (prima di aggiungere il device storage)
  QEMU_ARGS+=( -device qemu-xhci )
fi

if [ "$ACTIVE_IMG" = "DIRFAT" ]; then
  # Directory export: QEMU internal FAT driver; considerato rimovibile
  QEMU_ARGS+=( -drive file=fat:rw:"${DIST_DIR}",format=raw )
else
  IMG_PATH="$ACTIVE_IMG"
  # Strategia di attacco del disco in ordine di priorità diagnostica:
  # 1. AHCI esplicito (spesso più affidabile con OVMF build ridotte senza virtio)
  # 2. Virtio block
  # 3. IDE implicito
  # 4. USB (se richiesto) sovrascrive controller di trasporto
  if [ "$USE_USB" -eq 1 ]; then
    QEMU_ARGS+=( -drive if=none,id=esp,file="${IMG_PATH}",format=raw )
    if [ "$USB_REMOVABLE" -eq 1 ]; then
      QEMU_ARGS+=( -device usb-storage,drive=esp,removable=true,bootindex=1 )
    else
      QEMU_ARGS+=( -device usb-storage,drive=esp,bootindex=1 )
    fi
  elif [ "$USE_NVME" -eq 1 ]; then
    # NVMe: spesso enumerato in modo affidabile
    QEMU_ARGS+=( -drive if=none,id=esp,file="${IMG_PATH}",format=raw )
    QEMU_ARGS+=( -device nvme,drive=esp,serial=SECOSNVME )
  elif [ "$USE_AHCI" -eq 1 ]; then
    # Controller SATA AHCI + disco come dispositivo IDE su bus SATA
    QEMU_ARGS+=( -device ich9-ahci,id=ahci )
    QEMU_ARGS+=( -drive if=none,id=esp,file="${IMG_PATH}",format=raw )
    QEMU_ARGS+=( -device ide-hd,drive=esp,bus=ahci.0 )
  elif [ "$USE_VIRTIO" -eq 1 ]; then
    QEMU_ARGS+=( -drive if=none,id=esp,file="${IMG_PATH}",format=raw )
    QEMU_ARGS+=( -device virtio-blk-pci,drive=esp )
  else
    QEMU_ARGS+=( -drive if=ide,format=raw,media=disk,file="${IMG_PATH}" )
  fi
fi

# Verifica contenuto ESP (monta e lista) se richiesto VERBOSE=1
if [ -n "${VERBOSE:-}" ]; then
  info "Verifica immagine ESP ($ACTIVE_IMG)"
  if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
    TMPMNT=$(mktemp -d)
    if sudo mount -o loop "$ACTIVE_IMG" "$TMPMNT" 2>/dev/null; then
      echo "--- ESP root ---"; ls -l "$TMPMNT"; echo "--- EFI/BOOT ---"; ls -l "$TMPMNT/EFI/BOOT" || true
      sudo umount "$TMPMNT"; rm -rf "$TMPMNT"
    else
      warn "Mount ESP fallito (verifica)"
    fi
  else
    warn "sudo non disponibile per verifica ESP"
  fi
fi

if [ "$NO_VARS" -eq 1 ]; then
  warn "Avvio senza variable store (NO_VARS=1)"
else
  if [ -n "$USE_VARS" ]; then
    QEMU_ARGS+=( -drive if=pflash,format=raw,file="${USE_VARS}" )
  else
    warn "Avvio senza OVMF_VARS (nessuna persistenza variabili UEFI)"
  fi
fi

# Echo configurazione disco per diagnosi rapida
info "Controller selezionato: $(
  if [ "$USE_USB" -eq 1 ]; then echo USB; 
  elif [ "$USE_NVME" -eq 1 ]; then echo NVME; 
  elif [ "$USE_AHCI" -eq 1 ]; then echo AHCI; 
  elif [ "$USE_VIRTIO" -eq 1 ]; then echo VIRTIO; 
  else echo IDE; fi) | Machine: $( [ "$USE_BIOS_MONO" -eq 1 ] && echo BIOS_MONO || echo ${MACHINE_TYPE})"

if [ -n "${EXTRA_QEMU:-}" ]; then
  # shellcheck disable=SC2206
  QEMU_ARGS+=( ${EXTRA_QEMU} )
fi

if [ "$OVMF_CPU_BUGCHECK_OVERRIDE" -eq 1 ]; then
  warn "Override bugcheck CPU hotplug attivato (rischio corruzione)"
  QEMU_ARGS+=( -fw_cfg name=opt/org.tianocore/X-Cpuhp-Bugcheck-Override,string=yes )
fi

set -x
if [ "$CAPTURE_LOG" -eq 1 ]; then
  LOG_FILE="${DIST_DIR}/qemu_run.log"
  mkdir -p "${DIST_DIR}" || true
  "$QEMU" "${QEMU_ARGS[@]}" >"${LOG_FILE}" 2>&1 || true
  ok "Log QEMU salvato in $LOG_FILE"
else
  "$QEMU" "${QEMU_ARGS[@]}" || true
fi
set +x

info "QEMU terminato"

# Diagnostica: dump BPB (primi 512 byte) se immagine non è DIRFAT
if [ "$ACTIVE_IMG" != "DIRFAT" ] && [ -f "$ACTIVE_IMG" ]; then
  info "Dump BPB di $ACTIVE_IMG"
  dd if="$ACTIVE_IMG" bs=512 count=1 2>/dev/null | hexdump -C | head -n 20 || true
fi
if [ -f uefi_debug.log ]; then
  info "Prime 120 linee di uefi_debug.log"
  head -n 120 uefi_debug.log || true
  # Avviso su versione QEMU se vecchia (<8) per bug CPU hotplug
  QEMU_VER_STR="$($QEMU --version 2>/dev/null | head -n1 || echo unknown)"
  info "Versione QEMU: $QEMU_VER_STR"
  if echo "$QEMU_VER_STR" | grep -qE '([0-7]\.)'; then
    warn "Versione QEMU <8: possibile bug CPU hotplug (consigliato aggiornare). Usa OVMF_CPU_BUGCHECK_OVERRIDE=1 per bypass temporaneo."
  fi
fi

# Dump del boot sector della partizione EFI (LBA 2048) se abbiamo usato immagine GPT
if [ "$ACTIVE_IMG" = "$GPT_IMG" ] && [ -f "$GPT_IMG" ]; then
  info "Dump boot sector partizione EFI (LBA 2048)"
  # Calcolo offset in byte: 2048 * 512 = 1048576
  dd if="$GPT_IMG" bs=512 skip=2048 count=1 2>/dev/null | hexdump -C | head -n 40 || true
fi
