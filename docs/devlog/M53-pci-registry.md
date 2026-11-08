# M53a — PCI device registry, ECAM, isolation groups (`feat/pci-registry`)

Phase 5 groundwork (plan `docs/PHASE5_PLAN.md` §3 M53a). No behaviour change
for the drivers: every probe sees the same devices in the same order.

## What changed

- **One enumeration at boot** (`drivers/pci_registry.c`, `pci_init()` called
  from `kernel_main_phase2` right after `acpi_init`, before the APs start). The
  same brute force over buses 0..255 that `pci_find` used to repeat on every
  call, in the same order, into a read-only table of `pci_dev_t`
  (`drivers/pci.h`): BDF, ids, class, header type, subsystem ids, INTx line/pin,
  BARs with base, flags and size, capability offsets (PM, MSI, MSI-X, PCIe, AF),
  PCIe port type, extended capabilities (AER, ACS with its capability/control
  words, ATS, SR-IOV), FLR support (PCIe DevCap bit 28 or AF TP+FLR), bridge
  secondary/subordinate buses, the upstream bridge, and the isolation result.
  Up to `PCI_MAX_DEVS` (256) functions; past that, `pci_find*` fall back to the
  old live scan and the boot line says how many were not recorded.
- **`pci_find` / `pci_find_class`** search the table; API and first-match order
  unchanged, so no caller changed. New lookups: `pci_dev_count/at/index`,
  `pci_dev_lookup(seg,bus,slot,func)`, `pci_dev_of(pci_device_t*)`,
  `pci_bridge_for_bus(seg,bus)` (for M53b's DMAR device-scope walk),
  `pci_config_read32_ext`, `pci_ecam_active`, `pci_assign_reason`.
- **BAR sizing** happens once, at boot, for every function except host bridges
  (as Linux), with I/O + memory decode off and interrupts off (single CPU at
  that point). `pci_bar_size()` returns the cached size unless the BAR moved,
  so `pci_bar_map()` no longer re-sizes a live device.
- **Config access** (`drivers/pci.c`):
  - CF8/CFC pairs run under **`pci_cfg_lock`** (`LK_S64_LEAF`, IRQ-safe, nothing
    nested inside). `pci_config_write16`'s read-modify-write is one lock hold.
    This closes the latent SMP race (two CPUs interleaving CF8 writes).
  - **ECAM** from the ACPI **MCFG** table (`arch/x86/acpi.c`: `parse_mcfg`,
    `acpi_mcfg_count/get`; checksum and length checked, at most 8 entries).
    Segment 0 only (other segments are logged and ignored). The window is
    refused if not 1 MiB aligned, start > end bus, outside the 64 TiB physmap,
    or overlapping RAM; otherwise mapped with `vmm_map_mmio(UC)`.
  - **Self-check** before ECAM is used: for every slot/function of every bus in
    the window the id dword must match through both mechanisms, and for every
    enumerated function the 64-byte header (status, BIST and a bridge's
    secondary status masked). Any difference → `[PCI] ecam self-check FAIL ...`
    and CF8/CFC stays. Then `[PCI] ecam self-check OK (N functions)` and every
    config access in range goes through ECAM (one MMIO access, no lock).
  - Boot word **`pci=noecam`** skips ECAM.
- **Isolation groups** (for M57). A function is *assignable* (may be given to a
  ring-3 driver alone) when:
  - it is not a bridge (header type 1/2 or class 06) and not the IOMMU itself
    (class 08/06 — QEMU's `amd-iommu` is a PCI function);
  - no bridge on its path is conventional PCI or PCIe-to-PCI (those alias the
    requester ID: `no(behind-pci-bridge)`, grouped at the highest such bridge);
  - every root port / downstream port on its path has ACS SV+RR+CR+UF in its
    ACS capability register (`no(port-without-acs)`);
  - if it is one of several functions of a multifunction slot, it has ACS
    itself (`no(multifunction-without-acs)`, grouped with function 0);
  - its bus is reached from the root bus by valid bridges (`no(orphan-bus)`).
  `pci_dev_t.group` is the registry index of the group representative. M57
  should enable the ACS controls on the path before trusting a group (this
  step only reads the capability bits; it changes no ACS control).
- **Listings:** boot log one `[PCI] bb:dd.f vvvv:dddd class cccccc hdr N <type>
  ... up <bridge> grp G assign yes|no(reason) <caps> barN=...` line per
  function plus `[PCI] registry: N functions, config ecam|cf8, M assignable`;
  `/proc/bus/pci/devices` (Linux format: BDF, ids, IRQ, 7 bases with flag bits,
  7 sizes; no driver column); `/proc/bus/pci/topology` (SecOS: PCIe type,
  upstream bridge, group, assignable, ACS, FLR/ATS/MSI-X); kernel shell `pci`.

## Deviations from the plan

- **Root-bus conventional functions are assignable.** The plan says "a
  root-complex integrated endpoint, or below a PCIe root port with ACS". QEMU's
  `edu` (the M57 gate device) and the i440fx devices are conventional PCI on the
  root bus: they have their own requester ID and no bridge to alias them, so
  (as Linux does) they get their own group. The multifunction rule then keeps
  the q35 ICH9 functions (LPC/AHCI/SMBus at 00:1f.x, conventional, no ACS)
  together — **AHCI on q35 is not assignable alone** (relevant for M63).
- The registry lives in a new file, `drivers/pci_registry.c` (+ the internal
  `drivers/pci_internal.h`), not in `pci.c`, so M53c's MSI move out of `pci.c`
  does not collide with it. `pci.c` keeps only the accessors and the BAR/MSI
  helpers; its MSI/MSI-X functions are untouched.
- Extra: the IOMMU-is-never-assignable rule and the `pci=noecam` boot word.

## Tests

`tools/pci_gate.sh [--uefi] [--qemu BIN]` (new; private `/tmp` via
`unshare -rm`, QEMU stopped through QMP or its pid file; per guest
`tools/pci_check.py` compares the boot listing with QMP `query-pci`, checks the
ECAM state and the driver probe lines, and fails on `[EXC]`/panic/lockdep).
Guests: i440fx (virtio-blk, e1000 ×2, xHCI + usb-kbd + usb-storage), q35,
q35 + `intel-iommu`, q35 + `amd-iommu` (AHCI disk, NVMe, e1000e ×2 — one behind
a `pcie-root-port` —, e1000 behind a `pcie-pci-bridge`, xHCI + kbd + MSC), and
q35 UEFI/OVMF.

| Run | Result |
|---|---|
| gate, QEMU 6.2, incl. `--uefi` | 5/5 PASS (listing = query-pci: 9/13/13/14/13 functions) |
| gate, QEMU 9.2, incl. `--uefi` | 5/5 PASS (q35 guests at -smp 4 in a manual run too) |
| driver probe lines vs the base kernel (96bb08b), i440fx + q35 | identical |
| q35 + intel-iommu, `-m 8G` | identical driver lines, ECAM OK |
| negative: `-DSECOS_PCI_ECAM_TEST=1` (ECAM window off by one bus) | `self-check FAIL at 00:00.0 id ecam=10d38086 cf8=29c08086`, CF8 used, drivers identical |
| negative: `-DSECOS_PCI_ECAM_TEST=2` (misaligned MCFG base) | `refused: base not 1 MiB aligned`, CF8 used, drivers identical |
| negative: `pci=noecam` | CF8 used, drivers identical; 5 instead of 6 assignable (no ACS readable without ECAM → the function behind the root port is not assignable: the safe direction) |
| lockdep build, -smp 4, q35+intel-iommu and i440fx | `[LDTEST] all 0xA/0xA`, no report; `pci_cfg` 164 in `/proc/lockstat`; 4 parallel readers of `/proc/bus/pci/topology` |
| `/proc/bus/pci/{devices,topology}`, `ls /proc/bus/pci` (bash), kernel shell `pci` (`-DSECOS_NO_INIT -DSECOS_NO_BASH`) | as expected |
| `tools/smoke.sh --mb2` / `--uefi` | PASS / PASS |
| `-Wall -Wextra` warnings, normal and `-DSECOS_LOCKDEP` | 0 / 0 (base 0 / 0) |

## Findings

- `pci_config_write16` was a read-modify-write over two unlocked CF8 pairs.
  The RMW also writes the other half of the dword back, so a write to the
  command register writes the status register's RW1C bits back (clearing any
  that were set). That semantics is kept on both paths (no behaviour change);
  M57's mediated config writes should write 16 bits through ECAM or mask RW1C.
- QEMU facts: `pcie-root-port` advertises ACS 0x5f (SV TB RR CR UF DT);
  e1000e/NVMe/xHCI on `pcie.0` are RC integrated endpoints; `pcie-pci-bridge`
  is type 7 (PCIe-to-PCI); `amd-iommu` is PCI function 00:03.0 class 0806;
  QEMU 6.2 advertises no FLR on e1000e or NVMe, QEMU 9.2's NVMe has FLR (still
  none on e1000e: M57/M58 reset needs the PM-reset fallback); `-cdrom` on q35
  takes `ide.2`.
- **vvfat write-back damaged `dist/kernel.elf`**: QEMU's `fat:rw:dist/` (used by
  `tools/smoke.sh --uefi` and the UEFI run targets) writes back into the
  directory. After a few runs `dist/kernel.elf` differed from `kernel.bin` and
  the UEFI guest showed nothing at all on debugcon (first seen as a QEMU 9.2
  failure; the base kernel had the same problem with a fresh `dist/`, it is not
  a kernel bug). `make uefi` restores it. `tools/pci_gate.sh` boots a fresh
  copy of `dist/`; smoke.sh still uses the directory itself (not changed here).
- `/proc/bus/pci/devices` is generated into procfs' 16 KiB buffer: about 60
  functions fit; beyond that the file is truncated (no overflow).

## For M53b / M53c / M54+

- The table is filled once and read-only after `pci_init()`; no hotplug. Read
  it without a lock. Segment 0 only.
- DMAR/IVRS device scopes: start at the scope's bus, follow the path entries
  with `pci_dev_lookup` (secondary bus of each bridge = `sec_bus`), or go up
  with `parent` / `pci_bridge_for_bus`.
- After `pci_init()` all config accesses for covered buses are ECAM (atomic,
  lock-free); the CF8 path remains for i440fx and refused windows.
- M53c moves the MSI functions out of `pci.c`; they use only the public
  accessors and `pci_find_capability`, so the move needs no registry change.
  Later code can use `pci_dev_t.cap_msi/cap_msix` instead of re-walking.
