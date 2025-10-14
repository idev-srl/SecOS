## Makefile riorganizzato con sottodirectory
AS      = nasm
CC      = gcc
LD      = ld

BOOT_DIR    = boot
ARCH_DIR    = arch/x86
KERNEL_DIR  = kernel
DRIVERS_DIR = drivers
MM_DIR      = mm
LIB_DIR     = lib

INCLUDES = -I. -I$(BOOT_DIR) -I$(ARCH_DIR) -I$(KERNEL_DIR) -I$(DRIVERS_DIR) -I$(MM_DIR) -I$(LIB_DIR)

ASFLAGS = -f elf64
CFLAGS  = -ffreestanding -O2 -nostdlib -lgcc -m64 -mno-red-zone -mno-sse -mno-sse2 $(INCLUDES)
LDFLAGS = -n -T linker.ld

SRC_ASM = $(BOOT_DIR)/boot.asm $(ARCH_DIR)/idt_asm.asm
SRC_C   = \
	$(KERNEL_DIR)/kernel.c \
	$(ARCH_DIR)/idt.c $(ARCH_DIR)/tss.c \
	$(DRIVERS_DIR)/keyboard.c $(DRIVERS_DIR)/timer.c $(DRIVERS_DIR)/rtc.c \
	$(DRIVERS_DIR)/fb.c \
	$(MM_DIR)/pmm.c $(MM_DIR)/heap.c $(MM_DIR)/vmm.c \
	$(MM_DIR)/elf.c \
	mm/elf_unload.c \
	elf_manifest.c \
	$(KERNEL_DIR)/process.c \
	$(KERNEL_DIR)/panic.c $(KERNEL_DIR)/shell.c \
	$(LIB_DIR)/terminal.c

OBJS_ASM = $(SRC_ASM:%.asm=%.o)
OBJS_C   = $(SRC_C:%.c=%.o)
OBJS     = $(OBJS_ASM) $(OBJS_C)

KERNEL  = kernel.bin
ISO     = myos.iso
ISODIR  = isodir

.PHONY: all clean run iso

all: $(KERNEL)

$(BOOT_DIR)/%.o: $(BOOT_DIR)/%.asm
	$(AS) $(ASFLAGS) $< -o $@
$(ARCH_DIR)/%.o: $(ARCH_DIR)/%.asm
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL_DIR)/%.o: $(KERNEL_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(ARCH_DIR)/%.o: $(ARCH_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(DRIVERS_DIR)/%.o: $(DRIVERS_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(MM_DIR)/%.o: $(MM_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(LIB_DIR)/%.o: $(LIB_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# Crea l'immagine ISO bootable
iso: $(KERNEL)
	mkdir -p $(ISODIR)/boot/grub
	cp $(KERNEL) $(ISODIR)/boot/kernel.bin
	echo 'set timeout=3' > $(ISODIR)/boot/grub/grub.cfg
	echo 'set default=0' >> $(ISODIR)/boot/grub/grub.cfg
	echo 'set gfxpayload=1024x768x32' >> $(ISODIR)/boot/grub/grub.cfg
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
	rm -rf $(ISODIR) $(ISO) grub-mkrescue.log

tree:
	@echo "ASM: $(SRC_ASM)"
	@echo "C  : $(SRC_C)"
