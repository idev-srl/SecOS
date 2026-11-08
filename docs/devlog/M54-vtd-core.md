# M54 — VT-d core: DMA remapping, opt-in (`feat/vtd-core`)

Phase 5, plan `docs/PHASE5_PLAN.md` §3 "M54" (with D1, D2, §1.5). Base:
`integ/phase5` at `b5af912` (M53a–c merged). **The default boot is
unchanged**: without the boot word `iommu=on` no IOMMU register is touched;
the only difference is one log line (`[IOMMU] DMA remapping off (default;
boot word iommu=on enables it)`, printed only when a DMAR/IVRS exists) and a
new `/proc/iommu` that says `iommu off`.

## What it does

With `iommu=on` and an ACPI DMAR, every PCI function a VT-d unit translates
is attached to **one shared passthrough domain**, so the in-kernel drivers
(which still hand physical addresses to their devices until M55's DMA API)
work unchanged, now behind an IOMMU that is on. Ring-3 drivers (M57) and the
strict kernel domain (M55) build on the same calls.

### `kernel/iommu.{c,h}`: vendor-neutral (plan D1)
- Domains: `IDOM_BLOCKED`, `IDOM_IDENTITY` (IOVA == phys over all RAM and
  every RMRR), `IDOM_PT` (passthrough; identity tables when the hardware has
  no `ECAP.PT` or with `iommu.pt=off`), `IDOM_TRANSLATED` (empty, filled by
  `iommu_map`).
- `iommu_domain_new/free`, `iommu_map/unmap/translate` (page granular;
  `iommu_map` refuses an already mapped range, so a translation only changes
  through unmap + map; **`iommu_unmap` returns after the IOTLB invalidation
  completed** on every unit the domain is attached to),
  `iommu_domain_set_dma_mask` + a bump `iommu_iova_alloc` (test grade; the
  real allocator is M55's), `iommu_attach` (moves a function: the old context
  is invalidated before the new one is written; RMRRs named for the device
  are identity-mapped into a translated domain), `iommu_detach` (blocked =
  context not present, returns after the context-cache + IOTLB invalidation),
  `iommu_dev_domain`, `iommu_dev_covered`, `iommu_irq_compose` (plain MSI
  until M56 plugs interrupt remapping in), `iommu_present`.
- Faults: every record goes to `iommu_report_fault` (counters, last fault,
  **rate-limited log** — 5 lines per second, then `[IOMMU] N DMA fault
  message(s) suppressed (rate limit)` — and a **hook**,
  `iommu_set_fault_hook`, called in interrupt context with no IOMMU lock
  held, for M57 to attribute a fault to a ring-3 binding and kill it).
  `iommu_fault_poll` drains without the interrupt.
- `iommu_ops_t`: what a vendor implements (`dom_init/fini`, `map`, `unmap`,
  `translate`, `set_context`, `covers`, `max_iova`, `irq_compose`,
  `fault_reason`, `fault_poll`, `enable`, `generate`). AMD-Vi (M64) is one
  more ops table.
- `/proc/iommu`: state, per-unit capabilities, invalidation counters
  (context, IOTLB PSI/DSI, waits, write-buffer flushes, timeouts, queue
  errors), domains, one line per translated function with its domain, fault
  counters (total, IRQs, polled, logged, suppressed, overflow) and the last
  fault.

### `arch/x86/vtd.c`, `vtd_pt.c`: Intel VT-d, legacy mode
- **Probe** per DRHD: registers mapped UC, VER/CAP/ECAP logged. Refused
  (left untouched, logged): units with `scopes_incomplete`, graphics-only
  units, segments ≠ 0, a DMAR with `checksum_bad`/`haw_bad`/`truncated`, no
  39/48-bit SAGAW, no fault-recording registers, a register set outside its
  window.
- **Common format**: the deepest AGAW all units share (4 levels / 48 bits
  when SAGAW has it, else 3 / 39), input width capped at MGAW; superpages =
  the SLLPS bits all units share; PT only if every unit has it. Table pages
  come from `pmm_alloc_below(2^haw)` (a unit cannot address a table above
  the host address width — relevant for >512 GiB guests on a 39-bit haw),
  with `clflush` when some unit has `ECAP.C == 0` (QEMU's has C = 0).
- **Domain ids**: 1..min(1023, ND domains); **0 is never handed out**
  (caching mode tags not-present entries with it).
- **Firmware handoff**: a queued invalidation the firmware left on is
  drained and disabled before IQA is reprogrammed; a unit already
  translating keeps TE on while our root table replaces the firmware's (SRTP
  + global context + IOTLB invalidation); PMRs are disabled once we
  translate. Every step is logged. (Not exercised in QEMU: its firmware
  never enables VT-d.)
- **Context entries** (`ctx_write`): an existing entry is cleared, then a
  device-selective context-cache invalidation (old DID) + domain-selective
  IOTLB invalidation; the new entry is written high qword first, then the low
  one with P, then a device-selective context invalidation with **DID 0 in
  caching mode** (spec: not-present → present) or the new DID otherwise, and
  a domain IOTLB invalidation. Context tables are allocated per bus on
  demand.
- **Requester-id aliases**: a function behind a PCIe-to-PCI bridge reaches
  the IOMMU as (secondary bus, 00.0); behind a conventional PCI bridge on
  PCIe, with the bridge's id. Its alias gets its own context, refcounted
  across the functions that share it; an attach that would give the alias to
  a second domain is refused (`IOMMU_EBUSY`) — such functions are one
  isolation group (M53a already marks them not assignable).
- **Mapping** (`vtd_pt.c`): second-level tables, R/W leaf bits only (no
  snoop/EMT, no ATS), non-leaf entries R|W. Translated domains use 4 KiB
  pages; the identity build uses 1 GiB / 2 MiB pages where aligned. Every
  walk is bounded by the depth and every loop advances at least one page. A
  superpage partly inside an unmap range refuses the unmap before anything
  changes. Empty tables are kept until the domain is freed.
- **Flushes**: after a map, caching mode → page-selective IOTLB
  invalidation of the smallest aligned block holding the range (domain if it
  exceeds MAMV or 1 GiB); otherwise a write-buffer flush when `CAP.RWBF`.
  After an unmap, always the IOTLB invalidation + wait. Drain bits when
  `CAP.DRD/DWD`.
- **Invalidation**: queued (one 4 KiB queue, 256 descriptors, wait
  descriptor with a status write to a private page, 200 ms timeout,
  `IQE` recovery by replacing the refused descriptor with the wait
  descriptor), or register-based CCMD/IOTLB without `ECAP.QI` or with the
  boot word `iommu.qi=off`.
- **Faults**: one vector per unit from the M53c allocator (BSP, FEDATA /
  FEADDR compatibility format; FEUADDR 0), masked until translation is on.
  The handler drains every fault-record register (F is RW1C, PFO cleared),
  re-reads FSTS until PPF is clear (a new interrupt only comes once PPF went
  to 0), then reports with no lock held. Stale records from the firmware are
  drained at setup.
- Not done here (by design): interrupt remapping (M56: `iommu_irq_compose`
  is the hook), the DMA API and strict kernel domain (M55), scalable mode,
  first-level tables, ATS/device-TLB, PASID.

### Boot integration (`kernel/kernel.c`)
`iommu_init()` runs right after the M53c vector self-test (it allocates a
vector, and the self-test checks the allocator hands out exactly 144) and
before any driver probe. Attach every covered function to the passthrough
domain, then `enable` (TE on). `iommu=on` without a DMAR logs `[IOMMU]
iommu=on: off (no DMA-remapping hardware), continuing without DMA
remapping`; with only an IVRS, `... (AMD-Vi: not supported before M64)`.

### Locks (`kernel/lock_order.h`)
| Level | Lock | Notes |
|---|---|---|
| S65 (165) `LK_S65_IOMMU_DEV` | `g_dev_lock` | device → domain table, held across a context change |
| S67 (167) `LK_S67_IOMMU_DOM` | `iommu_domain.lock` | page-table edit + the IOTLB flush that follows |
| S68 (168) `LK_S68_IOMMU_UNIT` | `vtd_unit.lock` | registers, queue, context tables, fault records; the only lock the fault IRQ takes |

All irqsave. PMM (S75) and heap (S70, not used under them) nest inside, so
`iommu_map/unmap` can be called under `tx_lock` (N3) or `xhci_lock` (S45).
**Deviation:** the plan put the domain lock at S66, which M53c gave to the
vector allocator; the domain lock is S67.

## Tests

`tools/iommu_gate.sh` (new; private `/tmp`, QMP or pid-file stop, host peers
on random 127.0.0.1 ports) with the per-guest checker `tools/iommu_check.py`;
`tools/selftest.sh` runs it (section "M54"). In-kernel tests:
`kernel/iommu_test.c` (`-DSECOS_IOMMU_TEST`).

### In-kernel edu tests (`[IOMMU-TEST]`, 26 checks)
QEMU's `edu` (1234:11e8) DMA engine, driven by the kernel through a
translated domain with a 28-bit mask:
- **positive**: RAM → edu → RAM through IOVAs ≠ phys, data intact, 0 faults;
  `iommu_translate` agrees; mapping an occupied IOVA is refused;
- **negative 1**: a DMA write to an unmapped IOVA → exactly one fault, edu's
  BDF, that IOVA, reason 5, W; the kernel continues;
- **negative 2 (IOTLB flush)**: map, DMA OK, unmap, DMA again → one fault and
  the page untouched;
- **negative 3**: read-only page: device write faults (reason 5), device read
  works;
- **negative 4**: IOVA ≥ 256 MiB refused by the 28-bit mask (`ERANGE`), the
  IOVA allocator never goes past it, an IOVA past the AGAW refused;
- **blocked**: after `iommu_detach`, DMA faults with reason 2 (context not
  present); the domain is freed;
- **passthrough**: re-attached to the kernel domain, IOVA == phys works;
- **loop**: `iommu.testloops` rounds (1000 in the main guest, 100 in the
  matrix) of map / DMA / unmap / DMA-must-fault, the domain freed and
  rebuilt every 100 rounds: every round faults exactly once, **PMM free
  bytes identical before and after** (delta 0);
- **storm**: 60 back-to-back faulting DMAs: all 60 counted, 30 lines logged,
  the rest suppressed by the rate limit, the fault IRQ delivered every time
  (`irqs == faults`, `polled 0`).

QEMU's edu refuses a transfer that ends exactly at the end of its 4 KiB
buffer (an off-by-one in `edu_check_range` → `hw_error`, QEMU aborts): the
tests copy 2 KiB.

### Gate `tools/iommu_gate.sh` (15 guests, ~20 min)
Every guest: q35, `-m 1G`, an AHCI data disk (ide.3), NVMe, qemu-xhci +
usb-kbd + usb-storage, e1000e on user networking, edu. `iommu_check.py`
checks the driver probe lines, then over the serial bash: 1 MiB raw read of
each disk equal to what the host wrote, 1 MiB raw write that the host reads
back after QEMU exits (AHCI, NVMe, USB MSC), the boot DHCP lease, 1 MiB
download from a host HTTP server and 1 MiB upload to a host TCP sink (md5
both ways), keys typed into the usb-kbd through QMP reaching bash,
`/proc/iommu`, and that `/proc/iommu`'s fault count equals the edu test's
(the real drivers caused none); no `[EXC]`/panic/lockdep/timeout line.

| Guest | Setup | Result |
|---|---|---|
| default | default image (no `iommu=`), q35 + intel-iommu | PASS: `DMA remapping off (default…)`, no `vt-d` line, `/proc/iommu` off, all I/O |
| main | `iommu=on`, aw 39, CM off, 1000 loops | PASS: 26/26, 1064 faults all from edu, all I/O |
| noflush | `-DSECOS_IOMMU_TEST_NOFLUSH` (negative control) | PASS = the test **fails** `dma-after-unmap-faults` (count 0) and the loop |
| aw48-cm | `aw-bits=48,caching-mode=on` | PASS: SAGAW 0x6, 4-level tables, 48-bit IOVA |
| cm-qemu-ptoff | `caching-mode=on,pt=off` (no ECAP.PT) | PASS: kernel domain = identity tables (1G+2M superpages) |
| kernel-ptoff | boot word `iommu.pt=off`, aw 48 | PASS: identity tables although PT exists |
| qioff-cm | boot word `iommu.qi=off`, CM on | PASS: register-based invalidation (0 waits) |
| block-ahci | `iommu.block=00:1f.2` | PASS: the built-in AHCI faults (reason 2), `IDENTIFY failed`, no disk; NVMe fine; the kernel boots — so q35's built-in AHCI really is behind the vIOMMU |
| no-iommu | `iommu=on`, plain q35 | PASS: `iommu=on: off (no DMA-remapping hardware), continuing`, all I/O |
| amd-iommu | `iommu=on`, q35 + amd-iommu | PASS: `off (AMD-Vi: not supported before M64)`, all I/O |
| lockdep-smp4 | `-DSECOS_LOCKDEP`, `-smp 4`, CM on | PASS: `[LDTEST] all 0xA/0xA`, no lockdep report, 26/26 |
| q9-x2apic | QEMU 9.2, `-cpu max,+x2apic`, `intremap=on,eim=on,aw-bits=48,caching-mode=on`, `-smp 4` | PASS: x2APIC mode, fault IRQ delivered, all I/O |
| q9-aw39 | QEMU 9.2, plain intel-iommu | PASS |
| uefi | OVMF q35 + intel-iommu, `iommu=on` in the load options | PASS: DMAR found through the loader's RSDP, 26/26, all I/O |
| fsstress-ahci-root | `fs_stress_drive.py --data vfat` (ext4 root on AHCI sda + FAT32 sdb), `caching-mode=on`, `-smp 2` | PASS: fsstress OK, host `e2fsck -fn` + `fsck.vfat` clean, 0 DMA faults |

The first full run had 10/15: the five failures were gate bugs (an off
system's `/proc/iommu` checked for "on", the lockdep line regex, the UEFI
ESP taking `sda`), fixed and re-run with `--only` (5/5).

### Default boot and regressions
Run on the final tree with the default build (no `iommu=`):
- `tools/smoke.sh --mb2` PASS, `--uefi` PASS (both log `[IOMMU] none`:
  i440fx / plain q35 have no DMAR);
- the selftest sections NETSMP (83 checks: smp4-msix, smp4-poll, smp1-msix,
  lockdep image), M48 (53, incl. stress -smp 4/8, nosmp, x2APIC on QEMU 9.2),
  M53b DMAR/IVRS gate and M53a PCI gate on QEMU 6.2 and 9.2 (incl. UEFI),
  extracted from `tools/selftest.sh` and run alone: **170 passed, 0 failed**.
The full `tools/selftest.sh` was not run (the integrator does).

### Warnings
`-Wall -Wextra`: 35 in the normal build and 35 with `-DSECOS_LOCKDEP`
(base `b5af912`: 35 / 35); also 35 with `-DSECOS_IOMMU_TEST`, with `-DSECOS_IOMMU_TEST
-DSECOS_LOCKDEP` and with `-DSECOS_IOMMU_TEST_NOFLUSH`; none in the new files.

## Findings

- **QEMU's caching mode does not catch a missing flush after a map.** The
  plan expected `caching-mode=on` to make the hardware cache not-present
  entries, so that a missing flush-after-map fails. An experiment build that
  skipped the page-selective invalidation after every map passed all 26 edu
  tests under `caching-mode=on`: QEMU's emulated IOMMU caches only
  successful translations; CM only changes what QEMU notifies its shadowing
  users (vfio, vhost). The flush is implemented as the spec requires, but
  QEMU cannot prove it; real hardware with CM (VMware's virtual IOMMU sets
  it) is the only check. The **unmap** flush, in contrast, is proven: QEMU
  caches translations, and the NOFLUSH control lets the stale DMA through.
- QEMU's IOMMU (6.2 and 9.2): `ECAP.C = 0` (page walks not coherent, so the
  `clflush` path runs), one fault-recording register (NFR 1: a second fault
  while the first is not cleared only sets PFO), PSI and MAMV present, PT
  on unless `pt=off`, SLLPS 2M+1G, ND 65536 domains; an unmapped page faults
  with reason 5/6 (write/read), not a "not present" code.
- The DMAR lists every device QEMU created at ACPI build time, the q35
  built-ins included (00:1f.0/2/3), and the built-in AHCI is translated
  (the block-ahci guest). With `-device edu` etc. the scope count grows; still
  no `INCLUDE_PCI_ALL` (M53b's finding).
- With the UEFI ESP on vvfat (`-drive file=fat:…`, default `if=ide`) the ESP
  is AHCI port 0 = `sda`; a data disk on ide.3 is `sdb`.
- **devfs block nodes refuse `O_TRUNC`** with EACCES, even for root, so
  `dd of=/dev/sda` without `conv=notrunc` fails ("Permission denied"). Not
  touched here (devfs is not this step's file); a later usability fix.
- BusyBox `ping` needs raw sockets (`can't create raw socket: Protocol not
  supported`); the gate checks the network with TCP both ways instead.

## Shared files touched (small, additive)
- `kernel/kernel.c`: one `iommu_init()` call after the vector self-test.
- `kernel/lock_order.h`: S65, S67, S68.
- `fs/procfs.c`: the `/proc/iommu` node.
- `Makefile`: four sources.
- `tools/selftest.sh`: the M54 section (runs `tools/iommu_gate.sh`).
- `tools/fs_stress_drive.py`: `--qemu-pre`.

## Not verified here
- Firmware handoff (TE / QIE / PMRs left on by firmware), multiple DRHDs,
  RMRRs, non-zero segments, `ECAP.C = 1` units: QEMU has none of them. The
  paths are written from the spec, bounded, and logged; VMware's virtual
  IOMMU and real hardware are the next check.
- The caching-mode flush after a map (see Findings).

## For M55 / M56 / M57 / M64

- **Domains**: `iommu_domain_new(IDOM_TRANSLATED)` + `iommu_map/unmap` are
  ready for M55's `dma_alloc_coherent` and M57's `DMA_ALLOC`. `iommu_unmap`
  waits for the IOTLB invalidation, so a frame may be freed when it returns
  (the D10 teardown order relies on this). `iommu_detach` likewise waits for
  the context + IOTLB invalidation.
- **M55 strict domain**: create one `IDOM_TRANSLATED` (or `IDOM_IDENTITY`
  emptied) domain, identity-map `.dmabss` + coherent frames with
  `iommu_map(d, phys, phys, …)`, and `iommu_attach` every kernel-driven
  function to it instead of `iommu_kernel_domain()`. The RMRR identity maps
  are added by `iommu_attach` for translated domains. Translated domains use
  4 KiB pages only; the identity build already uses superpages
  (`vtd_pt_map(..., huge, idem)`).
- **M56 interrupt remapping**: `iommu_irq_compose()` is the hook (today plain
  `msi_compose`); the VT-d unit lock (S68) is the natural place for the IRTE
  table and its invalidation (`inv()` / `inv_sync()` take one more descriptor
  type, IEC = 4). Keep the fault-event interrupt (FEDATA/FEADDR) in
  compatibility format: it is never remapped. With x2APIC IDs > 254 the fault
  vector needs FEUADDR (EIM) — today a BSP ID > 254 leaves the unit without a
  fault interrupt (logged; `iommu_fault_poll` still works).
- **M57 attribution**: `iommu_set_fault_hook()` gets every fault in IRQ
  context with no IOMMU lock held; look up the binding by `bdf` and kill.
  Do not call `iommu_detach` from the hook (it spins on invalidations under
  the unit lock); defer it to the teardown path.
- **Aliases**: functions behind a PCIe-to-PCI / conventional bridge share a
  refcounted alias context and can only be in one domain (`IOMMU_EBUSY`).
- **Limits**: 1023 domain ids (min with ND), PCI segment 0 only, 32 alias
  contexts, one 4 KiB invalidation queue per unit. `iommu_iova_alloc` is a
  never-reusing bump allocator (tests only).
- **M64 (AMD-Vi)**: implement `iommu_ops_t` and call `iommu_register_ops`
  from a probe next to `vtd_probe()` in `iommu_init` (the IVRS branch logs
  "not supported before M64" today).
- `make`-level: `-DSECOS_IOMMU_TEST` adds the edu tests and the
  `iommu.block=` / `iommu.testloops=` boot words; `-DSECOS_IOMMU_TEST_NOFLUSH`
  is the negative control. Boot words always available with `iommu=on`:
  `iommu.qi=off`, `iommu.pt=off`.
