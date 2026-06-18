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
CRYPTO_DIR = crypto
INCLUDES = -I. -I$(BOOT_DIR) -I$(ARCH_DIR) -I$(KERNEL_DIR) -I$(DRIVERS_DIR) -I$(MM_DIR) -I$(LIB_DIR) -I$(FS_DIR) -I$(CRYPTO_DIR)

ASFLAGS = -f elf64
BUILD_TS := $(shell date -u +%Y%m%d%H%M%S)
GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo NOHASH)
# Nota: CONFIG_MULTIBOOT e CONFIG_UEFI sono ora definiti in config.h (dual-boot support)
CFLAGS  = -ffreestanding -O2 -nostdlib -lgcc -m64 -mcmodel=kernel -fno-pic -fno-pie -mno-red-zone -mno-sse -mno-sse2 $(INCLUDES) \
		  -DBUILD_TS="\"$(BUILD_TS)\"" -DGIT_HASH="\"$(GIT_HASH)\"" $(CFLAGS_EXTRA)
LDFLAGS = -n -T linker.ld

SRC_ASM = $(BOOT_DIR)/boot.asm $(ARCH_DIR)/idt_asm.asm $(ARCH_DIR)/syscall_asm.asm $(ARCH_DIR)/context_switch_asm.asm
SRC_C   = \
	$(KERNEL_DIR)/kernel.c \
	$(ARCH_DIR)/idt.c $(ARCH_DIR)/tss.c $(ARCH_DIR)/context_switch.c \
	$(DRIVERS_DIR)/keyboard.c $(DRIVERS_DIR)/timer.c $(DRIVERS_DIR)/rtc.c \
	$(DRIVERS_DIR)/serial.c \
	$(DRIVERS_DIR)/fb.c $(DRIVERS_DIR)/fb_console.c \
	$(DRIVERS_DIR)/pci.c $(DRIVERS_DIR)/virtio_blk.c \
	$(MM_DIR)/pmm.c $(MM_DIR)/heap.c $(MM_DIR)/vmm.c \
	$(MM_DIR)/vma.c \
	$(MM_DIR)/elf.c \
	$(MM_DIR)/elf_unload.c \
	$(MM_DIR)/elf_manifest.c \
	$(MM_DIR)/elf_sign.c \
	$(MM_DIR)/user_copy.c \
	$(KERNEL_DIR)/process.c \
	$(KERNEL_DIR)/panic.c $(KERNEL_DIR)/shell.c $(KERNEL_DIR)/sched.c \
	$(KERNEL_DIR)/syscall.c \
	$(KERNEL_DIR)/syscall_trap.c \
	$(KERNEL_DIR)/driver_if.c \
	$(KERNEL_DIR)/ipc.c \
	$(KERNEL_DIR)/selftest.c \
	user/testdriver.c \
	$(LIB_DIR)/terminal.c \
	$(FS_DIR)/ramfs.c \
	$(FS_DIR)/vfs.c \
	$(FS_DIR)/ramfs_vfs.c \
	$(FS_DIR)/block.c \
	$(FS_DIR)/fat32.c \
	$(FS_DIR)/ext2.c \
	$(FS_DIR)/ext2ramdev.c \
	$(CRYPTO_DIR)/sha256.c \
	$(CRYPTO_DIR)/sha512.c \
	$(CRYPTO_DIR)/ed25519.c \
	$(CRYPTO_DIR)/crypto_selftest.c

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
$(CRYPTO_DIR)/%.o: $(CRYPTO_DIR)/%.c
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

# --- User programs (signed) ---
# Build + sign the user programs and regenerate the embedded C headers. The
# kernel includes the committed crypto/user_*_elf.h, so this only needs to run
# when a user program changes (keeps the kernel build python-free).
USER_CFLAGS = -ffreestanding -nostdlib -fno-pie -no-pie -mno-red-zone -mcmodel=large -m64 -O2 -Wall -Iuser
.PHONY: user-progs
user-progs:
	$(CC) $(USER_CFLAGS) -c user/crt0.S       -o user/crt0.o
	$(CC) $(USER_CFLAGS) -c user/note.S       -o user/note.o
	$(CC) $(USER_CFLAGS) -c user/note_driver.S -o user/note_driver.o
	$(CC) $(USER_CFLAGS) -c user/libsecos.c   -o user/libsecos.o
	$(CC) $(USER_CFLAGS) -c user/hello.c      -o user/hello.o
	$(CC) $(USER_CFLAGS) -c user/drvprobe.c   -o user/drvprobe.o
	$(CC) $(USER_CFLAGS) -c user/driver_demo.c -o user/driver_demo.o
	$(CC) $(USER_CFLAGS) -c user/userprobe.c  -o user/userprobe.o
	$(CC) $(USER_CFLAGS) -c user/note_maxmem.S -o user/note_maxmem.o
	$(CC) $(USER_CFLAGS) -c user/ipc_send.c   -o user/ipc_send.o
	$(CC) $(USER_CFLAGS) -c user/ipc_recv.c   -o user/ipc_recv.o
	$(LD) -T user/user.ld -o user/hello.elf user/crt0.o user/note.o user/libsecos.o user/hello.o
	python3 tools/secos-sign user/hello.elf --dev
	python3 tools/elf2h.py user/hello.elf user_hello_elf crypto/user_hello_elf.h
	# [M11] Signed user-space driver (PROC_TYPE_DRIVER manifest)
	$(LD) -T user/user.ld -o user/driver_demo.elf user/crt0.o user/note_driver.o user/libsecos.o user/drvprobe.o user/driver_demo.o
	python3 tools/secos-sign user/driver_demo.elf --dev
	python3 tools/elf2h.py user/driver_demo.elf user_driver_elf crypto/user_driver_elf.h
	# [M11] Signed plain-user probe (PROC_TYPE_USER manifest) — driver calls denied
	$(LD) -T user/user.ld -o user/userprobe.elf user/crt0.o user/note.o user/libsecos.o user/drvprobe.o user/userprobe.o
	python3 tools/secos-sign user/userprobe.elf --dev
	python3 tools/elf2h.py user/userprobe.elf user_userprobe_elf crypto/user_userprobe_elf.h
	# [M13] IPC producer/consumer (signed, normal manifest)
	$(LD) -T user/user.ld -o user/ipc_send.elf user/crt0.o user/note.o user/libsecos.o user/ipc_send.o
	python3 tools/secos-sign user/ipc_send.elf --dev
	python3 tools/elf2h.py user/ipc_send.elf user_ipc_send_elf crypto/user_ipc_send_elf.h
	$(LD) -T user/user.ld -o user/ipc_recv.elf user/crt0.o user/note.o user/libsecos.o user/ipc_recv.o
	python3 tools/secos-sign user/ipc_recv.elf --dev
	python3 tools/elf2h.py user/ipc_recv.elf user_ipc_recv_elf crypto/user_ipc_recv_elf.h
	# [M13] max_mem-limited build of hello (manifest limit too small -> refused at load)
	$(LD) -T user/user.ld -o user/maxmem.elf user/crt0.o user/note_maxmem.o user/libsecos.o user/hello.o
	python3 tools/secos-sign user/maxmem.elf --dev
	python3 tools/elf2h.py user/maxmem.elf user_maxmem_elf crypto/user_maxmem_elf.h
	# [M15] crashtest: deliberately faults in ring-3 -> kernel kills it (no halt)
	$(CC) $(USER_CFLAGS) -c user/crashtest.c   -o user/crashtest.o
	$(LD) -T user/user.ld -o user/crashtest.elf user/crt0.o user/note.o user/libsecos.o user/crashtest.o
	python3 tools/secos-sign user/crashtest.elf --dev
	python3 tools/elf2h.py user/crashtest.elf user_crash_elf crypto/user_crash_elf.h
	# [M16] exec model: parent spawns child with argv, blocks in waitpid for status
	$(CC) $(USER_CFLAGS) -c user/m16_child.c   -o user/m16_child.o
	$(LD) -T user/user.ld -o user/m16_child.elf user/crt0.o user/note.o user/libsecos.o user/m16_child.o
	python3 tools/secos-sign user/m16_child.elf --dev
	python3 tools/elf2h.py user/m16_child.elf user_m16_child_elf crypto/user_m16_child_elf.h
	$(CC) $(USER_CFLAGS) -c user/m16_parent.c  -o user/m16_parent.o
	$(LD) -T user/user.ld -o user/m16_parent.elf user/crt0.o user/note.o user/libsecos.o user/m16_parent.o
	python3 tools/secos-sign user/m16_parent.elf --dev
	python3 tools/elf2h.py user/m16_parent.elf user_m16_parent_elf crypto/user_m16_parent_elf.h
	# [M17] blocking sleep
	$(CC) $(USER_CFLAGS) -c user/m17_sleeper.c -o user/m17_sleeper.o
	$(LD) -T user/user.ld -o user/m17_sleeper.elf user/crt0.o user/note.o user/libsecos.o user/m17_sleeper.o
	python3 tools/secos-sign user/m17_sleeper.elf --dev
	python3 tools/elf2h.py user/m17_sleeper.elf user_m17_sleeper_elf crypto/user_m17_sleeper_elf.h
	# [M18] dynamic memory: malloc/free + mmap/mprotect
	$(CC) $(USER_CFLAGS) -c user/m18_mem.c     -o user/m18_mem.o
	$(LD) -T user/user.ld -o user/m18_mem.elf user/crt0.o user/note.o user/libsecos.o user/m18_mem.o
	python3 tools/secos-sign user/m18_mem.elf --dev
	python3 tools/elf2h.py user/m18_mem.elf user_m18_mem_elf crypto/user_m18_mem_elf.h
	# [M19] copy-on-write fork
	$(CC) $(USER_CFLAGS) -c user/m19_fork.c    -o user/m19_fork.o
	$(LD) -T user/user.ld -o user/m19_fork.elf user/crt0.o user/note.o user/libsecos.o user/m19_fork.o
	python3 tools/secos-sign user/m19_fork.elf --dev
	python3 tools/elf2h.py user/m19_fork.elf user_m19_fork_elf crypto/user_m19_fork_elf.h
	@echo "user-progs: built+signed hello + driver_demo + userprobe + ipc_send + ipc_recv + maxmem + crashtest + m16_{child,parent} + m17_sleeper + m18_mem + m19_fork -> crypto/*.h"

# --- Test disk images (virtio-blk) ---
# A small FAT32 data disk with a known test file. Used by the storage smoke
# tests; attach with -drive file=$(DISK_IMG),if=virtio,format=raw.
DISK_IMG = disk.img
.PHONY: disk disk-fat32 disk-ext2 disk-ext4
disk: disk-fat32
disk-fat32:
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=64 status=none
	mkfs.fat -F 32 -n SECOSDATA $(DISK_IMG) >/dev/null
	printf 'hello from disk\n' > /tmp/secos_disktest.txt
	mcopy -i $(DISK_IMG) /tmp/secos_disktest.txt ::HELLO.TXT
	@echo "disk-fat32: $(DISK_IMG) (FAT32, 64MB, contains HELLO.TXT)"
disk-ext2:
	rm -rf /tmp/secos_diskstage && mkdir -p /tmp/secos_diskstage
	printf 'hello from disk\n' > /tmp/secos_diskstage/hello.txt
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=64 status=none
	mkfs.ext2 -q -L SECOSDATA -d /tmp/secos_diskstage $(DISK_IMG)
	@echo "disk-ext2: $(DISK_IMG) (ext2, 64MB, contains hello.txt)"
disk-ext4:
	rm -rf /tmp/secos_diskstage && mkdir -p /tmp/secos_diskstage
	printf 'hello from disk\n' > /tmp/secos_diskstage/hello.txt
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=64 status=none
	mkfs.ext4 -q -O ^has_journal,^metadata_csum -L SECOSDATA -d /tmp/secos_diskstage $(DISK_IMG)
	@echo "disk-ext4: $(DISK_IMG) (ext4 no-journal/no-csum, extents+64bit, 64MB, hello.txt)"

# Run with QEMU (graphical window — needs a working display backend)
run: iso
	qemu-system-x86_64 -cdrom $(ISO) -debugcon stdio -global isa-debugcon.iobase=0xe9

# Run headless with a virtio-blk disk attached (debugcon to stdio).
.PHONY: run-disk
run-disk: iso
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),if=virtio,format=raw \
		-debugcon stdio -global isa-debugcon.iobase=0xe9 -display none -no-reboot -m 256M

# Headless: interact with the shell entirely in this terminal over COM1.
# No GUI/VNC needed (works where WSLg/GTK does not). Ctrl-A X to quit QEMU.
.PHONY: run-serial
run-serial: iso
	qemu-system-x86_64 -cdrom $(ISO) -serial stdio -display none -no-reboot

# Run with a VNC server on :0 (connect a VNC viewer to localhost:5900)
.PHONY: run-vnc
run-vnc: iso
	qemu-system-x86_64 -cdrom $(ISO) -display vnc=:0 -debugcon stdio -global isa-debugcon.iobase=0xe9

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
