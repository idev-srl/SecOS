# M56 — VT-d interrupt remapping (`feat/vtd-intremap`)

Phase 5, plan `docs/PHASE5_PLAN.md` §3 "M56" (and D4). Base: `cba08ac`
(`feat/vtd-core`, M54 on top of M53a–c). **The default boot is unchanged**:
interrupt remapping (IR) exists only with the boot word `iommu=on`, which
stays opt-in until M59. With `iommu=on` on hardware that has IR, it is on by
default; the boot word `iommu.ir=off` keeps the M54 behaviour.

## Why

Without IR, a device that can DMA can also write any `{0xFEEx_xxxx, data}`
pair. That is an MSI, so the device can raise any vector on any CPU. With IR,
every interrupt request goes through an entry that names its vector, its
destination and the source allowed to use it, and the unit blocks the rest.
The user's decision is that M57 will **require** IR for ring-3 drivers that
do DMA. The query for that is `iommu_ir_enabled()` (and `iommu_ir_covers(dev)`
per function).

## Design

### Table: one entry per vector (`arch/x86/vtd_ir.c`)
- There is one shared 4 KiB table of 256 entries, programmed into every unit
  (IRTA with S=7, SIRTP, then a global IEC). EIME is set when EIM is used.
- **The entry index is the vector number.** The M53c allocator gives every
  vector to exactly one source system-wide, so:
  - vector → entry never collides;
  - re-composing a message is idempotent (`pci_irq_setup_single` composes
    twice, and nothing leaks);
  - `irq_vec_free()` clears exactly the right entry.
  The IOAPIC lines use their fixed vectors (0x21 keyboard, 0x2C mouse).
  MSI messages are allowed only for allocator vectors (0x50–0xDF), so a
  message can never point at an exception, the syscall gate (0x80), an IPI
  (0xF0–0xF2) or an IOAPIC line's entry.
- Entry contents:
  - present, physical destination mode, fixed delivery;
  - edge trigger, or level for a level-triggered IOAPIC line;
  - the vector;
  - the destination: APIC ID in bits 47:40 (xAPIC format), or bits 63:32
    with EIM;
  - **source-id validation**: SVT=01b SQ=00b with the requester id (an exact
    16-bit match); SVT=10b with the bus range for a function behind a
    PCIe-to-PCI bridge (the M54 alias rule, `vtd_dev_requester`); for an
    IOAPIC, the DMAR scope id (`acpi_dmar_ioapic_sid`).
- Messages use remappable format: address `0xFEE | handle[14:0]<<5 | 1<<4 |
  SHV<<3 | handle[15]<<2`, SHV=1, data 0 (subhandle 0). Multi-message MSI
  therefore lands on entry vec+i: `pci_msi_enable` now composes every
  message of the block.
- Updates:
  - **new owner** (different source id): clear P, invalidate, write the high
    half, write the low half with P, invalidate. Neither source can use the
    other's entry at any moment;
  - **retarget** (same owner, `irq_vec_set_cpu`): one atomic 64-bit store of
    the low half, then invalidate;
  - **free** (`irq_vec_free`): clear P, invalidate (wait), clear the high
    half.
  Every invalidation is index-selective IEC + wait on every remapping unit.
  `clflush` follows table writes when `ECAP.C=0` (QEMU), like M54's tables.
- **Compatibility format blocked**: GSTS.CFIS is cleared if the firmware
  left it set, and never set.
- The IOMMU's own fault-event MSI stays in compatibility format and is never
  remapped (as M54 noted). FEADDR/FEDATA do not go through the table.

### Conditions (otherwise IR stays off, logged, M54 behaviour)
`vtd_ir_init()` runs from `iommu_init()` right after DMA translation is on and
before any driver probes. It requires:
- `ECAP.IR` and queued invalidation on **every** unit in use (IEC exists only
  as a QI descriptor, so `iommu.qi=off` means no IR);
- in x2APIC mode: EIM on every unit and no DMAR `X2APIC_OPT_OUT`, otherwise
  8-bit destinations (refused if any CPU has an APIC ID > 255);
- every MADT IOAPIC named by a DMAR device scope on a used unit;
- no MSI/MSI-X source live yet (`pci_msi_any_enabled()`). This holds by
  construction: the switchover runs before the drivers.
A firmware-left IRE is turned off before our table is installed. If IR is
refused it is also turned off, because it would otherwise block our
compatibility-format MSIs.

### One switchover (`arch/x86/lapic.c` `ioapic_remap_switch`)
The IOAPIC code now records every routed line (IOAPIC index, pin, vector,
trigger, polarity, 32-bit destination) and owns its registers under
`g_ioapic_lock`. Before M56 there was one global `g_ioapic` pointer and an
8-bit destination cast. The switch:
1. masks every live line;
2. gets each line's remapping entry through the callback (IR still off);
3. turns IRE on on every unit;
4. rewrites every line in remappable format (bit 48, handle in 63:49 and
   bit 11, RTE vector = entry vector so the EOI broadcast still matches) and
   unmasks it.
Lines routed later use the callback directly. If anything fails, the lines go
back to their old compatibility entries. An edge that arrives while a line is
masked is lost on a real IOAPIC, and the 8042 would then keep its output
buffer full forever. So if the 8042 holds a byte after the switch, its
handler runs once through a self-IPI (the handlers read the status port
first).

### Message composition (`drivers/pci_msi.c`)
`msi_compose()` is still the one place every MSI/MSI-X write goes through. It
asks `iommu_ir_msi_compose()` first; a function no remapping unit covers gets
the compatibility format (`msi_compose_compat()`, the old code). The M54 hook
`iommu_irq_compose()` / `ops->irq_compose` now produces the remappable
message.

### Faults
`drain_unit` marks reasons 0x20–0x2F as IR faults: the FI field carries the
interrupt index, not a page. `iommu_report_fault` sends them to separate
counters with their own rate limit (5 lines/s, then "N IR fault message(s)
suppressed"), so an interrupt storm never hides DMA faults and the reverse.
The log line is `[IOMMU] IR fault bdf=… index=… reason=… (…) interrupt
blocked`. The M54 fault hook gets them too (`is_ir`, `ir_index`), so M57 can
attribute an injection attempt to a binding. They are **not** in the DMA fault
total, so the M54 gate's fault counts are unchanged.

### `/proc/iommu`
New lines:
- `ir on entries 256 eim E cfis C set … cleared … retargeted … iec …
  iec-errors … compat-fallback … refused …`, or `ir off (<reason>)`;
- one `irte VV msi|ioapic sid bb:dd.f|buses xx-yy dest D edge|level` line
  per used entry;
- `ir faults N logged … suppressed … last bdf … index … reason …`.

### Locks (`kernel/lock_order.h`)
| Level | Lock | Notes |
|---|---|---|
| S61 (161) `LK_S61_IOAPIC` | `g_ioapic_lock` | routed lines + IOREGSEL/IOWIN pairs; leaf (the IR callback runs outside it) |
| S63 (163) `LK_S63_IOMMU_IR` | `g_ir_lock` | the table and the IEC waits after each change; unit S68 nests inside |

Both are IRQ-safe. `irq_dispatch` takes neither. `msi_compose()` is never
called with `g_msi_lock` held (it was already composed before taking it), and
the `irq_vec_free`/`irq_vec_set_cpu` hooks run after `g_irqvec_lock` is
released.

## API for M57 (`kernel/iommu.h`, additive)
- `iommu_ir_enabled()`: IR on (all units).
- `iommu_ir_covers(dev)`: this function's messages are remapped.
- `iommu_ir_fault_stats()`; `iommu_fault_t.is_ir/.ir_index` in the fault
  hook.
- `iommu_ops_t` gained `ir_init/ir_enabled/ir_covers/ir_free/ir_set_cpu/
  ir_generate` (AMD-Vi, M64, fills the same).
- The vendor-neutral hooks `iommu_ir_vec_free/iommu_ir_vec_set_cpu` are
  called by `arch/x86/irq.c`.

## QEMU findings (important for what the gate can prove)
Checked in the QEMU 6.2 and 9.2 sources (`hw/i386/intel_iommu.c`,
`include/hw/pci/pci_device.h`):
1. **QEMU passes compatibility-format interrupts through even with IR on**
   (`vtd_interrupt_remap_msi`: `if (int_mode != REMAP) { memcpy; goto out; }`).
   GCMD.CFI is not implemented at all. On QEMU, a device that DMA-writes
   `{0xFEE00000, vec}` is still delivered; on hardware with CFIS=0 it is
   blocked (fault 0x25).
2. **DMA writes carry no requester id** (`pci_dma_write` uses
   `MEMTXATTRS_UNSPECIFIED`, and `vtd_mem_ir_write` then skips source-id
   validation). A DMA-injected remappable message that names another
   function's live entry is therefore delivered on QEMU. Genuine MSIs
   (`msi_send_message`) do carry the requester id, so the source-id check is
   provable with them.
3. QEMU 6.2 **records no IR fault** (it drops the request with only a trace);
   9.2 records them (`vtd_report_ir_fault`). On 9.2 a DMA-injected request's
   fault shows source id `ff:1f.7` (0xFFFF, "invalid").
4. Under TCG, `intremap` defaults to `auto` = **on**: a plain `-device
   intel-iommu` already has `ECAP.IR`, and IR works without
   `kernel-irqchip=split` (only KVM needs that). The no-IR guest therefore
   uses `intremap=off`. The M54 gate's `iommu=on` guests now run with IR on;
   they all pass (see Tests).
5. edu clamps every DMA address to its `dma_mask` (28 bits by default):
   0xFEE00000 becomes 0x0EE00000, which is RAM. The IR test probes the mask
   first, in a translated domain (never RAM, never a fault), and skips the
   DMA-injection checks on the M54 gate's default edu. The M56 gate uses
   `-device edu,dma_mask=0xffffffff`.

So the plan's injection test ("edu DMA-writes {0xFEE00000, 0x80} and
{…, 0xF0} → blocked") **cannot pass on QEMU**. What the gate proves on QEMU:
- every source is programmed correctly;
- the source-id check works (genuine MSI);
- a freed entry blocks;
- bad and out-of-range handles block;
- GSTS.CFIS reads 0.
The two cases QEMU cannot emulate are logged as `LIMIT` lines and expected
by the gate. Vectors 0x80 and 0xF0 are deliberately **not** injected: on QEMU
they would be delivered and would run the syscall path or the shootdown
handler in kernel context. On hardware the same CFIS=0 check that blocks the
harmless test vector blocks them. **For M57, the IR requirement is real
protection on hardware and in VMware; on QEMU it is weaker than on silicon.**

## Tests
`kernel/iommu_ir_test.c` (`-DSECOS_IOMMU_TEST`, after the M54 edu tests) and
the gate `tools/ir_gate.sh` (private `/tmp`, per-guest checker
`tools/iommu_check.py`, which gained `--ir`, `--ir-off`, `--irq NAME` and the
`ps2` I/O item; `tools/smp_stress_drive.py` gained `--qemu-pre`). Selftest
section "M56" runs the gate.

### In-kernel IR tests (`[IR-TEST]`, 14 checks + 2 LIMIT on QEMU)
- `ir-cfis-clear`;
- `ioapic-entry` (0x21, SID ff:00.0 = QEMU's IOAPIC scope);
- `msi-remappable-format` (the address edu holds), `msi-entry-sid`;
- `msi-delivered` (10/10 on CPU 0);
- `msi-retarget-ap` / `-back` (`irq_vec_set_cpu` → the handler runs on CPU 1
  and back; only the entry changed);
- `msi-sid-blocked`: edu's MSI capability written by hand with a message
  composed for another function (AHCI) → not delivered, not spurious; on
  9.2 fault 0x26 with edu's BDF and that index;
- `stale-entry-blocked`: `irq_vec_free` with edu's MSI still enabled → the
  entry is cleared, the next MSI is neither delivered nor spurious; 9.2
  fault 0x22;
- `dma-free-entry-blocked` (9.2: 0x22), `dma-index-over-blocked` (9.2: 0x21);
- `dma-foreign-entry` and `dma-compat`: LIMIT on QEMU (findings 1 and 2);
- `ir-storm-rate-limited` (9.2) / `ir-storm-not-recorded` (6.2): 60
  back-to-back injections, no delivery, timer ticking, log rate-limited;
- `freed-entries-cleared`, `no-dma-faults`.

### Gate results (`tools/ir_gate.sh`, 13 guests)
| Guest | Setup | Result |
|---|---|---|
| q6-xapic | 6.2 intremap, -smp 4, full device set + usb-kbd | PASS: IR on xapic, 2 IOAPIC lines; M54 edu 26/26; IR 14 pass + 2 LIMIT; AHCI/NVMe/USB-MSC host-verified I/O, usb-kbd typing, DHCP + 1 MiB each way; e1000e MSI-X counted, off_cpu 0 |
| q6-ps2 | same, no USB input | PASS: guitest sees PS/2 mouse motion + button + key |
| q6-irqdrv | `-DXHCI_USE_IRQ -DNVME_USE_IRQ` | PASS: net/nvme/xhci vectors counting |
| q9-x2apic | 9.2, `-cpu max,+x2apic`, `intremap=on,eim=on`, -smp 8 | PASS: `x2apic eim`; faults 0x26/0x22/0x21 logged; storm suppressed |
| q9-ps2 | 9.2 x2APIC, PS/2 | PASS |
| q9-irqdrv | 9.2 x2APIC, NVMe/xHCI IRQs | PASS (net 745, nvme 535, xhci 1268 interrupts) |
| smp-ipi-q6 / -q9 | `smp_stress_drive --quick`, -smp 4 / 8 x2APIC, IR on | PASS: IPIs, shootdowns, LAPIC timer |
| ir-off-word | `iommu.ir=off` | PASS: `IR: off: boot word`, everything works |
| no-ir-hw | `intel-iommu,intremap=off` | PASS (after the fix for finding 4): `IR: off: a DMA-remapping unit has no interrupt remapping`, M54 tests pass |
| qi-off | `iommu.qi=off` | PASS: `IR: off: queued invalidation off` |
| default | default image on intremap hardware | PASS: no `[IOMMU] IR` / vt-d line, I/O unchanged |
| lockdep-smp4 | `-DSECOS_LOCKDEP`, -smp 4 | PASS: `[LDTEST] all 0xA/0xA`, no lockdep report |

**Negative control** (temporary build, not committed): IOAPIC lines given a
wrong handle → `ps2-mouse-motion`, `ps2-mouse-button`, `ps2-keyboard` FAIL. So
the PS/2 checks really depend on the remapped IOAPIC path.

The final full runs on the finished tree are in "Final runs" below.

### Warnings
`-Wall -Wextra`: 35 normal / 35 `-DSECOS_LOCKDEP` (base `cba08ac`: 35/35);
also 35 with `-DSECOS_IOMMU_TEST` and with `-DSECOS_IOMMU_TEST -DSECOS_LOCKDEP`;
none in the new files.

## Final runs
(filled in below)

## Shared files touched (small, additive)
- `kernel/lock_order.h`: S61, S63.
- `arch/x86/irq.c`/`irq.h`: two hook calls (free / set_cpu).
- `Makefile`: `vtd_ir.c`, `iommu_ir_test.c`.
- `tools/selftest.sh`: the M56 section.
- `tools/iommu_check.py`: `--ir`, `--ir-off`, `--irq`, `ps2` (M54 options
  unchanged).
- `tools/smp_stress_drive.py`: `--qemu-pre`.
- `kernel/iommu.{c,h}`: additive (IR API, ops fields, fault fields, a rate
  limiter per fault class).

## Not verified here (real hardware / VMware)
- CFIS=0 blocking and DMA-write source-id checks (QEMU cannot, see above).
- Multiple DRHDs with different IR capabilities; firmware-left IRE handoff.
- `X2APIC_OPT_OUT` with x2APIC on; APIC IDs > 255 (no QEMU guest has them
  under TCG here).
- SVT=10b bus-range entries (no PCIe-to-PCI bridge with MSI in the gate).
- The IOMMU fault MSI with an x2APIC BSP ID > 254 still needs FEUADDR
  (unchanged from M54; the BSP is ID 0 everywhere we test).

## For M57 / M58 / M64
- **Policy**: refuse a ring-3 bus-master claim unless `iommu_ir_covers(dev)`
  (or the `udrv.allow_no_ir` override the plan names). Say in `/proc/udrv` that
  on QEMU this does not stop compatibility-format or DMA-injected
  foreign-entry messages.
- **Vectors for a ring-3 driver**: allocate with `irq_vec_alloc`, program the
  device through `pci_msix_set` / `pci_msi_enable` as usual: the entry is
  owned by the device's requester id. Never let ring 3 write the MSI-X table
  or capability: the kernel composes. On teardown, mask, then
  `irq_vec_free` (which clears the entry and waits for the IEC).
- **Fault attribution**: the fault hook gets `is_ir=1` records with the
  source id and the interrupt index; a genuine-MSI fault names the device's
  BDF, a DMA-injected one on 9.2 names `ff:1f.7`.
- M64 (AMD-Vi): fill the same `ir_*` ops; `msi_compose()` and the IOAPIC
  switch are vendor-neutral (the switch takes a callback).
