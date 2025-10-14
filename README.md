# Kernel 64-bit con GRUB

Un kernel scritto in C che si avvia in modalità Long Mode (64-bit) utilizzando GRUB come bootloader, con supporto per tastiera PS/2 e shell interattiva.

## Caratteristiche

- ✅ Avvio in Long Mode (64-bit)
- ✅ Supporto Multiboot per GRUB
- ✅ Gestione base del terminale VGA
- ✅ Identity mapping iniziale (transitorio)
- ✅ Stack funzionante
- ✅ **Interrupt Descriptor Table (IDT)**
- ✅ **Timer PIT con interrupt periodici (IRQ0)**
- ✅ **Sistema di tick e uptime**
- ✅ **Funzioni di sleep (bloccanti)**
- ✅ **Driver tastiera PS/2 con buffer circolare (IRQ1)**
- ✅ **Physical Memory Manager (PMM)** - frame allocator
- ✅ **Heap Allocator** - kmalloc/kfree con espansione dinamica
- ✅ **Virtual Memory Manager (VMM)** con supporto spazi utente e traduzione in-space
- ✅ **NX Bit** e policy **W^X** per regioni kernel e segmenti ELF
- ✅ **ELF64 Loader** (segmenti PT_LOAD, enforcement W^X, p_align, tracking pagine per processo)
- ✅ **Address Space** per processi utente + stack con guard page
- ✅ **PCB esteso** (state, registri, manifest, lista pagine mappate per unload preciso)
- ✅ **Parsing Multiboot Memory Map**
- ✅ **Shell interattiva con comandi**
- ✅ Gestione errori durante il boot & unload processi (elfunload)
- ✅ Comando ps (elenco processi base)

## Requisiti

### Software necessario:
- **NASM** - Assembler per il codice boot
- **GCC** - Compilatore C (con supporto cross-compilation per x86_64)
- **GNU ld** - Linker
- **GRUB** - Per creare l'immagine ISO bootable
- **xorriso** - Necessario per grub-mkrescue
- **QEMU** - Per testare il kernel (opzionale ma consigliato)

### Installazione su Ubuntu/Debian:
```bash
sudo apt update
sudo apt install nasm gcc binutils grub-common grub-pc-bin xorriso qemu-system-x86
```

### Installazione su Arch Linux:
```bash
sudo pacman -S nasm gcc binutils grub xorriso qemu
```

## Compilazione

## Struttura del progetto (semplificata)

```
.
├── boot.asm      # Codice assembly per entrare in long mode
├── idt_asm.asm   # Handler interrupt in assembly
├── kernel.c      # Codice principale del kernel
├── idt.c/h       # Gestione Interrupt Descriptor Table
├── timer.c/h     # Driver timer PIT (Programmable Interval Timer)
├── keyboard.c/h  # Driver tastiera PS/2
├── multiboot.h   # Strutture Multiboot standard
├── pmm.c/h       # Physical Memory Manager
├── heap.c/h      # Heap allocator
├── vmm.c/h       # Virtual Memory Manager + spazi utente
├── elf.c/h       # Loader ELF64
├── process.c/h   # Process creation (PCB)
├── shell.c/h     # Shell interattiva
├── terminal.h    # API terminale VGA condivisa
├── linker.ld     # Script del linker
├── Makefile      # Script di compilazione
└── README.md     # Questo file
```

## Comandi della shell

Una volta avviato il kernel, potrai usare questi comandi:

- **help** - Mostra la lista dei comandi disponibili
- **clear** - Pulisce lo schermo
- **echo [testo]** - Stampa il testo specificato
- **info** - Mostra informazioni sul sistema
- **uptime** - Mostra il tempo di attività del sistema
- **sleep [ms]** - Attende per N millisecondi (1-10000)
- **mem** - Mostra statistiche memoria (PMM + Heap)
- **memtest** - Test di allocazione e deallocazione memoria
- **memstress** - Stress allocator heap
- **elfload** - Carica ELF di test embedded
- **elfunload** - Distrugge ultimo processo caricato
- **ps** - Elenca processi attivi (minimal)
- **colors** - Test dei colori VGA disponibili
- **reboot** - Riavvia il sistema

## Requisiti

### Compilare il kernel:
```bash
make
```
Questo comando genera il file `kernel.bin`.

### Creare l'immagine ISO:
```bash
make iso
```
Questo comando crea `myos.iso`, un'immagine ISO bootable con GRUB.

### Eseguire con QEMU:
```bash
make run
```
Questo comando compila, crea l'ISO ed esegue il kernel in QEMU.

### Pulire i file generati:
```bash
make clean
```

## Come funziona

### 1. Boot Process (boot.asm)
- GRUB carica il kernel in modalità protetta a 32-bit
- Il codice verifica il supporto per CPUID e Long Mode
- Imposta le page tables per il paging in 64-bit (4MB identity mapping)
- Abilita PAE (Physical Address Extension)
- Configura l'EFER MSR per abilitare il long mode
- Abilita il paging
- Salta al codice a 64-bit

### 2. Inizializzazione IDT (idt.c)
- Crea l'Interrupt Descriptor Table con 256 entry
- Rimappa il PIC (Programmable Interrupt Controller)
- Configura l'handler per IRQ0 (timer)
- Configura l'handler per IRQ1 (tastiera)
- Abilita gli interrupt

### 3. Timer PIT (timer.c)
- Configura il Programmable Interval Timer a 1000 Hz (1ms per tick)
- Gestisce gli interrupt del timer (IRQ0)
- Mantiene un contatore di tick da 64-bit
- Fornisce funzioni per uptime e sleep bloccante
- Base per lo scheduling futuro

### 4. Driver Tastiera (keyboard.c)
- Gestisce gli interrupt della tastiera (IRQ1)
- Converte scancode PS/2 in caratteri ASCII
- Supporta Shift, Caps Lock e caratteri speciali
- Buffer circolare per l'input
- Funzioni bloccanti e non-bloccanti per leggere caratteri

### 5. Shell (shell.c)
- Loop principale che legge comandi dall'utente
- Parser semplice per separare comando e argomenti
- Esecuzione comandi integrati
- Prompt colorato e gestione backspace

### 6. Kernel (kernel.c)
- Inizializza il terminale VGA in modalità testo
- Mostra informazioni di boot
- Verifica il magic number Multiboot
- Inizializza IDT, timer, tastiera e shell
- Passa il controllo alla shell interattiva

## Caratteristiche

- ✅ Avvio in Long Mode (64-bit)
- ✅ Supporto Multiboot per GRUB
- ✅ Gestione base del terminale VGA
- ✅ Identity mapping delle prime 1GB di memoria
- ✅ Stack funzionante
- ✅ Gestione errori durante il boot

## Test su hardware reale

Per testare su hardware reale:

1. Scrivi l'ISO su una chiavetta USB:
   ```bash
   sudo dd if=myos.iso of=/dev/sdX bs=4M status=progress
   ```
   (Sostituisci `/dev/sdX` con il dispositivo corretto)

2. Avvia il computer dalla chiavetta USB

## Sviluppi futuri

Questo kernel è un ottimo punto di partenza. Puoi estenderlo con:
- **Scheduler preemptive** - Context switching usando PCB.regs
- **Transizione a ring3** - Syscall gate e TSS.rsp0
- **Manifest di sicurezza** - Parsing sezione `.note.secos` per policy
## Manifest di Sicurezza (.note.secos)

Il loader cerca una nota ELF (PT_NOTE) con name `SECOS` e type `QSEC` contenente una struttura:
```
uint32_t version;
uint32_t flags;   // MANIFEST_FLAG_REQUIRE_WX_BLOCK, STACK_GUARD, NX_DATA, RX_CODE
uint64_t max_mem; // limite attivo: se usage > max_mem abort
uint64_t entry_hint; // entry attesa (0 = ignora)
```
Se presente viene validata (entry match, flag supportati). Segmenti W|X vengono rifiutati a prescindere. Il campo max_mem viene confrontato con memoria totale occupata (pagine * 4096) post-caricamento e prima dell'avvio processo: se eccede il limite il processo viene abortito.
- **ASLR** - Randomizzazione indirizzi codice e stack
- **File system** - FAT32/exFAT + VFS
- **File system** - Sistema di file in RAM o su disco
- **Driver per dispositivi** - Mouse, serial port, AHCI/IDE
- **Networking** - Stack TCP/IP base
- **Shell avanzata** - Piping, redirection, job control
- **Syscalls** - Interfaccia user/kernel space

## Debugging

Per debug con QEMU e GDB:
```bash
qemu-system-x86_64 -cdrom myos.iso -s -S
```

In un altro terminale:
```bash
gdb kernel.bin
(gdb) target remote localhost:1234
(gdb) continue
```

## Risorse utili

- [OSDev Wiki](https://wiki.osdev.org/) - Risorsa completa sullo sviluppo OS
- [Intel Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) - Documentazione CPU Intel
- [AMD Manual](https://www.amd.com/en/support/tech-docs) - Documentazione CPU AMD
- [GRUB Documentation](https://www.gnu.org/software/grub/manual/) - Manuale GRUB

## Licenza

Questo codice è fornito come esempio educativo e può essere usato liberamente.

## Note Memoria & Sicurezza

Il kernel applica W^X alle sue sezioni e marca NX le regioni di dati. Le pagine utente sono mappate con USER, mentre quelle kernel condivise mantengono USER=0 dopo hardening (`vmm_harden_user_space`). Lo stack utente ha una guard page non mappata per intercettare overflow. Il loader ELF verifica che nessun segmento sia sia scrivibile che eseguibile e valida l'allineamento (p_align 0 o 0x1000). Ogni pagina code/data/stack viene tracciata nel PCB per unload preciso e accounting memoria (manifest max_mem).