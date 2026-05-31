# SECoS — Development Plan (current → real, functioning secure OS)

This is the authoritative execution plan. `ROADMAP_SECoS.md` holds the
high-level milestone list; this document holds the mission, the definition of
done, the per-phase acceptance gates, and the methodology for **autonomous**
development. Per-milestone implementation notes go in `docs/devlog/M*.md`.

## Locked decisions (2026-05-31)

- **Ambition:** full secure OS. Phases **A–E are mandatory**; F–G are stretch.
- **ABI:** small custom syscall ABI + a minimal in-house libc. **No POSIX.**
- **Platform:** UEFI is the golden path; Multiboot2 is kept for CI only.
  **Storage:** virtio-blk (clean, modern, QEMU-native).

---

## 1. Mission & identity

SECoS is a **minimal, security-first, x86-64 operating system with a
microkernel bias**. It is not a POSIX clone and not general-purpose. Its
identity rests on four properties:

1. **Isolation by construction** — every process in a private address space;
   W^X + NX everywhere; a validated, auditable syscall boundary.
2. **Capability-mediated device access (Driver Space)** — drivers are
   least-privilege user-space processes; no ambient authority; every device
   access is capability-checked and audited.
3. **Policy-carrying binaries** — the `.note.secos` manifest declares limits and
   permissions enforced at load time; deny-by-default.
4. **Small, deterministic, auditable TCB** — every mechanism is provable by an
   automated self-test.

## 2. Definition of "real, functioning OS" (acceptance criteria)

A SECoS image that, **unattended**, can:

1. Boot on UEFI (OVMF/QEMU; ideally real hardware) and on MB2 (CI).
2. Load multiple **independent** ELF user programs from a real on-disk filesystem.
3. Run them in ring-3, **preemptively multitasked**, fully isolated, with clean
   `exit`/reaping (no PMM leak).
4. Provide a useful syscall ABI (process, I/O, file, IPC) + a minimal user libc.
5. Mediate **at least one real device** through Driver Space from a **user-space**
   driver, capability-checked and audited.
6. Persist data to disk (filesystem read **and** write).
7. Run a **non-interactive self-test boot** that exercises the above and reports
   PASS/FAIL on debugcon with a deterministic exit code.

## 3. Current state (HEAD, build `73a45db` family)

Solid: dual-path boot, kernel-owned paging with W^X/NX, PMM/heap/VMM, ELF64
loader with manifest hooks, validated user/kernel isolation (12/12 selftest),
trapframe syscall ABI (M5), context-switch machinery (M6), capability Driver
Space scaffold, VFS over ramfs (+ FAT32/ext2 parsers), kernel-owned GDT/TSS/IST.

Gaps to close: M7 ring-3 demo stalls in `process_create_from_elf` (no working
user process yet); no preemption / `SYS_EXIT` reaping; **no real userland**
(user "programs" are compiled into the kernel); no block device / persistence
wired; Driver Space is a shadow stub with in-kernel drivers; shell is
interactive-only (not headless-testable).

---

## 4. Phased plan

Each phase ends with a **scripted gate** (`tools/smoke.sh` + a self-test that
prints `[SELFTEST] … PASS/FAIL` and a final marker on debugcon) and a **stable
tag**. Mandatory: A–E. Stretch: F–G.

### Phase A — Finish M7: make ring-3 real + build the autonomy harness
**Milestone:** M7 → `M7_STABLE`
- Diagnose and fix the `process_create_from_elf` stall (trapframe alloc /
  address-space setup / ELF parse / stack mapping).
- Put the ring-3 demo behind a build flag (`M7_RING3_DEMO`) so normal boot still
  reaches the shell.
- **Build the self-test boot harness**: a flagged boot mode that runs scripted
  checks and emits `[SELFTEST] name PASS/FAIL` + a terminal `[SELFTEST] DONE
  n/n` marker to debugcon. This is the foundation for every later gate.

**Gate:** demo-on → at least one `[SCHED] switch` between two ring-3 processes,
no triple fault; demo-off → boots to shell; harness prints a green summary.

### Phase B — Multitasking lifecycle
**Milestone:** M8
- `SYS_EXIT`: release the address space (`vmm_space_destroy`), mark ZOMBIE,
  reap, schedule next; verify no PMM leak across repeated spawn/exit.
- Preemptive scheduling: per-PCB tick budget driven from IRQ0 on top of the
  M6/M7 trapframe path; `TSS.RSP0` updated on every switch.
- A **serial (COM1 / 0x3F8) console driver** so user-program output is
  capturable headlessly; `ps` shows live RUNNING/READY/ZOMBIE states.

**Gate:** N ELF processes interleave output via the harness, all exit cleanly,
PMM stats identical before/after (no leak). Tag `M8`.

### Phase C — Real userland (identity-defining step)
**Milestone:** M9
- Separate **user build** target: `crt0` + a minimal libc (syscall wrappers for
  `write/read/open/close/exit/getpid/yield`, plus `spawn/wait` once available).
- Custom syscall ABI finalized (small, documented in `docs/SYSCALL_ABI.md`).
- Embed an **initrd** (or a CPIO/ramdisk) of user ELFs; the loader runs an ELF
  resolved through the VFS rather than a hand-built buffer.
- `SYS_SPAWN` (load+start an ELF by path) and `SYS_WAIT`.

**Gate:** a `hello` ELF **built by the user toolchain, not the kernel** is loaded
from the VFS and prints via `SYS_WRITE`, captured on serial/debugcon by the
harness. Tag `M9`.

### Phase D — Storage & persistence
**Milestone:** M10
- **virtio-blk** driver (MMIO/PCI) exposing `block_read/block_write`.
- Wire FAT32 (and/or ext2) **read-write** through the VFS over the block device.
- Load user programs from the disk image; mount the data FS at boot.

**Gate:** write a file, reboot the VM, read it back identical; run a program
loaded from disk — all asserted by the harness. Tag `M10`.

### Phase E — Driver Space for real (proves the security thesis)
**Milestone:** M11
- Add `proc_type` (USER/DRIVER) to the PCB, set from the manifest
  (`MANIFEST_FLAG_DRIVER`); deny `SYS_DRIVER` to USER processes (`DRV_ERR_PERM`).
- Move one driver (target: **keyboard**, fallback: a simple device) to a
  **user-space process** mediated by Driver Space: real register/MMIO access via
  validated `DRIVER_OP_*`, capability + range checks, audit log.
- IRQ delivery: kernel ISR enqueues events to an IPC queue the driver consumes
  via `SYS_READ` on a special fd; bounded auto-restart on driver crash.

**Gate:** the user-space driver delivers real input/events end-to-end; a
capability/range violation is denied **and** recorded in the audit log; a USER
process gets `DRV_ERR_PERM`. Harness-asserted. Tag `M11`.

### Phase F — Hardening & memory scalability (stretch)
**Milestone:** M12
- Higher-half kernel at `0xFFFFFFFF80000000`; drop the low identity map after
  switch. Buddy/free-list PMM; remove the 128 MB/512 MB clamps (test `-m 2G`).
- Complete demand paging with per-process limits. UEFI handoff hardening: real
  4 KB post-EBS ELF mapping with W^X; copy `secos_boot_info` + memory map into a
  kernel-owned frame. Full W^X audit.

**Gate:** UEFI boot on real firmware; >512 MB managed on a 2 GB VM; W+X request
rejected. Tag `M12`.

### Phase G — Usability & policy enforcement (stretch)
**Milestone:** M13
- Shell that launches user programs from the FS; minimal IPC/pipes; a few more
  syscalls; end-to-end manifest enforcement (`max_mem`, capability gating).

**Gate:** shell runs on-disk programs; a program exceeding its manifest
`max_mem` is aborted at load. Tag `M13`.

---

## 5. Autonomous-development methodology

Because development runs unattended, every change must be verifiable without a
human at the keyboard:

- **Self-checking boot.** The Phase-A harness is the contract: each phase adds
  assertions; a run is green only if the final `[SELFTEST] DONE n/n` matches.
- **Headless output.** Serial (Phase B) + debugcon carry all diagnostics; the
  interactive shell is never on the critical path for verification.
- **Deterministic gates.** `tools/smoke.sh --mb2`/`--uefi`: exit 124 = alive;
  combine with a grep for the green marker. No flakiness tolerated.
- **Small, tagged steps.** One logical change per commit; a devlog per milestone;
  tag the stable points; never merge a red gate.
- **English-only project content**; minimal, auditable code consistent with the
  security thesis.

## 6. Sequencing rationale

A unblocks everything (first working user process + the harness). C (userland)
is what turns a booting kernel into an OS. D (persistence) and E (user-space
driver) are what **prove** the security thesis — isolation and capability
mediation demonstrated on real programs and a real device. F/G harden and polish
once the core is real.
