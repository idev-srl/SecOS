# M53c — Dynamic interrupt vectors, generic dispatch, MSI/MSI-X API

Phase 5 groundwork (`docs/PHASE5_PLAN.md` §3 "M53c", branch `feat/irq-vectors`).
Before this step every interrupt source had a hard-coded vector (0x40 xHCI,
0x41 NVMe, 0x42 NET), its own NASM stub, and MSI-X could only program table
entry 0 to LAPIC ID 0. Ring-3 drivers (M57) need vectors handed out at run
time, per-entry masking, and one place that builds the MSI message so M56 can
switch it to the remappable format.

## Vector allocator (`arch/x86/irq.{c,h}`)

- Range **0x50–0xDF** (144 vectors). Never handed out: 0x00–0x1F exceptions,
  0x20–0x2F legacy IRQs (timer 0x20, keyboard 0x21, PS/2 mouse 0x2C),
  0x40–0x42 (the old fixed MSI vectors, now reserved and not installed),
  0x80 syscall, 0xF0–0xF2 IPIs, 0xFF LAPIC spurious.
- API:
  ```c
  typedef void (*irq_handler_t)(uint8_t vec, void* ctx);
  int  irq_vec_alloc(uint32_t cpu, irq_handler_t h, void* ctx, const char* name);
  int  irq_vec_alloc_block(uint32_t cpu, uint32_t count, irq_handler_t h, void* ctx, const char* name);
  void irq_vec_free(int vec);              /* + irq_vec_free_block */
  int  irq_vec_cpu(int vec);  int irq_vec_set_cpu(int vec, uint32_t cpu);
  ```
  `cpu` is a CPU index (`IRQ_CPU_ANY` = the BSP for now, which is what every
  user had before). Blocks are a power of two ≤ 32, aligned to their size
  (MSI multi-message ORs the message number into the low data bits).
- **One system-wide vector namespace, each vector bound to a target CPU.**
  The plan says "vector allocation per CPU"; I kept one namespace on purpose:
  144 vectors are far more than M57/M58 need (an e1000e claim uses 1–3), and
  moving a source to another CPU never changes its vector, so an interrupt
  already in flight to the old CPU still reaches the right handler (Linux
  needs a vector-cleanup IPI dance for exactly this). If we ever run out,
  per-CPU tables can be added behind the same API.
- Lifetime: `irq_vec_free` clears the handler and then waits until the
  vector's `active` counter is 0 (a Dekker pair with `irq_dispatch`, both
  seq_cst), so on return neither handler nor ctx is used any more. The wait
  answers TLB shootdowns (`cpu_relax_poll`). The caller must silence the
  source first; a message already latched in a LAPIC arrives later as
  spurious (or, after a reuse, once at the new owner: handlers must tolerate
  a stray call). Never free a vector from its own handler.
- Lock: `g_irqvec_lock`, **LK_S66_IRQVEC** (leaf, IRQ-safe), only around
  alloc/free/set_cpu and the /proc snapshot. `irq_dispatch` takes no lock.

## Stubs and dispatch (`arch/x86/idt_asm.asm`, `arch/x86/idt.c`)

A NASM `%rep` generates one 3-instruction stub per vector (`push 0; push vec;
jmp irq_common`) plus `irq_stub_table` in `.rodata` (relocated by KASLR like
any other pointer table). `irq_common` saves the GPRs, `clac`, `cld`,
`irq_enter`, `irq_dispatch(vec)`, `irq_exit`, restores, `iretq`. The frame is
16-byte aligned at the call (2 + 15 pushes over the 5-word hardware frame).
`irq_dispatch` runs the handler and **sends the LAPIC EOI itself** (handlers
never EOI any more); an unbound vector in the range is counted as spurious
and EOIed too. `idt_init` installs all 144 gates at boot; before M53c those
vectors pointed at `isr_stub` (a bare `iretq`, no EOI). The old `isr_xhci`,
`isr_nvme`, `isr_net` stubs and `idt_install_msi_vector` are gone.

`/proc/interrupts` lists every allocated vector: target CPU, count,
`off_cpu` (deliveries to another CPU than the target: should stay 0), name,
then the spurious total and the number of free vectors.

## MSI / MSI-X (`drivers/pci_msi.{c,h}`, moved out of `drivers/pci.c`)

- `msi_compose(dev, vec, cpu, &addr, &data)` is the single place that knows
  the message format: compatibility format, physical destination, fixed
  delivery, edge. Without interrupt remapping the destination is 8 bits, so a
  CPU whose APIC ID is > 254 is refused (0xFF is broadcast in xAPIC and in
  x2APIC). M56 replaces this function with the remappable format.
- MSI-X: `pci_msix_enable(dev, nvec)` (table offset/size checked against the
  BAR before anything is mapped — a hostile offset could otherwise retag RAM
  as UC in the physmap — enable with the function masked, mask every entry,
  unmask the function), `pci_msix_set(dev, entry, vec, cpu)` (rewrites the
  entry masked and restores its mask state), `pci_msix_mask/unmask(dev, entry)`
  (one MMIO RMW of the vector-control word + read-back, **no lock**, usable
  from IRQ context), `pci_msix_disable`, `pci_msix_table_size`.
- MSI: `pci_msi_enable(dev, nvec, vec, cpu)` (multi-message: nvec a power of
  two the device supports, vec aligned to it; enabled messages start
  unmasked, the rest masked when PVM exists), `pci_msi_mask/unmask(dev, idx)`
  (−1 without PVM), `pci_msi_disable`, `pci_msi_max_vectors`, `pci_msi_has_pvm`.
  MSI and MSI-X are never left on together.
- `pci_irq_setup_single(dev, vec, cpu)`: the common driver path (MSI-X entry
  0, else one MSI message).
- Per-device records (16, never removed) keyed by BDF; lock `g_msi_lock`,
  **LK_S58_MSI** (IRQ-safe; only PCI config access inside, so it ranks below
  M53a's `pci_cfg_lock` at S64; never held across `vmm_map_mmio`).
- `pci.c` keeps `pci_find_capability`; only `pci_enable_msix`/`pci_enable_msi`
  left it. M53a will want to feed BAR sizes from its registry instead of
  `pci_bar_size` re-probing a live BAR in `msix_geometry` (marked TODO).

## Users moved to allocated vectors

- **NET** (`net/net_core.c`): `net_request_irq` allocates a vector (BSP,
  ctx = the `net_dev_t`) and calls `pci_irq_setup_single`; the handler is
  `static`, gets the device from ctx. Log line now
  `[NET] MSI-X NAPI armed vector=0x50 (+poll backstop)` (the NETSMP grep
  matches the prefix). Still only the first NIC.
- **xHCI** (`-DXHCI_USE_IRQ`): allocated vector; **the handler now parks
  drained transfer events (`evt_park`) instead of dropping them** — dropping
  would lose armed HID completions; and **`USBCMD.INTE` is now set**, without
  which the controller never sent an interrupt at all (the gated path had
  never actually fired).
- **NVMe** (`-DNVME_USE_IRQ`): allocated vector; **armed before the I/O CQ is
  created** and the CQ is created with IEN=1, IV=0. The old code created the
  CQ with IEN=0 and armed MSI-X afterwards, so no interrupt ever fired (QEMU's
  nvme also only notifies a vector it marked in use at CQ creation, which
  needs MSI-X already enabled). Completions are still consumed by the poller.

## Bug found in existing code: e1000e MSI-X fired once

On the default boot the e1000e MSI-X interrupt fired **exactly once** per
boot (base 96bb08b, measured with a counter: 1 interrupt after DHCP + ssh +
2 MB over ssh); RX ran entirely on the 1 ms timer-poll backstop. In MSI-X
mode a read of ICR does not clear it (read-to-clear only applies without
MSI-X or with CTRL_EXT.IAME), and the ack wrote 0x1580 (EICR), which is an
82576 register the 82574 does not have. A cause left set in ICR stops further
messages. `e1000e_irq_ack` now clears ICR write-1-to-clear before the drain.
After the fix: ~90 interrupts by the time ssh is up, ~180 after a 2 MB
transfer, and NETSMP passes with the RX path really interrupt-driven. This
is a behaviour change on the default boot for e1000e (RX processing now
happens in IRQ context on CPU 0 for each burst, as M24/M47 designed it); it
is in its own commit so it can be dropped or reverted alone. VMware's
82574L emulation should behave like QEMU's, but that is untested.

## Boot self-test

`irq_vec_selftest()` (from `kernel_main_phase2` right after SMP bring-up,
before any driver allocates): a self-IPI to an allocated vector reaches its
handler on CPU 0 with its ctx; the vector retargeted to an AP, an IPI to that
AP runs it there; **negative**: a freed vector never calls the old handler,
is counted spurious and EOIed (the system keeps taking interrupts); an 8-block
is 8-aligned; bad arguments (count 3, NULL handler, CPU out of range) are
refused; the allocator returns exactly 144 vectors then −1; nothing leaks.
Logs `[IRQ] vector selftest PASS`.

## Tests run

All QEMU runs in a private `/tmp` (`unshare -rm`).

- Warnings (`-Wall -Wextra`): 35 = base, in the normal, `-DSECOS_LOCKDEP` and
  `-DXHCI_USE_IRQ -DNVME_USE_IRQ -DSECOS_LOCKDEP` builds. The first commit
  (allocator only) builds alone, 35 warnings.
- `tools/smoke.sh --mb2` and `--uefi`: PASS, `[IRQ] vector selftest PASS` on both.
- **NETSMP** (lockdep image, as in `selftest.sh`): smp4-msix, smp4-poll,
  smp1-msix 25/25 each, no lockdep report / panic / exception,
  `[NET] MSI-X NAPI armed vector=0x50`, 4 CPUs online. With the e1000e fix
  the RX interrupt really fires (`/proc/interrupts` net: 87 → 180 over a
  2 MB ssh transfer; base: 1 for the whole boot).
- **`-DNVME_USE_IRQ`** (+lockdep, q35 + `-device nvme`, -smp 4): mounts FAT32,
  write+readback OK, 32 MB raw read, file write + md5; nvme vector 30 → 55323
  interrupts. Before the CQ-ordering fix: 0. (FAT32 writes on NVMe run at
  ~65 KB/s with lockdep; the base image is the same, so not a regression.)
- **`-DXHCI_USE_IRQ`** (+lockdep, `qemu-xhci` + `usb-kbd` + `usb-storage`,
  -smp 4): keystrokes typed through QMP `sendkey` reach the console bash
  (`touch /tmp/hidok` runs), `usb0` FAT32 mounted, 16 MB raw read, file write
  + md5; xhci vector 145 → 10925 interrupts.
- **x2APIC** (QEMU 9.2, `-cpu max,+x2apic -smp 8`, lockdep): `mode=x2apic`,
  8 CPUs, vector selftest PASS (incl. the AP IPI), NET MSI-X counting.
- **Affinity** (temporary build with the NET vector on CPU 3, not committed):
  xAPIC (QEMU 6.2) and x2APIC (QEMU 9.2) both deliver every NET interrupt to
  CPU 3 (`off_cpu 0`): `msi_compose` destination formats correct.
- **M48 gate** (extracted from `selftest.sh`, `SECOS_QEMU_X2APIC` = QEMU 9.2):
  53/53 incl. stress -smp 4/8, nosmp, x2APIC.
- **M40 GUI gate** (`gui_drive.py --scenario main`): 0 failures: PS/2 keyboard
  (IRQ1), PS/2 mouse (IRQ12), timer, USB HID tablet/mouse unchanged.
- **`make lockdep-check`** (full run, -smp 4/8): 16/18 on the first run; the
  two failures were alt4 (`bkl=on`, `--quick`) `fuzz-par`: a `sysfuzz` child
  took a ring-3 write fault and was killed (`[EXC] ... ring3`, which the
  driver counts as a kernel marker). Rerun of alt4: 13/13, clean. The fault is
  in user space (random syscalls under per-boot ASLR); unrelated to vectors.
- Negative tests: the boot self-test (freed vector → spurious, no handler
  call; bad arguments refused; exhaustion at exactly 144), MSI-X table outside
  its BAR refused (code path, not exercised by QEMU devices).

## For M54+

- Allocate with `irq_vec_alloc(cpu, handler, ctx, name)`, program with
  `pci_msix_set` / `pci_msi_enable`, then `pci_msix_unmask`. Free: mask /
  disable the source, then `irq_vec_free` (never from the handler).
- M57's IRQ-fd mask-until-ack: `pci_msix_mask(dev, entry)` from the handler is
  lock-free; MSI `pci_msi_mask` takes `g_msi_lock` (IRQ-safe) and needs PVM.
- M56: replace `msi_compose` only; every MSI/MSI-X write goes through it.
  The 8-bit destination limit (APIC ID ≤ 254) goes away with IR + EIM.
- `/proc/interrupts` `off_cpu` should stay 0; a non-zero value means a
  message reached a CPU other than the recorded target.
