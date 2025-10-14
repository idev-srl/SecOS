# Makefile per kernel 64-bit con GRUB

# Compilatori e assembler
AS = nasm
CC = gcc
LD = ld

# Flag per l'assembler
ASFLAGS = -f elf64

# Flag per il compilatore C
CFLAGS = -ffreestanding -O2 -nostdlib -lgcc -m64 \
         -mno-red-zone -mno-sse -mno-sse2

# Flag per il linker
LDFLAGS = -n -T linker.ld

# File oggetto
OBJS = boot.o idt_asm.o kernel.o idt.o keyboard.o timer.o pmm.o heap.o tss.o panic.o shell.o vmm.o

# Nome del kernel finale
KERNEL = kernel.bin

# Nome dell'immagine ISO
ISO = myos.iso

# Directory per GRUB
ISODIR = isodir

.PHONY: all clean run iso

all: $(KERNEL)

# Compila il bootloader assembly
boot.o: boot.asm
	$(AS) $(ASFLAGS) $< -o $@

# Compila gli handler interrupt assembly
idt_asm.o: idt_asm.asm
	$(AS) $(ASFLAGS) $< -o $@

# Compila il kernel C
kernel.o: kernel.c terminal.h idt.h keyboard.h timer.h multiboot.h pmm.h heap.h shell.h
	$(CC) $(CFLAGS) -c $< -o $@

# Compila IDT
idt.o: idt.c idt.h
	$(CC) $(CFLAGS) -c $< -o $@

# Compila keyboard driver
keyboard.o: keyboard.c keyboard.h
	$(CC) $(CFLAGS) -c $< -o $@

# Compila timer driver
timer.o: timer.c timer.h
	$(CC) $(CFLAGS) -c $< -o $@

# Compila PMM
pmm.o: pmm.c pmm.h multiboot.h terminal.h
	$(CC) $(CFLAGS) -c $< -o $@

# Compila Heap
heap.o: heap.c heap.h pmm.h terminal.h
	$(CC) $(CFLAGS) -c $< -o $@

# Compila TSS
tss.o: tss.c tss.h pmm.h terminal.h
	$(CC) $(CFLAGS) -c $< -o $@

# Compila Panic handler
panic.o: panic.c panic.h terminal.h
	$(CC) $(CFLAGS) -c $< -o $@

# Compila shell
shell.o: shell.c shell.h keyboard.h terminal.h timer.h pmm.h heap.h
	$(CC) $(CFLAGS) -c $< -o $@

# Compila VMM
vmm.o: vmm.c vmm.h pmm.h terminal.h
	$(CC) $(CFLAGS) -c $< -o $@

# Collega tutti gli oggetti per creare il kernel
$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# Crea l'immagine ISO bootable
iso: $(KERNEL)
	mkdir -p $(ISODIR)/boot/grub
	cp $(KERNEL) $(ISODIR)/boot/kernel.bin
	echo 'set timeout=3' > $(ISODIR)/boot/grub/grub.cfg
	echo 'set default=0' >> $(ISODIR)/boot/grub/grub.cfg
	echo '' >> $(ISODIR)/boot/grub/grub.cfg
	echo 'menuentry "My OS 64-bit Kernel" {' >> $(ISODIR)/boot/grub/grub.cfg
	echo '    multiboot /boot/kernel.bin' >> $(ISODIR)/boot/grub/grub.cfg
	echo '}' >> $(ISODIR)/boot/grub/grub.cfg
	grub-mkrescue --output=$(ISO) $(ISODIR) 2>&1 | tee grub-mkrescue.log
	@echo "ISO creata: $(ISO)"
	@echo "Verifica contenuto ISO..."
	@ls -lh $(ISO)

# Esegui con QEMU
run: iso
	qemu-system-x86_64 -cdrom $(ISO)

# Pulisci i file generati
clean:
	rm -f $(OBJS) $(KERNEL)
	rm -rf $(ISODIR)
	rm -f $(ISO)
