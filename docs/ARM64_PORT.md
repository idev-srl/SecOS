# SECoS — ARM64 Port Plan (deferred, post-x64)

**Status: DECIDED, not started.** SECoS will get an AArch64 (ARM64) backend
**after the x86-64 roadmap is complete**. Target class: **Raspberry Pi / SBC**
(device-tree boot, GIC, ARM generic timer, MMIO-only). This is a deliberate
*second-backend* effort, not a recompile.

> **Standing rule for every future change:** keep the ARM64 port in mind. When you
> add or modify code, do not make the eventual port *worse* (see "Forward
> discipline" below). We are not abstracting for ARM64 now — we are just not
> calcifying new x86 assumptions into the portable layers.

## Why defer (not restructure now)

1. **Single developer, x86 still a moving target** — open bugs (custom UEFI loader
   triple-faults on real firmware; SMP TLB-shootdown), roadmap incomplete (Phase L).
   Abstracting now means abstracting something still changing, and from that point
   **every milestone must be validated on two architectures** — 2× test burden on
   an unstable base.
2. **Premature abstraction builds the wrong seams.** The correct shape of a HAL is
   learned *by doing* the second port, not by guessing before it starts.
3. **The security thesis is arch-neutral and already proven on x86** (Ed25519
   signing, W^X, capabilities, Driver Space). ARM64 is "the same story on another
   CPU," not a conceptual advance — so finish the story on x86 first.

## What ports cheaply vs. what is a rewrite (measured at HEAD ~8041267)

Kernel ≈ 25.8k LOC. `arch/x86/` ≈ 1.95k LOC + 5 NASM files.

| Portable (light touch) | Rewrite (new `arch/arm64` backend) |
|---|---|
| crypto, signing, ELF loader logic, VFS, FAT32/ext2/ext4/JBD2, net L2–L4, page cache, IPC, pipes, libc surface, scheduler *policy*, security model | `boot/*.asm` + UEFI handoff; `mm/vmm.c` page-table format (152 x86 refs: PML4/PDPT/PRESENT/NX/cr3/invlpg) + higher-half constant; interrupt model (IDT → **GIC**); context switch + exception entry asm; timer (PIT/LAPIC/TSC → **ARM generic timer** CNTP); syscall entry asm + trapframe layout; port-I/O → **MMIO** in `drivers/{serial,keyboard,rtc,timer}.c`; PCI config (port/CAM → **ECAM** MMIO); MSI |

**Good news, conserved:** the higher-half design *ports well* — ARM64's
TTBR0/TTBR1 split (user low / kernel high) is a better fit than x86's single CR3.

**Nastiest leak:** the trapframe / SysV-register access (`tf->rdi`, `tf->rax`,
`iretq`) is spread across `kernel/{syscall_trap,signal,sched,process,smp}.c`. This
is the highest-value thing to keep clean (see below).

## Forward discipline (cheap, do it as you touch these files — NOT a refactor now)

- **Trapframe accessors.** When you next touch the syscall/signal/sched path,
  prefer small accessors — `arch_sysarg(tf, n)` / `arch_setret(tf, v)` /
  `arch_sysno(tf)` — over scattered `tf->rdi` / `tf->rax` literals. Single highest
  leverage move; isolates the syscall ABI from the register file.
- **No new port-I/O outside `drivers/` and `arch/`.** Keep `inb`/`outb` behind the
  existing arch helpers (today this is almost respected).
- **Keep page-table bit manipulation and the higher-half constant confined to
  `mm/vmm.{c,h}`** (today they already are).
- Keep using the existing clean interface boundaries (`net/net.h` NIC contract,
  `block_dev_t`, `vfs_fs_ops_t`) — these are the model for how the rest should look.

Do **not** introduce an empty `arch/arm64/` or a generic `arch/` indirection layer
yet. That is the port project, not today.

## When the time comes — decide the bring-up target first

The effort depends heavily on the board:

- **QEMU `virt` (SBSA-like) — recommended first step.** UEFI + ACPI + ECAM are
  standard, GIC, no physical hardware to debug blind, and it gives the same
  deterministic smoke-test loop we already have on x86. Bring up here *before* real
  silicon.
- **Raspberry Pi / SBC (the goal).** Device-tree boot, board-specific UART (PL011 /
  mini-UART), GIC, no standard UEFI on older Pis → more low-level work. Do this
  after `virt` boots.
- **Apple Silicon — out of scope** (proprietary boot, no standard UEFI).

### Rough bring-up order (when started)
1. QEMU `virt`: EL1 entry, MMU on (4-level, TTBR1 high half), one UART, exceptions.
2. ARM generic timer + GIC → preemptive tick.
3. Context switch + ring-3 (EL0) + syscall (SVC) + trapframe.
4. Re-validate the signing gate (crypto is portable) → a signed ELF runs at EL0.
5. PCIe ECAM + a virtio-mmio (or virtio-pci) block device → mount a filesystem.
6. Move to a real Pi/SBC: device-tree, board UART, SD/eMMC, USB.

## See also
- `docs/ROADMAP_TO_COMPLETE_OS.md` — long-term plan (x64).
- `TASKS.md` — actionable checklist; ARM64 is a post-roadmap line item there.
- memory `arm64-port-plan` — the one-line standing constraint.
