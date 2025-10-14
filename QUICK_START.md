# Guida Rapida - SecOS Kernel

## 🚀 Setup e compilazione

### 1. Assicurati di avere tutti i file

Dovresti avere questi file nella stessa directory:

```
boot.asm
idt_asm.asm
kernel.c
idt.c
idt.h
timer.c
timer.h
keyboard.c
keyboard.h
shell.c
shell.h
terminal.h
linker.ld
Makefile
```

### 2. Compila il kernel

```bash
# Pulisci eventuali build precedenti
make clean

# Compila e crea l'immagine ISO
make iso

# Esegui in QEMU
make run
```

## 🎮 Usa la shell

Una volta avviato vedrai:

```
==================================
   Kernel 64-bit con GRUB
==================================

Kernel avviato in modalita' Long Mode (64-bit)!
[OK] Bootloader Multiboot rilevato
[OK] Inizializzazione IDT...
[OK] Inizializzazione tastiera PS/2...
[OK] Sistema pronto!

==================================
   Benvenuto in SecOS Shell!
==================================

Digita 'help' per vedere i comandi disponibili.

secos$
```

### Comandi disponibili:

```bash
help      # Lista comandi
info      # Info sistema (mostra anche freq timer)
uptime    # Mostra tempo di attività
echo test # Stampa "test"
sleep 500 # Attende 500ms
colors    # Mostra i colori VGA
clear     # Pulisce schermo
reboot    # Riavvia
```

## ⏱️ Test del Timer

Prova questi comandi per testare il timer:

```bash
# Mostra l'uptime attuale
uptime

# Attendi 1 secondo
sleep 1000

# Mostra di nuovo l'uptime (dovrebbe essere ~1 secondo in più)
uptime

# Test con valori diversi
sleep 100   # 100ms
sleep 2000  # 2 secondi
sleep 5000  # 5 secondi
```

Il timer è configurato a **1000 Hz** (1 tick ogni millisecondo), quindi puoi:
- Misurare il tempo con precisione al millisecondo
- Usare `sleep` per creare pause nel codice
- Vedere l'uptime del sistema in ore:minuti:secondi

## 🐛 Risoluzione problemi

### Errore di compilazione

```bash
# Reinstalla i pacchetti necessari
sudo apt install nasm gcc binutils grub-common grub-pc-bin xorriso

# Riprova
make clean && make iso
```

### QEMU non si apre (WSL2)

Se usi WSL2 e QEMU non mostra la finestra:

1. Compila in WSL2: `make iso`
2. Usa VirtualBox su Windows con l'ISO da `\\wsl$\Ubuntu-22.04\...\myos.iso`

### Tastiera non funziona

- Assicurati che QEMU abbia il focus della finestra
- Prova a cliccare nella finestra QEMU
- Se usi una VM, assicurati che la tastiera sia catturata

## 📝 Note importanti

- La tastiera usa il layout **US QWERTY**
- Supporta **Shift** e **Caps Lock**
- Il **backspace** funziona correttamente
- I comandi sono **case-sensitive**
- Il timer genera **1000 interrupt al secondo** (1ms per tick)
- `sleep` accetta valori da **1 a 10000** millisecondi
- `uptime` mostra il tempo in formato **ore:minuti:secondi**

## 🎯 Prossimi passi

Ora che hai un kernel funzionante con shell e timer, puoi:

1. **Multitasking cooperativo** - Usa il timer per lo scheduling
2. **Heap allocator** - Implementa kmalloc/kfree
3. **File System** - Inizia con un semplice FS in RAM
4. **Driver aggiuntivi** - Serial port per debug, mouse PS/2
5. **Syscalls** - Crea un'interfaccia user/kernel mode

Divertiti a sviluppare il tuo OS! 🚀