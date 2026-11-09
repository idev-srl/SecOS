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
NET_DIR    = net
INCLUDES = -I. -I$(BOOT_DIR) -I$(ARCH_DIR) -I$(KERNEL_DIR) -I$(DRIVERS_DIR) -I$(MM_DIR) -I$(LIB_DIR) -I$(FS_DIR) -I$(CRYPTO_DIR) -I$(NET_DIR)

ASFLAGS = -f elf64
# [M37] Reproducible builds: BUILD_TS is derived from SOURCE_DATE_EPOCH when set
# (the reproducible-builds.org standard), so the same source tree yields a
# byte-identical kernel.bin (the measured-boot hash is then stable). Falls back to
# the current UTC time for ordinary dev builds.
#   make iso SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)   # reproducible
BUILD_TS := $(shell if [ -n "$$SOURCE_DATE_EPOCH" ]; then date -u -d @$$SOURCE_DATE_EPOCH +%Y%m%d%H%M%S; else date -u +%Y%m%d%H%M%S; fi)
GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo NOHASH)
# Nota: CONFIG_MULTIBOOT e CONFIG_UEFI sono ora definiti in config.h (dual-boot support)
# [M40] Boot keyboard layout (drivers/keymap.c): `make iso KEYMAP=it`. The code
# default is "us"; switch at runtime with `loadkeys it|us`, which saves the
# choice as KEYMAP= in /etc/vconsole.conf. The kernel applies that file at boot,
# so the build default only matters while the file does not exist.
KEYMAP ?=
KEYMAP_DEF := $(if $(KEYMAP),-DSECOS_KEYMAP_DEFAULT='"$(KEYMAP)"',)
# GUI at boot (opt-in): `make iso GUI_AUTOSTART=1` makes the GUI start at boot
# when /etc/gui-autostart does not exist (the kernel passes the default to
# `startgui --auto`, the default inittab's gui entry, as secos.gui_autostart=1
# on /proc/cmdline). /etc/gui-autostart ("1"/"0") overrides it. Same as
# CFLAGS_EXTRA=-DSECOS_GUI_AUTOSTART. The default boot stays the text console.
GUI_AUTOSTART ?=
GUI_DEF := $(if $(filter 1 yes y,$(GUI_AUTOSTART)),-DSECOS_GUI_AUTOSTART,)
# [M43] Console policy (docs/USERS.md). Default: init runs /bin/login on the
# console. `make iso CONSOLE=root` (-DSECOS_CONSOLE_ROOT) keeps the root bash of
# before, with no login; tools/selftest.sh builds that way. It sets the default
# /etc/inittab (and switches an unmodified default one) and the
# secos.console= hint on /proc/cmdline.
CONSOLE ?= login
CONSOLE_DEF := $(if $(filter root,$(CONSOLE)),-DSECOS_CONSOLE_ROOT,)
# [M52] Kernel lock mode when the command line has neither `bkl=on` nor
# `bkl=off` (kernel/bkl.c). Default off: no syscall, user fault or teardown
# takes the big kernel lock (docs/devlog/M52-bkl-removal.md). The third GRUB
# entry boots the other mode: "fallback: big kernel lock (bkl=on)" serializes
# every syscall as M48 did, if a problem shows up on some machine. `make iso
# BKL_DEFAULT=on` builds the old default. Read from the environment too, so
# `BKL_DEFAULT=on tools/selftest.sh` runs every gate in the other mode.
BKL_DEFAULT ?= off
BKL_DEF := $(if $(filter off,$(BKL_DEFAULT)),-DSECOS_BKL_DEFAULT_OFF,)
BKL_ALT := $(if $(filter off,$(BKL_DEFAULT)),on,off)
comma := ,
BKL_ALT_LABEL := $(if $(filter off,$(BKL_DEFAULT)),fallback: big kernel lock$(comma) bkl=on,no big kernel lock: bkl=off)
CFLAGS  = -ffreestanding -O2 -nostdlib -lgcc -m64 -mcmodel=kernel -fno-pic -fno-pie -mno-red-zone -mno-sse -mno-sse2 \
		  -fstack-protector-strong -mstack-protector-guard=global $(INCLUDES) \
		  -DBUILD_TS="\"$(BUILD_TS)\"" -DGIT_HASH="\"$(GIT_HASH)\"" $(KEYMAP_DEF) $(GUI_DEF) $(CONSOLE_DEF) $(BKL_DEF) $(CFLAGS_EXTRA)
# Header dependency tracking: every kernel object also depends on the headers it
# includes (gcc writes foo.d next to foo.o). Without it, changing only a header
# (e.g. a regenerated embedded program crypto/user_*_elf.h) left the objects that
# #include it stale, so `make iso` silently shipped the old programs.
DEPFLAGS = -MMD -MP
CFLAGS  += $(DEPFLAGS)
LDFLAGS = -n -T linker.ld
# Compiler-flag tracking: -MMD tracks headers, not flags, so an object built
# with other flags (CFLAGS_EXTRA, KEYMAP, a compiler upgrade...) was linked as
# is. That happened: a months-old tsc.o with the pre-M44 %gs:0x28 stack canary
# panicked the boot. FLAGS_STAMP holds the effective compiler/assembler flags
# and the compiler version, and is rewritten (at parse time, with $(file), so
# quotes in the defines need no shell escaping) only when they change; every
# kernel object depends on it. BUILD_TS and GIT_HASH are left out, or every
# build (and every commit) would recompile everything; as before, they are
# refreshed only in the files that get recompiled.
FLAGS_STAMP := .cflags.stamp
FLAGS_SIG := cc=$(CC) $(shell $(CC) -dumpfullversion 2>/dev/null) \
	cflags=$(filter-out -DBUILD_TS=% -DGIT_HASH=%,$(CFLAGS)) as=$(AS) asflags=$(ASFLAGS)
ifneq ($(file <$(FLAGS_STAMP)),$(FLAGS_SIG))
$(file >$(FLAGS_STAMP),$(FLAGS_SIG))
endif
$(FLAGS_STAMP):
	$(file >$@,$(FLAGS_SIG))

SRC_ASM = $(BOOT_DIR)/boot.asm $(ARCH_DIR)/idt_asm.asm $(ARCH_DIR)/syscall_asm.asm $(ARCH_DIR)/context_switch_asm.asm \
          $(ARCH_DIR)/uaccess.asm
SRC_C   = \
	$(KERNEL_DIR)/kernel.c \
	$(KERNEL_DIR)/bkl.c $(KERNEL_DIR)/klock.c $(KERNEL_DIR)/lockdep.c \
	$(ARCH_DIR)/idt.c $(ARCH_DIR)/tss.c $(ARCH_DIR)/context_switch.c \
	$(ARCH_DIR)/lapic.c \
	$(ARCH_DIR)/acpi.c \
	$(ARCH_DIR)/acpi_dmar.c $(ARCH_DIR)/acpi_dmar_pci.c \
	$(ARCH_DIR)/tsc.c \
	$(ARCH_DIR)/percpu.c \
	$(ARCH_DIR)/tlb.c \
	$(ARCH_DIR)/irq.c \
	$(ARCH_DIR)/vtd.c $(ARCH_DIR)/vtd_pt.c $(ARCH_DIR)/vtd_ir.c \
	$(DRIVERS_DIR)/keyboard.c $(DRIVERS_DIR)/timer.c $(DRIVERS_DIR)/rtc.c \
	$(DRIVERS_DIR)/serial.c \
	$(DRIVERS_DIR)/fb.c $(DRIVERS_DIR)/fb_console.c $(DRIVERS_DIR)/vt.c \
	$(DRIVERS_DIR)/input.c $(DRIVERS_DIR)/keymap.c $(DRIVERS_DIR)/ps2mouse.c \
	$(DRIVERS_DIR)/fbdev.c $(DRIVERS_DIR)/pci_registry.c \
	$(DRIVERS_DIR)/pci.c $(DRIVERS_DIR)/pci_msi.c $(DRIVERS_DIR)/virtio_blk.c \
	$(DRIVERS_DIR)/ahci.c $(DRIVERS_DIR)/nvme.c \
	$(DRIVERS_DIR)/xhci.c $(DRIVERS_DIR)/usb.c \
	$(DRIVERS_DIR)/usb_hid.c $(DRIVERS_DIR)/usb_msc.c \
	$(DRIVERS_DIR)/usb_hub.c \
	$(MM_DIR)/pmm.c $(MM_DIR)/heap.c $(MM_DIR)/vmm.c \
	$(MM_DIR)/vma.c \
	$(MM_DIR)/pagecache.c \
	$(MM_DIR)/pagecache_test.c \
	$(MM_DIR)/elf.c \
	$(MM_DIR)/elf_unload.c \
	$(MM_DIR)/elf_manifest.c \
	$(MM_DIR)/elf_sign.c \
	$(MM_DIR)/user_copy.c \
	$(KERNEL_DIR)/process.c \
	$(KERNEL_DIR)/panic.c $(KERNEL_DIR)/shell.c $(KERNEL_DIR)/sched.c \
	$(KERNEL_DIR)/syscall.c \
	$(KERNEL_DIR)/syscall_trap.c \
	$(KERNEL_DIR)/signal.c \
	$(KERNEL_DIR)/cap.c \
	$(KERNEL_DIR)/cred.c \
	$(KERNEL_DIR)/driver_if.c \
	$(KERNEL_DIR)/ipc.c \
	$(KERNEL_DIR)/pipe.c \
	$(KERNEL_DIR)/tty.c \
	$(KERNEL_DIR)/tty_test.c \
	$(KERNEL_DIR)/pty.c \
	$(KERNEL_DIR)/power.c \
	$(KERNEL_DIR)/sysroot.c \
	$(KERNEL_DIR)/kclock.c \
	$(KERNEL_DIR)/klog.c \
	$(KERNEL_DIR)/kiface.c \
	$(KERNEL_DIR)/smp.c \
	$(KERNEL_DIR)/kaslr.c \
	$(KERNEL_DIR)/pkg.c \
	$(KERNEL_DIR)/selftest.c \
	$(KERNEL_DIR)/exec_test.c \
	$(KERNEL_DIR)/iommu.c $(KERNEL_DIR)/iommu_test.c $(KERNEL_DIR)/iommu_ir_test.c $(KERNEL_DIR)/dma.c \
	$(KERNEL_DIR)/udrv.c \
	$(KERNEL_DIR)/ldwatch_test.c \
	$(LIB_DIR)/terminal.c \
	$(FS_DIR)/ramfs.c \
	$(FS_DIR)/vfs.c \
	$(FS_DIR)/ramfs_vfs.c \
	$(FS_DIR)/block.c \
	$(FS_DIR)/partition.c \
	$(FS_DIR)/fat32.c \
	$(FS_DIR)/exfat.c \
	$(FS_DIR)/exfat_test.c \
	$(FS_DIR)/blkstress.c \
	$(FS_DIR)/vfs_test.c \
	$(FS_DIR)/fsops_test.c \
	$(FS_DIR)/fsvirt_test.c \
	$(FS_DIR)/ext4locktest.c \
	$(FS_DIR)/c4_fsstress.c \
	$(FS_DIR)/ntfs.c \
	$(FS_DIR)/ntfs_test.c \
	$(FS_DIR)/ext2.c \
	$(FS_DIR)/ext2ramdev.c \
	$(FS_DIR)/devfs.c \
	$(FS_DIR)/procfs.c \
	$(FS_DIR)/sysfs.c \
	$(CRYPTO_DIR)/sha256.c \
	$(CRYPTO_DIR)/sha512.c \
	$(CRYPTO_DIR)/sha1.c \
	$(CRYPTO_DIR)/wpa2.c \
	$(CRYPTO_DIR)/wpa2_eapol.c \
	$(CRYPTO_DIR)/ed25519.c \
	$(CRYPTO_DIR)/csprng.c \
	$(CRYPTO_DIR)/crypto_selftest.c \
	$(DRIVERS_DIR)/ath9k.c \
	$(DRIVERS_DIR)/e1000.c \
	$(DRIVERS_DIR)/e1000e.c \
	$(DRIVERS_DIR)/vmxnet3.c \
	$(DRIVERS_DIR)/igc.c \
	$(NET_DIR)/net_core.c \
	$(NET_DIR)/net_lock.c \
	$(NET_DIR)/eth.c \
	$(NET_DIR)/arp.c \
	$(NET_DIR)/ipv4.c \
	$(NET_DIR)/icmp.c \
	$(NET_DIR)/udp.c \
	$(NET_DIR)/dhcp.c \
	$(NET_DIR)/netctl.c \
	$(NET_DIR)/dns.c \
	$(NET_DIR)/tcp.c \
	$(NET_DIR)/socket.c \
	$(NET_DIR)/wpa2_supplicant.c

OBJS_ASM = $(SRC_ASM:%.asm=%.o)
OBJS_C   = $(SRC_C:%.c=%.o)
# [M29] AP startup trampoline: a flat binary (org 0x8000) wrapped into an object
# so the kernel can copy it to low memory at runtime.
AP_TRAMPOLINE = $(BOOT_DIR)/ap_trampoline.o
# [secure-boot] kernel code-signing note (.note.secos, GAS-assembled .S)
KERNEL_NOTE = $(BOOT_DIR)/kernel_note.o
# [M39] ported upstream ath9k_hw WiFi driver (built by tools/port-ath9k.sh against
# the Linux-compat shim; prebuilt + committed so the kernel build needs no network).
ATH9K_HW = drivers/ath9k_port/ath9k_hw.o
# [M45] KASLR core (boot/kaslr_core.c): runs from the LOW .boot section before
# the high half is mapped, so every allocated section gets a ".boot" prefix.
KASLR_CORE = $(BOOT_DIR)/kaslr_core.o
OBJS     = $(OBJS_ASM) $(OBJS_C) $(AP_TRAMPOLINE) $(KERNEL_NOTE) $(KASLR_CORE) $(ATH9K_HW)
DEPS     = $(OBJS_C:%.o=%.d)
-include $(DEPS)
# Every object built here is rebuilt when the flags change (not the prebuilt,
# committed ath9k_hw.o).
$(filter-out $(ATH9K_HW),$(OBJS)): $(FLAGS_STAMP)

# Keep `make` (no target) building the kernel: the rule below would otherwise be
# the first target in the file and become the default goal.
.DEFAULT_GOAL := all
$(ATH9K_HW):
	bash tools/port-ath9k.sh
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

# [M29] AP trampoline: assemble flat (org 0x8000), wrap into an ELF object whose
# _binary_ap_trampoline_bin_{start,end} symbols the kernel copies to 0x8000.
# Explicit rule wins over the generic boot/%.o:%.asm pattern below.
$(BOOT_DIR)/ap_trampoline.o: $(BOOT_DIR)/ap_trampoline.asm
	$(AS) -f bin $< -o $(BOOT_DIR)/ap_trampoline.bin
	cd $(BOOT_DIR) && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 ap_trampoline.bin ap_trampoline.o

$(BOOT_DIR)/%.o: $(BOOT_DIR)/%.asm
	$(AS) $(ASFLAGS) $< -o $@

# [M45] KASLR core for the Multiboot2 path: kernel flags, but no stack
# protector / unwind tables / jump tables / synthesized memset (none of which
# could be reached from the low .boot section), then every allocated section is
# renamed .boot.* so linker.ld places it in .boot. A reference to a high-half
# symbol would fail the link (truncated PC32): the intended tripwire.
KASLR_CORE_CFLAGS = $(filter-out -fstack-protector-strong,$(CFLAGS)) -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-jump-tables -fno-tree-loop-distribute-patterns
$(BOOT_DIR)/kaslr_core.o: $(BOOT_DIR)/kaslr_core.c $(BOOT_DIR)/kaslr.h $(KASLR_TOOL)
	$(CC) $(KASLR_CORE_CFLAGS) -c $< -o $(BOOT_DIR)/kaslr_core.raw.o
	objcopy --remove-section=.note.gnu.property --prefix-alloc-sections=.boot $(BOOT_DIR)/kaslr_core.raw.o $@
	python3 $(KASLR_TOOL) boot-only $@

# [secure-boot] GAS-assembled kernel signing note (data only; minimal flags).
$(BOOT_DIR)/kernel_note.o: $(BOOT_DIR)/kernel_note.S
	$(CC) -c $< -o $@
$(ARCH_DIR)/%.o: $(ARCH_DIR)/%.asm
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL_DIR)/%.o: $(KERNEL_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(ARCH_DIR)/%.o: $(ARCH_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@
# [M40] drivers/keymap.o bakes in KEYMAP: rebuild it whenever the value changes.
# The stamp is rewritten (at parse time) only when its content would change.
KEYMAP_STAMP := .keymap.stamp
$(shell echo '$(KEYMAP)' | cmp -s - $(KEYMAP_STAMP) 2>/dev/null || echo '$(KEYMAP)' > $(KEYMAP_STAMP))
$(DRIVERS_DIR)/keymap.o: $(KEYMAP_STAMP)
# Same for GUI_AUTOSTART, which only kernel/kernel.c reads.
GUI_STAMP := .gui-autostart.stamp
$(shell echo '$(GUI_DEF)' | cmp -s - $(GUI_STAMP) 2>/dev/null || echo '$(GUI_DEF)' > $(GUI_STAMP))
$(KERNEL_DIR)/kernel.o $(KERNEL_DIR)/sysroot.o $(FS_DIR)/procfs.o: $(GUI_STAMP)
# Same for CONSOLE (kernel/sysroot.c: default inittab; fs/procfs.c: cmdline).
CONSOLE_STAMP := .console.stamp
$(shell echo '$(CONSOLE_DEF)' | cmp -s - $(CONSOLE_STAMP) 2>/dev/null || echo '$(CONSOLE_DEF)' > $(CONSOLE_STAMP))
$(KERNEL_DIR)/sysroot.o $(FS_DIR)/procfs.o: $(CONSOLE_STAMP)
# Same for BKL_DEFAULT (kernel/bkl.c only).
BKL_STAMP := .bkl-default.stamp
$(shell echo '$(BKL_DEF)' | cmp -s - $(BKL_STAMP) 2>/dev/null || echo '$(BKL_DEF)' > $(BKL_STAMP))
$(KERNEL_DIR)/bkl.o: $(BKL_STAMP)

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
$(NET_DIR)/%.o: $(NET_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# SSH public keys baked into the image: `make iso SSH_AUTHORIZED_KEYS=~/.ssh/id.pub`
# (one or more .pub / authorized_keys files, space-separated). The kernel installs
# them as /root/.ssh/authorized_keys at boot. Without the variable no key is baked
# and Dropbear refuses every login. The generated header lives in build/ (ignored
# by git) and is rewritten only when the key set changes. Never commit a key.
SSH_AUTHORIZED_KEYS ?=
AUTHKEYS_H = build/authorized_keys.h
.PHONY: FORCE
FORCE:
$(AUTHKEYS_H): FORCE
	@python3 tools/gen-authorized-keys.py $@ $(SSH_AUTHORIZED_KEYS)
$(KERNEL_DIR)/kernel.o: $(AUTHKEYS_H)

# [M43] First root password, baked as a hash: `make iso ROOT_PASSWORD=...`
# (hashed on the build host with the vendored musl SHA-512 crypt) or
# ROOT_PASSWORD_HASH='$$6$$...' (a ready hash, e.g. `openssl passwd -6`). At
# boot the kernel gives it to root only while root's password is still unset
# ("!") in /etc/shadow. Only the hash reaches the image; build/root_pwhash.h is
# ignored by git and keeps its salt while the password is unchanged.
ROOT_PASSWORD ?=
ROOT_PASSWORD_HASH ?=
ROOT_PWHASH_H = build/root_pwhash.h
$(ROOT_PWHASH_H): export ROOT_PASSWORD := $(ROOT_PASSWORD)
$(ROOT_PWHASH_H): export ROOT_PASSWORD_HASH := $(ROOT_PASSWORD_HASH)
$(ROOT_PWHASH_H): FORCE
	@tools/gen-root-pwhash.sh $@
$(KERNEL_DIR)/sysroot.o: $(ROOT_PWHASH_H)

# [M45] KASLR: two-pass link. Pass 1 (--emit-relocs) gives the relocations;
# tools/kaslr-relocs.py turns the absolute high-half ones into the compact table
# (boot/kaslr.h), which is linked back as section .kaslr (after .data, before
# .bss, so nothing it describes moves). Pass 2 is re-extracted and must match;
# the final link is pass 2 without the .rela sections. -DSECOS_NO_KASLR in
# CFLAGS_EXTRA marks the table disabled (both relocators then use slide 0).
KASLR_TOOL  = tools/kaslr-relocs.py
KASLR_BIN   = build/kaslr_relocs.bin
KASLR_OBJ   = build/kaslr_relocs.o
KASLR_FLAGS = $(if $(findstring SECOS_NO_KASLR,$(CFLAGS_EXTRA)),--disabled,)
$(KERNEL): $(OBJS) linker.ld $(KASLR_TOOL)
	@mkdir -p build
	$(LD) $(LDFLAGS) --emit-relocs -o build/kernel.pass1.elf $(OBJS)
	python3 $(KASLR_TOOL) extract build/kernel.pass1.elf $(KASLR_BIN) $(KASLR_FLAGS)
	cd build && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.kaslr,alloc,load,readonly,data,contents \
		kaslr_relocs.bin kaslr_relocs.o
	$(LD) $(LDFLAGS) --emit-relocs -o build/kernel.pass2.elf $(OBJS) $(KASLR_OBJ)
	python3 $(KASLR_TOOL) check build/kernel.pass2.elf $(KASLR_BIN)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(KASLR_OBJ)
	@# [secure-boot] Sign kernel.bin in place (deterministic dev key → reproducible).
	@# The UEFI loader Ed25519-verifies this before jumping. MB2/GRUB ignores the
	@# note, so an unsigned kernel (no python3 cryptography) still boots via ISO/CI.
	@python3 tools/secos-sign $@ --dev || echo "[secure-boot] WARNING: kernel signing skipped (need python3 'cryptography'); UEFI secure boot will REFUSE this kernel."

# Create bootable ISO image. KERNEL_CMDLINE is passed on the multiboot2 line
# (e.g. `make iso KERNEL_CMDLINE=nokaslr` disables KASLR for that image).
KERNEL_CMDLINE ?=
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
	echo '    multiboot2 /boot/kernel.bin$(if $(strip $(KERNEL_CMDLINE)), $(strip $(KERNEL_CMDLINE)))' >> $(ISODIR)/boot/grub/grub.cfg
	echo '}' >> $(ISODIR)/boot/grub/grub.cfg
	echo 'menuentry "SecOS x64 (single CPU: nosmp)" {' >> $(ISODIR)/boot/grub/grub.cfg
	echo '    multiboot2 /boot/kernel.bin nosmp' >> $(ISODIR)/boot/grub/grub.cfg
	echo '}' >> $(ISODIR)/boot/grub/grub.cfg
	echo 'menuentry "SecOS x64 ($(BKL_ALT_LABEL))" {' >> $(ISODIR)/boot/grub/grub.cfg
	echo '    multiboot2 /boot/kernel.bin bkl=$(BKL_ALT)' >> $(ISODIR)/boot/grub/grub.cfg
	echo '}' >> $(ISODIR)/boot/grub/grub.cfg
	grub-mkrescue --output=$(ISO) $(ISODIR) 2>&1 | tee grub-mkrescue.log
	@echo "ISO creata: $(ISO)"
	@echo "Verifica contenuto ISO..."
	@ls -lh $(ISO)

# --- User programs (signed) ---
# Build + sign the user programs and regenerate the embedded C headers. The
# kernel includes the committed crypto/user_*_elf.h, so this only needs to run
# when a user program changes (keeps the kernel build python-free).
USER_CFLAGS = -ffreestanding -nostdlib -fno-pie -no-pie -mno-red-zone -mcmodel=large -m64 -O2 -Wall -Iuser -Iuser/include -ffunction-sections -fdata-sections
# crypt()/crypt_r() from musl 1.2.5 (MIT, third_party/musl-crypt/, unmodified;
# refresh with tools/vendor-musl-crypt.sh) + user/crypt_glue.c + the pwhash
# helpers (user/pwhash.c), archived as user/libcrypt.a - the SecOS -lcrypt.
# Put it last on the link line (after the program objects). Being an archive,
# only programs that call crypt()/crypt_r()/secos_* pull it in, so programs
# with their own crypt stub (busybox_compat.c, dropbear_compat.c) are
# unaffected. musl's internal <crypt.h> must win over user/include/crypt.h
# for the vendored files, hence the include order.
# Host KAT: tools/crypt_kat/run.sh.
MUSL_CRYPT      = third_party/musl-crypt
LIBCRYPT        = user/libcrypt.a
LIBCRYPT_CFLAGS = -I$(MUSL_CRYPT)/src/include -Iuser/musl-crypt-compat $(USER_CFLAGS) -Wno-sign-compare -Wno-missing-braces
LIBCRYPT_VENDOR = crypt crypt_sha256 crypt_sha512 crypt_blowfish
.PHONY: libcrypt
libcrypt:
	mkdir -p user/cryptobj
	rm -f user/cryptobj/*.o
	for f in $(LIBCRYPT_VENDOR); do \
	    $(CC) $(LIBCRYPT_CFLAGS) -c $(MUSL_CRYPT)/src/crypt/$$f.c -o user/cryptobj/musl_$$f.o || exit 1; \
	done
	$(CC) $(LIBCRYPT_CFLAGS) -c user/crypt_glue.c -o user/cryptobj/crypt_glue.o
	$(CC) $(USER_CFLAGS)     -c user/pwhash.c     -o user/cryptobj/pwhash.o
	rm -f $(LIBCRYPT)
	ar rcs $(LIBCRYPT) user/cryptobj/*.o
# Signed in-guest crypt() known-answer test -> user/crypt_test.elf (+ the
# embeddable crypto/user_crypt_test_elf.h). `make crypt-test` builds it on its
# own (the selftest crypt gate runs it from an ext2 data disk at /mnt).
.PHONY: crypt-test crypt-test-link
crypt-test: libcrypt
	$(CC) $(USER_CFLAGS) -c user/crt0.S       -o user/crt0.o
	$(CC) $(USER_CFLAGS) -c user/note.S       -o user/note.o
	$(CC) $(USER_CFLAGS) -c user/libsecos.c   -o user/libsecos.o
	$(CC) $(USER_CFLAGS) -c user/libc.c       -o user/libc.o
	$(MAKE) --no-print-directory crypt-test-link
crypt-test-link:
	$(CC) $(USER_CFLAGS) -c user/crypt_test.c -o user/crypt_test.o
	$(LD) --gc-sections -T user/user.ld -o user/crypt_test.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/crypt_test.o $(LIBCRYPT)
	python3 tools/secos-sign user/crypt_test.elf --dev
	python3 tools/elf2h.py user/crypt_test.elf user_crypt_test_elf crypto/user_crypt_test_elf.h
.PHONY: user-progs
user-progs: libcrypt
	$(CC) $(USER_CFLAGS) -c user/crt0.S       -o user/crt0.o
	$(CC) $(USER_CFLAGS) -c user/note.S       -o user/note.o
	$(CC) $(USER_CFLAGS) -c user/libsecos.c   -o user/libsecos.o
	$(CC) $(USER_CFLAGS) -c user/libc.c       -o user/libc.o
	$(CC) $(USER_CFLAGS) -c user/setjmp.S     -o user/setjmp.o
	$(CC) $(USER_CFLAGS) -c user/libm.c        -o user/libm.o
	$(CC) $(USER_CFLAGS) -c user/hello.c      -o user/hello.o
	$(CC) $(USER_CFLAGS) -c user/note_maxmem.S -o user/note_maxmem.o
	$(CC) $(USER_CFLAGS) -c user/ipc_send.c   -o user/ipc_send.o
	$(CC) $(USER_CFLAGS) -c user/ipc_recv.c   -o user/ipc_recv.o
	$(CC) $(USER_CFLAGS) -c user/spin.c       -o user/spin.o
	$(CC) $(USER_CFLAGS) -c user/m31_libc.c   -o user/m31_libc.o
	$(LD) --gc-sections -T user/user.ld -o user/hello.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/hello.o
	python3 tools/secos-sign user/hello.elf --dev
	python3 tools/elf2h.py user/hello.elf user_hello_elf crypto/user_hello_elf.h
	# [M13] IPC producer/consumer (signed, normal manifest)
	$(LD) --gc-sections -T user/user.ld -o user/ipc_send.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/ipc_send.o
	python3 tools/secos-sign user/ipc_send.elf --dev
	python3 tools/elf2h.py user/ipc_send.elf user_ipc_send_elf crypto/user_ipc_send_elf.h
	$(LD) --gc-sections -T user/user.ld -o user/ipc_recv.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/ipc_recv.o
	python3 tools/secos-sign user/ipc_recv.elf --dev
	python3 tools/elf2h.py user/ipc_recv.elf user_ipc_recv_elf crypto/user_ipc_recv_elf.h
	# [M29] CPU stress spinner for SMP validation (shell `stress`)
	$(LD) --gc-sections -T user/user.ld -o user/spin.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/spin.o
	python3 tools/secos-sign user/spin.elf --dev
	python3 tools/elf2h.py user/spin.elf user_spin_elf crypto/user_spin_elf.h
	# [M31] libc test (printf/string/stdlib/ctype) — signed, run from the shell
	$(LD) --gc-sections -T user/user.ld -o user/m31_libc.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/m31_libc.o
	python3 tools/secos-sign user/m31_libc.elf --dev
	python3 tools/elf2h.py user/m31_libc.elf user_m31_libc_elf crypto/user_m31_libc_elf.h
	# [M31] coreutils — busybox-style multi-call binary (echo/cat/ls/wc/...)
	$(CC) $(USER_CFLAGS) -c user/coreutils.c   -o user/coreutils.o
	$(CC) $(USER_CFLAGS) -c user/cu_text.c     -o user/cu_text.o
	$(CC) $(USER_CFLAGS) -c user/cu_file.c     -o user/cu_file.o
	$(CC) $(USER_CFLAGS) -c user/cu_misc.c     -o user/cu_misc.o
	$(LD) --gc-sections -T user/user.ld -o user/coreutils.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/coreutils.o user/cu_text.o user/cu_file.o user/cu_misc.o
	python3 tools/secos-sign user/coreutils.elf --dev
	python3 tools/elf2h.py user/coreutils.elf user_coreutils_elf crypto/user_coreutils_elf.h
	# [M32] signed package (.spkg): a dir + a text file, sealed with the dev key
	python3 tools/secos-pkg create user/testpkg.spkg --v1 user/pkg_motd.txt:/etc/pkg-motd.txt --mkdir /opt/pkgdir user/pkg_motd.txt:/opt/pkgdir/info.txt
	python3 tools/elf2h.py user/testpkg.spkg user_testpkg_spkg crypto/user_testpkg_spkg.h
	# [M32] init/service-manager + a demo service (signed)
	$(CC) $(USER_CFLAGS) -c user/svc_demo.c    -o user/svc_demo.o
	$(LD) --gc-sections -T user/user.ld -o user/svc_demo.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/svc_demo.o
	python3 tools/secos-sign user/svc_demo.elf --dev
	python3 tools/elf2h.py user/svc_demo.elf user_svc_elf crypto/user_svc_elf.h
	$(CC) $(USER_CFLAGS) -c user/init.c        -o user/init.o
	$(LD) --gc-sections -T user/user.ld -o user/init.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/init.o
	python3 tools/secos-sign user/init.elf --dev
	python3 tools/elf2h.py user/init.elf user_init_elf crypto/user_init_elf.h
	# [M13] max_mem-limited build of hello (manifest limit too small -> refused at load)
	$(LD) --gc-sections -T user/user.ld -o user/maxmem.elf user/crt0.o user/note_maxmem.o user/libsecos.o user/libc.o user/hello.o
	python3 tools/secos-sign user/maxmem.elf --dev
	python3 tools/elf2h.py user/maxmem.elf user_maxmem_elf crypto/user_maxmem_elf.h
	# [M15] crashtest: deliberately faults in ring-3 -> kernel kills it (no halt)
	$(CC) $(USER_CFLAGS) -c user/crashtest.c   -o user/crashtest.o
	$(LD) --gc-sections -T user/user.ld -o user/crashtest.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/crashtest.o
	python3 tools/secos-sign user/crashtest.elf --dev
	python3 tools/elf2h.py user/crashtest.elf user_crash_elf crypto/user_crash_elf.h
	# [M16] exec model: parent spawns child with argv, blocks in waitpid for status
	$(CC) $(USER_CFLAGS) -c user/m16_child.c   -o user/m16_child.o
	$(LD) --gc-sections -T user/user.ld -o user/m16_child.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/m16_child.o
	python3 tools/secos-sign user/m16_child.elf --dev
	python3 tools/elf2h.py user/m16_child.elf user_m16_child_elf crypto/user_m16_child_elf.h
	$(CC) $(USER_CFLAGS) -c user/m16_parent.c  -o user/m16_parent.o
	$(LD) --gc-sections -T user/user.ld -o user/m16_parent.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/m16_parent.o
	python3 tools/secos-sign user/m16_parent.elf --dev
	python3 tools/elf2h.py user/m16_parent.elf user_m16_parent_elf crypto/user_m16_parent_elf.h
	# [M17] blocking sleep
	$(CC) $(USER_CFLAGS) -c user/m17_sleeper.c -o user/m17_sleeper.o
	$(LD) --gc-sections -T user/user.ld -o user/m17_sleeper.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/m17_sleeper.o
	python3 tools/secos-sign user/m17_sleeper.elf --dev
	python3 tools/elf2h.py user/m17_sleeper.elf user_m17_sleeper_elf crypto/user_m17_sleeper_elf.h
	# [M18] dynamic memory: malloc/free + mmap/mprotect
	$(CC) $(USER_CFLAGS) -c user/m18_mem.c     -o user/m18_mem.o
	$(LD) --gc-sections -T user/user.ld -o user/m18_mem.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/m18_mem.o
	python3 tools/secos-sign user/m18_mem.elf --dev
	python3 tools/elf2h.py user/m18_mem.elf user_m18_mem_elf crypto/user_m18_mem_elf.h
	# [M19] copy-on-write fork
	$(CC) $(USER_CFLAGS) -c user/m19_fork.c    -o user/m19_fork.o
	$(LD) --gc-sections -T user/user.ld -o user/m19_fork.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/m19_fork.o
	python3 tools/secos-sign user/m19_fork.elf --dev
	python3 tools/elf2h.py user/m19_fork.elf user_m19_fork_elf crypto/user_m19_fork_elf.h
	# [M20] file-backed mmap via page cache
	$(CC) $(USER_CFLAGS) -c user/m20_mmap.c    -o user/m20_mmap.o
	$(LD) --gc-sections -T user/user.ld -o user/m20_mmap.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/m20_mmap.o
	python3 tools/secos-sign user/m20_mmap.elf --dev
	python3 tools/elf2h.py user/m20_mmap.elf user_m20_mmap_elf crypto/user_m20_mmap_elf.h
	# [M23] POSIX FS personality: a signed program using /dev + /proc + stat + lseek
	$(CC) $(USER_CFLAGS) -c user/m23_fs.c      -o user/m23_fs.o
	$(LD) --gc-sections -T user/user.ld -o user/m23_fs.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/m23_fs.o
	python3 tools/secos-sign user/m23_fs.elf --dev
	python3 tools/elf2h.py user/m23_fs.elf user_m23_fs_elf crypto/user_m23_fs_elf.h
	# [M24] networking CAP_NET: same program signed with vs without the CAP_NET
	# manifest. The CAP_NET build gets a socket; the plain build is refused.
	$(CC) $(USER_CFLAGS) -c user/note_net.S    -o user/note_net.o
	$(CC) $(USER_CFLAGS) -c user/m24_net.c     -o user/m24_net.o
	$(LD) --gc-sections -T user/user.ld -o user/m24_net.elf user/crt0.o user/note_net.o user/libsecos.o user/libc.o user/m24_net.o
	python3 tools/secos-sign user/m24_net.elf --dev
	python3 tools/elf2h.py user/m24_net.elf user_m24net_elf crypto/user_m24net_elf.h
	$(LD) --gc-sections -T user/user.ld -o user/m24_nonet.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/m24_net.o
	python3 tools/secos-sign user/m24_nonet.elf --dev
	python3 tools/elf2h.py user/m24_nonet.elf user_m24nonet_elf crypto/user_m24nonet_elf.h
	# [M25] anonymous pipes across fork
	$(CC) $(USER_CFLAGS) -c user/m25_pipe.c    -o user/m25_pipe.o
	$(LD) --gc-sections -T user/user.ld -o user/m25_pipe.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/m25_pipe.o
	python3 tools/secos-sign user/m25_pipe.elf --dev
	python3 tools/elf2h.py user/m25_pipe.elf user_m25_pipe_elf crypto/user_m25_pipe_elf.h
	# [M30] signals: handler + raise + sigreturn, and SIG_IGN of SIGPIPE
	$(CC) $(USER_CFLAGS) -c user/m30_sig.c     -o user/m30_sig.o
	$(LD) --gc-sections -T user/user.ld -o user/m30_sig.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/m30_sig.o
	python3 tools/secos-sign user/m30_sig.elf --dev
	python3 tools/elf2h.py user/m30_sig.elf user_m30_sig_elf crypto/user_m30_sig_elf.h
	# [M39] POSIX shell-from-source foundation: getcwd/chdir, environ, dup2, termios
	$(CC) $(USER_CFLAGS) -c user/m39_posix.c   -o user/m39_posix.o
	$(LD) --gc-sections -T user/user.ld -o user/m39_posix.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/m39_posix.o
	python3 tools/secos-sign user/m39_posix.elf --dev
	python3 tools/elf2h.py user/m39_posix.elf user_m39_posix_elf crypto/user_m39_posix_elf.h
	# [M35] capability confinement demo (CONFINED manifest: CAP_ENFORCE|FS_READ|TIME)
	$(CC) $(USER_CFLAGS) -c user/note_confined.S -o user/note_confined.o
	$(CC) $(USER_CFLAGS) -c user/m35_caps.c    -o user/m35_caps.o
	$(LD) --gc-sections -T user/user.ld -o user/m35_caps.elf user/crt0.o user/note_confined.o user/libsecos.o user/libc.o user/m35_caps.o
	python3 tools/secos-sign user/m35_caps.elf --dev
	python3 tools/elf2h.py user/m35_caps.elf user_m35caps_elf crypto/user_m35caps_elf.h
	# [stress] seeded ring-3 syscall fuzzer (tools/soak.sh + selftest fuzz gate)
	$(CC) $(USER_CFLAGS) -c user/sysfuzz.c     -o user/sysfuzz.o
	$(LD) --gc-sections -T user/user.ld -o user/sysfuzz.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/sysfuzz.o
	python3 tools/secos-sign user/sysfuzz.elf --dev
	python3 tools/elf2h.py user/sysfuzz.elf user_sysfuzz_elf crypto/user_sysfuzz_elf.h
	# [M48] SMP regression checks (FPU across migration + signals, fork bursts)
	$(CC) $(USER_CFLAGS) -c user/smptest.c     -o user/smptest.o
	$(LD) --gc-sections -T user/user.ld -o user/smptest.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/smptest.o
	python3 tools/secos-sign user/smptest.elf --dev
	python3 tools/elf2h.py user/smptest.elf user_smptest_elf crypto/user_smptest_elf.h
	# [M50 B2] page-cache coherence stress (read/mmap vs write/truncate/append)
	$(CC) $(USER_CFLAGS) -c user/pcstress.c    -o user/pcstress.o
	$(LD) --gc-sections -T user/user.ld -o user/pcstress.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/pcstress.o
	python3 tools/secos-sign user/pcstress.elf --dev
	python3 tools/elf2h.py user/pcstress.elf user_pcstress_elf crypto/user_pcstress_elf.h
	# [M51 C3] generic filesystem stress (tools/fs_stress_drive.py)
	$(CC) $(USER_CFLAGS) -c user/fsstress.c    -o user/fsstress.o
	$(LD) --gc-sections -T user/user.ld -o user/fsstress.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/fsstress.o
	python3 tools/secos-sign user/fsstress.elf --dev
	python3 tools/elf2h.py user/fsstress.elf user_fsstress_elf crypto/user_fsstress_elf.h
	# [uaccess] bad-user-pointer -EFAULT regression test (selftest uaccess gate)
	$(CC) $(USER_CFLAGS) -c user/uaccess_test.c -o user/uaccess_test.o
	$(LD) --gc-sections -T user/user.ld -o user/uaccess_test.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/uaccess_test.o
	python3 tools/secos-sign user/uaccess_test.elf --dev
	python3 tools/elf2h.py user/uaccess_test.elf user_uaccess_elf crypto/user_uaccess_elf.h
	# [abi] kernel/user ABI regression test: wait status, DF on entry, PROT_NONE
	$(CC) $(USER_CFLAGS) -c user/abi_test.c    -o user/abi_test.o
	$(LD) --gc-sections -T user/user.ld -o user/abi_test.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/abi_test.o
	python3 tools/secos-sign user/abi_test.elf --dev
	python3 tools/elf2h.py user/abi_test.elf user_abi_test_elf crypto/user_abi_test_elf.h
	# kernel interfaces for userland tools: procfs/klog/clocks/statfs/rename/link/poll test
	$(CC) $(USER_CFLAGS) -c user/kitest.c      -o user/kitest.o
	$(LD) --gc-sections -T user/user.ld -o user/kitest.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/kitest.o
	python3 tools/secos-sign user/kitest.elf --dev
	python3 tools/elf2h.py user/kitest.elf user_kitest_elf crypto/user_kitest_elf.h
	# [pipe] blocking-pipe integrity test: PIPE_BUF atomicity, full blocking writes, EINTR/EPIPE
	$(CC) $(USER_CFLAGS) -c user/pipe_test.c   -o user/pipe_test.o
	$(LD) --gc-sections -T user/user.ld -o user/pipe_test.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/pipe_test.o
	python3 tools/secos-sign user/pipe_test.elf --dev
	python3 tools/elf2h.py user/pipe_test.elf user_pipe_test_elf crypto/user_pipe_test_elf.h
	# [M47] network stack stress test (CAP_NET manifest): parallel TCP/UDP/DNS + stats
	$(CC) $(USER_CFLAGS) -c user/netstress.c   -o user/netstress.o
	$(LD) --gc-sections -T user/user.ld -o user/netstress.elf user/crt0.o user/note_net.o user/libsecos.o user/libc.o user/netstress.o
	python3 tools/secos-sign user/netstress.elf --dev
	python3 tools/elf2h.py user/netstress.elf user_netstress_elf crypto/user_netstress_elf.h
	$(CC) $(USER_CFLAGS) -c user/power.c       -o user/power.o
	$(LD) --gc-sections -T user/user.ld -o user/power.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/power.o
	python3 tools/secos-sign user/power.elf --dev
	python3 tools/elf2h.py user/power.elf user_power_elf crypto/user_power_elf.h
	# netcfg/ifup/ifdown/ifconfig over SYS_NETCTL (CAP_NET manifest)
	$(CC) $(USER_CFLAGS) -c user/netcfg.c      -o user/netcfg.o
	$(LD) --gc-sections -T user/user.ld -o user/netcfg.elf user/crt0.o user/note_net.o user/libsecos.o user/libc.o user/netcfg.o
	python3 tools/secos-sign user/netcfg.elf --dev
	python3 tools/elf2h.py user/netcfg.elf user_netcfg_elf crypto/user_netcfg_elf.h
	# in-place execve test (EXECVE_DEMO): signed build + an unsigned twin
	$(CC) $(USER_CFLAGS) -c user/exectest.c   -o user/exectest.o
	$(LD) --gc-sections -T user/user.ld -o user/exectest.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/exectest.o
	cp user/exectest.elf user/exectest_unsigned.elf
	python3 tools/secos-sign user/exectest.elf --dev
	python3 tools/elf2h.py user/exectest.elf user_exectest_elf crypto/user_exectest_elf.h
	python3 tools/elf2h.py user/exectest_unsigned.elf user_exectest_unsigned_elf crypto/user_exectest_unsigned_elf.h
	# [tty] termios/raw-mode/VMIN-VTIME/winsize regression test (selftest line-editing gate)
	$(CC) $(USER_CFLAGS) -c user/ttytest.c     -o user/ttytest.o
	$(LD) --gc-sections -T user/user.ld -o user/ttytest.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/ttytest.o
	python3 tools/secos-sign user/ttytest.elf --dev
	python3 tools/elf2h.py user/ttytest.elf user_ttytest_elf crypto/user_ttytest_elf.h
	# [pty] pseudo-terminal regression test + script(1)-style relay (selftest pty gate)
	$(CC) $(USER_CFLAGS) -c user/ptytest.c     -o user/ptytest.o
	$(LD) --gc-sections -T user/user.ld -o user/ptytest.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/ptytest.o
	python3 tools/secos-sign user/ptytest.elf --dev
	python3 tools/elf2h.py user/ptytest.elf user_ptytest_elf crypto/user_ptytest_elf.h
	# [crypt] crypt()/pwhash known-answer test (musl SHA-crypt + bcrypt vectors)
	$(MAKE) --no-print-directory crypt-test-link
	# [M40] GUI kernel interfaces: /dev/fb0 + /dev/input/eventN demo and test
	# (signed with the CAP_INPUT|CAP_DISPLAY manifest), a capability-less twin
	# of the test (refusals), and loadkeys (console keyboard layout)
	$(CC) $(USER_CFLAGS) -c user/note_gui.S    -o user/note_gui.o
	$(CC) $(USER_CFLAGS) -c user/fbdemo.c      -o user/fbdemo.o
	$(LD) --gc-sections -T user/user.ld -o user/fbdemo.elf user/crt0.o user/note_gui.o user/libsecos.o user/libc.o user/fbdemo.o
	python3 tools/secos-sign user/fbdemo.elf --dev
	python3 tools/elf2h.py user/fbdemo.elf user_fbdemo_elf crypto/user_fbdemo_elf.h
	$(CC) $(USER_CFLAGS) -c user/guitest.c     -o user/guitest.o
	$(LD) --gc-sections -T user/user.ld -o user/guitest.elf user/crt0.o user/note_gui.o user/libsecos.o user/libc.o user/guitest.o
	$(LD) --gc-sections -T user/user.ld -o user/guitest_nocap.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/guitest.o
	python3 tools/secos-sign user/guitest.elf --dev
	python3 tools/secos-sign user/guitest_nocap.elf --dev
	python3 tools/elf2h.py user/guitest.elf user_guitest_elf crypto/user_guitest_elf.h
	python3 tools/elf2h.py user/guitest_nocap.elf user_guitest_nocap_elf crypto/user_guitest_nocap_elf.h
	$(CC) $(USER_CFLAGS) -c user/loadkeys.c    -o user/loadkeys.o
	$(LD) --gc-sections -T user/user.ld -o user/loadkeys.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/loadkeys.o
	python3 tools/secos-sign user/loadkeys.elf --dev
	python3 tools/elf2h.py user/loadkeys.elf user_loadkeys_elf crypto/user_loadkeys_elf.h
	$(MAKE) --no-print-directory gui-progs
	$(MAKE) --no-print-directory acct-progs
	$(MAKE) --no-print-directory m46-progs
	$(MAKE) --no-print-directory udrv-progs
	$(MAKE) --no-print-directory pkg-progs
	@echo "user-progs: built+signed hello + udrv_edu + udrv_negtest + udrv_user + ipc_send + ipc_recv + spin + m31_libc + coreutils + testpkg.spkg + svc_demo + init + maxmem + crashtest + m16_{child,parent} + m17_sleeper + m18_mem + m19_fork + m20_mmap + m23_fs + m24_net{,_nonet} + m25_pipe + m30_sig + m39_posix + m35_caps + sysfuzz + smptest + pcstress + fsstress + uaccess_test + abi_test + kitest + pipe_test + power + netcfg + exectest{,_unsigned} + ttytest + ptytest + fbdemo + guitest{,_nocap} + loadkeys + secwm + sysinfo + swptest + spkg -> crypto/*.h"

# [M57] Ring-3 driver test programs (docs/devlog/M57-udrv-core.md): udrv_edu
# (positive: edu BAR, IRQ fd, DMA) and udrv_negtest (inputs the kernel must
# refuse) carry the v3 DRIVER manifest note_udrv; udrv_user is the same
# negative-test binary with a plain manifest (every claim must be refused).
.PHONY: udrv-progs
udrv-progs:
	$(CC) $(USER_CFLAGS) -c user/crt0.S          -o user/crt0.o
	$(CC) $(USER_CFLAGS) -c user/note.S          -o user/note.o
	$(CC) $(USER_CFLAGS) -c user/note_udrv.S     -o user/note_udrv.o
	$(CC) $(USER_CFLAGS) -c user/libsecos.c      -o user/libsecos.o
	$(CC) $(USER_CFLAGS) -c user/libc.c          -o user/libc.o
	$(CC) $(USER_CFLAGS) -c user/udrv_edu.c      -o user/udrv_edu.o
	$(CC) $(USER_CFLAGS) -c user/udrv_negtest.c  -o user/udrv_negtest.o
	$(LD) --gc-sections -T user/user.ld -o user/udrv_edu.elf user/crt0.o user/note_udrv.o user/libsecos.o user/libc.o user/udrv_edu.o
	$(LD) --gc-sections -T user/user.ld -o user/udrv_negtest.elf user/crt0.o user/note_udrv.o user/libsecos.o user/libc.o user/udrv_negtest.o
	$(LD) --gc-sections -T user/user.ld -o user/udrv_user.elf user/crt0.o user/note.o user/libsecos.o user/libc.o user/udrv_negtest.o
	for n in udrv_edu udrv_negtest udrv_user; do \
	    python3 tools/secos-sign user/$$n.elf --dev || exit 1; \
	    python3 tools/elf2h.py user/$$n.elf user_$${n}_elf crypto/user_$${n}_elf.h || exit 1; \
	done

# [U6] /bin/spkg, the package installer v2 (docs/devlog/U6-pkg2.md): confined
# manifest (CAP_ENFORCE|FS_READ|FS_WRITE|SIGNAL|TIME), SHA-256 from crypto/,
# the signature itself is checked by the kernel (SYS_SIGVERIFY).
.PHONY: pkg-progs
pkg-progs:
	$(CC) $(USER_CFLAGS) -c user/crt0.S       -o user/crt0.o
	$(CC) $(USER_CFLAGS) -c user/note_spkg.S  -o user/note_spkg.o
	$(CC) $(USER_CFLAGS) -c user/libsecos.c   -o user/libsecos.o
	$(CC) $(USER_CFLAGS) -c user/libc.c       -o user/libc.o
	$(CC) $(USER_CFLAGS) -c user/spkg.c       -o user/spkg.o
	$(CC) $(USER_CFLAGS) -c crypto/sha256.c   -o user/sha256_u.o
	$(LD) --gc-sections -T user/user.ld -o user/spkg.elf user/crt0.o user/note_spkg.o user/libsecos.o user/libc.o \
	    user/spkg.o user/sha256_u.o
	python3 tools/secos-sign user/spkg.elf --dev
	python3 tools/elf2h.py user/spkg.elf user_spkg_elf crypto/user_spkg_elf.h
	@echo "pkg-progs: built+signed spkg -> crypto/user_spkg_elf.h"

# [M46] Exec confinement selftest (docs/devlog/M46-exec-confinement.md): one
# source, three signed manifests -- m46conf (confined driver), m46probe (ambient
# + explicit NET/INPUT/DISPLAY) and m46setid (SETID, installed root 04755).
.PHONY: m46-progs
m46-progs:
	$(CC) $(USER_CFLAGS) -c user/crt0.S          -o user/crt0.o
	$(CC) $(USER_CFLAGS) -c user/note_m46conf.S  -o user/note_m46conf.o
	$(CC) $(USER_CFLAGS) -c user/note_m46probe.S -o user/note_m46probe.o
	$(CC) $(USER_CFLAGS) -c user/note_setid.S    -o user/note_setid.o
	$(CC) $(USER_CFLAGS) -c user/libsecos.c      -o user/libsecos.o
	$(CC) $(USER_CFLAGS) -c user/libc.c          -o user/libc.o
	$(CC) $(USER_CFLAGS) -c user/m46_confine.c   -o user/m46_confine.o
	for v in conf:m46conf probe:m46probe setid:setid; do \
	    n=$${v%%:*}; note=$${v##*:}; \
	    $(LD) --gc-sections -T user/user.ld -o user/m46$$n.elf user/crt0.o user/note_$$note.o \
	        user/libsecos.o user/libc.o user/m46_confine.o || exit 1; \
	    python3 tools/secos-sign user/m46$$n.elf --dev || exit 1; \
	    python3 tools/elf2h.py user/m46$$n.elf user_m46$${n}_elf crypto/user_m46$${n}_elf.h || exit 1; \
	done
	@echo "m46-progs: built+signed m46conf + m46probe + m46setid -> crypto/*.h"

# [M43] Multi-user tools (docs/USERS.md). login/su/sudo/passwd are signed with
# the SETID manifest (user/note_setid.S) and installed root 04755 by
# kernel/sysroot.c; usertools (useradd/userdel/usermod/groupadd/groupdel/
# chpasswd/id/whoami/groups) carries the plain
# manifest, so a set-uid bit on them is ignored. libcrypt goes last.
ACCT_SETID = login su sudo passwd
.PHONY: acct-progs
acct-progs: libcrypt
	$(CC) $(USER_CFLAGS) -c user/crt0.S       -o user/crt0.o
	$(CC) $(USER_CFLAGS) -c user/note.S       -o user/note.o
	$(CC) $(USER_CFLAGS) -c user/note_setid.S -o user/note_setid.o
	$(CC) $(USER_CFLAGS) -c user/libsecos.c   -o user/libsecos.o
	$(CC) $(USER_CFLAGS) -c user/libc.c       -o user/libc.o
	$(CC) $(USER_CFLAGS) -c user/acct.c       -o user/acct.o
	for p in $(ACCT_SETID) usertools; do \
	    $(CC) $(USER_CFLAGS) -c user/$$p.c -o user/$$p.o || exit 1; \
	done
	for p in $(ACCT_SETID); do \
	    $(LD) --gc-sections -T user/user.ld -o user/$$p.elf user/crt0.o user/note_setid.o user/libsecos.o user/libc.o \
	        user/$$p.o user/acct.o $(LIBCRYPT) || exit 1; \
	done
	$(LD) --gc-sections -T user/user.ld -o user/usertools.elf user/crt0.o user/note.o user/libsecos.o user/libc.o \
	    user/usertools.o user/acct.o $(LIBCRYPT)
	for p in $(ACCT_SETID) usertools; do \
	    python3 tools/secos-sign user/$$p.elf --dev || exit 1; \
	    python3 tools/elf2h.py user/$$p.elf user_$${p}_elf crypto/user_$${p}_elf.h || exit 1; \
	done
	@echo "acct-progs: built+signed $(ACCT_SETID) (SETID) + usertools -> crypto/*.h"

# Graphical UI (docs/GUI.md): the secwm compositor + terminal (signed
# CAP_INPUT|CAP_DISPLAY, the only program holding fb0 and the input devices),
# and its clients, signed with the confined manifest (CAP_ENFORCE|FS_READ|TIME):
# sysinfo (System panel) and swptest (isolation selftest). The keymap tables
# are the kernel's own drivers/keymap.c compiled once more for user space.
# font16.h is generated from the vendored Terminus Font (SIL OFL 1.1):
#   python3 tools/bdf2c.py user/secwm/font16.h third_party/terminus-font/ter-u16{n,b}.bdf
SWM = user/secwm
SWM_CFLAGS = $(USER_CFLAGS) -I$(SWM)
.PHONY: gui-progs
gui-progs:
	$(CC) $(USER_CFLAGS) -c user/crt0.S       -o user/crt0.o
	$(CC) $(USER_CFLAGS) -c user/note_gui.S   -o user/note_gui.o
	$(CC) $(USER_CFLAGS) -c user/note_confined.S -o user/note_confined.o
	$(CC) $(USER_CFLAGS) -c user/libsecos.c   -o user/libsecos.o
	$(CC) $(USER_CFLAGS) -c user/libc.c       -o user/libc.o
	$(CC) $(USER_CFLAGS) -c user/libm.c       -o user/libm.o
	$(CC) $(SWM_CFLAGS) -c $(SWM)/gfx.c       -o $(SWM)/gfx.o
	$(CC) $(SWM_CFLAGS) -c $(SWM)/term.c      -o $(SWM)/term.o
	$(CC) $(SWM_CFLAGS) -c $(SWM)/swpc.c      -o $(SWM)/swpc.o
	$(CC) $(SWM_CFLAGS) -c $(SWM)/secwm.c     -o $(SWM)/secwm.o
	$(CC) $(SWM_CFLAGS) -c drivers/keymap.c   -o $(SWM)/keymap.o
	$(CC) $(SWM_CFLAGS) -c $(SWM)/sysinfo.c   -o $(SWM)/sysinfo.o
	$(CC) $(SWM_CFLAGS) -c $(SWM)/swptest.c   -o $(SWM)/swptest.o
	$(LD) --gc-sections -T user/user.ld -o $(SWM)/secwm.elf user/crt0.o user/note_gui.o user/libsecos.o user/libc.o user/libm.o \
	  $(SWM)/secwm.o $(SWM)/term.o $(SWM)/gfx.o $(SWM)/keymap.o
	$(LD) --gc-sections -T user/user.ld -o $(SWM)/sysinfo.elf user/crt0.o user/note_confined.o user/libsecos.o user/libc.o user/libm.o \
	  $(SWM)/sysinfo.o $(SWM)/swpc.o $(SWM)/gfx.o
	$(LD) --gc-sections -T user/user.ld -o $(SWM)/swptest.elf user/crt0.o user/note_confined.o user/libsecos.o user/libc.o \
	  $(SWM)/swptest.o $(SWM)/swpc.o $(SWM)/gfx.o
	python3 tools/secos-sign $(SWM)/secwm.elf --dev
	python3 tools/secos-sign $(SWM)/sysinfo.elf --dev
	python3 tools/secos-sign $(SWM)/swptest.elf --dev
	python3 tools/elf2h.py $(SWM)/secwm.elf user_secwm_elf crypto/user_secwm_elf.h
	python3 tools/elf2h.py $(SWM)/sysinfo.elf user_sysinfo_elf crypto/user_sysinfo_elf.h
	python3 tools/elf2h.py $(SWM)/swptest.elf user_swptest_elf crypto/user_swptest_elf.h
	@echo "gui-progs: built+signed secwm + sysinfo + swptest -> crypto/*.h"

# Staleness guard for the committed embedded programs: rebuild + re-sign them
# (deterministic: same sources -> byte-identical headers) and fail if any
# crypto/user_*_elf.h differs from the committed one, i.e. someone changed the
# libc / an ABI struct / a program without regenerating. Needs python3
# 'cryptography'. Does not cover port-bash / port-lua (network / long builds).
.PHONY: check-user-progs
check-user-progs: user-progs
	@git diff --quiet -- 'crypto/user_*_elf.h' 'crypto/user_*_spkg.h' || { \
	  echo "check-user-progs: STALE embedded programs (regenerated, commit them):"; \
	  git diff --stat -- 'crypto/user_*_elf.h' 'crypto/user_*_spkg.h'; exit 1; }
	@echo "check-user-progs: embedded programs match their sources"

# [M34] Port of lua 5.4.7 FROM SOURCE (third_party/lua), signed + embedded.
# Compiles the lua core + the OS-independent standard libs (base/table/string/
# math/coroutine/utf8 + io/os) against the SecOS libc/libm/setjmp, links a freestanding
# front-end with a REPL (user/lua_main.c), signs it, and emits crypto/user_lua_elf.h
# (installed to /bin/lua at boot). The
# committed header lets the default kernel build without recompiling lua; run
# this after changing the libc or the lua sources. Gate: M34_LUA_DEMO=1.
LUASRC = third_party/lua/src
LUA_UF = -ffreestanding -nostdlib -fno-pie -no-pie -mno-red-zone -mcmodel=large -m64 -O2 -w \
         -Iuser -Iuser/include -I$(LUASRC) -ffunction-sections -fdata-sections -include user/lua_port.h
LUA_CORE = lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser \
           lstate lstring ltable ltm lundump lvm lzio lauxlib
LUA_LIBS = lbaselib lcorolib lmathlib lstrlib ltablib lutf8lib liolib loslib
.PHONY: port-lua
port-lua:
	$(CC) $(USER_CFLAGS) -c user/crt0.S     -o user/crt0.o
	$(CC) $(USER_CFLAGS) -c user/note.S     -o user/note.o
	$(CC) $(USER_CFLAGS) -c user/libsecos.c -o user/libsecos.o
	$(CC) $(USER_CFLAGS) -c user/libc.c     -o user/libc.o
	$(CC) $(USER_CFLAGS) -c user/libm.c     -o user/libm.o
	$(CC) $(USER_CFLAGS) -c user/setjmp.S   -o user/setjmp.o
	@mkdir -p user/luaobj
	for f in $(LUA_CORE) $(LUA_LIBS); do $(CC) $(LUA_UF) -c $(LUASRC)/$$f.c -o user/luaobj/$$f.o || exit 1; done
	$(CC) $(LUA_UF) -c user/lua_main.c -o user/luaobj/lua_main.o
	$(LD) --gc-sections -T user/user.ld -o user/lua.elf \
	  user/crt0.o user/note.o user/libsecos.o user/libc.o user/libm.o user/setjmp.o user/luaobj/*.o
	python3 tools/secos-sign user/lua.elf --dev
	python3 tools/elf2h.py user/lua.elf user_lua_elf crypto/user_lua_elf.h
	@echo "port-lua: lua 5.4.7 built from source, signed -> crypto/user_lua_elf.h"

# [M39] GNU bash 5.2 from source. Builds the SecOS user objects, then runs
# tools/port-bash.sh (fetch + native-build-to-generate-sources + cross-compile +
# link + sign + embed). Needs network (first run) + python3 'cryptography'.
.PHONY: port-bash
port-bash:
	$(CC) $(USER_CFLAGS) -c user/crt0.S     -o user/crt0.o
	$(CC) $(USER_CFLAGS) -c user/note.S     -o user/note.o
	$(CC) $(USER_CFLAGS) -c user/libsecos.c -o user/libsecos.o
	$(CC) $(USER_CFLAGS) -c user/libc.c     -o user/libc.o
	$(CC) $(USER_CFLAGS) -c user/libm.c     -o user/libm.o
	bash tools/port-bash.sh
	@echo "port-bash: GNU bash 5.2 built from source, signed -> crypto/user_bash_elf.h"

# BusyBox 1.36.1 from source (GPL-2.0, fetched + SHA-256-verified into
# /tmp/secos-busybox, never vendored): configured from user/busybox.config,
# compiled against the SecOS libc + user/busybox-include + user/busybox_compat.c,
# signed -> crypto/user_busybox_elf.h + the applet stub crypto/user_bbstub_elf.h
# + kernel/busybox_applets.h. Needs network (first run) + python3 'cryptography'.
# LVGL 9.2.2 (MIT) from source: tools/port-lvgl.sh fetches the pinned release
# (SHA-256 checked), compiles it with user/lvgl/lv_conf.h and links the LVGL
# system monitor (user/lvgl/sysmon.c, an SWP client of secwm, confined
# manifest) -> crypto/user_sysmon_elf.h, installed as /bin/sysmon.
.PHONY: port-lvgl
port-lvgl:
	bash tools/port-lvgl.sh
	@echo "port-lvgl: LVGL $(shell sed -n 's/^LV=//p' tools/port-lvgl.sh) built from source, sysmon signed -> crypto/user_sysmon_elf.h"

.PHONY: port-busybox
port-busybox:
	bash tools/port-busybox.sh
	@echo "port-busybox: BusyBox 1.36.1 built from source, signed -> crypto/user_busybox_elf.h"

# Dropbear SSH (2026.94, SHA-256 pinned) from source: multi-user, public-key or
# /etc/shadow password auth (crypt() from user/libcrypt.a; root by key only),
# ed25519 + curve25519/mlkem768 + chacha20-poly1305/aes-gcm/aes-ctr. Builds the
# SecOS user objects, then runs tools/port-dropbear.sh (fetch + verify + cross-
# compile + link with the confined CAP_NET manifest + sign + embed as
# crypto/user_dropbear_elf.h, installed to /bin/dropbearmulti). See docs/SSH.md.
.PHONY: port-dropbear
port-dropbear: libcrypt
	$(CC) $(USER_CFLAGS) -c user/crt0.S     -o user/crt0.o
	$(CC) $(USER_CFLAGS) -c user/libsecos.c -o user/libsecos.o
	$(CC) $(USER_CFLAGS) -c user/libc.c     -o user/libc.o
	$(CC) $(USER_CFLAGS) -c user/libm.c     -o user/libm.o
	bash tools/port-dropbear.sh
	@echo "port-dropbear: Dropbear SSH built from source, signed -> crypto/user_dropbear_elf.h"

# --- Test disk images (virtio-blk) ---
# A small FAT32 data disk with a known test file. Used by the storage smoke
# tests; attach with -drive file=$(DISK_IMG),if=virtio,format=raw.
DISK_IMG = disk.img
.PHONY: disk disk-fat32 disk-ext2 disk-ext4 disk-journal
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

# [M41] exFAT data disk built with the real host tools (exfatprogs mkfs.exfat
# + exfat-fuse in an unprivileged user namespace; fetched on first use without
# sudo, see tools/exfat-host.sh): long Unicode names, a 3 MiB file, two
# fragmented files. `make disk-exfat-corrupt` builds the damaged-image suite.
EXFAT_CORRUPT_DIR = exfat_corrupt
.PHONY: disk-exfat disk-exfat-corrupt
disk-exfat:
	bash tools/exfat-host.sh mkgood $(DISK_IMG)
	@echo "disk-exfat: $(DISK_IMG) (exFAT, 64MB, hello.txt + long-name/big/fragmented files)"
disk-exfat-corrupt:
	rm -rf $(EXFAT_CORRUPT_DIR)
	bash tools/exfat-host.sh mkcorrupt $(EXFAT_CORRUPT_DIR)
	@echo "disk-exfat-corrupt: $(EXFAT_CORRUPT_DIR)/{files,geo,csum1,csum2,upcase,rootcycle}.img"

# [M42] NTFS data disk built with the real host tools (mkntfs + ntfs-3g in an
# unprivileged user namespace; fetched without sudo if missing, see
# tools/ntfs-host.sh): long Unicode names, a fragmented file with an
# $ATTRIBUTE_LIST, sparse + LZNT1-compressed files, symlinks/junctions, an EFS
# file, a 2000-entry multi-level directory. $(DISK_IMG).list is the ntfs-3g
# reference listing. `make disk-ntfs-corrupt` builds the damaged-image suite.
NTFS_CORRUPT_DIR = ntfs_corrupt
.PHONY: disk-ntfs disk-ntfs-corrupt
disk-ntfs:
	bash tools/ntfs-host.sh mkgood $(DISK_IMG)
	@echo "disk-ntfs: $(DISK_IMG) (NTFS, 96MB, reference listing $(DISK_IMG).list)"
disk-ntfs-corrupt:
	rm -rf $(NTFS_CORRUPT_DIR)
	bash tools/ntfs-host.sh mkcorrupt $(NTFS_CORRUPT_DIR)
	@echo "disk-ntfs-corrupt: $(NTFS_CORRUPT_DIR)/{files,bootmain,bootboth,mft0,mftboth,upcase,rootbad,geo}.img"

# [M27a] ext4 image with a DIRTY JBD2 journal: a committed transaction rewrites
# /target.bin from OLD to NEW, pending replay. e2fsck-validated synthetic journal
# (tools/mk_dirty_journal.py). A correct journal-recovery driver replays it on mount.
disk-journal:
	rm -rf /tmp/secos_jstage && mkdir -p /tmp/secos_jstage
	printf 'OLDOLDOLDOLD\n' > /tmp/secos_jstage/target.bin
	mke2fs -F -q -t ext4 -O has_journal,^metadata_csum,^64bit -b 1024 -d /tmp/secos_jstage $(DISK_IMG) 16384
	python3 tools/mk_dirty_journal.py $(DISK_IMG) /target.bin NEW
	@echo "disk-journal: $(DISK_IMG) (ext4 + dirty JBD2 journal, /target.bin OLD->NEW pending replay)"

# [M23] Persistent ext2 SYSTEM root disk: FHS skeleton + the /.secosroot marker
# that makes the kernel adopt it as the VFS root "/". Attach as a SATA/NVMe/USB
# data disk (or virtio "vda"); the kernel mounts it persistently at "/".
SYSDISK_IMG = sysdisk.img
.PHONY: sysdisk-ext2 sysdisk-vmdk
sysdisk-ext2:
	rm -rf /tmp/secos_sysroot
	mkdir -p /tmp/secos_sysroot/bin /tmp/secos_sysroot/etc /tmp/secos_sysroot/dev
	mkdir -p /tmp/secos_sysroot/proc /tmp/secos_sysroot/sys /tmp/secos_sysroot/tmp
	mkdir -p /tmp/secos_sysroot/usr /tmp/secos_sysroot/opt /tmp/secos_sysroot/home
	mkdir -p /tmp/secos_sysroot/lib /tmp/secos_sysroot/root /tmp/secos_sysroot/var
	printf 'SecOS persistent root marker\n' > /tmp/secos_sysroot/.secosroot
	printf 'SecOS 0.1.0-dev\n' > /tmp/secos_sysroot/etc/secos-release
	printf 'Welcome to SecOS (persistent ext2 root)\n' > /tmp/secos_sysroot/etc/motd
	printf 'hello from the persistent root\n' > /tmp/secos_sysroot/home/hello.txt
	dd if=/dev/zero of=$(SYSDISK_IMG) bs=1M count=64 status=none
	mkfs.ext2 -q -O ^has_journal,^metadata_csum -L SECOSROOT -d /tmp/secos_sysroot $(SYSDISK_IMG)
	@echo "sysdisk-ext2: $(SYSDISK_IMG) (ext2 persistent root, marker /.secosroot)"
sysdisk-vmdk: sysdisk-ext4
	qemu-img convert -f raw -O vmdk $(SYSDISK_IMG) sysdisk.vmdk
	@echo "sysdisk-vmdk: sysdisk.vmdk ready (attach as SATA/NVMe data disk in VMware)"

# [M33] Persistent ext4 SYSTEM root disk (journal + extents + 64bit). Same FHS
# skeleton + /.secosroot marker as sysdisk-ext2, but a real ext4 volume — the
# kernel adopts it as "/", JBD2 write-journaling (M27b) makes its metadata writes
# crash-atomic. No metadata_csum (proven M27 path).
.PHONY: sysdisk-ext4 sysdisk-ext4-csum sysroot-stage
sysroot-stage:
	rm -rf /tmp/secos_sysroot
	mkdir -p /tmp/secos_sysroot/bin /tmp/secos_sysroot/etc /tmp/secos_sysroot/dev
	mkdir -p /tmp/secos_sysroot/proc /tmp/secos_sysroot/sys /tmp/secos_sysroot/tmp
	mkdir -p /tmp/secos_sysroot/usr /tmp/secos_sysroot/opt /tmp/secos_sysroot/home
	mkdir -p /tmp/secos_sysroot/lib /tmp/secos_sysroot/root /tmp/secos_sysroot/var
	printf 'SecOS persistent root marker\n' > /tmp/secos_sysroot/.secosroot
	printf 'SecOS 0.1.0-dev\n' > /tmp/secos_sysroot/etc/secos-release
	printf 'Welcome to SecOS (persistent ext4 root)\n' > /tmp/secos_sysroot/etc/motd
	printf 'hello from the persistent root\n' > /tmp/secos_sysroot/home/hello.txt
sysdisk-ext4: sysroot-stage
	dd if=/dev/zero of=$(SYSDISK_IMG) bs=1M count=64 status=none
	mkfs.ext4 -q -O has_journal,extent,64bit,^metadata_csum -L SECOSROOT -d /tmp/secos_sysroot $(SYSDISK_IMG)
	@echo "sysdisk-ext4: $(SYSDISK_IMG) (ext4 journal+extents persistent root, marker /.secosroot)"
# [M33] Full ext4 root WITH metadata_csum (default mkfs.ext4 layout). Exercises the
# crc32c metadata_csum write path. Must stay e2fsck-clean after SecOS writes.
sysdisk-ext4-csum: sysroot-stage
	dd if=/dev/zero of=$(SYSDISK_IMG) bs=1M count=64 status=none
	mkfs.ext4 -q -O has_journal,extent,64bit,metadata_csum -L SECOSROOT -d /tmp/secos_sysroot $(SYSDISK_IMG)
	@echo "sysdisk-ext4-csum: $(SYSDISK_IMG) (ext4 journal+extents+metadata_csum root)"

# --- [M41] Persistent system disk (default for every run target) ---
# secos-root.img is a sparse ext4 volume (journal + metadata_csum) carrying the
# /.secosroot marker, so the kernel mounts it as "/": files, /root/.ssh, the SSH
# host key, /etc edits and the keyboard layout survive reboots. It is created
# once (tools/mkroot.sh) when missing and never overwritten by a build; the
# kernel refreshes the signed programs in /bin from its image at boot and
# leaves the rest alone (docs/devlog/M41-persistent-root.md).
#   make run-gui                 boot with the persistent root
#   make run-gui ROOT=ram        boot without it (RAMFS root, nothing persists)
#   make reset-root              DELETE the disk and create a fresh one
# It is attached as virtio-blk ("vda"): the one bus that works on both the
# MB2/ISO path (i440fx, -boot d so SeaBIOS boots the CD) and the UEFI/q35 path.
ROOT      ?= disk
ROOT_IMG  ?= secos-root.img
ROOT_SIZE ?= 8G
ROOT_DRIVE := $(if $(filter ram,$(ROOT)),,-drive file=$(ROOT_IMG),if=virtio,format=raw)
ROOT_DEP   := $(if $(filter ram,$(ROOT)),,root-img)
.PHONY: root-img reset-root grow-root
root-img:
	@bash tools/mkroot.sh $(ROOT_IMG) $(ROOT_SIZE)
reset-root:
	@bash tools/mkroot.sh --reset $(ROOT_IMG) $(ROOT_SIZE)
# [U6] Grow an existing system disk in place, offline (SecOS shut down):
#   make grow-root                         secos-root.img -> 8 GiB
#   make grow-root SIZE=16G ROOT_IMG=...   another size / image
# e2fsck before and after, resize2fs in between, a sparse backup copy first
# (BACKUP=0 skips it). Nothing on the disk changes but its size.
SIZE ?= 8G
grow-root:
	@bash tools/growroot.sh $(ROOT_IMG) $(SIZE)

# [U6] Transfer disk for packages: an exFAT image holding the given .spkg
# files (each verified on the host first), for `spkg install` in SecOS:
#   make pkgdisk PKGS="a.spkg b.spkg"           -> pkgdisk.img
#   make run-serial PKGDISK=pkgdisk.img          attached as USB storage (usb0)
#   (SecOS) mount -t exfat /dev/usb0 /media; spkg install /media/a.spkg
# For VMware: `make pkgdisk-vmdk` converts it (attach as a second SATA disk).
PKGS ?=
PKGDISK_IMG ?= pkgdisk.img
.PHONY: pkgdisk pkgdisk-vmdk
pkgdisk:
	@bash tools/mkpkgdisk.sh $(PKGDISK_IMG) $(PKGS)
pkgdisk-vmdk: pkgdisk
	qemu-img convert -f raw -O vmdk $(PKGDISK_IMG) $(PKGDISK_IMG:.img=.vmdk)
	@echo "pkgdisk-vmdk: $(PKGDISK_IMG:.img=.vmdk)"
# Any run target: PKGDISK=<image> attaches it as a USB stick (always probed,
# also next to the virtio root disk).
PKGDISK ?=
PKGDISK_DEV := $(if $(PKGDISK),-drive if=none$(comma)id=pkgdisk$(comma)file=$(PKGDISK)$(comma)format=raw -device usb-storage$(comma)drive=pkgdisk,)
PKGDISK_HCI := $(if $(PKGDISK),-device qemu-xhci,)

# Run with QEMU (graphical window — needs a working display backend)
run: iso $(ROOT_DEP)
	qemu-system-x86_64 -cdrom $(ISO) -boot d $(ROOT_DRIVE) -debugcon stdio -global isa-debugcon.iobase=0xe9 $(PKGDISK_HCI) $(PKGDISK_DEV)

# Run headless with a virtio-blk disk attached (debugcon to stdio).
.PHONY: run-disk
run-disk: iso
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),if=virtio,format=raw \
		-debugcon stdio -global isa-debugcon.iobase=0xe9 -display none -no-reboot -m 256M

# Headless: interact with the shell entirely in this terminal over COM1.
# No GUI/VNC needed (works where WSLg/GTK does not). Ctrl-A X to quit QEMU.
.PHONY: run-serial
run-serial: iso $(ROOT_DEP)
	qemu-system-x86_64 -cdrom $(ISO) -boot d $(ROOT_DRIVE) -serial stdio -display none -no-reboot $(PKGDISK_HCI) $(PKGDISK_DEV)

# Serial console + an e1000 NIC with host port SSH_PORT (default 2222) forwarded
# to guest :22, for the Dropbear SSH server (docs/SSH.md). -cpu max gives the
# guest RDRAND/RDSEED for the kernel RNG.
SSH_PORT ?= 2222
.PHONY: run-ssh
run-ssh: iso $(ROOT_DEP)
	qemu-system-x86_64 -cdrom $(ISO) -boot d $(ROOT_DRIVE) -m 512M -cpu max -serial stdio -display none -no-reboot \
		-netdev user,id=n0,hostfwd=tcp::$(SSH_PORT)-:22 -device e1000,netdev=n0 $(PKGDISK_HCI) $(PKGDISK_DEV)

# Graphical UI in a GTK window (docs/GUI.md): PS/2 keyboard + a USB tablet, so
# the guest pointer follows the host cursor without a mouse grab. Type
# `startgui` at the bash prompt; Ctrl+Alt+Backspace leaves the GUI. Italian
# keyboard: `make run-gui KEYMAP=it` (or `loadkeys it` in SecOS, which is saved
# on the persistent root). Debugcon (kernel log + GUI markers) goes to
# secos-gui.log. An e1000 NIC forwards host port GUI_SSH_PORT (default 2222) to
# the guest's sshd (docs/SSH.md):
#   make run-gui KEYMAP=it SSH_AUTHORIZED_KEYS=~/.ssh/id_ed25519.pub GUI_AUTOSTART=1
# GUI_SMP / GUI_MEM size the guest (SMP is on by default since M48):
#   make run-gui GUI_SMP=4 GUI_MEM=2G
# From a Windows ssh client (QEMU in WSL2) use `ssh -4` or 127.0.0.1: hostfwd is
# IPv4-only and `localhost` -> ::1 costs a ~21 s connect timeout (docs/SSH.md).
GUI_SSH_PORT ?= 2222
GUI_SMP ?= 2
GUI_MEM ?= 512M
.PHONY: run-gui
run-gui: iso $(ROOT_DEP)
	qemu-system-x86_64 -cdrom $(ISO) -boot d $(ROOT_DRIVE) -m $(GUI_MEM) -smp $(GUI_SMP) -cpu max -display gtk \
		-device qemu-xhci,id=xhci -device usb-tablet,bus=xhci.0 \
		-netdev user,id=n0,hostfwd=tcp::$(GUI_SSH_PORT)-:22 -device e1000,netdev=n0 \
		-debugcon file:secos-gui.log -global isa-debugcon.iobase=0xe9 $(PKGDISK_DEV)

# Run with a VNC server on :0 (connect a VNC viewer to localhost:5900)
.PHONY: run-vnc
run-vnc: iso $(ROOT_DEP)
	qemu-system-x86_64 -cdrom $(ISO) -boot d $(ROOT_DRIVE) -display vnc=:0 -debugcon stdio -global isa-debugcon.iobase=0xe9 $(PKGDISK_HCI) $(PKGDISK_DEV)

# [M50] Lock validator run: a -DSECOS_LOCKDEP image through the SMP stress
# (-smp 4/8), the network stress and a bkl=off boot; must stay free of
# [LOCKDEP] reports (tools/lockdep_check.sh, ~25 min). Leaves a lockdep build
# in the tree: `make clean` afterwards.
.PHONY: lockdep-check
lockdep-check:
	tools/lockdep_check.sh

# [M51] Every forced-concurrency lock test (B2, B3, C1-C6) on a lockdep build
# at -smp 4, one image per test (tools/locktests.sh).
.PHONY: locktests
locktests:
	tools/locktests.sh

# [M52] Phase D userland stress on a lockdep image (smp/proc-race/net/tty/
# exec/fsstress on ext4, ramfs, FAT32, exFAT, crash replay), kernel lock off
# by default; `make bklstress BKLSTRESS_MODE=on` runs it with the BKL on.
BKLSTRESS_MODE ?= off
.PHONY: bklstress
bklstress:
	tools/bkl_stress.sh --mode $(BKLSTRESS_MODE)

# Clean generated files
clean:
	# keep the committed prebuilt ath9k_hw.o (rebuilding it needs network + upstream sources)
	rm -f $(filter-out $(ATH9K_HW),$(OBJS)) $(DEPS) $(KERNEL) $(KEYMAP_STAMP) $(GUI_STAMP) $(CONSOLE_STAMP) $(BKL_STAMP) $(FLAGS_STAMP)
	rm -f $(BOOT_DIR)/ap_trampoline.bin $(BOOT_DIR)/kaslr_core.raw.o
	rm -f build/kernel.pass1.elf build/kernel.pass2.elf $(KASLR_BIN) $(KASLR_OBJ)
	rm -rf $(ISODIR) $(ISO) grub-mkrescue.log
	rm -rf $(DIST_DIR) $(UEFI_LOADER_ELF)
	rm -f $(UEFI_CRT0) $(UEFI_OBJS) $(UEFI_CRYPTO_OBJS) $(UEFI_FLAGS_STAMP)

tree:
	@echo "ASM: $(SRC_ASM)"
	@echo "C  : $(SRC_C)"

# --- UEFI build (Strategy B: external bootloader) ---
# [secure-boot] The loader Ed25519-verifies the kernel before jumping. Enforced
# by default; set SECURE_BOOT=0 to build a non-enforcing loader (e.g. to boot an
# unsigned kernel during bring-up).
SECURE_BOOT ?= 1
UEFI_SRC = uefi/boot.c uefi/elf_load.c uefi/secure_boot.c
UEFI_HELLO_SRC = uefi/hello.c
UEFI_OBJS = $(UEFI_SRC:%.c=%.o)
# Freestanding crypto compiled with the UEFI flags (distinct object names so they
# never collide with the kernel's crypto/*.o).
UEFI_CRYPTO_OBJS = uefi/u_sha256.o uefi/u_sha512.o uefi/u_ed25519.o uefi/u_kaslr_core.o
UEFI_HELLO_OBJS = $(UEFI_HELLO_SRC:%.c=%.o)
UEFI_HELLO_APP = $(EFI_BOOT_DIR)/HELLO.EFI
UEFI_CFLAGS = -ffreestanding -fshort-wchar -O2 -mno-red-zone -m64 -fno-stack-protector -fPIE -fno-omit-frame-pointer -fno-asynchronous-unwind-tables -fno-unwind-tables -fcf-protection=none -mno-sse -mno-mmx $(INCLUDES) -DCONFIG_UEFI
ifeq ($(SECURE_BOOT),1)
UEFI_CFLAGS += -DSECOS_SECURE_BOOT
endif
UEFI_CRT0 = uefi/crt0.o

# Same flag tracking as the kernel (FLAGS_STAMP): e.g. a SECURE_BOOT=0 build
# must not leave a non-enforcing loader object behind for the next build.
UEFI_FLAGS_STAMP := .uefi-cflags.stamp
UEFI_FLAGS_SIG := cc=$(CC) $(shell $(CC) -dumpfullversion 2>/dev/null) cflags=$(UEFI_CFLAGS)
ifneq ($(file <$(UEFI_FLAGS_STAMP)),$(UEFI_FLAGS_SIG))
$(file >$(UEFI_FLAGS_STAMP),$(UEFI_FLAGS_SIG))
endif
$(UEFI_FLAGS_STAMP):
	$(file >$@,$(UEFI_FLAGS_SIG))
$(UEFI_CRT0) $(UEFI_OBJS) $(UEFI_CRYPTO_OBJS): $(UEFI_FLAGS_STAMP)

uefi/%.o: uefi/%.c uefi/efi.h $(BOOT_DIR)/kaslr.h
	$(CC) $(UEFI_CFLAGS) -c $< -o $@

# [secure-boot] crypto for the loader (same sources as the kernel, UEFI flags).
uefi/u_sha256.o: $(CRYPTO_DIR)/sha256.c
	$(CC) $(UEFI_CFLAGS) -c $< -o $@
uefi/u_sha512.o: $(CRYPTO_DIR)/sha512.c
	$(CC) $(UEFI_CFLAGS) -c $< -o $@
uefi/u_ed25519.o: $(CRYPTO_DIR)/ed25519.c
	$(CC) $(UEFI_CFLAGS) -c $< -o $@
# [M45] the shared KASLR core (relocation, entropy, window mapping).
uefi/u_kaslr_core.o: $(BOOT_DIR)/kaslr_core.c $(BOOT_DIR)/kaslr.h
	$(CC) $(UEFI_CFLAGS) -fno-tree-loop-distribute-patterns -c $< -o $@

uefi/crt0.o: uefi/crt0.s
	$(CC) -c $< -o $@

uefi: $(UEFI_CRT0) $(UEFI_OBJS) $(UEFI_CRYPTO_OBJS)
	mkdir -p $(EFI_BOOT_DIR)
	$(LD) -nostdlib -znocombreloc -shared -Bsymbolic -T /usr/lib/elf_x86_64_efi.lds \
		-L /usr/lib $(UEFI_CRT0) $(UEFI_OBJS) $(UEFI_CRYPTO_OBJS) -lgnuefi -lefi -o $(UEFI_LOADER_ELF)
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

# Bootable UEFI GPT+ESP disk image (for VMware / physical PC / QEMU+OVMF).
# Builds kernel + loader, then a raw disk with an ESP holding BOOTX64.EFI + kernel.elf.
.PHONY: uefi-disk uefi-vmdk run-uefi-disk
uefi-disk: all uefi
	bash tools/mkuefidisk.sh secos-uefi.img

# VMware-friendly virtual disk (convert the raw image to VMDK).
uefi-vmdk: uefi-disk
	qemu-img convert -f raw -O vmdk secos-uefi.img secos-uefi.vmdk
	@echo "✅ secos-uefi.vmdk ready for VMware"

# [M21] Pre-formatted FAT32 DATA disk for VMware (attach as a second SATA disk,
# driven by the AHCI driver, mounted at /mnt). Contains HELLO.TXT.
.PHONY: data-vmdk
data-vmdk: disk-fat32
	qemu-img convert -f raw -O vmdk disk.img data.vmdk
	@echo "✅ data.vmdk (FAT32) — attach in VMware as a SATA disk; mounts at /mnt"

# Boot the built image locally in QEMU+OVMF (sanity check before real hardware).
# The persistent root (ROOT_DRIVE, virtio "vda") is attached here too; OVMF
# ignores it (no ESP) and boots secos-uefi.img. ROOT=ram leaves it out.
run-uefi-disk: uefi-disk $(ROOT_DEP)
	qemu-system-x86_64 -machine q35 -m 256M -bios /usr/share/ovmf/OVMF.fd \
		-drive file=secos-uefi.img,format=raw,if=ide $(ROOT_DRIVE) \
		-serial stdio -display none -no-reboot
