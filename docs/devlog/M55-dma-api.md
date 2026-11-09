# M55 — DMA API + strict kernel DMA domain (`feat/dma-api`)

Phase 5, plan `docs/PHASE5_PLAN.md` §3 "M55" (with D2, D3). Base: `cba08ac`
(`feat/vtd-core`, M54 on top of the merged M53). **The default boot is
unchanged** except for one debugcon line (`[DMA] .dmabss phys … size … KiB`);
without `iommu=on` the API only computes and checks addresses, and they are
the same physical addresses the drivers used before.

## What it does

Every bus address an in-kernel driver hands to its device now comes from
`kernel/dma.{c,h}`, and with `iommu=on` the devices can reach **only** the
DMA region of the kernel image and memory the DMA API handed out: kernel
code, read-only data, the rest of `.bss`, the heap, the page cache and user
memory are invisible to them (W^X extended to devices).

### `kernel/dma.{c,h}` (plan D3)
- `dma_dev(const pci_dev_t*)` / `dma_dev_legacy(const pci_device_t*)`: the
  per-function handle (registry index; DMA mask, default 64 bit). In strict
  mode the **first call moves the function into the strict domain** — so
  drivers call it before enabling bus mastering. Only a function still in
  the M54 passthrough domain moves; one left blocked (the M54
  `iommu.block=` hook) or placed elsewhere (M57, later) stays put.
  Functions nobody drives stay in passthrough (plan D2: M59 blocks them).
- `dma_set_mask` / `dma_get_mask`: the 4 GiB checks of AHCI (`!CAP.S64A`),
  xHCI (`!AC64`) and ath9k became the mask; virtio legacy gets 44 bits
  (QUEUE_PFN).
- `dma_static(dd, kva, len)`: bus address of a `__dma` buffer (or of
  coherent / streaming memory). A range outside the DMA regions or above the
  mask logs `[DMA] BUG: …` (16 lines max, counted in `/proc/iommu`) and
  returns the physical address anyway (old behaviour; the strict domain
  blocks it). `.dmabss` addresses take no lock (one compare + subtract; hot
  path: every TX descriptor).
- `dma_reachable(dd, kva, len)`: the same check without the complaint
  (xHCI uses it to decide whether a caller buffer must bounce).
- `dma_alloc_coherent` / `dma_free_coherent`: page granular, zeroed,
  physically contiguous, below the mask (`pmm_alloc_contiguous_below`, so
  DMA32 for 32-bit devices), identity-mapped into the strict domain; the
  free unmaps first (`iommu_unmap` returns after the IOTLB flush) and only
  then gives the frames back to the PMM. No driver needs one yet; the boot
  self-test uses one.
- `dma_map_single` / `dma_unmap_single`: whole pages of a physically
  contiguous kernel buffer; in strict mode refused if a page is already
  mapped (a buffer sharing a page with `.dmabss` or another mapping). Only
  ath9k's legacy-object fallback uses it (below). `dma_sync_*` are no-ops.
- `-DSECOS_DMA_DEBUG`: every violation panics, also with no IOMMU (i440fx),
  and every `dma_dev()` is logged.
- `-DSECOS_DMA_TEST`: helpers for the negative test (the bus address of a
  `.text` page / a heap page, the `.text+.rodata` hash).
- Lock `g_dma_lock`, **new level `LK_S63_DMA` (163)**: device table +
  mapping table (64 entries); IOMMU S65/S67/S68, heap and PMM nest inside.
  Taken under S45 `xhci_lock`, N3 `tx_lock`, K40 block locks (only on the
  non-`.dmabss` path). IRQ-safe; nothing sleeps.

### `.dmabss` (`linker.ld`)
- `__dma` = `__attribute__((section(".bss.dma")))`. The plan named the
  section `.dmabss`; GCC emits a section named `.dmabss` as **PROGBITS**
  (it decides NOBITS by the `.bss.` prefix), which would have put ~2.4 MiB of
  zeros into `kernel.bin`. The input section is `.bss.dma` (NOBITS), the
  region is still called `.dmabss` in the code and logs.
- Placed inside the `.bss` output section, **after** `*(.bss)` and before
  `*(.bss.*)`, page-aligned at both ends (`__dma_start` / `__dma_end`). Not
  at the start: `stack_bottom` must stay the first `.bss` symbol (the kernel
  stack guard logic in `vmm_setup_kernel_guard_pages` assumes it, and a
  guard page there would unmap a DMA buffer).
- `tools/kaslr-relocs.py` needed no change (pass-2 check byte-identical):
  the region is ordinary `.bss`. KASLR slides only the virtual base, so the
  region is one physically contiguous run below 4 GiB (checked at boot).
- Size today 2.4 MiB (613 pages). Every DMA buffer of every driver is in
  there; nothing else is.

### Strict kernel DMA domain (plan D2 (b)); boot word `iommu.kernel=`
- With `iommu=on` and a working IOMMU, `dma_init()` (right after
  `iommu_init()` in `kernel/kernel.c`) builds one `IDOM_TRANSLATED` domain
  and identity-maps `[__dma_start, __dma_end)` RW. Coherent and streaming
  mappings are added/removed at run time. RMRRs of the attached devices are
  mapped by `iommu_attach` (M54).
- Boot self-test: a coherent allocation is mapped, zeroed and reachable;
  after the free it is unmapped; `.dmabss` first/last byte mapped, the page
  after it and `.text` not → `[DMA] strict domain self-test PASS`.
- `iommu.kernel=pt` keeps the M54 passthrough domain (logged; `/proc/iommu`
  says so). Unknown values warn and use strict. If the domain cannot be
  built the kernel falls back to passthrough (logged).
- `/proc/iommu`: state text now names the mode (`iommu_set_state()`, a
  small additive hook in `kernel/iommu.c`), plus
  `dma mode strict dmabss <phys> <KiB> KiB devices N strict N coherent N
  pages streaming N violations N` and one `dma dev` line per handle.

### Driver conversions (one commit each)
| Driver | Change |
|---|---|
| virtio-blk | vq/bounce/header/status `__dma`, `dma_static`, mask 44 bit. **Bypasses QEMU's vIOMMU** (legacy device, no ACCESS_PLATFORM): in the strict domain on paper, never confined in practice. |
| AHCI | CLB/FIS/command table/bounce `__dma`; `S64A` → mask; **a failed command now restarts the port** (`port_recover`: ST/FRE off, clear SERR/IS, on) and the wait loop also stops on HBFS/HBDS/IFS — before, a failed command left the port stopped. |
| NVMe | queues, identify, PRP list, bounce `__dma`; PRP entries from the bus address of the bounce range. |
| xHCI + USB | rings, contexts, DCBAA, scratchpad, ERST, bounce `__dma`; AC64 → mask. **A caller buffer that is not device-reachable always bounces** through the 64 KiB `g_bounce32` (before: only above 4 GiB on AC64=0) — the USB core/HID/hub/MSC pass stack buffers for small control transfers (`&maxlun`, …). Async submits of a non-reachable buffer are refused as before (HID's are `__dma`). Class buffers in `usb.c`, `usb_hid.c`, `usb_hub.c`, `usb_msc.c` `__dma`. |
| e1000, e1000e, vmxnet3, igc | rings + packet buffers `__dma`, `dma_static`. |
| ath9k | `ath9k_host_v2p` → `dma_static` (32-bit mask). See below. The port has no TX DMA path (only the EDMA RX pool). |

**ath9k and the prebuilt object.** `g_rxpool` lives in
`drivers/ath9k_port/ath9k_secos.c`, which is compiled into the committed,
prebuilt `drivers/ath9k_port/ath9k_hw.o` (rebuilding it needs the network:
`tools/port-ath9k.sh` fetches upstream Linux master). The source now tags the
pool `.bss.dma` (attribute spelled out: `kernel/dma.h` clashes with the
compat headers), but the committed object still has it in its own `.bss`.
`ath9k_host_v2p` handles both: if the slot is not DMA memory it maps the
whole 48 KiB pool once with `dma_map_single` (the first call is slot 0, the
pool start). The pool's first/last page are shared with other ath9k `.bss`
variables, which are then device-visible: acceptable until the object is
regenerated (real hardware only; the ASUS has no VT-d anyway).

## Tests run

(`tools/iommu_gate.sh`, extended; every iommu=on guest checks the strict
domain: domain built, self-test PASS, AHCI 8086:2922 / NVMe 1b36:0010 /
xHCI 1b36:000d / NIC in it, no `[DMA] BUG`, `/proc/iommu` `dma mode strict
… violations 0`, fault count 0 — or exactly the edu tests' faults — and real
I/O: 1 MiB read + write on sda/nvme0n1/usb0 checked from the host, DHCP,
1 MiB each way over TCP, a command typed on the USB keyboard.)

`tools/iommu_gate.sh` (full run, 23 min): **22 passed, 0 failed** —
dma-scan, default (iommu off on q35+vIOMMU), main, **dma-neg**, noflush,
aw48-cm, cm-qemu-ptoff, kernel-ptoff, qioff-cm, **kernel-pt**,
**e1000-strict**, **dmadebug-pc**, **dmadebug-vblk**, **dmadebug-strict**,
block-ahci, no-iommu, amd-iommu, **lockdep-smp4** (strict, `-smp 4`,
`[LDTEST] all 0xA/0xA`, no lockdep report), q9-x2apic (QEMU 9.2, x2APIC +
intremap/eim, `-smp 4`), q9-aw39, **uefi** (OVMF q35, `iommu=on` from the
load options, strict), **fsstress-ahci-root** (ext4 root on the q35 AHCI
disk in the strict domain + FAT32 data disk, fsstress, host `e2fsck -fn`
and `fsck.vfat` clean, 0 DMA faults, `[FSD] RESULT ok=12 fail=0`). New
guests in bold. Every strict guest: AHCI, NVMe, xHCI + HID + MSC, e1000e
(or e1000) did real I/O, 0 faults besides the edu tests'.

- **dma-neg** (`-DSECOS_DMA_TEST`): `[DMA-TEST] ahci read into .text
  phys=0x…21A000 (command completed), IOMMU write fault seen, .text hash
  unchanged`; `ahci write from a heap page … IOMMU read fault seen, heap
  bytes not on disk`; `read after the faults ok`; `RESULT pass=3 fail=0`;
  `/proc/iommu` exactly 2 faults (00:1f.2, reasons 5 W / 6 R); then the full
  I/O set passed. The kernel kept running.
- **dmadebug-pc / -vblk** (i440fx, no IOMMU, `-DSECOS_DMA_DEBUG`: any
  stray address panics): AHCI (`-device ahci`), NVMe, xHCI/HID/MSC, e1000e;
  then virtio-blk (vda), xHCI/HID/MSC, e1000 — all I/O passed, no panic.
- `tools/dma_scan.sh`: PASS; negative control (a `kvirt_to_phys` call
  appended to `drivers/e1000.c`) → FAIL. Remaining calls outside `mm/` and
  `kernel/dma.c`: `kernel/smp.c` (AP trampoline page) and `kernel/kernel.c`
  (a debug print), neither a DMA address.

Default boot (copy of the tree, private /tmp):
- `tools/smoke.sh --mb2` PASS, `--uefi` PASS;
- NETSMP (lockdep image; -smp 4 e1000e MSI-X, -smp 4 e1000 polled, -smp 1):
  25/25 each, no lockdep/panic/exception;
- M48: 4 CPUs online, shootdown selftest PASS, boot clean;
  `smp_stress_drive` -smp 4 and -smp 8 19/19 each.

Warnings (`-Wall -Wextra`): 35 = base 35, normal and `-DSECOS_LOCKDEP`
builds, also with `-DSECOS_DMA_DEBUG -DSECOS_DMA_TEST -DSECOS_IOMMU_TEST`.

Not run: the full `tools/selftest.sh` (integrator), `make lockdep-check` /
`make locktests` (the lockdep coverage here is the gate's lockdep-smp4 guest
and NETSMP).

## Findings

- **QEMU's ich9-ahci completes a command whose DMA the IOMMU refused with
  success** (no HBFS/TFES; the data is dropped). So "the command errors"
  (plan) cannot be observed in QEMU; the negative test checks what matters
  instead: the fault record (bdf 00:1f.2, the page, W resp. R), the
  unchanged `.text` hash, and that the heap page's bytes never reach the
  disk. Real HBAs report a host-bus error, which `port_recover()` handles.
- QEMU fault reasons for an unmapped page in a translated domain: 5 (write)
  / 6 (read), as M54 found.
- GCC section flags: a custom section name gets NOBITS only with the
  `.bss.` prefix (see `.dmabss` above).
- The USB stack passes stack buffers to `xhci_control` (e.g. `&maxlun` in
  `usb_msc.c`), so "every DMA buffer is static" (plan §1.3) is not quite
  true; the bounce covers it.
- The default `/proc/iommu` state text of M54 said "kernel drivers:
  passthrough" even after the move; now set by `dma_init`.

## Shared files touched (small, additive)
- `kernel/kernel.c`: one `dma_init()` call after `iommu_init()`.
- `kernel/lock_order.h`: `LK_S63_DMA`.
- `kernel/iommu.{c,h}`: `iommu_set_state()`, one `dma_generate()` call in
  `iommu_generate()`.
- `linker.ld`: the `.dmabss` region inside `.bss`.
- `Makefile`: `kernel/dma.c`.
- `tools/iommu_gate.sh`, `tools/iommu_check.py` (`--proc-expect`), new
  `tools/dma_scan.sh`.

## For M56 / M57 / M58 / M59
- **M57 (ring-3 drivers)**: a function claimed by a ring-3 driver must not
  be in the strict kernel domain; `dma_dev()` only moves functions that are
  still in `iommu_kernel_domain()`, so attach the claimed function to its
  private domain before (or instead of) any kernel `dma_dev()` call, and a
  kernel driver probing later leaves it alone. If a kernel driver already
  owns it, detach it from the kernel driver first (there is no
  `dma_dev_release` yet — add one if M57 hands a kernel-driven NIC to ring
  3; it must drop the handle and re-attach to the passthrough/blocked
  domain).
- The strict domain is **shared**: a compromised kernel-driven device can
  still DMA into any other kernel driver's `.dmabss` buffers. Per-device
  domains (D2 (c)) would need per-device `.dmabss` subsections; not worth it
  while all are kernel drivers.
- `dma_alloc_coherent` is page granular with a 64-entry mapping table.
  Real users (M58 shared rings) may want a sub-page pool on top.
- M59 (`iommu=on` by default): the strict domain is ready; the passthrough
  fallback is `iommu.kernel=pt` — add a GRUB entry next to "iommu=off".
- virtio-blk stays unconfined in QEMU until a modern (ACCESS_PLATFORM)
  driver exists (M63).
