# UEFI loader — real-hardware boot fix (open bug #1)

**Goal:** make the custom UEFI loader (`uefi/boot.c`) boot on real firmware. It
worked on QEMU/OVMF + VMware but reboot-looped on the ASUS E406S (Braswell, 4 GB),
faulting with no visible `[EXC]` (no serial on that laptop). The current real-HW
boot path is the GRUB hybrid ISO; this restores the native UEFI path.

## Two root causes found (both fixed)

### 1. ACPI tables in reserved memory above the usable-RAM top (the real killer)
`arch/x86/acpi.c:pv()` translates an ACPI physical address to a VA: `< 512 MiB` →
low identity map, else → **physmap** (`phys_to_virt`). But `vmm_init_physmap()`
sizes the physmap to **usable** RAM (`pmm_get_total_memory()`), and firmware places
the ACPI tables in **reserved** memory *above* the usable top. So on any machine
big enough that the tables land > 512 MiB, `pv()` returned a physmap pointer into
an **unmapped** region → kernel ring-0 `#PF` in `acpi_init`.

- Invisible on small-RAM OVMF (smoke runs ~207 MiB → tables < 512 MiB → identity
  path, never touches the physmap). Reproduced instantly at `-m 2G`:
  `[PF] UNHANDLED addr=0xFFFF88807FB7E014 … kernel` (physmap + ~2042 MiB) with
  `total_MB=0x7CD` (1997 MiB usable) — the tables sit in the ~51 MiB reserved gap.
- On the ASUS (4 GB) this fires for sure. It's a kernel-side fault (post-IDT, so it
  *does* emit `[EXC]` — just onto a serial line the laptop doesn't have, hence the
  "shows briefly, resets" with no visible exception).
- **Fix:** `pv()` calls `vmm_extend_physmap(phys + 1 MiB)` before returning the
  physmap pointer. Idempotent, 2 MiB-granular; `acpi_init` runs in phase2 after the
  physmap is initialized, so the extend always engages.

### 2. Loader installed a 512 MiB-only identity map (latent, placement-dependent)
`uefi/boot.c` built a fresh PML4 that identity-mapped only 0–512 MiB and loaded it
into CR3 right before jumping to the kernel — assuming the loader image, stack,
`bootinfo` and framebuffer all live below 512 MiB. OVMF/VMware do place everything
low; real firmware may not, in which case the **next instruction fetch / data
access after the CR3 load is unmapped → triple fault before any kernel IDT** (the
originally-reported "no `[EXC]`, just resets" symptom).

- **Fix (loader):** instead of a fresh 512 MiB PML4, **copy the live firmware
  PML4** (inherit its complete identity map, wherever RAM/MMIO is) and add only
  entry 511 → the kernel high-half PDPT (`install_kernel_mapping`). Strictly a
  superset of the old map → no OVMF/VMware regression.
- **Fix (kernel):** the `bootinfo` struct lives in loader memory (possibly high).
  `kernel_main` now **copies it into a kernel-owned struct in phase1** while still
  on the loader's (full-identity) CR3, before `vmm_init()` installs the kernel's
  own tables — so phase2 (framebuffer) and `acpi_init` never deref a foreign,
  no-longer-mapped pointer.

### Blind-debug aid (kept for the first HW validation)
`uefi/boot.c` paints full-width colored stripes straight into the GOP framebuffer
at each handoff stage (no serial on the ASUS): **blue** = entered/GOP, **cyan** =
ELF loaded, **yellow** = page tables built, **magenta** = ExitBootServices done,
**green** = CR3 installed / jumping to kernel. If the kernel takes over it clears
the screen (bars vanish = success). If the machine stops, the last visible color
says how far the handoff got. Safe after ExitBootServices because the final CR3
inherits the firmware identity map (the framebuffer is mapped wherever it lives).

## Validation
- QEMU+OVMF, raw `secos-uefi.img`, single + with data disk, **`-m 2G / 4G / 8G`**:
  boots clean to coreutils/init, ACPI parsed (`CPUs/lapic/ioapic`), no `[EXC]`.
- `tools/smoke.sh --uefi` and `--mb2`: PASS. Full `tools/selftest.sh`: **179/179**.
- **Pending: validation on the ASUS E406S** (the actual open bug). Watch the stage
  colors; expect a clean boot to the shell.

## Files
- `arch/x86/acpi.c` — `pv()` extends the physmap for high ACPI tables.
- `uefi/boot.c` — `install_kernel_mapping` (copy firmware PML4 + add high half);
  GOP stage bars; removed the 512 MiB-only PML4 build.
- `kernel/kernel.c` — copy UEFI `bootinfo` into kernel memory in phase1.
