# SecOS UEFI Boot Issue - Debug Status

## Status
- ✅ QEMU 9.2 with GTK display working (`/usr/bin/qemu-system-x86_64`)
- ✅ UEFI Shell (v2.2, EDK II v2.70) loading and interactive
- ✅ IDE drive recognized as FS0: in UEFI
- ✅ BOOTX64.EFI file visible and readable in FS0:\EFI\BOOT\
- ✅ Kernel binary ready (dist/kernel.elf)
- ❌ **BOOTX64.EFI execution blocked**: UEFI shell reports "Script Error Status: Unsupported"

## Technical Details

### BOOTX64.EFI Format Validation
```
File type: PE32+ executable (EFI application) x86-64
Subsystem: 0x0a (EFI application)
Machine: 0x8664 (x86-64)
ImageBase: 0x0000000000000000
SectionAlignment: 0x1000
Entry Point: 0x1080 (post-crt0)
```

### Build Process
1. Compilation: `gcc` with `-ffreestanding -fPIE`
2. Linking: `ld` with `-T /usr/lib/elf_x86_64_efi.lds` + `crt0-efi-x86_64.o`
3. Object copy: `objcopy` with `--target=efi-app-x86_64`, copying sections:
   - .hash, .gnu.hash, .dynsym, .dynstr
   - .text, .sdata, .data, .rodata
   - .eh_frame, .dynamic, .rel, .rela, .reloc

### Root Cause Hypothesis
The UEFI firmware rejects BOOTX64.EFI during execution with "Unsupported" status, despite:
1. systemd-boot (96K, known working) executes successfully in same environment
2. Our bootloader has identical PE headers structure
3. All required EFI app sections present

**Possible causes:**
- Missing relocation entry in .reloc section
- Position-independent code (PIE) flag incompleteness
- Unresolved symbol at runtime (despite static linking)
- UEFI shell limitation (some UEFI implementations require Boot Manager for app execution)

## Next Steps

### Option 1: Boot Manager Instead of Shell
Instead of UEFI shell, use UEFI Boot Manager to execute BOOTX64.EFI:
```bash
# Modify OVMF_VARS to add boot entry
efi-guids-tool --add-boot-entry

# Or test by booting from UEFI menu (F12 in QEMU)
```

### Option 2: Static Analysis Comparison
```bash
# Compare our BOOTX64.EFI with working systemd-boot
readelf -SW dist/EFI/BOOT/BOOTX64.EFI
readelf -SW /usr/lib/systemd/boot/efi/systemd-bootx64.efi
objdump -d dist/EFI/BOOT/BOOTX64.EFI | head -100
```

### Option 3: Minimal Stub Bootloader
Replace full bootloader with 200-byte stub that only:
1. Prints "Hello" to verify execution
2. Exits
3. Gradually add features if this works

### Option 4: Use GRUB2 EFI
As a workaround, use GRUB2-efi to chain-load SecOS kernel:
```bash
# Install GRUB2-efi
sudo apt install grub-efi-amd64

# Create grub.cfg to load kernel
```

## Files Involved
- **Bootloader**: `uefi/boot.c`, `uefi/elf_load.c`
- **Build**: `Makefile` (lines 135-145)
- **Linker**: `/usr/lib/elf_x86_64_efi.lds`
- **Output**: `dist/EFI/BOOT/BOOTX64.EFI` (13K)
- **Test**: QEMU command in `run_uefi.sh`

## Reference Commands

### Rebuild Bootloader
```bash
cd /home/luigi/secos
make uefi
```

### Update ESP and Test
```bash
mformat -i dist/esp_fat.img ::
mmd -i dist/esp_fat.img ::/EFI ::/EFI/BOOT
mcopy -i dist/esp_fat.img dist/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/
mcopy -i dist/esp_fat.img dist/kernel.elf ::/kernel.elf

/usr/bin/qemu-system-x86_64 \
  -machine pc -cpu qemu64 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=dist/OVMF_VARS.fd \
  -drive file=dist/esp_fat.img,format=raw,if=ide \
  -display gtk -serial file:serial.log -no-reboot
```

### Manual UEFI Shell Testing
```
FS0:
cd \EFI\BOOT
dir BOOTX64.EFI          # Should show 6024-13312 bytes
BOOTX64.EFI              # Currently fails with "Unsupported"
```

## Session History
1. Discovered QEMU 9.2 local build lacks GTK → switched to system QEMU
2. UEFI firmware boots successfully with QEMU machine=pc, IDE drive
3. UEFI shell recognizes FS0: and lists files correctly
4. Tested systemd-boot as control → **execution succeeded** ("Boot in 1 s.")
5. Rebuilt SecOS bootloader with:
   - Added .rodata section
   - Switched to /usr/lib/elf_x86_64_efi.lds
   - Integrated /usr/lib/crt0-efi-x86_64.o
   - Added gnu-efi libraries
6. Result: BOOTX64.EFI still marked "Unsupported" by UEFI

## Success Criteria
- ✅ QEMU displays boot process
- ✅ UEFI firmware loads  
- ✅ Kernel binary prepared
- ❌ **BOOTX64.EFI executes and loads kernel** ← BLOCKER
- ❌ Kernel runs and displays message

---
Last updated: 2025-12-19 01:15 UTC
