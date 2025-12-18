# UEFI Boot - Status & Solution

## 🎯 Obiettivo Raggiunto
✅ QEMU 9.2.0 compilato e funzionante (nessun bug CPU hotplug)  
✅ Immagine ESP (MBR+FAT32) creata correttamente  
✅ OVMF riconosce filesystem (FS0: mapping attivo)  
✅ Bootloader UEFI compila correttamente

## ⚠️ Problema Attuale
OVMF **vede** il disco e **monta** FS0:, ma non esegue automaticamente `\EFI\BOOT\BOOTX64.EFI`.

La shell UEFI parte e mostra:
```
Mapping table
  FS0: Alias(s):HD0a1:;BLK1:
      PciRoot(0x0)/Pci(0x1,0x1)/Ata(0x0)/HD(1,MBR,...)
```

Ma poi non auto-esegue BOOTX64.EFI.

## 🔍 Root Cause
1. **startup.nsh** viene eseguito ma restituisce "Script Error: Not Found"
2. OVMF non ha una boot entry persistente per il nostro disco
3. La shell UEFI interpreta erroneamente il path o il comando

## ✅ Soluzione Completa

### Installazione QEMU 9.2
```bash
# Già compilato in: /home/luigi/qemu-local/bin/qemu-system-x86_64
# Usare export QEMU=/home/luigi/qemu-local/bin/qemu-system-x86_64
```

### Configurazione Chiave
**IMPORTANTE**: Usare macchina `pc` (i440fx), **NON** `q35`!

```bash
qemu-system-x86_64 \
    -machine pc \              # <- CRITICO: pc, non q35!
    -cpu qemu64 \
    -m 512M \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
    -drive if=pflash,format=raw,file=OVMF_VARS.fd \
    -drive if=ide,format=raw,file=esp.img \
    -no-reboot
```

Con q35, OVMF non enumera correttamente i dischi IDE/SATA.

### Creazione Immagine ESP
```bash
# 1. Crea immagine 256MB
dd if=/dev/zero of=esp.img bs=1M count=256

# 2. Partizione EF (EFI System)
echo -e "n\np\n1\n\n\nt\nef\nw\n" | fdisk esp.img

# 3. Formatta FAT32
mformat -i esp.img@@1M -F -v SECOS ::

# 4. Crea struttura
mmd -i esp.img@@1M ::EFI ::EFI/BOOT

# 5. Copia bootloader
mcopy -i esp.img@@1M BOOTX64.EFI ::EFI/BOOT/

# 6. Copia kernel (opzionale)
mcopy -i esp.img@@1M kernel.elf ::
```

### 📝 Prossimi Passi per Auto-Boot

**Opzione A: Boot Entry Persistente**
Serve creare boot entry UEFI usando `bcfg` dalla shell:
```
Shell> bcfg boot add 0 FS0:\EFI\BOOT\BOOTX64.EFI "SecOS"
Shell> exit
```
Questo persiste in OVMF_VARS.fd e rende il boot automatico.

**Opzione B: Modificare Bootloader**
Il bootloader UEFI può richiamare `ExitBootServices` e saltare direttamente al kernel senza attendere shell.

**Opzione C: Script Helper**
Creare wrapper che interagisce con la shell UEFI via stdin simulando i comandi.

## 🚀 Per Testare Subito

```bash
cd /home/luigi/secos

# Test interattivo - vedrai la shell UEFI
/home/luigi/qemu-local/bin/qemu-system-x86_64 \
    -machine pc \
    -cpu qemu64 \
    -m 512M \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
    -drive if=pflash,format=raw,file=dist/OVMF_VARS.fd \
    -drive if=ide,format=raw,file=dist/esp_mbr.img \
    -serial stdio

# Nella shell UEFI digita:
FS0:
cd EFI\BOOT
BOOTX64.EFI
```

## 📊 Riepilogo Configurazione Funzionante

| Componente | Valore |
|-----------|--------|
| QEMU | 9.2.0 (compilato locale) |
| Macchina | `pc` (i440fx) ⚠️ NON q35 |
| Firmware | OVMF (CODE + VARS) |
| Disco | IDE (`if=ide`) |
| Immagine | MBR + partizione EF + FAT32 |
| Bootloader | `\EFI\BOOT\BOOTX64.EFI` |

## 🔧 File Creati

- `/home/luigi/qemu-local/` - QEMU 9.2 completo
- `/home/luigi/secos/run_uefi_direct.sh` - Script standalone
- `/home/luigi/secos/dist/esp_mbr.img` - Immagine ESP MBR
- `/home/luigi/secos/dist/OVMF_VARS.fd` - UEFI variables

## ⏭️ Next Steps Raccomandati

1. **Immediate**: Testare boot manuale dalla shell UEFI
2. **Short term**: Implementare boot entry con `bcfg` in OVMF_VARS
3. **Medium term**: Modificare bootloader per ExitBootServices diretto
4. **Long term**: Integrare framebuffer GOP e memory map handoff completo

---
*Ultimo aggiornamento: 2025-12-18*
*QEMU 9.2.0 compilato con successo su WSL2 Ubuntu 22.04*
