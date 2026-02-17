 MILESTONE STATUS
 ─────────────────────────────────────────────────────────
  M0  DONE       Tag: —                    UEFI boot chain
  M1  DONE       Tag: M1_STABLE            Memory model hardening
  M2  DONE       Tag: M2_STABLE            Stack hardening + IST guard pages + physmap reorder
  M3  DONE       Tag: M3_ISOLATION_BASE    User/Kernel isolation (user_range_valid, copy_from/to_user, syscall hardening)
  M4  DONE       Tag: M4_STABLE            Stabilization + isolation selftest (12/12 PASS) + vmm_map_in_space hardening
  M5  PLANNED    —                         Context switch + first ring-3 user task
 ─────────────────────────────────────────────────────────

 1) ANALISI ARCHITETTURALE CRITICA

  1. UEFI page tables mai attivate. uefi/boot.c:118 — le tabelle custom sono allocate con AllocatePool (non
  page-aligned, requisito CR3 violato) e l'attivazione e' commentata. Il kernel eredita CR3 del firmware UEFI, su cui
  non ha controllo.
  2. Console calls dopo ExitBootServices. uefi/boot.c:168,194,199 — puts16() chiama ConOut->OutputString, un Boot
  Service. Dopo ExitBootServices() (linea 163) e' comportamento indefinito. Su firmware reale = crash.
  3. PMM clamp a 128MB nel path UEFI. pmm.c:278 — regioni oltre 128MB troncate a zero, nonostante identity mapping copra
   512MB. Il kernel UEFI usa al massimo ~128MB di RAM fisica.
  4. Bitmap PMM piazzata a _kernel_end senza protezione. pmm.c:100 — nessuna verifica che la bitmap non collida con
  segmenti ELF copiati dal bootloader (che scrive a vaddr = indirizzo fisico identity-mapped).
  5. _bss_start dichiarato DOPO *(COMMON) nel linker script. linker.ld:35-36 — i simboli COMMON non ricadono tra
  _bss_start e _bss_end, quindi vmm_protect_kernel_sections non applica W^X su quelle pagine.
  6. Scheduler senza context switch. sched.c:47 — sched_yield() cambia puntatori e stato, ma non salva/ripristina
  registri ne' CR3. I processi non girano realmente.
  7. VMM accede a page tables tramite cast fisico diretto. vmm.c:96,104 — (uint64_t*)pml4_phys assume identity mapping.
  Funziona solo finche' le page table risiedono sotto 512MB. Dopo physmap_init dovrebbe usare phys_to_virt().
  8. vmm_space_destroy non libera frame delle page tables. vmm.c:358-362 — chiama solo kfree(space) senza walk
  ricorsivo. Memory leak permanente per ogni processo distrutto.
  9. Stack kernel fisso a 16KB senza guard page. boot.asm:41 — overflow silenzioso corrompe .bss.
  vmm_protect_kernel_sections dichiara guard page nel commento ma non la implementa (vmm.c:239).
  10. UEFI bootloader usa AllocatePool per page tables. uefi/boot.c:101-103 — AllocatePool garantisce allineamento a 8
  byte, non 4KB. Se le tabelle venissero attivate, CR3 con indirizzo non allineato causa #GP.
  11. secos_boot_info e' static nel bootloader. uefi/boot.c:171 — risiede in segmento EFI_LOADER_DATA. Il kernel la
  referenzia via pointer ma il PMM potrebbe riciclare quei frame.
  12. Nessun GDT kernel-owned. Il kernel usa il GDT del bootloader (boot.asm:263), 3 entry senza TSS descriptor.
  tss_init() deve avere un GDT proprio per caricare il selettore TSS.
  13. find_free_frame O(n) bit-a-bit. pmm.c:46-53 — scan lineare su ogni bit. Accettabile ora, ma scala male oltre 1GB.
  14. ELF segment copy nel bootloader sovrascrive memoria kernel. uefi/boot.c:133-140 — dst = (uint8_t*)vaddr scrive
  direttamente sull'identity mapping. Se vaddr coincide con aree usate dal firmware o dal bootloader stesso, corruzione
  silenziosa.
  15. kernel_main riceve uint64_t multiboot_info come secondo parametro. kernel.c:60 — ma il path UEFI passa un pointer
  a struct. La firma e' semanticamente confusa e type-unsafe (non si distingue pointer da intero).

  ---
  2) ROADMAP TECNICA

  M0 — Handoff UEFI corretto e boot affidabile

  Obiettivo: Eliminare comportamento indefinito nel bootloader UEFI, garantire che il kernel riceva un ambiente
  deterministico.

  Task:
  - Rimuovere TUTTE le chiamate puts16() dopo ExitBootServices() (boot.c:168,194,199). Il debug post-EBS deve usare
  serial port o scrivere direttamente in framebuffer.
  - Sostituire AllocatePool con AllocatePages (tipo EfiLoaderData, AllocateAnyPages) per pml4/pdpt/pdt — garantisce
  allineamento 4KB.
  - Attivare le page tables custom PRIMA del jump al kernel (de-commentare e correggere activate_page_tables, linea
  118).
  - Copiare secos_boot_info in un buffer nel kernel (.bss o primo frame allocato) prima che PMM possa riciclare la
  memoria del bootloader.
  - Alzare il clamp PMM UEFI da 128MB a 512MB (pmm.c:278), coerente con l'identity mapping.
  - Correggere il linker script: spostare _bss_start PRIMA di *(COMMON).

  Dipendenze: Nessuna.

  Rischi: Attivare le nuove page tables puo' esporre mapping mancanti. Testare con OVMF e almeno un firmware reale. Se
  il kernel crasha subito, il problema e' un mapping mancante nella PDT (aggiungere entry per area framebuffer se
  necessario).

  Done quando: QEMU + OVMF boot senza UB, kernel stampa banner, PMM riporta ~480MB+ free su VM con 512MB, #GP test su
  indirizzo non mappato produce page fault gestito.

  ---
  M1 — Memory model robusto  [DONE — tag M1_STABLE]

  Obiettivo: Il kernel possiede le proprie page tables, accede alla memoria fisica in modo corretto e protegge le
  sezioni kernel.

  Task:
  - Ricostruire page tables kernel-owned all'avvio di vmm_init(): allocare nuove PML4/PDPT/PDT/PT via PMM, replicare
  identity mapping 512MB + physmap, caricare CR3 con le nuove tabelle. Non dipendere piu' dalle tabelle del
  bootloader/UEFI.
  - Transizione da identity cast a physmap. In get_or_create_table, get_pt, get_pt_space: dopo physmap init, usare
  phys_to_virt() per accedere a page table entries. Mantenere il fallback identity solo prima di vmm_init_physmap().
  - Guard page kernel stack. Unmap la pagina a stack_bottom - PAGE_SIZE. Triple fault su overflow diventa double fault
  gestito (IST gia' configurato).
  - GDT kernel-owned con entry null, code64, data64, TSS64 (16 byte). Caricare via lgdt in vmm_init() o funzione
  dedicata. tss_init() scrive il descriptor TSS nel nuovo GDT.
  - Fix vmm_space_destroy: walk ricorsivo PML4→PDPT→PDT→PT, free di ogni frame allocato per user-space (non liberare
  frame kernel condivisi).
  - Implementare guard page stack utente in vmm_alloc_user_stack_in_space: la pagina sotto lo stack bottom deve restare
  unmapped.

  Dipendenze: M0 completata (bootloader corretto).

  Rischi: La transizione identity→physmap e' il punto piu' critico. Se un singolo accesso a page table usa il path
  sbagliato, triple fault. Procedere in due fasi: (a) prima far funzionare physmap access, (b) poi rimuovere identity
  fallback. Aggiungere assert su allineamento in ogni get_or_create_table.

  Done quando: vmm_translate() funziona per indirizzi kernel, physmap, e user-space. vmm_space_destroy libera tutti i
  frame (verificabile con PMM stats prima/dopo). Guard page kernel stack provoca double fault catchato dall'IST.

  ---
  M2 — Context switch e multiprogrammazione reale

  Obiettivo: Due o piu' processi ELF user-space girano in round-robin con isolamento di address space.

  Task:
  - Context switch completo in assembly: salvare RSP, RBP, RBX, R12-R15, CR3 del processo corrente nel PCB. Caricare
  quelli del prossimo. Usare iretq per il ritorno a user-space (RSP, SS, RFLAGS, CS, RIP dallo stack).
  - TSS.RSP0 update ad ogni switch: puntare allo stack kernel del processo entrante.
  - CR3 switch in sched_yield(): vmm_switch_space(next->space) prima del salto.
  - Quantum-based preemption: contatore tick nel PCB, yield dopo N tick (suggerito: 10ms = 10 tick a 1000Hz). Non yield
  ad ogni tick.
  - Syscall exit(): rilasciare address space (vmm_space_destroy corretto da M1), rimuovere da process table, schedule
  next.
  - Test: due processi ELF che stampano identificatori alternati via syscall write. Verificare interleaving.

  Dipendenze: M1 completata (page tables kernel-owned, space destroy funzionante, GDT con TSS).

  Rischi: Bug nel context switch assembly sono difficili da diagnosticare. Implementare prima uno switch cooperativo
  (syscall yield) prima del preemptivo. Verificare che iretq stack frame sia corretto (SS=0x1B, CS=0x23 per ring3).
  Rischio di stack corruption se RSP0 non aggiornato.

  Done quando: Due processi ELF producono output interleaved. ps nella shell mostra PID diversi con stato RUNNING/READY
  alternato. Nessun memory leak dopo exit() di un processo (PMM stats stabili).

  ---
  M3 — Solidificazione e scalabilita' memoria

  Obiettivo: Il sistema gestisce memoria oltre 512MB, ha protezione W^X completa e foundation per sviluppo futuro.

  Task:
  - Higher-half kernel. Spostare il kernel a 0xFFFFFFFF80000000 (top 2GB). Identity mapping mantenuta transitoriamente
  per boot, rimossa dopo setup. Aggiornare linker script (. = 0xFFFFFFFF80200000), bootloader (aggiungere entry PML4 per
   higher-half), e tutti i cast fisici residui.
  - Demand paging. Estendere vmm_handle_page_fault per allocare on-demand pagine user-space da regioni registrate con
  vmm_region_add. Gia' abbozzato (vmm.c:579-587), completare con validazione e limiti.
  - PMM scalabile. Sostituire bitmap scan lineare con free-list o buddy allocator. Rimuovere il clamp a 512MB, gestire
  tutta la RAM fisica riportata dalla memory map.
  - W^X enforcement completo. Verificare che OGNI pagina mappata rispetti W^X. Aggiungere check in vmm_map e
  vmm_alloc_page: rifiutare flag RW|~NX simultanei (writable + executable).
  - Serial console driver per debug post-ExitBootServices e ambienti headless.

  Dipendenze: M2 completata (context switch funzionante necessario per testare demand paging in user-space).

  Rischi: Higher-half migration tocca ogni indirizzo hardcoded nel kernel. Procedere con una fase intermedia: mappare il
   kernel sia a 2MB sia a higher-half, switchare, poi rimuovere il mapping basso. Buddy allocator puo' essere
  implementato incrementalmente sopra il bitmap esistente.

  Done quando: Kernel gira a 0xFFFFFFFF80…, identity mapping low rimossa. PMM gestisce >512MB (testare con QEMU -m 2G).
  Page fault su pagina W+X genera #GP/panic. Serial output funzionante.

  ---
  M4 — Driver Space enforcement

  Obiettivo: Il Driver Space diventa un confine di sicurezza verificabile: processi driver
  identificati esplicitamente, DRIVER_OP_MAP_MEM funzionante, restart automatico su crash,
  IRQ subscribe via coda IPC.

  Task:
  - Aggiungere campo proc_type (PROC_TYPE_USER / PROC_TYPE_DRIVER) al PCB; il loader lo imposta
  in base al manifest ELF (MANIFEST_FLAG_DRIVER).
  - Vietare SYS_DRIVER ai processi PROC_TYPE_USER: il dispatcher ritorna DRV_ERR_PERM senza
  consultare il registro dispositivi.
  - Implementare DRIVER_OP_MAP_MEM: validare mem_offset e mem_length contro device_desc_t.mem_base
  e mem_size, creare mapping virtuale nel processo driver (USER=1, RW=1, NX=1), registrarlo nel PCB
  per cleanup preciso.
  - Cleanup mapping su unload: vmm_space_destroy gia' funzionante (M1); aggiungere rimozione
  binding dal registro dispositivi al momento della distruzione del processo.
  - Restart automatico driver critico: kernel rileva exit anomala, ricarica ELF dal RAMFS,
  ripristina binding. Limite N restart in K tick; oltre soglia: DEV_FLAG_FAILED.
  - DRIVER_OP_IRQ_SUBSCRIBE: associa un IRQ a un processo driver; l'ISR kernel inserisce evento
  in coda IPC; il driver consuma via SYS_READ su fd speciale.

  Dipendenze: M3 completata (VMM higher-half funzionante necessario per MAP_MEM affidabile;
  context switch M2 necessario per IPC e restart).

  Done quando: Un processo PROC_TYPE_USER riceve DRV_ERR_PERM su SYS_DRIVER. Un driver che
  crasha viene riavviato automaticamente. MAP_MEM mappa la regione corretta e la dealloca
  all'unload (PMM stats stabili). Un IRQ simulato via drvtest raggiunge il processo driver
  via coda e viene consumato senza race.

  ---
  3) DECISIONI ARCHITETTURALI DA PRENDERE SUBITO

  1. Kernel higher-half o identity-mapped? Tutta l'architettura VMM assume identity mapping per accesso a page tables.
  Decidere ORA se migrare a higher-half (M3) influenza come scrivere il codice in M0-M2. Se si', usare phys_to_virt()
  ovunque da subito.
  2. Unica page table set ownership o split? Decidere se il kernel mantiene un unico PML4 (condiviso con processi user
  via entry alte) o se ogni processo ha PML4 privata con entry kernel duplicate. La scelta impatta
  vmm_space_create_user.
  3. Boot path primario: UEFI o Multiboot? Mantenere entrambi ha costo. Se il target e' hardware moderno, deprecare
  Multiboot e concentrare lo sforzo su UEFI. Se serve legacy, documentare quale path e' "golden".
  4. Convenzione chiamata kernel_main. Definire una firma tipizzata (struct boot_params*) unica per tutti i path di
  boot, eliminando la disambiguazione su magic number.
  5. Allocatore fisico a lungo termine. Bitmap, free-list, o buddy? Decide la complessita' di M3 e le performance di
  alloc/free per demand paging.

  ---
  4) 5 ERRORI POTENZIALMENTE FATALI SE NON CORRETTI ORA

  1. Console calls dopo ExitBootServices (uefi/boot.c:168+). Su firmware reale, corruzione di stato UEFI runtime o hard
  hang. Nessun output diagnostico possibile. Correggi prima di qualsiasi test su hardware.
  2. Page tables non page-aligned (uefi/boot.c:101-103 usa AllocatePool). Se attivi activate_page_tables senza fix, CR3
  non allineato = #GP immediato, sistema irrecuperabile. Devi usare AllocatePages.
  3. VMM assume identity mapping per accesso a page tables (vmm.c:96,104). Qualsiasi page table allocata sopra i 512MB
  identity-mapped diventa inaccessibile. Con >512MB di RAM e molti processi, le nuove PT potrebbero essere allocate in
  frame alti = triple fault non diagnosticabile.
  4. _bss_start dopo *(COMMON) nel linker script. Le variabili globali non inizializzate (COMMON) non sono protette da
  W^X. Un buffer overflow in una di esse puo' sovrascrivere codice kernel senza che la protezione NX intervenga.
  5. secos_boot_info in memoria riciclabile. La struct e' static nel bootloader (EFI_LOADER_DATA). Il PMM marca
  EfiConventionalMemory come libera ma non protegge EfiLoaderData. Tuttavia, se il tipo di memoria e' riciclato (dipende
   dal firmware), il kernel legge dati corrotti dal pointer bi. Su alcuni firmware, EfiLoaderData = available dopo EBS.
  Copiare i dati in area kernel-owned prima di inizializzare il PMM.