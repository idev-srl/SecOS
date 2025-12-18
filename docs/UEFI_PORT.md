# UEFI Port (Strategy B: External Bootloader + Existing ELF Kernel)

## Goal
Replace BIOS/Multiboot2 bootstrap with a UEFI bootloader that loads the existing ELF64 `kernel.bin` (or renamed `kernel.elf`), prepares environment (framebuffer, memory map, page tables, long mode) and transfers control.

## Phases
1. Stub Bootloader (efi_main) + GOP + Memory Map size probe.
2. Implement full memory map retrieval and pass descriptors to kernel.
3. Implement ELF64 loader inside bootloader: parse headers, allocate pages, map segments with correct permissions.
4. Page Tables & Long Mode finalization pre-ExitBootServices (enable NXE).
5. Framebuffer handoff struct; unify with existing `fb_console` expectations.
6. ExitBootServices and jump to kernel entry shim (`kernel_main_uefi`).
7. Refactor kernel to remove Multiboot assumptions (conditional compile).
8. Cleanup and documentation + security hardening notes.

## Bootloader Responsibilities
- Locate kernel file (initially from a FAT image provided by QEMU).
- Query GOP for framebuffer info.
- Retrieve memory map, convert descriptors to internal region list.
- Build page tables similar to current `boot.asm` (identity + higher-half).
- Enable paging, long mode, NXE.
- ExitBootServices to obtain stable memory map key.
- Jump to kernel entry with a handoff struct.

## Handoff Structure (Draft)
```c
struct secos_boot_info {
    uint64_t fb_addr;
    uint32_t fb_width, fb_height, fb_pitch, fb_bpp;
    uint64_t mem_desc_count;
    EFI_MEMORY_DESCRIPTOR* mem_descs; // physical pointer (identity mapped)
};
```
Passed in RDI (pointer) and RSI reserved for future flags.

## Kernel Adjustments
- New entry `void kernel_main_uefi(struct secos_boot_info* bi, uint64_t flags);`
- Add `pmm_init_uefi` that consumes UEFI descriptors.
- Guard Multiboot-only code with `#ifdef CONFIG_MULTIBOOT`.

## Build Changes
- New `uefi/` directory: `efi.h`, `boot.c`, later `elf_load.c`, `paging.c`.
- Makefile target `uefi` builds bootloader -> `BOOTX64.EFI` using `objcopy -O efi-app-x86_64`.
- FAT directory layout: `dist/EFI/BOOT/BOOTX64.EFI`, `dist/kernel.elf`.

## Testing
Use QEMU + OVMF:
```
qemu-system-x86_64 -bios OVMF.fd -drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd \
                   -drive if=pflash,format=raw,file=OVMF_VARS.fd \
                   -drive format=raw,file=fat:rw:dist -serial stdio
```
Expect initial messages, framebuffer base hex, then halt.

## Security Considerations (Upcoming)
- NXE enabled early, W^X on loaded segments.
- Validate ELF segment boundaries (no overlap, within allocated region).
- Consider Secure Boot signature step (future).

## Security Hardening (Detailed)
Questo paragrafo espande le note di sicurezza e definisce azioni concrete.

### Obiettivi
- Ridurre superficie di attacco del bootloader.
- Garantire integrità del kernel ELF prima dell'esecuzione.
- Evitare esecuzione di codice in pagine scrivibili (W^X) e abilitare NX ovunque possibile.
- Separare memoria UEFI runtime da memoria kernel per prevenire uso improprio post-ExitBootServices.

### Misure Implementate
1. NXE (bit 11 EFER) attivato prima di ExitBootServices.
2. Segmenti ELF copiati in aree identity controllate; future rimappature a granularità 4KB.
3. `ExitBootServices` chiamato prima del salto al kernel: rimozione servizi runtime e blocco allocazioni tardive.

### Misure Pianificate
1. W^X completo: ricostruire page tables con pagine 4KB marcando:
    - .text, .rodata: RX
    - .data, .bss, heap early: RW, NX
2. Validazione ELF avanzata:
    - Verifica assenza overlapping tra segmenti
    - Verifica allineamento `p_align`
    - Rifiuto di segmenti fuori range previsto (superiore al limite fisico identity configurato)
3. Integrità del kernel:
    - Hash SHA-256 del file `kernel.elf` confrontato con valore noto (future manifest)
    - Possibile estensione a firma digitale (Secure Boot custom) con verifica RSA/ECDSA.
4. Riduzione privilegi prima del salto:
    - Pulizia registri (azzerare RBX, R12-R15) per non passare leak di informazioni
    - Randomizzazione semplice stack kernel (offset entro pagina) se supportata.
5. Sanitizzazione memoria:
    - Azzerare pool temporanei usati per caricamento segmenti dopo copia definitiva.
6. Protezioni runtime:
    - Mark pagine contenenti tabelle di pagine come read-only dopo setup.
    - Aggiunta guard page sotto e sopra lo stack iniziale del kernel.

### Considerazioni Secure Boot
L’integrazione con Secure Boot standard richiede firma PE/COFF del loader. Poiché il kernel viene caricato da file system, è consigliabile:
1. Aggiungere certificato pubblico incorporato nel loader.
2. Verificare firma del kernel (struttura custom: header + firma).
3. Fallire con messaggio chiaro e non avviare il kernel se la firma non corrisponde.

### ACPI / Firmware Interaction
Post-ExitBootServices, runtime services UEFI non più disponibili (salvo regioni marcate EFI_RUNTIME). Evitare di sovrascrivere tali regioni. La mappa finale andrà filtrata nel PMM per non allocare frame provenienti da tipi:
 - EfiRuntimeServicesCode
 - EfiRuntimeServicesData

### Threat Model Sintetico
Attaccante può:
 - Modificare kernel.elf su disco FAT.
 - Inserire segmenti ELF crafted per sforare mapping.
 - Sfruttare pagine RWX per code injection.

Contromisure:
 - Firma + hash kernel.
 - W^X obbligatorio.
 - Rifiuto segmenti out-of-range.
 - Validazione header & limiti.

### Checklist Implementazione (da tracciare)
- [ ] Hash & firma kernel
- [ ] Rimappaggio W^X 4KB
- [ ] Validazione integrità segmenti
- [ ] Guard page stack
- [ ] Page tables read-only
- [ ] Filtraggio regioni runtime services
- [ ] Zero pool temporanei


## Next Steps
- Implement full memory map fetch & allocation.
- Add ELF loader.
- Populate handoff struct and adapt kernel.
- Replace direct halt with actual jump.
```
