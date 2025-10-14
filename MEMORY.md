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
* Funzioni: `vmm_init`, `vmm_map`, `vmm_unmap`, `vmm_translate`, `vmm_alloc_page`, `vmm_dump_entry`.
* Physmap: mappa tutta la memoria fisica disponibile in un range alto (`VMM_PHYSMAP_BASE = 0xFFFF888000000000`) usando pagine da 2MB (huge pages) per ridurre il numero di tabelle.
* Region allocator (stub): permette di registrare regioni virtuali per future allocazioni on-demand (es. heap utente, stack, cache FS).
* Page Fault handler: se la pagina non è presente e l'indirizzo cade in una regione registrata, viene allocata (demand-zero). Altrimenti viene generato panic.

### NX Bit
Abilitato impostando EFER.NXE in `boot.asm`. Tutte le pagine physmap sono marcate NX per evitare esecuzione di codice da pagine di dati.

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
1. Migrare kernel a higher-half eliminando identity mapping.
2. Completare region allocator (merge/fragmentation handling).
3. Implementare cache blocchi (LRU) e astrazione dispositivo a blocchi.
4. Introduzione ELF loader e address space per processi user.
5. Syscall gate e transizione ring3 con TSS.rsp0.


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

Per caricare applicazioni userspace avremo bisogno di:

1. **Virtual Memory Manager (VMM)**
   - Paging avanzato
   - Separazione kernel/user space
   - Page fault handler

2. **ELF Loader**
   - Parser formato ELF64
   - Caricamento sezioni
   - Symbol resolution

3. **Process Manager**
   - Strutture task/process
   - Context switching
   - Scheduling

4. **File System**
   - Supporto FAT32/exFAT
   - VFS (Virtual File System)
   - Driver disco (AHCI/IDE)

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