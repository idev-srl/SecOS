# M53b — ACPI DMAR / IVRS parsing (read-only)

Branch `feat/acpi-dmar` (from `96bb08b`). Phase 5 groundwork, plan
`docs/PHASE5_PLAN.md` §3 "M53b". No IOMMU register is touched. The default
boot only gains a few `[IOMMU]` log lines.

## What it does

At boot, right after `acpi_init()`, `acpi_dmar_init()` looks up the **DMAR**
(Intel VT-d) and **IVRS** (AMD-Vi) tables, parses them into static structures
and logs them:

```
[IOMMU] DMAR: 1 DRHD haw=39 flags=0x1 INTR_REMAP rmrr=0 atsr=0 rhsa=0 satc=0
[IOMMU] DRHD 0: base=0xfed90000 seg=0 scopes=8
[IOMMU]    ioapic#0=ff:00.0 dev=00:00.0 dev=00:01.0(display) bridge=00:02.0[bus 01-01] bridge=00:03.0[bus 02-03] dev=00:1f.0
[IOMMU] IVRS: 2 IVHD primary-type=0x11 ivinfo=0x2801 va=0 pa=40 ivmd=0
[IOMMU] IVHD 0: type=0x10 (legacy) iommu=00:02.0 base=0xfed80000 seg=0 flags=0xd1 entries=12
[IOMMU] IVHD 1: type=0x11 (used) iommu=00:02.0 base=0xfed80000 seg=0 flags=0x11 entries=12
[IOMMU]    dev=00:00.0 ... range=01:00.0..01:1f.7 ... alias=03:00.0..03:1f.7->03:00.0 ... special-ioapic#0=00:14.0
[IOMMU] none                      (neither table: i440fx, plain q35, most laptops without VT-d)
```

A DRHD made only of display endpoints is tagged `graphics-only`. Each RMRR is
logged with its range and devices. ATSR and SATC are logged. RHSA proximity
domains are attached to their DRHD (`node=N`). A table with problems gets a
`DMAR WARN:` / `IVRS WARN:` line: bad checksum, bad haw, truncated walk, and
the counts of invalid, dropped and unknown entries.

### Parsed content
- **DMAR**: header (haw, the flags `INTR_REMAP`, `X2APIC_OPT_OUT` and
  `DMA_CTRL_PLATFORM_OPT_IN`); DRHD (register base, size field, segment,
  `INCLUDE_PCI_ALL`, device scopes); RMRR (base, inclusive limit, scopes);
  ATSR (`ALL_PORTS`, root-port scopes); RHSA; ANDD (counted); SATC
  (`ATC_REQUIRED`, scopes). Unknown types, such as SIDP, are counted and skipped.
- **Device scopes** (endpoint, bridge, IOAPIC, HPET, ACPI namespace) are
  resolved to a BDF by walking the path through the bridges' secondary bus
  numbers (VT-d 8.3.1). A bridge scope also records its secondary..subordinate
  range. An endpoint whose class code is 03h is marked display.
- **IVRS**: header (IVinfo); IVHD types 10h/11h/40h (devid, capability offset,
  base, segment, info, attributes, EFR/EFR2 images); device entries: ALL,
  select, start/end range, alias select/range, extended select/range, special
  (IOAPIC/HPET handle → devid) and ACPI HID; IVMD 20h/21h/22h. The OS uses
  the blocks of the **highest IVHD type present** (`primary`). Lower-type
  blocks describe the same IOMMU for older OSes, are kept but marked
  `(legacy)`, and the lookups ignore them.

### API for M54 / M56 / M64 (`arch/x86/acpi_dmar.h`)
- `acpi_dmar_get()` / `acpi_ivrs_get()`: the parsed tables. They are never
  NULL; `present == 0` means the table is absent. A unit with `valid == 0`
  does not exist. `scopes_incomplete` means some of its scopes were rejected
  or dropped, so its coverage is not fully known. M54 should then treat the
  unit conservatively, for example as not usable for strict mode.
- `acpi_dmar_drhd_for(seg, bdf)`: the unit that translates a requester. The
  order is an explicit endpoint scope, then a bridge scope whose bus range
  holds the bus, then the segment's `INCLUDE_PCI_ALL` unit.
- `acpi_dmar_rmrr_covers(seg, bdf, &first)`: the device needs RMRR identity
  maps.
- `acpi_dmar_ioapic_sid()` / `acpi_dmar_hpet_sid()`: source ids for interrupt
  remapping (M56).
- `acpi_ivrs_ivhd_for(seg, devid, &alias)`: the primary IVHD that lists a
  device, and the requester id it really uses (alias entries).
- `acpi_ivrs_special_devid(variety, handle, &devid)`: IOAPIC/HPET devids for
  AMD IR.

The parsed state is written once at boot, before the APs start, and is
read-only afterwards. No lock.

## Untrusted firmware input
Every length, count and offset comes from the firmware:
- A structure or scope is accepted only if its length is at least the
  minimum for its type and fits in what is left of its parent. If a length
  fails that check, the walk stops and is marked `truncated`, because nothing
  after it can be located.
- A structure whose length is sane but whose contents are bad is skipped and
  counted in `invalid`. Examples: an unaligned or zero register base, an RMRR
  with limit < base or a limit+1 that is not 4 KiB-aligned, an IVMD that wraps
  around, a second `INCLUDE_PCI_ALL` unit on the same segment, a duplicate
  register base, a path of more than 8 hops, a dev > 31 or fn > 7, an IVHD
  device entry type ≥ 80h other than F0h (its length is unknown), and range
  ends without a start.
- Entries beyond the static limits are counted in `dropped`: 16 DRHD, 32
  RMRR, a pool of 128 scopes, 16 IVHD, 32 IVMD, a pool of 256 device entries.
- Every loop also has an iteration guard. Offsets strictly increase, so the
  guards are a second line of defence, set above what a legal table needs.
- The bridge walk only reads config space. It requires the bus number to
  strictly increase at each hop, so a firmware or topology loop cannot spin.
- A table larger than 1 MiB is refused.
- A bad checksum is logged and the table is still parsed (ACPICA behaves the
  same way). `checksum_bad` is exposed so M54 can decide.

## PCI config reads: the one hook for M53a
Scope resolution needs config reads: vendor/header type/class, plus bridge
bus numbers. They go through one function,
`acpi_dmar_cfg_read32(seg, bus, dev, fn, off)`, in its own file
`arch/x86/acpi_dmar_pci.c`:
- Today it calls `pci_config_read32()` (CF8/CFC, segment 0 only; another
  segment returns all-ones, so its scopes show as absent).
- **When M53a's registry lands**, switch this one function to the registry
  or ECAM accessor, which reaches other segments and is locked. Nothing in
  `acpi_dmar.c` changes. Because the read goes through `pci_config_read32`,
  it already picks up M53a's `pci_cfg_lock` if M53a puts the lock there.
- Resolution runs once at `acpi_dmar_init()` time, before any PCI driver and
  before the APs, so the race on the unlocked CF8 is not an issue here.

## Shared-file changes (small, additive)
- `arch/x86/acpi.c/.h`: `acpi_table_find(sig, &len)` maps a whole table by
  signature. It records the RSDT/XSDT that `acpi_init()` found (`g_sdt`).
  M53a adds an MCFG lookup to the same file. If it adds its own find helper,
  keep one of the two at merge time.
- `kernel/kernel.c`: one call to `acpi_dmar_init()` right after `acpi_init()`.
- `Makefile`: two sources. `.gitignore`: the harness binary.
- `tools/selftest.sh`: one section that runs `tools/acpi_dmar_gate.sh`.

## Host harness (`tools/acpi_fuzz/`, `tools/acpi_tables.py`)
- `acpi_fuzz` builds `arch/x86/acpi_dmar.c` **unchanged** with ASan+UBSan.
  It supplies a stub `acpi_table_find()` and a stub config space:
  - `dump` mode uses a fixed topology (host bridge, display, USB, a root port
    to a bridge to a NIC, ISA);
  - `fuzz` mode randomizes the topology every iteration (bridge loops, bogus
    bus numbers, random absent reads).
- `fuzz ITER SEED tables...` mutates the seeds in many ways: bit flips,
  random bytes and words, the length field, truncation, extension, splicing
  between seeds, and an optional checksum fix-up. Each input sits in an
  exact-size heap buffer, so any read past the table is an ASan error. Every
  iteration runs:
  - both parsers;
  - the boot entry `acpi_dmar_init()` with its logs;
  - every lookup helper on random BDFs;
  - consistency checks: pool ranges, counts within limits, path lengths.

  Iteration i uses seed SEED+i, and a crash or hang (SIGALRM 2 s) prints that
  seed.
- `acpi_tables.py gen DIR` writes 10 seed tables:
  - QEMU-like DMAR;
  - a client-laptop DMAR (iGPU unit, catch-all, 2 RMRRs, ATSR, RHSA, ANDD,
    SATC, all three flags);
  - bridge paths, an absent device, a broken path and an over-deep path;
  - invalid entries, a truncated table, 20 DRHDs (limit), QEMU 9.2-like IVRS
    10h+11h;
  - a full IVRS (10h/11h/40h, ALL, ext, ACPI HID, special HPET, IVMD
    20h/21h/22h, bad entries), a bad checksum and an unknown entry type.
- `acpi_tables.py check HARNESS DIR` compares `acpi_fuzz dump` with the
  values it encoded. These are known answers, not only "doesn't crash".

## Tests run
- `tools/acpi_dmar_gate.sh`: **20/20 PASS**:
  - known answers 10/10;
  - fuzz 50 000 clean;
  - q35 + intel-iommu: one DRHD, `INTR_REMAP`, base 0xfed90000, IOAPIC sid
    ff:00.0, scopes resolved;
  - q35 + intel-iommu with two PCIe root ports, a pcie-pci-bridge, e1000e and
    e1000: bridge scopes with bus ranges 01-01 / 02-03;
  - q35 + amd-iommu on QEMU 6.2 (IVHD 10h; IOAPIC special entry; alias range
    for the PCI bridge's bus);
  - QEMU 9.2 amd-iommu (10h + 11h, 11h used);
  - plain q35 and i440fx: `[IOMMU] none`;
  - UEFI/OVMF q35 + intel-iommu: DMAR found through the loader's RSDP;
  - every guest reaches `/bin/init` with no `[EXC]`, and no `[IOMMU] WARN`
    appears on any QEMU table.
- Longer fuzz run: 5 000 000 iterations (seed 1000) clean, 1 m 50 s.
- **Negative control** for the harness: a copy of the parser with the
  scope-length bound check removed is caught within 108 iterations
  (heap-buffer-overflow in `dmar_scopes`).
- `tools/smoke.sh --mb2` PASS, `--uefi` PASS (both log `[IOMMU] none`).
- `-DSECOS_LOCKDEP` image, q35 + intel-iommu, `-smp 4`: `[LDTEST] all 0xA/0xA`,
  no `[LOCKDEP]` report, boots to init.
- Warnings: 0 in the normal build and 0 in the lockdep build (the base also
  had 0 and 0).

## Findings
- **QEMU's DMAR has no `INCLUDE_PCI_ALL`.** QEMU 6.2 and 9.2 both emit one
  DRHD that lists every device on bus 0 explicitly: endpoints, plus bridge
  scopes for root ports and bridges, and the IOAPIC as ff:00.0. The plan
  expected an `INCLUDE_PCI_ALL` unit. So M54 must handle explicit scopes
  (bridge ranges) from the start. The catch-all path is covered only by the
  synthesized tables.
- QEMU sets `INTR_REMAP` with a plain `-device intel-iommu`, because
  `intremap` defaults to auto. M56 must still check `ECAP.IR` on the unit,
  not only the DMAR flag.
- QEMU 6.2 emits only IVHD 10h. 9.2 emits 10h + 11h for the same IOMMU, so
  M64 must use only the primary blocks (the API already does).

## For M54+
- Call `acpi_dmar_get()` after `acpi_dmar_init()` (kernel.c, right after
  `acpi_init()`). It is safe at any later time.
- Honour `scopes_incomplete`, `checksum_bad` and `haw_bad`. An RMRR named for
  an absent device (`resolved == 0`) has no BDF worth mapping.
- `gfx_only` units: the plan says to never translate them by default.
- A device behind a conventional PCI bridge reaches the IOMMU with the
  bridge's requester id (AMD: the alias entry, `acpi_ivrs_ivhd_for`'s
  `*alias`; Intel: the bridge scope). This matches M53a's isolation-group rule.
