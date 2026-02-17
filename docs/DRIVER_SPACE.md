# SECoS — Modello Driver Space

**Documento:** Specifica del livello privilegiato intermedio tra kernel e user space.
**Stato:** Architettura target (implementazione parziale in `milestone/M1`).
**Lingua:** Italiano (tecnico).

---

## 1. Perché esiste il Driver Space

I sistemi operativi tradizionali scelgono tra due estremi: driver in kernel space (Ring 0, pieno
accesso, crash = panic) oppure driver in user space puro (Ring 3 senza capacità hardware, IPC
costosa). Nessuno dei due è ottimale per un kernel sicuro ma pratico.

**SECoS introduce un terzo livello — il Driver Space** — come strato intermedio:

| Livello         | Anello CPU | Accesso hardware | Isolamento crash |
|-----------------|-----------|-----------------|-----------------|
| Kernel Space    | Ring 0    | Diretto          | Nessuno         |
| **Driver Space**| **Ring 3**| **Mediato**      | **Isolato**     |
| User Space      | Ring 3    | Vietato          | Isolato         |

**Motivazione:**
- Un bug in un driver non deve portare il kernel al panic.
- L'accesso a registri hardware deve essere auditabile e limitato a range noti.
- La syscall `SYS_DRIVER` fornisce un gate controllato senza esporre MMIO raw.

**Trade-off accettati:**
- Overhead di contesto per ogni operazione I/O (syscall invece di accesso diretto).
- Complessità del dispatcher kernel per validazione e audit.
- I driver non possono usare DMA o IRQ diretti (vincolo rimovibile in milestone future).

---

## 2. Modello di privilegio

### Livelli hardware

```
Ring 0  ─────  Kernel (vmm, pmm, idt, tss, scheduler)
Ring 3  ─────  Tutti i processi utente, inclusi i driver
```

L'architettura x86-64 espone solo Ring 0 e Ring 3 ai sistemi operativi moderni (Ring 1/2
non vengono usati). Il Driver Space NON è un anello CPU aggiuntivo.

### Privilegio logico in Ring 3

La distinzione tra un processo driver e un processo utente normale è **puramente software**,
applicata dal kernel tramite:

1. **`caps_mask`** nella `device_desc_t`: bitmask che indica quali operazioni sono permesse per
   quel dispositivo.
2. **Binding esclusivo**: solo un processo alla volta può essere legato a un dato `device_id`.
3. **Syscall gating**: `SYS_DRIVER` chiama `handle_driver_call()` che verifica il binding e la
   capability prima di eseguire qualsiasi operazione.

```
Processo in Ring 3
     │
     │ syscall SYS_DRIVER (INT 0x80 / SYSCALL)
     ▼
Kernel dispatcher
     │ check: processo legato al device_id?
     │ check: caps_mask include l'opcode richiesto?
     │ check: offset/length dentro i limiti del device?
     ▼
handle_driver_call() → operazione su shadow buffer / mappa virtuale
     │
     │ ritorno a Ring 3
     ▼
Processo driver riceve risultato
```

Un processo con binding e caps_mask appropriati opera come "driver privilegiato in Ring 3".
Un processo senza binding ottiene `DRV_ERR_BINDING` su qualsiasi chiamata `SYS_DRIVER`.

---

## 3. Tipi di processo

### Stato attuale (M1)

Non esiste un campo esplicito `DRIVER_PROCESS` nel PCB. Il tipo di processo è determinato
implicitamente dalla presenza di un binding nel registro dispositivi kernel.

**Strutture rilevanti:**

```c
// kernel/process.c — Process Control Block (semplificato)
typedef struct pcb {
    uint32_t pid;
    int      state;        // PROC_NEW, PROC_READY, PROC_RUNNING
    char     name[32];
    // ... registri, spazio virtuale, manifest ...
} pcb_t;

// kernel/driver_if.c — Descrittore di dispositivo
typedef struct {
    uint64_t reg_base;    // Base del buffer registri shadow
    uint64_t reg_size;    // Dimensione buffer registri (byte)
    uint64_t mem_base;    // Base regione memoria dispositivo
    uint64_t mem_size;    // Dimensione regione memoria
    uint32_t caps_mask;   // Bitmask capacità (DEV_CAP_*)
    uint32_t flags;       // Flag interni
} device_desc_t;
```

### Stato target (M4+)

Introdurre un campo `proc_type` nel PCB con valori `PROC_TYPE_USER` e `PROC_TYPE_DRIVER`,
validato al momento del binding e consultato dal loader per applicare restrizioni di mapping.

**Proprietà di un processo driver:**

| Proprietà                    | USER_PROCESS   | DRIVER_PROCESS (target) |
|------------------------------|---------------|------------------------|
| Binding dispositivo          | No            | Sì, esclusivo          |
| Accesso `SYS_DRIVER`         | Vietato       | Permesso (via caps)    |
| Accesso MMIO diretto         | Vietato       | Vietato (shadow)       |
| IRQ subscribe (futuro)       | No            | Sì (con cap)           |
| DMA (futuro)                 | No            | Sì (sandboxato)        |
| Restart automatico su crash  | No            | Sì (politica kernel)   |

**Vincoli immutabili per entrambi i tipi:**
- Nessun accesso a memoria kernel (USER bit assente sulle pagine kernel).
- Nessun `IN`/`OUT` diretto (IOPL = 0 per tutti i processi Ring 3).
- W^X applicato: nessun segmento contemporaneamente scrivibile ed eseguibile.

---

## 4. Modello syscall

### Syscall disponibili (stato attuale)

```
SYS_GETPID    (0x01)  — PID del processo corrente
SYS_EXIT      (0x02)  — Terminazione processo
SYS_OPEN      (0x03)  — Apertura file VFS
SYS_CLOSE     (0x04)  — Chiusura file descriptor
SYS_READ      (0x05)  — Lettura da file descriptor
SYS_WRITE     (0x06)  — Scrittura su file descriptor / terminale
SYS_DRIVER    (0x07)  — Gate driver space (driver_call_t*)
```

### Flusso SYS_DRIVER

```
User (Ring 3):
    driver_call_t dc = { .opcode = DRIVER_OP_READ_REG,
                         .device_id = 0,
                         .reg_offset = 0x4,
                         .value = 0 };
    long result = driver_syscall(&dc);  // sposta ptr in rdi, int 0x80

Kernel (Ring 0, syscall_handler):
    case SYS_DRIVER:
        return driver_syscall(arg0);    // arg0 = puntatore a driver_call_t

handle_driver_call(dc):
    1. Valida device_id (< MAX_DEVICES)
    2. Recupera device_desc_t dal registro
    3. Verifica binding: processo corrente == owner del device?
    4. Verifica cap: caps_mask & cap_per_opcode?
    5. Verifica range: reg_offset < reg_size?
    6. Esegue operazione su shadow buffer
    7. Registra evento in audit log
    8. Ritorna DRV_OK o codice errore
```

### Opcodes supportati

| Opcode                  | Cap richiesta          | Azione                              |
|-------------------------|------------------------|-------------------------------------|
| `DRIVER_OP_READ_REG`    | `DEV_CAP_READ_REG`     | Lettura da shadow register buffer   |
| `DRIVER_OP_WRITE_REG`   | `DEV_CAP_WRITE_REG`    | Scrittura su shadow register buffer |
| `DRIVER_OP_MAP_MEM`     | `DEV_CAP_MAP_MEM`      | Mappa regione memoria (stub)        |
| `DRIVER_OP_UNMAP_MEM`   | `DEV_CAP_UNMAP_MEM`    | De-mappa regione memoria (stub)     |
| `DRIVER_OP_GET_INFO`    | `DEV_CAP_GET_INFO`     | Legge metadati dispositivo          |

### Opcodes pianificati (non implementati)

- `DRIVER_OP_DMA_SETUP` — Configura trasferimento DMA entro buffer sandboxato
- `DRIVER_OP_IRQ_SUBSCRIBE` — Registra handler per IRQ specifico (notifica via coda)
- `DRIVER_OP_BULK_XFER` — Trasferimento bulk con scatter-gather

### Codici di errore

| Codice          | Significato                                  |
|-----------------|----------------------------------------------|
| `DRV_OK`        | Operazione completata con successo           |
| `DRV_ERR_DEVICE`| device_id non valido o non registrato        |
| `DRV_ERR_BINDING`| Processo non legato al dispositivo          |
| `DRV_ERR_OPCODE`| Opcode non riconosciuto                     |
| `DRV_ERR_RANGE` | Offset/length fuori dai limiti del device    |
| `DRV_ERR_PERM`  | caps_mask non include l'operazione richiesta |
| `DRV_ERR_ARGS`  | Argomenti della chiamata non validi          |

---

## 5. Modello memoria

### Mappa concettuale degli spazi virtuali (x86-64, post-M2)

```
Indirizzo virtuale            Contenuto                    Accessibile da
──────────────────────────────────────────────────────────────────────────
0x0000000000000000            NULL (NP)                    —
0x0000000000001000 ..         Codice + dati user (ELF)     Ring 3 (USER=1)
  [dipende da ELF]
0x0000007FFFFFFFFF            (limite spazio utente low)
          ...                 (gap non mappato)
0xFFFF888000000000            Physmap kernel (NX, NU)      Ring 0 only
0xFFFFFF8000000000            Stack kernel + IST (M2)      Ring 0 only
0xFFFFFFFF80000000            (target higher-half, M3+)    Ring 0 only
```

### Regole di mapping per processi driver (stato attuale)

Un processo driver gira nello stesso spazio virtuale di un processo utente normale.
Le differenze sono applicate via policy `caps_mask`, NON via diverso spazio di indirizzi.

**Divieti assoluti per tutti i processi Ring 3:**
1. Nessun mapping a indirizzi kernel (USER bit assente): accesso → #PF.
2. Nessun mapping al physmap (`0xFFFF888000000000`).
3. Nessun mapping diretto a MMIO hardware (apertura tramite `DRIVER_OP_MAP_MEM` futura).
4. Nessun segmento W+X (W^X enforcement in vmm_map e ELF loader).

### Stato target

Con `DRIVER_OP_MAP_MEM` completamente implementato, il kernel:
1. Valida che l'indirizzo MMIO richiesto sia dentro `mem_base..mem_base+mem_size` del device.
2. Crea un mapping virtuale nel processo driver con USER=1, RW=1, NX=1.
3. Registra il mapping nel PCB per cleanup preciso all'unload.
4. Il processo driver accede alla memoria mappata come memoria normale (no syscall aggiuntive).

---

## 6. Modello interrupt / I/O

### Stato attuale

Gli interrupt hardware sono gestiti esclusivamente dal kernel (Ring 0):
- IRQ0 (PIT) → timer tick → futuro: preemption scheduler
- IRQ1 (PS/2) → keyboard driver → circular buffer → VFS read

I processi driver non ricevono IRQ direttamente.

### Flusso IRQ → Driver Space (stato target)

```
Hardware IRQ N
      │
      ▼
IDT entry → kernel ISR (Ring 0)
      │
      │  Il kernel identifica il processo driver registrato
      │  via DRIVER_OP_IRQ_SUBSCRIBE per IRQ N
      │
      ▼
Kernel inserisce evento in coda IPC del processo driver
      │
      ▼
Processo driver (Ring 3) legge dalla coda tramite SYS_READ
su fd speciale (o futuro SYS_WAIT_IRQ syscall)
      │
      ▼
Driver elabora l'evento, esegue DRIVER_OP_* necessari
```

**Garanzie del modello:**
- Il kernel non chiama mai codice Ring 3 durante l'ISR (nessun upcall diretto).
- La coda IPC è nel kernel; il driver consuma eventi in modo cooperativo o preemptivo.
- Un driver lento non causa IRQ loss permanente (la coda ha capacità finita; overflow
  registrato nell'audit log).

### I/O diretto (IOPL)

L'IOPL per tutti i processi Ring 3 è 0. Le istruzioni `IN`/`OUT` generano `#GP`.
L'accesso ai registri hardware avviene esclusivamente tramite il shadow buffer kernel-side,
che implementa la semantica hardware richiesta (o stub per dispositivi futuri).

---

## 7. Stabilità e sicurezza

### Isolamento dei crash

Un processo driver che va in crash (page fault, GPF, doppio fault in Ring 3) genera un'eccezione
che il kernel gestisce come terminazione di processo, non come panic:
- Lo scheduler rimuove il processo dalla run queue.
- `vmm_space_destroy()` libera l'address space (walk PML4→PT, nessun leak).
- Il binding dispositivo viene rimosso dal registro.
- Un messaggio viene aggiunto all'audit log con PID, opcode in corso, codice di errore.

Il kernel continua a operare normalmente.

### Audit log

Il kernel mantiene un buffer circolare di eventi driver:

```c
typedef struct {
    uint64_t tick;      // PIT tick al momento dell'evento
    uint32_t pid;       // PID del processo richiedente
    uint32_t opcode;    // DRIVER_OP_*
    uint32_t device_id; // indice nel registro
    int      result;    // DRV_OK o DRV_ERR_*
} drv_audit_entry_t;
```

Ogni errore (qualsiasi `DRV_ERR_*`) viene sempre registrato, anche se causato da argomenti
errati. Le operazioni riuscite vengono registrate solo se `DRIVER_FLAG_AUDIT` è impostato
nella chiamata.

Comandi shell: `drvlog` stampa gli ultimi N eventi.

### Restart automatico (stato target)

Un processo driver marcato come "critico" nel manifest ELF (flag `MANIFEST_FLAG_DRIVER_CRITICAL`,
futuro) viene riavviato dal kernel dopo il crash:
1. Il kernel rileva terminazione anomala (exit code non zero o eccezione non gestita).
2. Ricarica l'ELF dal RAMFS, crea nuovo address space, ripristina il binding dispositivo.
3. Limita i riavvii a N tentativi nell'arco di K tick; se supera la soglia, il dispositivo
   viene marcato `DEV_FLAG_FAILED` e future `SYS_DRIVER` ritornano `DRV_ERR_DEVICE`.

### Superficie di attacco

| Vettore                       | Mitigazione attuale                                  |
|-------------------------------|------------------------------------------------------|
| device_id fuori range         | Controllo bounds prima di qualsiasi accesso          |
| Offset registro fuori range   | `reg_offset < reg_size` verificato                   |
| Processo non legato           | Binding check prima di ogni operazione               |
| Operazione non autorizzata    | `caps_mask` check per ogni opcode                   |
| Accesso MMIO raw              | Solo shadow buffer (nessun mapping diretto ora)      |
| Overflow audit log            | Buffer circolare (evita DoS da log flooding)         |

---

## 8. Non-obiettivi attuali

Le seguenti funzionalità NON fanno parte del Driver Space nella versione corrente e nelle
milestone prossime (M2, M3). Sono documentate qui per chiarire i confini del sistema.

1. **IOMMU / DMA remapping**: nessuna protezione hardware DMA. Un futuro `DRIVER_OP_DMA_SETUP`
   opererà su buffer kernel-owned, non con DMA diretto al hardware.

2. **Isolamento di namespace I/O**: tutti i driver condividono lo stesso spazio di device_id
   globale. Nessun container o namespace per dispositivi.

3. **Driver hot-reload senza reboot**: un driver aggiornato richiede unload + reload manuale
   (`elfunload` / `elfload`). Nessun live-patching.

4. **Firma ELF e attestazione**: il loader non verifica firme crittografiche sul binario driver.
   Il manifest (`.note.secos`) è validato strutturalmente ma non firmato.

5. **Priority inversion prevention**: nessun meccanismo di inversione priorità tra driver e
   processi utente. La coda IPC è FIFO semplice.

6. **Driver in kernel loadable module**: non esiste supporto per moduli kernel caricabili. Tutti
   i driver di sistema sono compilati staticamente nel kernel oppure girano in Ring 3.

7. **Accesso a MSR / CPUID privilegiati**: vietato. Nessun piano per esporli via `SYS_DRIVER`.

---

## 9. Roadmap suggerita

Questa sezione descrive il minimum feature set per rendere il Driver Space operativo e sicuro,
organizzato per dipendenze.

### Prerequisiti (già in M1/M2)

- [x] `SYS_DRIVER` dispatcher e `handle_driver_call()` (M1)
- [x] `device_desc_t` con `caps_mask` e registro dispositivi (M1)
- [x] Audit log circolare (M1)
- [x] Binding esclusivo per processo (M1)
- [x] Shadow register buffer (M1)
- [ ] Context switch reale con CR3 switch (M2)
- [ ] `vmm_space_destroy` completo senza leak (M1 — fatto)

### M4 — Driver Space enforcement (target primario)

**Obiettivo**: Il Driver Space diventa un confine di sicurezza verificabile.

| Task                                     | Dipendenza | Priorità |
|------------------------------------------|-----------|---------|
| Campo `proc_type` nel PCB               | M2        | Alta    |
| Loader rifiuta `SYS_DRIVER` a USER_PROCESS | M2     | Alta    |
| `DRIVER_OP_MAP_MEM` implementato (no stub) | M3 VMM  | Alta    |
| Cleanup mapping su unload driver         | M1 vmm_space_destroy | Alta |
| Restart automatico driver critico        | M2 sched  | Media   |
| `DRIVER_OP_IRQ_SUBSCRIBE` + coda IPC     | M2 sched  | Media   |
| Rate limiting per processo/opcode        | M2        | Bassa   |
| Filtro audit log (`drvlog errors|dev=N`) | —         | Bassa   |

### M5 — DMA sandbox (futuro)

**Obiettivo**: Un driver può eseguire trasferimenti DMA entro un buffer kernel-controlled.

| Task                                     | Dipendenza  |
|------------------------------------------|------------|
| `DRIVER_OP_DMA_SETUP` con buffer owned   | M4 MAP_MEM |
| Verifica IOVA vs device mem_base/mem_size| M4         |
| Cleanup DMA buffer su crash driver       | M4 restart |

### M6 — Isolamento multiplo (futuro)

**Obiettivo**: Due driver dello stesso tipo possono girare in address space separati senza
interferire.

| Task                                     | Dipendenza |
|------------------------------------------|-----------|
| Namespace dispositivi per processo       | M4        |
| Firma ELF driver (secos note estesa)     | M4        |
| Audit persistente su RAMFS               | M5        |

---

*Documento generato su branch `milestone/M1`. Aggiornare a ogni milestone che modifica
il driver dispatch, il modello memoria, o le syscall.*
