# Recap UEFI (Strategia B) — SecOS

## Obiettivo
Avviare SecOS in ambiente UEFI (OVMF/QEMU) usando un bootloader UEFI esterno ("Strategia B") che carica `kernel.elf` (ELF64) dal filesystem dell’ESP e passa il controllo al kernel.

In pratica:
- Firmware UEFI (OVMF) → auto-carica `\EFI\BOOT\BOOTX64.EFI`
- `BOOTX64.EFI` → carica `kernel.elf` dal disco (ESP FAT32) → setup paging/handoff → jump al kernel

---

## Cosa abbiamo implementato
### 1) Bootloader UEFI
- Bootloader UEFI in `uefi/` (applicazione EFI x86_64).
- Loader ELF64:
  - parsing dei segmenti PT_LOAD
  - controlli di validità (allineamenti, range, overlap, ecc.)
- Setup memoria/paging:
  - identity-map (2 MiB pages) per una finestra iniziale
  - abilitazione NXE (EFER.NXE) e note su W^X (pianificazione, non completata)
- Handoff:
  - raccolta memory map UEFI
  - info framebuffer (GOP) quando disponibile
  - `ExitBootServices` e salto al kernel

### 2) Build & target
- Target Makefile `uefi` per produrre `dist/EFI/BOOT/BOOTX64.EFI`.
- Target `uefi_hello` per isolare problemi (app EFI minimale che stampa e aspetta input).

### 3) Creazione immagini ESP
Sono state provate più modalità per creare e popolare la ESP:
- “superfloppy” FAT32 (immagine FAT senza GPT)
- GPT + ESP FAT32 (con strumenti di sistema: `sgdisk` + `mkfs.vfat` + mount loop)
- GPT + ESP FAT32 con builder Python (custom) + script di ispezione FAT/GPT
- Fallback: copia di `BOOTX64.EFI` anche in root e `startup.nsh`

Script principale di esecuzione:
- `run_uefi.sh` con molte combinazioni (virtio/ide/usb, GPT/superfloppy, no-vars, ecc.)

Strumenti di diagnostica:
- dump BPB/bootsector
- script di enumerazione FAT/GPT (verifica directory `EFI/BOOT` e file)

---

## Problema incontrato
### Sintomo
OVMF non auto-eseguiva `BOOTX64.EFI` e spesso non compariva nessun mapping FS (es. `FS0:`), con fallback a PXE.

### Cosa abbiamo escluso
- ESP mal formattata: **GPT e FAT32 risultano corretti** (MBR protettivo, header GPT, entry EF00, BPB coerente).
- File mancante: `\EFI\BOOT\BOOTX64.EFI` è presente; anche copia in root `\BOOTX64.EFI`.
- Loader “rotto”: test anche con `uefi_hello` (EFI minimale).

### Root cause (identificata)
Quando abbiamo usato una build DEBUG di edk2/OVMF, è emerso dal log UEFI (`uefi_debug.log`) un **ASSERT intenzionale del firmware**:
- `PlatformCpuCountBugCheck`: firmware rileva un bug noto in QEMU (< 8) sul blocco di registri hotplug CPU.
- Con QEMU 6.2.0 (versione rilevata), il firmware **si blocca deliberatamente** prima di completare il boot.

Conseguenza:
- Il firmware non arriva alle fasi DXE/BDS → non carica i driver disco/partizioni/FAT → non monta ESP → `BOOTX64.EFI` non viene trovato/eseguito.

Nota: è stato aggiunto anche un override fw_cfg “a rischio” per proseguire oltre il bugcheck:
- `-fw_cfg name=opt/org.tianocore/X-Cpuhp-Bugcheck-Override,string=yes`

---

## Modifiche e miglioramenti nello script `run_uefi.sh`
Nel tempo, lo script è stato esteso per:
- Supportare più controller/trasporti: virtio-blk, IDE, USB storage, AHCI esplicito, NVMe.
- Supportare modalità “no VARS” e log capture (`dist/qemu_run.log`).
- Aggiungere override fw_cfg per il bug hotplug CPU (opzionale, con warning).
- Aggiungere fallback automatico quando si chiede `-cpu host` senza KVM (in TCG non è supportato).

---

## Piano concordato (prossimi passi)
### Priorità 1 — Sbloccare il boot UEFI end-to-end
1) **Aggiornare QEMU** a una versione >= 8 (raccomandato) per evitare il bug hotplug CPU senza override.
   - Obiettivo: far completare il firmware fino a DXE/BDS e ottenere `FS0:`.

2) In alternativa (temporanea) usare:
   - `OVMF_CPU_BUGCHECK_OVERRIDE=1` nello script
   - consapevoli del rischio (instabilità / possibili corruzioni)

3) Ripetere test con presentazione disco “robusta”:
   - NVMe (`USE_NVME=1`) e/o AHCI (`USE_AHCI=1`) e, se serve, `USE_PC_MACHINE=1`.

4) Verificare nel log che compaiano:
   - caricamenti driver DXE (Partition/Disk/Fat)
   - messaggi BdsDxe
   - tentativo di load di `\EFI\BOOT\BOOTX64.EFI`

### Priorità 2 — Consolidare la catena di boot
Una volta che OVMF carica `BOOTX64.EFI`:
- Verificare che il loader trovi e carichi `kernel.elf`.
- Validare `ExitBootServices` (memory map key) e jump al kernel.

### Priorità 3 — Hardening / qualità
- Completare (se desiderato) la parte W^X (mappature 4 KiB per separare RX/RW).
- Ripulire/riordinare la doc:
  - aggiungere istruzioni “known-good” per QEMU >= 8
  - opzioni consigliate `run_uefi.sh`

---

## Note operative rapide
- Per montare la partizione EFI dentro un’immagine GPT localmente:
  - start LBA = 2048
  - offset byte = 2048 * 512

Esempio:

```bash
sudo mount -o loop,offset=$((2048*512)) dist/esp_gpt.img /mnt
ls -l /mnt/EFI/BOOT/BOOTX64.EFI
sudo umount /mnt
```

---

## Stato attuale
- Loader UEFI + ELF loader: implementati.
- ESP builder + verifiche: implementati.
- Blocco principale: ambiente QEMU/OVMF (QEMU 6.2) → bugcheck CPU hotplug → firmware si ferma prima di DXE/BDS.
- Prossimo passo raccomandato: **aggiornare QEMU** e riprovare boot con NVMe/AHCI.
