# ANALISI GREZZA SECoS

## 1) INVENTARIO REALE

### Directory principali:
- boot/
- arch/x86/
- kernel/
- drivers/
- mm/
- lib/
- fs/
- user/
- uefi/
- edk2/

### File .c .cpp .h .S .asm .ld .efi .elf:
- **File C**: 125 files
- **File H**: 100+ files
- **File S**: 70+ files
- **File ASM**: 3 files
- **Linker script**: linker.ld
- **UEFI files**: 3 files (boot.c, elf_load.c, hello.c)
- **ELF files**: 3 files (kernel.elf, uefi_hello.elf, uefi_loader.elf)
- **EFI files**: 100+ files in edk2/Build/ directory

### Entry points trovati:
- **main**: kernel/kernel.c (function kernel_main)
- **_start**: boot/boot.asm (function _start)
- **efi_main**: uefi/boot.c (function efi_main)

## 2) BUILD SYSTEM REALE

### Makefile:
- **Target principale**: all (builds kernel.bin)
- **File C compilati**: 48 files (kernel, arch/x86, drivers, mm, lib, fs, user)
- **File ASM compilati**: 3 files (boot/boot.asm, arch/x86/idt_asm.asm, arch/x86/syscall_asm.asm)
- **Flag C**: -ffreestanding -O2 -nostdlib -lgcc -m64 -mno-red-zone -mno-sse -mno-sse2 -I. -Iboot -Iarch/x86 -Ikernel -Idrivers -Imm -Ilib -Ifs
- **Flag ASM**: -f elf64
- **Linker script**: linker.ld
- **Output**: kernel.bin

### UEFI Build:
- **UEFI_SRC**: uefi/boot.c, uefi/elf_load.c
- **UEFI_HELLO_SRC**: uefi/hello.c
- **UEFI_CFLAGS**: -ffreestanding -fshort-wchar -O2 -mno-red-zone -m64 -fno-stack-protector -fPIE -fno-omit-frame-pointer -fno-asynchronous-unwind-tables -fno-unwind-tables -fcf-protection=none -mno-sse -mno-mmx -I. -Iboot -Iarch/x86 -Ikernel -Idrivers -Imm -Ilib -Ifs -DCONFIG_UEFI
- **Linker**: Uses /usr/lib/elf_x86_64_efi.lds, creates uefi_loader.elf, then objcopy to create EFI app at $(EFI_BOOT_DIR)/BOOTX64.EFI
- **Output**: EFI application at dist/EFI/BOOT/BOOTX64.EFI

## 3) BOOT FLOW REALE

### UEFI → loader → kernel:
- **UEFI Bootloader**: uefi/boot.c with efi_main function
- **Kernel Loading**: Uses elf_load.c to load kernel.elf from root of FAT volume
- **Memory Management**:
  - UEFI memory map is retrieved and preserved
  - Page tables are constructed with identity mapping for first 512MB
  - PAE enabled, paging enabled, long mode activated
- **Control Transfer**:
  - After ExitBootServices, control is passed to kernel_main with magic=0 and info pointer to secos_boot_info structure
  - Memory map and framebuffer information are passed in secos_boot_info structure

### CPU Mode:
- **Initial**: 32-bit protected mode
- **After boot**: 64-bit long mode

### Paging:
- **Enabled**: Yes, with identity mapping for first 512MB
- **Page Tables**: PML4, PDPT, PDT with 2MB pages

### Stack:
- **Initialized**: In boot.asm at stack_top

## 4) KERNEL ENTRY

### File preciso:
- **kernel/kernel.c**

### Funzione iniziale:
- **kernel_main** function

### Sezione linker:
- **.text** section with entry point defined as _start in linker.ld

### Mappatura memoria iniziale:
- **Identity mapping**: First 512MB mapped with 2MB pages
- **Page tables**: PML4, PDPT, PDT constructed in boot.asm

## 5) PROBLEMI TECNICI OSSERVATI

1. **Memory mapping**: The UEFI loader uses identity mapping for first 512MB only, which may limit kernel memory usage.
2. **Segment handling**: ELF segments are copied to temporary buffers and later remapped, but the actual mapping is deferred until after ExitBootServices.
3. **Page table management**: The page table setup is done in the UEFI bootloader, but the final permissions and memory layout are not fully implemented.
4. **W^X protection**: The code mentions that W^X permissions will be applied with 4KB granular pages, but this is not implemented.
5. **Memory layout**: The kernel assumes identity mapping for simplicity, but this may not be optimal for all use cases.
6. **Kernel ELF loading**: The UEFI loader loads the kernel ELF but does not fully validate memory layout or permissions.
7. **Stack management**: The stack is initialized in boot.asm but may not be fully secure.
8. **Security features**: The code does not implement full security features like SMEP/SMAP, KASLR, etc.
