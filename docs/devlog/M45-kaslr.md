# M45 — Kernel KASLR (Phase 4 hardening)

The kernel is still **linked** at `KERNEL_VMA = 0xFFFFFFFF80000000`
(`-mcmodel=kernel -fno-pic`), but it now **runs** at `KERNEL_VMA + slide`, where
the slide is a random multiple of 2 MiB chosen at every boot, on both boot paths
(GRUB Multiboot2 and the SecOS UEFI loader). Branch `feat/kaslr`.

```
[KASLR] on: kernel base randomized, 768 slots x 2 MiB (~9.6 bits), entropy=RDRAND, relocs=7774, by MB2 entry
```

## Design

### Why "slide inside the top 2 GiB + link-time relocation table"

Options considered:

| Option | Verdict |
|---|---|
| Build the kernel PIE (`-fpie`, `-mcmodel=small/large`) and self-relocate with `R_X86_64_RELATIVE` | Rejected: changes code generation of the whole kernel (GOT/PLT, `-mcmodel` change, every asm file reviewed), and makes the build very different from what has been validated since M12. |
| Keep `-mcmodel=kernel`, slide within the top 2 GiB, apply relocations from `ld --emit-relocs` (what Linux does with `arch/x86/tools/relocs`) | **Chosen.** Code generation is unchanged; only absolute references to the image need a fixup. |

With `-mcmodel=kernel` every symbol must live in the top 2 GiB (32-bit sign-
extended addresses), so the image can move anywhere in
`[0xFFFFFFFF80000000, 2^64)` as long as the whole mapping fits. The kernel maps
a **512 MiB high-half window** (phys 0..512 MiB, used by the image and by the
PMM bitmap placed at `_kernel_end`), so the window base can take
`(2 GiB − 512 MiB) / 2 MiB = 768` values: **log2(768) ≈ 9.58 bits**. The 2 MiB
granularity keeps the 2 MiB huge-page layout of the window (the W^X pass splits
the image pages to 4 KiB later, as before).

The **physical** load address is unchanged (2 MiB): GRUB loads the ELF at its
LMA, and the low identity map, the AP trampoline and the DMA paths all assume
the image stays below 512 MiB.

### Relocation table (link time)

`tools/kaslr-relocs.py` (Python 3 stdlib only):

1. **Pass 1**: `ld --emit-relocs` keeps the relocation sections in the output.
2. The tool walks every `.rela.*` of an allocated section and keeps the
   absolute relocations whose target `S+A` is a high-half address:
   `R_X86_64_64` and `R_X86_64_32S` (≈2070 + 5700 = ~7770 fields today).
   PC-relative ones (`PC32/PLT32/PC64`) need nothing, because the whole image
   moves together; absolute references to **low** `.boot` symbols (e.g. the
   boot GDT) stay as they are. Every value is cross-checked against the linked
   bytes; GOT relocations, `R_X86_64_32` to a high address, references outside
   the window, overlapping fields and unknown types are hard errors.
3. Output: a 56-byte header + one `u32` per field (bit 31 = 32-bit field,
   bits 0..30 = **physical** address of the field), sorted. ~31 KB.
4. **Pass 2**: the table is linked back as section `.kaslr`, placed **after
   `.data` and before `.bss`**: no section that carries relocations moves, so
   the table describes pass 2 exactly. The Makefile re-extracts the table from
   pass 2 and requires it byte-identical (`kaslr-relocs.py check`), then does
   the final link (pass 2 without `.rela.*`) and signs it as before.

Format and constants: `boot/kaslr.h`.

### Applying it (boot time)

A single freestanding core, `boot/kaslr_core.c`, is compiled twice:

- **Multiboot2**: into the kernel's low `.boot` section (kernel flags without
  stack protector / unwind / jump tables; `objcopy --prefix-alloc-sections=.boot`;
  `kaslr-relocs.py boot-only` asserts the object has only `.boot*` allocated
  sections and no undefined symbol). `boot/boot.asm` `long_mode_low` (64-bit,
  still on the boot identity map) calls `kaslr_boot_mb2()`: it parses the MB2
  command line for `nokaslr`, gathers entropy, applies the table to the loaded
  image in place, writes slide/source/state into the table header, and maps the
  high-half window at the slid base (`pdpt_hi[510]/[511]` → two PDTs, since a
  slid window can straddle the 1 GiB boundary). CR3 is reloaded (TLB flush +
  serialization: the relocated `mov rax, long_mode_high` immediate is then
  executed) and the trampoline jumps to the slid high-half entry.
- **UEFI**: into the loader (`uefi/u_kaslr_core.o`). After the Ed25519 secure-
  boot check and the segment copy, `uefi/boot.c` reads its load options for
  `nokaslr` (`EFI_LOADED_IMAGE_PROTOCOL`), mixes `EFI_RNG_PROTOCOL` output (when
  the firmware has one) into the CPU entropy, applies the table found in the
  verified file (`.kaslr` section → physical address via its PT_LOAD), maps the
  slid window and enters `_uefi_start + slide`.

The relocator refuses to apply a table twice (`KASLR_ST_APPLIED`).

### Kernel side

`kernel/kaslr.c`:

- `kaslr_init()` is the first call of `kernel_main`. It reads the slide from the
  table header and **self-checks** it: the RIP-relative address of
  `_kaslr_start` must equal its relocated `movabs` value (relocations applied),
  and `that address − _kaslr_phys − KERNEL_VMA` must equal the recorded slide.
  Any mismatch halts with `[KASLR] FATAL` instead of running a half-relocated
  image.
- `g_kaslr_slide` + `kernel_vma_base()` (`mm/vmm.h`) replace the hard-coded base
  in `kvirt_to_phys` (all DMA "symbol-as-physical" spots and the PMM bitmap go
  through it), the kernel page-table build (`vmm_build_kernel_pml4_identity_512mb`,
  two PDTs), the W^X pass (`vmm_protect_kernel_sections`, alias-NX loop) and the
  W^X auditor (`vmm_audit_wx`).
- Fault reports (`kernel/panic.c`): `[EXC]` for a ring-0 fault adds
  `lrip=` (the address in `kernel.bin`, usable with `nm`/`addr2line`); the panic
  screen shows both; the verbose uaccess-fixup trace logs only the link-time
  address.

Nothing else needed changing: IDT/GDT/TSS/LSTAR/AP-entry addresses are computed
at run time from (relocated) symbol addresses; the AP trampoline is a flat
binary that receives `ap_entry` and the stack from C; `__ex_table` entries are
`dq` pairs (relocated); jump tables are absolute 32S/64 (relocated).

### Entropy

In order (`kaslr_entropy`): `RDSEED` (CPUID.7:EBX[18]) → else `RDRAND`
(CPUID.1:ECX[30]) → always mixed with `RDTSC`, the PIT channel-0 count and the
RTC seconds/minutes, plus (UEFI) `EFI_RNG_PROTOCOL`. Everything goes through the
splitmix64 finalizer; `slot = x mod 768` (bias < 2^-54). The **source** is
reported (`entropy=RDSEED|RDRAND|EFI_RNG|TSC+PIT+RTC jitter (no hardware RNG)`),
the slide **never** is: neither on the console, nor in the kernel log (dmesg,
`/proc/kmsg`). Only a `-DSECOS_KASLR_DEBUG` build writes
`[KASLR-DEBUG] slide=0x…` and only to raw debugcon (`debugcon_putchar_raw`, which
bypasses the kernel log ring); the selftest uses it.

QEMU's default `qemu64` CPU has no RDRAND: there the slide comes from TSC/PIT/RTC
jitter only (still different at every boot in practice, but not
cryptographically strong). Real hardware and `-cpu max/host` use RDRAND/RDSEED;
OVMF + `-device virtio-rng-pci` gives `EFI_RNG`.

### Opt-outs

- Build: `make CFLAGS_EXTRA=-DSECOS_NO_KASLR` → the table header is marked
  disabled (`kaslr-relocs.py --disabled`); both relocators keep slide 0.
- Boot: `nokaslr` on the multiboot2 line (`make iso KERNEL_CMDLINE=nokaslr`
  appends it to `grub.cfg`), or in the UEFI loader's load options
  (`\EFI\...\BOOTX64.EFI nokaslr` from the shell / a boot entry).

### Stack-protector guard

The canary (`__stack_chk_guard`, `kernel/panic.c`) used to be a compile-time
constant. The table header (version 2) records the guard's **physical address**
(the tool looks the symbol up in pass 1 and requires it in `.data`, which does
not move). Both relocators store a fresh random value there — a second draw,
independent of the slide — **before the first instruction of kernel C code**, so
no protected frame ever saved the old value and no `no_stack_protector`
annotation is needed. The low byte is zeroed (a C-string overflow cannot write
it back); `0` is never used. This happens even with `nokaslr` /
`-DSECOS_NO_KASLR`. The kernel logs `[SSP] stack canary randomized at boot`
(value only on raw debugcon in a `SECOS_KASLR_DEBUG` build).

This only matters once GCC actually reads that variable: on `main`, with
`-mcmodel=kernel`, it read `%gs:0x28` (the IVT through the low identity map).
The fix (`-mstack-protector-guard=global`, commit `913aafa` on
`feat/smap-noident`) is **cherry-picked** on this branch (`3367c30`, identical
patch, so merging both branches is clean); `objdump` shows 0 `%gs:0x28` and
~1000 `__stack_chk_guard` references.

### Kernel addresses in the kernel log

Debugcon output is also kept by the kernel log ring (dmesg, `/proc/kmsg`), so a
runtime kernel address printed there gives the slide away. `kaddr_link()`
(`mm/vmm.h`) converts an image address to its `kernel.bin` value; it is used by
the heap-corruption report (its boot selftest prints `caller=`/`owner=`), the
TSS banner, the verbose uaccess-fixup trace and the `[EXC]` line (`lrip=`). The
gate asserts that no slid image address appears in the boot log.

## Measured boot, secure boot, reproducible builds

- **Measured boot (M37)**: the live `.text`/`.rodata` bytes now depend on the
  slide. `boot_measure()` hashes them through `kaslr_sha256_unrelocated()`, which
  feeds every relocated field to SHA-256 with the slide subtracted. The result is
  the hash of the bytes **as they are in `kernel.bin`**: identical on every boot,
  on MB2 and UEFI, and still equal to the host-side `sha256` of the file range
  (the existing selftest check is unchanged). The table itself lives outside
  `.text+.rodata`, so the measured range is the same as before.
- **Secure boot (M39)**: the signature still covers the whole, unrelocated
  `kernel.elf`, now including `.kaslr`. The UEFI loader verifies first and only
  then reads the table, so a tampered table is refused like any other byte.
- **Reproducible builds (M37)**: the tool is deterministic (sorted output, no
  timestamps, fixed paths); two builds with the same `SOURCE_DATE_EPOCH` stay
  byte-identical (selftest "M37 reproducible build").

## What is and is not randomized

| Region | Randomized? |
|---|---|
| Kernel image (high-half text/rodata/data/bss, boot stack, idle stacks) | **Yes**, 768 × 2 MiB |
| User stack / heap / mmap | Yes (M39 user ASLR, unchanged) |
| Kernel physical load address (2 MiB) | No |
| Physmap (`0xFFFF888000000000`) and the kernel heap (kmalloc lives in the physmap) | No — follow-up |
| M2 kernel-stack region (`0xFFFFFF8000000000`) | No — follow-up |
| Low identity map 0..512 MiB (kernel-only; NX except the first 2 MiB) | No (being removed by the SMAP work) |

Consequence: the image's **executable** mapping is at an unknown address, but
its bytes are also reachable at fixed addresses through the physmap (RW/NX) and,
until it goes away, the low identity map (NX above 2 MiB). KASLR therefore
mainly defeats code-reuse (ROP/JOP) that needs gadget addresses; a data-only
attack that knows a physical address still finds data. Randomizing the physmap
base is the natural next step.

## Tests

- `tools/smoke.sh --mb2` and `--uefi`: PASS (KASLR on).
- New selftest gate "M45" (`tools/selftest.sh`, before the Phase L block):
  5 MB2 boots of a `SECOS_KASLR_DEBUG|SECOS_SMP_AT_BOOT` image → ≥ 4 distinct
  slides, all 2 MiB-aligned and < 1.5 GiB, measurement = file hash in 5/5, W^X
  audit clean in 5/5, no `[EXC]`; `-smp 4 -cpu max` (RDRAND, 4 CPUs online);
  MB2 `nokaslr`; UEFI ×2 (slides differ, secure boot OK, measurement equal to
  MB2's, W^X clean), UEFI + virtio-rng (`entropy=EFI_RNG`), UEFI `nokaslr` via
  `startup.nsh`; a `-DSECOS_NO_KASLR` build; the stack canary is randomized
  (≥ 4 distinct of 5 boots, also by the UEFI loader and with `SECOS_NO_KASLR`);
  no slid kernel address in the log. The default-image block also checks
  "KASLR on" and that `KASLR-DEBUG` appears neither in the log nor the binary.

## Open issues / follow-ups

- Physmap / heap / M2 stack region base randomization (see table).
- Kernel pointers in the kernel log: the boot log is clean (gate-checked) and
  the known printers use `kaddr_link()`, but there is no systematic
  `kptr_restrict`; a ring-0 `[EXC]` still prints the runtime RIP (the system
  halts right after). Physmap/heap addresses are logged freely (not randomized
  anyway).
- The `.kaslr` table stays mapped RW/NX after boot (it is read once more by the
  measurement); it could be made read-only or freed.
- 9.6 bits is the ceiling of a 2 MiB-granular slide inside the 2 GiB
  `-mcmodel=kernel` range with a 512 MiB window; shrinking the window to the
  image + PMM bitmap (≈ 48 MiB) would give ~9.95 bits, 4 KiB granularity would
  need 4 KiB high-half page tables from the first instruction.
- The custom UEFI loader still triple-faults on some real firmware (pre-existing,
  `docs/devlog/UEFI_REALHW.md`); KASLR was validated on OVMF only.
