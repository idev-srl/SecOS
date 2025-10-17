## SecOS Kernel - Build Makefile
## Copyright (c) 2025 iDev srl
## Author: Luigi De Astis <l.deastis@idev-srl.com>
## SPDX-License-Identifier: MIT
## Build system with subdirectories
AS      = nasm
CC      = gcc
LD      = ld

BOOT_DIR    = boot
ARCH_DIR    = arch/x86
KERNEL_DIR  = kernel
DRIVERS_DIR = drivers
MM_DIR      = mm
LIB_DIR     = lib

FS_DIR     = fs
INCLUDES = -I. -I$(BOOT_DIR) -I$(ARCH_DIR) -I$(KERNEL_DIR) -I$(DRIVERS_DIR) -I$(MM_DIR) -I$(LIB_DIR) -I$(FS_DIR)

ASFLAGS = -f elf64
BUILD_TS := $(shell date -u +%Y%m%d%H%M%S)
GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo NOHASH)
CFLAGS  = -ffreestanding -O2 -nostdlib -lgcc -m64 -mno-red-zone -mno-sse -mno-sse2 $(INCLUDES) \
		  -DBUILD_TS="\"$(BUILD_TS)\"" -DGIT_HASH="\"$(GIT_HASH)\""
LDFLAGS = -n -T linker.ld

SRC_ASM = $(BOOT_DIR)/boot.asm $(ARCH_DIR)/idt_asm.asm $(ARCH_DIR)/syscall_asm.asm
SRC_C   = \
	$(KERNEL_DIR)/kernel.c \
	$(ARCH_DIR)/idt.c $(ARCH_DIR)/tss.c \
	$(DRIVERS_DIR)/keyboard.c $(DRIVERS_DIR)/timer.c $(DRIVERS_DIR)/rtc.c \
	$(DRIVERS_DIR)/fb.c $(DRIVERS_DIR)/fb_console.c \
	$(MM_DIR)/pmm.c $(MM_DIR)/heap.c $(MM_DIR)/vmm.c \
	$(MM_DIR)/elf.c \
	$(MM_DIR)/elf_unload.c \
	$(MM_DIR)/elf_manifest.c \
	$(KERNEL_DIR)/process.c \
	$(KERNEL_DIR)/panic.c $(KERNEL_DIR)/shell.c $(KERNEL_DIR)/sched.c \
	$(KERNEL_DIR)/syscall.c \
	$(KERNEL_DIR)/driver_if.c \
	user/testdriver.c \
	$(LIB_DIR)/terminal.c \
	$(FS_DIR)/ramfs.c \
	$(FS_DIR)/vfs.c \
	$(FS_DIR)/ramfs_vfs.c \
	$(FS_DIR)/block.c \
	$(FS_DIR)/fat32.c \
	$(FS_DIR)/ext2.c \
	$(FS_DIR)/ext2ramdev.c

OBJS_ASM = $(SRC_ASM:%.asm=%.o)
OBJS_C   = $(SRC_C:%.c=%.o)
OBJS     = $(OBJS_ASM) $(OBJS_C)
KERNEL  = kernel.bin
ISO     = myos.iso
ISODIR  = isodir

.PHONY: all clean run iso

all: $(KERNEL)

# Guard: warn if a stray kernel.c exists in root (unused by build)
ifneq (,$(wildcard kernel.c))
$(warning WARNING: Found unused kernel.c in root; remove to avoid confusion.)
endif

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
$(FS_DIR)/%.o: $(FS_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# Create bootable ISO image
iso: $(KERNEL)
	mkdir -p $(ISODIR)/boot/grub
	cp $(KERNEL) $(ISODIR)/boot/kernel.bin
	echo 'set timeout=3' > $(ISODIR)/boot/grub/grub.cfg
	echo 'set default=0' >> $(ISODIR)/boot/grub/grub.cfg
	echo 'insmod all_video' >> $(ISODIR)/boot/grub/grub.cfg
	echo 'insmod gfxterm' >> $(ISODIR)/boot/grub/grub.cfg
	echo 'set gfxmode=1024x768x32,1024x768,800x600x32,800x600,640x480x32,640x480,auto' >> $(ISODIR)/boot/grub/grub.cfg
	echo 'set gfxpayload=keep' >> $(ISODIR)/boot/grub/grub.cfg
	echo 'terminal_output gfxterm' >> $(ISODIR)/boot/grub/grub.cfg
	echo '' >> $(ISODIR)/boot/grub/grub.cfg
	echo 'menuentry "SecOS x64" {' >> $(ISODIR)/boot/grub/grub.cfg
	echo '    multiboot2 /boot/kernel.bin' >> $(ISODIR)/boot/grub/grub.cfg
	echo '}' >> $(ISODIR)/boot/grub/grub.cfg
	grub-mkrescue --output=$(ISO) $(ISODIR) 2>&1 | tee grub-mkrescue.log
	@echo "ISO creata: $(ISO)"
	@echo "Verifica contenuto ISO..."
	@ls -lh $(ISO)

# Run with QEMU
run: iso
	qemu-system-x86_64 -cdrom $(ISO) -debugcon stdio -global isa-debugcon.iobase=0xe9

# Clean generated files
clean:
	rm -f $(OBJS) $(KERNEL)
	rm -rf $(ISODIR) $(ISO) grub-mkrescue.log

tree:
	@echo "ASM: $(SRC_ASM)"
	@echo "C  : $(SRC_C)"
