# TEAMMATES SECoS

------------------------------------------------------------------------

# Regole Globali

## 1. Branch per Milestone (OBBLIGATORIO)

Ogni milestone lavora su branch dedicato:

-   M0 → `milestone/M0`
-   M1 → `milestone/M1`
-   M2 → `milestone/M2`
-   M3 → `milestone/M3`

Merge su `main` solo dopo validazione completa da parte di Teammate 4.

------------------------------------------------------------------------

## 2. Commit Policy

-   1 task = 1 commit
-   Commit piccoli e descrittivi
-   Formato messaggio commit:

```{=html}
<!-- -->
```
    [Mx][Domain] Descrizione breve

Esempio:

    [M0][Boot] Remove Boot Services calls after ExitBootServices

Vietati mega-commit multi-task.

------------------------------------------------------------------------

## 3. Compilazione Obbligatoria

Ogni commit deve eseguire:

    make clean
    make
    make uefi

Se la milestone lo richiede:

    qemu-system-x86_64 ...

------------------------------------------------------------------------

## 4. File Critici Protetti

Modifiche consentite solo con commit isolato + test immediato:

-   `linker.ld`
-   `boot/boot.asm`

------------------------------------------------------------------------

## 5. Ownership Rigido per File

Ogni file ha un owner.\
Modifiche cross-dominio devono essere eseguite dall'owner del file.

------------------------------------------------------------------------

## 6. Divieti Assoluti

Non introdurre:

-   GUI
-   Networking
-   Package manager
-   Feature speculative
-   Refactor globale non richiesto dalla milestone

------------------------------------------------------------------------

# Teammate 1 --- UEFI / Boot Engineer

## Ownership

-   `uefi/*`
-   `boot/boot.asm`
-   `linker.ld`
-   `kernel/bootinfo.h`

## Responsabilità

-   Bootloader UEFI
-   Costruzione page tables iniziali
-   Attivazione CR3
-   Handoff corretto al kernel
-   Coerenza mapping iniziale

## Milestone

-   M0: completo owner
-   M1: supporto tecnico CR3
-   M3: supporto higher-half lato boot

## M0 Task

1.  Rimuovere tutte le chiamate Boot Services dopo `ExitBootServices()`
2.  Sostituire `AllocatePool` con `AllocatePages`
3.  Garantire allineamento 4KB page tables
4.  Attivare page tables custom prima del jump
5.  Coordinarsi con Teammate 2 per clamp PMM
6.  Fix `_bss_start` prima di `*(COMMON)` in `linker.ld`

## Deliverable M0

-   Nessuna chiamata Boot Services post-EBS
-   CR3 caricato con page tables valide
-   Boot stabile in OVMF
-   `make uefi` pulito

## NON può toccare

-   `mm/*`
-   `kernel/sched*`
-   `kernel/process*`
-   `arch/x86/tss*`
-   `kernel/syscall*`

------------------------------------------------------------------------

# Teammate 2 --- Memory Architect

## Ownership

-   `mm/*`
-   GDT kernel-owned
-   physmap
-   PMM
-   VMM

## Responsabilità

-   PMM
-   VMM
-   Page tables kernel-owned
-   Guard pages
-   W\^X enforcement
-   Higher-half migration (M3)

## Milestone

-   M0: clamp PMM 512MB
-   M1: completo owner
-   M3: memoria avanzata

## M0 Task

-   Modificare clamp PMM a 512MB (`pmm.c`)

## M1 Task

1.  Costruire page tables kernel-owned in `vmm_init()`
2.  Caricare CR3 con nuove tabelle
3.  Implementare accesso `phys_to_virt`
4.  Implementare guard page kernel stack
5.  Implementare GDT kernel-owned
6.  Correggere `vmm_space_destroy`
7.  Implementare guard page stack utente

## Deliverable M1

-   Kernel usa page tables proprie
-   Guard page attive
-   Nessun memory leak dopo destroy space

## NON può toccare

-   `uefi/*`
-   `boot/boot.asm`
-   `linker.ld`
-   `kernel/sched*`
-   `kernel/process*`
-   `kernel/syscall*`

------------------------------------------------------------------------

# Teammate 3 --- Scheduler & Process Engineer

## Ownership

-   `kernel/sched*`
-   `kernel/process*`
-   `kernel/syscall*`
-   `arch/x86/context_switch.asm`
-   `arch/x86/tss*`

## Responsabilità

-   Context switch
-   CR3 switch
-   Scheduler preemptivo
-   Gestione processi
-   Syscall exit/yield

## Milestone

-   M2: completo owner

## M2 Task

1.  Implementare context switch completo in assembly
2.  Aggiornare TSS.RSP0 ad ogni switch
3.  Implementare CR3 switch
4.  Scheduler con quantum
5.  Implementare syscall `exit()`
6.  Implementare syscall `yield()`

## Deliverable M2

-   Due processi ELF interleaved
-   Isolamento address space
-   Nessun memory leak dopo exit

## NON può toccare

-   `uefi/*`
-   `boot/boot.asm`
-   `linker.ld`
-   `mm/*`

------------------------------------------------------------------------

# Teammate 4 --- Integration & Validation Engineer

## Responsabilità

-   Test su QEMU e OVMF
-   Validazione criteri Done
-   Verifica PMM stats
-   Verifica assenza triple fault
-   Regressione completa

## Milestone

-   Validazione M0, M1, M2, M3

## Deliverable

-   Report test pass/fail
-   Log QEMU salvati
-   Conferma scritta:
    -   `Milestone Mx VALIDATA`
    -   oppure `Milestone Mx BLOCCATA — motivo`

## NON può modificare codice sorgente

Può solo creare file in: - `tests/` - `scripts/`

------------------------------------------------------------------------

# Sequenza Operativa

1.  Teammate 1 completa M0
2.  Teammate 2 completa M1
3.  Teammate 3 completa M2
4.  Teammate 1 + 2 completano M3
5.  Teammate 4 valida ogni fase prima di avanzare

Vietato lavorare in parallelo su milestone diverse.
