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
# Nota: CONFIG_MULTIBOOT e CONFIG_UEFI sono ora definiti in config.h (dual-boot support)
CFLAGS  = -ffreestanding -O2 -nostdlib -lgcc -m64 -mno-red-zone -mno-sse -mno-sse2 $(INCLUDES) \
		  -DBUILD_TS="\"$(BUILD_TS)\"" -DGIT_HASH="\"$(GIT_HASH)\""
LDFLAGS = -n -T linker.ld

SRC_ASM = $(BOOT_DIR)/boot.asm $(ARCH_DIR)/idt_asm.asm $(ARCH_DIR)/syscall_asm.asm $(ARCH_DIR)/context_switch_asm.asm
SRC_C   = \
	$(KERNEL_DIR)/kernel.c \
	$(ARCH_DIR)/idt.c $(ARCH_DIR)/tss.c $(ARCH_DIR)/context_switch.c \
	$(DRIVERS_DIR)/keyboard.c $(DRIVERS_DIR)/timer.c $(DRIVERS_DIR)/rtc.c \
	$(DRIVERS_DIR)/fb.c $(DRIVERS_DIR)/fb_console.c \
	$(MM_DIR)/pmm.c $(MM_DIR)/heap.c $(MM_DIR)/vmm.c \
	$(MM_DIR)/elf.c \
	$(MM_DIR)/elf_unload.c \
	$(MM_DIR)/elf_manifest.c \
	$(MM_DIR)/user_copy.c \
	$(KERNEL_DIR)/process.c \
	$(KERNEL_DIR)/panic.c $(KERNEL_DIR)/shell.c $(KERNEL_DIR)/sched.c \
	$(KERNEL_DIR)/syscall.c \
	$(KERNEL_DIR)/syscall_trap.c \
	$(KERNEL_DIR)/driver_if.c \
	$(KERNEL_DIR)/selftest.c \
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

# UEFI output directories
DIST_DIR = dist
EFI_BOOT_DIR = $(DIST_DIR)/EFI/BOOT
UEFI_LOADER_ELF = uefi_loader.elf
UEFI_APP = $(EFI_BOOT_DIR)/BOOTX64.EFI

.PHONY: uefi uefi-clean

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
	rm -rf $(DIST_DIR) $(UEFI_LOADER_ELF)

tree:
	@echo "ASM: $(SRC_ASM)"
	@echo "C  : $(SRC_C)"

# --- UEFI build (Strategy B: external bootloader) ---
UEFI_SRC = uefi/boot.c uefi/elf_load.c
UEFI_HELLO_SRC = uefi/hello.c
UEFI_OBJS = $(UEFI_SRC:%.c=%.o)
UEFI_HELLO_OBJS = $(UEFI_HELLO_SRC:%.c=%.o)
UEFI_HELLO_APP = $(EFI_BOOT_DIR)/HELLO.EFI
UEFI_CFLAGS = -ffreestanding -fshort-wchar -O2 -mno-red-zone -m64 -fno-stack-protector -fPIE -fno-omit-frame-pointer -fno-asynchronous-unwind-tables -fno-unwind-tables -fcf-protection=none -mno-sse -mno-mmx $(INCLUDES) -DCONFIG_UEFI
UEFI_CRT0 = uefi/crt0.o

uefi/%.o: uefi/%.c uefi/efi.h
	$(CC) $(UEFI_CFLAGS) -c $< -o $@

uefi/crt0.o: uefi/crt0.s
	$(CC) -c $< -o $@

uefi: $(UEFI_CRT0) $(UEFI_OBJS)
	mkdir -p $(EFI_BOOT_DIR)
	$(LD) -nostdlib -znocombreloc -shared -Bsymbolic -T /usr/lib/elf_x86_64_efi.lds \
		-L /usr/lib $(UEFI_CRT0) $(UEFI_OBJS) -lgnuefi -lefi -o $(UEFI_LOADER_ELF)
	objcopy -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel -j .rela -j .reloc --target=efi-app-x86_64 $(UEFI_LOADER_ELF) $(UEFI_APP)
	python3 -c "import struct; f=open('$(UEFI_APP)','r+b'); f.seek(0x3c); pe=struct.unpack('<I',f.read(4))[0]; f.seek(pe+22); c=struct.unpack('<H',f.read(2))[0]; f.seek(pe+22); f.write(struct.pack('<H',c&~0x0001)); f.close(); print('PE Characteristics: 0x{:04x} -> 0x{:04x}'.format(c, c&~0x0001))"
	@echo "✅ UEFI loader: $(UEFI_APP) ($(shell ls -lh $(UEFI_APP) | awk '{print $$5}'))"
	@cp $(KERNEL) $(DIST_DIR)/kernel.elf || true

uefi-clean:
	rm -rf $(DIST_DIR) $(UEFI_LOADER_ELF) $(UEFI_OBJS)

uefi_hello: $(UEFI_HELLO_OBJS)
	mkdir -p $(EFI_BOOT_DIR)
	$(LD) -nostdlib -znocombreloc -T $(GNU_EFI_LDS) -shared -Bsymbolic -L$(GNU_EFI_LIB) $(GNU_EFI_CRT) $(UEFI_HELLO_OBJS) -o uefi_hello.elf -lgnuefi -lefi
	objcopy -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel -j .rela -j .reloc --target=efi-app-x86_64 uefi_hello.elf $(UEFI_HELLO_APP)
	@echo "Minimal UEFI hello built: $(UEFI_HELLO_APP)"

# Avvio rapido UEFI (richiede script run_uefi.sh)
.PHONY: run-uefi
run-uefi: uefi
	@chmod +x run_uefi.sh 2>/dev/null || true
	./run_uefi.sh
