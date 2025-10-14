# Gestione Memoria in SecOS

## 📋 Architettura

Il sistema di gestione memoria di SecOS è diviso in due livelli:

### 1. Physical Memory Manager (PMM)
Gestisce la memoria fisica a livello di **frame** (4KB ciascuno).

**Caratteristiche:**
- Usa un **bitmap** per tracciare frame liberi/allocati
- Ogni bit rappresenta un frame da 4KB
- Parsing della memory map Multiboot per rilevare RAM
- API semplice: `pmm_alloc_frame()` / `pmm_free_frame()`

**Inizializzazione:**
```c
pmm_init(multiboot_info);  // Chiamato da kernel_main
```

**Uso:**
```c
void* frame = pmm_alloc_frame();  // Alloca un frame da 4KB
if (frame) {
    // Usa il frame...
    pmm_free_frame(frame);        // Libera quando finito
}
```

### 2. Heap Allocator
Fornisce allocazione dinamica di **memoria di dimensioni variabili**.

**Caratteristiche:**
- API simile a malloc/free
- Usa il PMM internamente per espandere l'heap
- Lista di blocchi liberi con coalescing
- Header per ogni blocco (dimensione + flag)

**Uso:**
```c
char* buffer = kmalloc(1024);     // Alloca 1KB
if (buffer) {
    // Usa il buffer...
    kfree(buffer);                 // Libera quando finito
}

// Allocazione allineata
void* aligned = kmalloc_aligned(size, 16);  // Allineato a 16 byte
```

## 🔍 Comandi Shell
## Virtual Memory Manager (VMM)

Il VMM introduce:
* Funzioni base: `vmm_init`, `vmm_map`, `vmm_unmap`, `vmm_translate`, `vmm_alloc_page`.
* Funzioni per spazi utente (address space separati): `vmm_space_create_user`, `vmm_map_in_space`, `vmm_alloc_page_in_space`, `vmm_map_user_code_in_space`, `vmm_alloc_user_page_in_space`, `vmm_alloc_user_stack_in_space`, `vmm_translate_in_space`, `vmm_harden_user_space`.
* Physmap: mappa tutta la memoria fisica disponibile in un range alto (`VMM_PHYSMAP_BASE = 0xFFFF888000000000`) usando pagine da 2MB (huge pages) per ridurre il numero di tabelle.
* Region allocator (stub): permette di registrare regioni virtuali per future allocazioni on-demand (es. heap utente, stack, cache FS).
* Page Fault handler: se la pagina non è presente e l'indirizzo cade in una regione registrata, viene allocata (demand-zero). Altrimenti viene generato panic.

### W^X + NX
Il kernel applica la politica "Write XOR Execute" alle sue regioni:
* `.text` → RX
* `.rodata` → R (NX)
* `.data` / `.bss` → RW (NX)
* Stack kernel → RW (NX)
* Pagine utente di codice → RX (USER)
* Pagine utente di dati/stack → RW (NX | USER)

NX è abilitato impostando EFER.NXE in `boot.asm`. Il loader ELF rifiuta segmenti che richiedono contemporaneamente W|X.

### Stack Utente con Guard Page
Lo stack utente è allocato come N pagine mappate sotto `USER_STACK_TOP` e una pagina non mappata immediatamente sotto per rilevare overflow (page fault se oltrepassata). Questo è gestito da `vmm_alloc_user_stack_in_space`.

### Address Space Utente
Ogni processo ha una copia iniziale delle PML4 del kernel (clone) poi "hardened" con `vmm_harden_user_space` che rimuove il bit USER dalle regioni alte del kernel per prevenire accessi accidentali. Le pagine utente sono marcate USER e solo quelle sono accessibili in ring3 (futuro).
Durante la creazione (`vmm_space_create_user`) le entry PDPT corrispondenti al range user (CODE/DATA/STACK) vengono azzerate per evitare che mapping huge ereditati o PDT/kernel page tables già riempite causino collisioni quando il loader ELF tenta di mappare nuove pagine (risolve l'errore "map fallita" nelle chiamate successive di `elfload`).

### Traduzione In-Space
Per operare su pagine di un address space non attivo si usa `vmm_translate_in_space` che risolve l'indirizzo virtuale rispetto alle tabelle dell'altro spazio, utile per copia segmenti ELF e zeroing BSS.

### Smappare pagine di uno spazio
La nuova API `vmm_unmap_in_space(space, virt)` permette di rimuovere la pagina da uno spazio utente senza switchare CR3, liberando il frame fisico e lasciando invariati gli altri spazi. Usata da `elf_unload_process` per rilasciare le pagine codice/dati/stack del processo.

### PCB (Process Control Block) Campi Memoria
Il PCB contiene:
* `space` → puntatore al suo address space
* `entry` → entry point (RIP iniziale)
* `stack_top` → top stack utente
* `kstack_top` → (stub) stack kernel per future transizioni ring3
* `regs` → snapshot registri iniziali (RIP/RSP/RFLAGS ecc.)
* `manifest` → puntatore a descrittore di sicurezza (stub non ancora usato)

### Layout virtuale (proposto)
| Area | Descrizione |
|------|-------------|
| Basso (< 16MB) | Kernel iniziale identity (transitorio) |
| Physmap (alto) | Mappatura diretta fisica (non eseguibile) |
| Heap kernel | Da definire, 4KB pages RW |
| Spazio utente | User code/data, con separazione tramite USER bit |
| Cache FS | Buffer blocchi FAT32/exFAT |
| Guard pages | Pagine non mappate per rilevare overflow |

### Prossimi passi
1. Migrazione completa a higher-half eliminando identity mapping iniziale.
2. Region allocator avanzato (merge/fragmentation) + demand paging per heap utente.
3. Cache blocchi (LRU) e astrazione dispositivo a blocchi.
4. Syscall gate e transizione a ring3 con TSS.rsp0 (uso di `kstack_top`).
5. Manifest di sicurezza ELF (`.note.secos`) con policy W^X enforcement esteso.
6. Scheduler + context switch salvando `regs`.


### mem
Mostra statistiche complete della memoria:
```bash
secos$ mem

     Memoria totale:   128 MB
     Memoria usata:    2 MB
     Memoria libera:   126 MB

=== Statistiche Heap ===
Allocata:   5120 bytes
Liberata:   2048 bytes
In uso:     3072 bytes
```

### memtest
Esegue test di allocazione/deallocazione:
```bash
secos$ memtest

Test allocazione memoria...
Test 1: Allocazione di 5 blocchi da 1KB...
  [OK] Blocco 1 allocato
  [OK] Blocco 2 allocato
  ...
Test 2: Liberazione blocchi...
  [OK] Blocco 1 liberato
  ...
Test completato!
```

## 🛠️ API Completa

### PMM (Physical Memory Manager)

```c
// Inizializza il PMM
void pmm_init(void* mboot_info);

// Alloca un frame fisico da 4KB
void* pmm_alloc_frame(void);

// Libera un frame fisico
void pmm_free_frame(void* addr);

// Statistiche
uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_used_memory(void);
uint64_t pmm_get_free_memory(void);
void pmm_print_stats(void);
```

### Heap Allocator

```c
// Inizializza l'heap
void heap_init(void);

// Alloca memoria dinamica
void* kmalloc(size_t size);

// Alloca memoria allineata
void* kmalloc_aligned(size_t size, size_t alignment);

// Libera memoria
void kfree(void* ptr);

// Statistiche
void heap_print_stats(void);
```

## 📊 Layout Memoria

```
0x00000000  +------------------+
            | BIOS / HW        |
0x00100000  +------------------+
            | Kernel Code      | ← Caricato da GRUB a 1MB
            | Kernel Data      |
            | Kernel BSS       |
_kernel_end +------------------+
            | PMM Bitmap       | ← Bitmap frame allocation
            +------------------+
            | Heap             | ← Cresce dinamicamente
            +------------------+
            | Frame liberi     | ← Gestiti dal PMM
            |                  |
            |    ...           |
            +------------------+
```

## 🚀 Prossimi Passi

Per caricare applicazioni userspace abbiamo introdotto:

1. **ELF Loader** (base) → carica segmenti PT_LOAD con verifiche W^X e allineamento.
2. **Address Space** → creato per processo con stack utente dedicato e guard page.
3. **PCB** → struttura minima per identità e registri iniziali.

In arrivo:
* Syscalls
* Scheduler
* Manifest parser
* File system

## 💡 Note Implementative

### Bitmap Allocator
- Efficiente per allocazioni di frame singoli
- O(n) per trovare frame libero (accettabile)
- Compatto in memoria (1 bit per frame)

### Heap Allocator
- First-fit algorithm
- Coalescing automatico dei blocchi liberi
- Espansione automatica dell'heap tramite PMM
- Header di 24 byte per blocco

### Limitazioni Attuali
- Heap non può ridursi (solo crescere)
- Nessuna protezione contro double-free
- Nessuna gestione fragmentazione avanzata
- Allocazioni > 4KB allocano frame multipli

### Possibili Miglioramenti
- Buddy allocator per il PMM
- Slab allocator per oggetti piccoli
- Guard pages per rilevare overflow
- Memory pools per allocazioni frequenti