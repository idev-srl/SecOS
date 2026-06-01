# SECoS — Roadmap

Single, linear milestone scheme (M0 → present → future). Earlier revisions of
this file mixed two conflicting numbering schemes (the executed git milestones
and an older pre-analysis plan); that has been collapsed into one timeline.

- **Done:** M0–M9 (M9 = real userland + mandatory ELF signing; signed `hello`
  built by the toolchain, loaded from the VFS, verified, and run in ring-3)
- **In progress:** —
- **Planned:** M10+ (next: storage & persistence, virtio-blk)

Per-milestone implementation notes live in `docs/devlog/M*.md`. The detailed
execution plan — mission, definition of done, per-phase acceptance gates, and
the autonomous-development methodology — is in
[`docs/DEVELOPMENT_PLAN.md`](docs/DEVELOPMENT_PLAN.md). Locked direction:
full secure OS, custom minimal ABI, UEFI-golden + virtio-blk.

---

## 1. Milestone status

| ID | Status | Tag | Summary |
|----|--------|-----|---------|
| M0 | DONE | — | UEFI boot chain (external loader → `kernel_main`) |
| M1 | DONE | `M1_STABLE` | Kernel-owned page tables, `phys_to_virt` access, `vmm_space_destroy` frees frames, kernel-owned GDT+TSS, linker COMMON fix |
| M2 | DONE | `M2_STABLE` | Virtual kernel-stack region, guard pages (main + IST1/2/3), debugcon boot markers, smoke harness |
| M3 | DONE | `M3_ISOLATION_BASE` | User/kernel isolation: `user_range_valid`, `copy_from/to_user`, syscall pointer hardening, `vmm_map` supervisor enforcement |
| M4 | DONE | `M4_STABLE` | Stabilization: 12/12 in-kernel isolation selftest, `vmm_map_in_space` supervisor enforcement |
| M5 | DONE | — | Trapframe-based `INT 0x80` entry + C `syscall_handler`; bounded kernel-stack slots |
| M6 | DONE | — | Minimal context switch: per-PCB trapframe, `arch_iret_to_tf`, CR3 switch on resume |
| M7 | DONE | `M7_STABLE` | Ring-3 entry (`arch_enter_user_mode`) + `SYS_YIELD` cooperative scheduling — two ring-3 processes alternate, verified by `tools/selftest.sh` |

---

## 2. Current focus — M8 (M7 done)

### M7 — resolved
Cooperative ring-3 scheduling works. Diagnosis found four independent bugs
(premature timer preemption, `EFLAGS.NT` causing `iretq` #GP, supervisor
`PML4[0]` + shared user PDPT, and a 2× stride in the ELF segment copy) — all
fixed; see `docs/devlog/M7.md`. Two ring-3 processes now alternate via
`SYS_YIELD`. The demo is gated behind `M7_RING3_DEMO` (off by default, so normal
boot reaches the shell) and asserted non-interactively by `tools/selftest.sh`
(M4 12/12 + alternating ring-3 switches + no `[EXC]`).

### Next — M8 (see §3)
Preemptive multitasking: `SYS_EXIT` + reaping, timer-driven preemption, serial
console. Note for M8: re-enable timer-driven switching (reverting the M7
cooperative-only `sched_on_timer_tick`) with a proper quantum, and make
`vmm_space_destroy` free the per-space private `PML4[0]` PDPT added in M7.

---

## 3. Planned milestones

> Future milestones continue the single timeline (M8+). They are grounded in
> open work already visible in the codebase, not speculative features.
> Per the locked plan, **M8–M11 are mandatory** and **M12–M13 are stretch**.
> See [`docs/DEVELOPMENT_PLAN.md`](docs/DEVELOPMENT_PLAN.md) for acceptance gates.

### M8 — Real multiprogramming  [DONE — verified N=4/N=6]
**Goal:** processes start, run, and exit cleanly under a preemptive scheduler.

- ✅ `SYS_EXIT` → ZOMBIE + switch; reaped from idle via `process_destroy` /
  `vmm_space_destroy` (now frees the M7 private `PML4[0]` PDPT). No PMM leak
  across rounds.
- ✅ Timer-driven preemption: trapframe `isr_timer`, quantum-based switch, only
  preempts ring-3, EOIs before switching; kernel idle task as fallback.
- ✅ Construction race fix (`PROC_BLOCKED` until the trapframe is ready).
- ✅ Fixed the heap allocator bug that corrupted page tables at N≥3 (`kfree`
  coalesced non-physically-contiguous blocks — see `docs/devlog/M8.md`).
- Still pending (small): `ps` live states / interleaved user output.

**Depends:** M7. **Done when:** two ELF processes produce interleaved output via
`SYS_WRITE`; `ps` reflects alternating states; clean exit leaves PMM stable.

### M9 — Real userland + signed ELFs (identity-defining)  [DONE — tag `M9_STABLE`]
**Goal:** independent, **signed** ELF programs, built by a user toolchain, run
from the VFS — with mandatory signature verification as the root of trust.
All verified by `tools/selftest.sh` (14/14). Details in `docs/devlog/M9.md`;
`SYS_SPAWN`/`SYS_WAIT` and a `proc_type`-driven driver-mode tie-in remain for
later milestones.

- **Crypto:** in-kernel SHA-256 + Ed25519 *verify* (freestanding, no malloc),
  known-answer self-tests.
- **Signing:** every ELF (user *and* driver) must be signed to run; Ed25519,
  refuse-by-default with `-DDEV_ALLOW_UNSIGNED`. Manifest v2 (`proc_type`,
  `caps_mask`) + `QSIG` signature note; embedded trusted public key; loader gate.
  Host tools `secos-keygen` / `secos-sign`. See `docs/SIGNING.md`.
- **Userland:** `crt0` + in-house libc — custom syscall ABI underneath, a
  POSIX-friendly API on top so open-source C ports from source. Documented in
  `docs/SYSCALL_ABI.md`.
- **Run from VFS:** embed an initrd of signed ELFs; the loader runs an ELF
  resolved through the VFS instead of a hand-built buffer. Add `SYS_SPAWN`/`SYS_WAIT`.

**Depends:** M8. **Done when:** crypto KATs pass; a `hello` ELF built+signed by
the toolchain loads from the VFS and prints via `SYS_WRITE` (harness-captured);
an unsigned/tampered ELF is refused; `-DDEV_ALLOW_UNSIGNED` allows the bootstrap
path.

### M10 — Storage & persistence
**Goal:** load programs and persist data on a real disk.

- virtio-blk driver exposing `block_read/block_write`.
- **FAT32, ext2 and ext4** **read-write** through the VFS over the block device
  (all three are target filesystems for SECoS).
- Mount a data filesystem at boot; load user programs from the disk image.

**Depends:** M9. **Done when:** write a file, reboot the VM, read it back
identical; run a program loaded from disk — all harness-asserted.

### M11 — Driver Space for real (proves the security thesis)
**Goal:** Driver Space becomes a verifiable security boundary with a user-space driver.

- Add `proc_type` (`PROC_TYPE_USER` / `PROC_TYPE_DRIVER`) to the PCB, set by the
  loader from the ELF manifest (`MANIFEST_FLAG_DRIVER`); reject `SYS_DRIVER` for
  USER processes with `DRV_ERR_PERM`.
- Move one driver (target: keyboard) to a **user-space process**: real
  register/MMIO access via validated `DRIVER_OP_*`, capability + range checks,
  audit log. Implement `DRIVER_OP_MAP_MEM` with precise cleanup on unload.
- IRQ delivery via an IPC queue consumed through `SYS_READ` on a special fd;
  bounded auto-restart of a crashing driver (`DEV_FLAG_FAILED` past threshold).

**Depends:** M8 (lifecycle/IPC), M10 (block device for the disk driver case).
**Done when:** a user-space driver delivers real input/events end-to-end; a
capability/range violation is denied **and** audited; a USER process gets
`DRV_ERR_PERM`.

### M12 — Hardening & memory scalability (stretch)
**Goal:** the kernel manages all RAM, runs higher-half, and UEFI handoff is firmware-safe.

- Higher-half kernel at `0xFFFFFFFF80000000`; drop the low identity map after the
  switch; update linker script, bootloader PML4 entry, residual physical casts.
- PMM scalability: replace the linear bitmap scan with a free-list/buddy
  allocator; remove the 128 MB / 512 MB clamps (test with QEMU `-m 2G`).
- Complete demand paging with per-process limits.
- UEFI handoff hardening: real 4 KB post-EBS ELF mapping with W^X; copy
  `secos_boot_info` + memory map into a kernel-owned frame; re-audit PMM bitmap
  placement at `_kernel_end`. Full W^X audit.

**Depends:** M11. **Done when:** UEFI boot on real firmware; PMM reports >512 MB
free on a 2 GB VM; a W+X page request is rejected (panic/#GP).

### M13 — Usability & policy enforcement (stretch)
**Goal:** a usable system with end-to-end manifest policy.

- Shell that launches user programs from the FS; minimal IPC/pipes; a few more
  syscalls; end-to-end `.note.secos` enforcement (`max_mem`, capability gating).

**Depends:** M11. **Done when:** the shell runs on-disk programs; a program
exceeding its manifest `max_mem` is aborted at load.

---

## 4. Open architectural debt (re-audited against HEAD)

The original critical analysis (pre-M0) listed 15 issues. Most were closed by
M1–M7; remaining ones feed the milestones above. Line numbers from the original
analysis have drifted — locations below are indicative.

| # | Issue | Status | Where it lands |
|---|-------|--------|----------------|
| 1 | UEFI page tables never activated | **Fixed** | `AllocatePages` + `activate_page_tables()` called pre-handoff |
| 2 | Console calls after ExitBootServices | **Fixed** | explicit "no boot-services past this point" guard in `uefi/boot.c` |
| 3 | PMM clamp limits usable RAM | **Partial** | 512 MB map limit + a 128 MB early-region clamp remain → M12 |
| 4 | PMM bitmap may collide with loaded ELF segments | **Open (re-audit)** | M12 |
| 5 | `_bss_start` after `*(COMMON)` in linker script | **Fixed** | M1 |
| 6 | Scheduler without real context switch | **Implemented** | M6/M7 (demo still WIP) |
| 7 | VMM accesses page tables via raw physical cast | **Fixed** | `phys_to_virt` (M1.2) |
| 8 | `vmm_space_destroy` leaks page-table frames | **Fixed** | M1 |
| 9 | Kernel stack without guard page | **Fixed** | M2 |
| 10 | `AllocatePool` for page tables (mis-aligned) | **Fixed** | now `AllocatePages` |
| 11 | `secos_boot_info` in recyclable `EFI_LOADER_DATA` | **Partial** | still static, mitigated by identity-map assumption → copy to kernel frame in M12 |
| 12 | No kernel-owned GDT with TSS descriptor | **Fixed** | `arch/x86/tss.c` builds GDT + TSS |
| 13 | `find_free_frame` is O(n) bitwise | **Open** | acceptable now; buddy/free-list in M12 |
| 14 | UEFI ELF segment copy relies on identity map | **Open** | real 4 KB mapping in M12 |
| 15 | `kernel_main(uint32_t, uint64_t)` magic-based signature | **Open (by design)** | dual-boot dispatch is intentional; optionally formalize as typed `boot_params` |

---

## 5. Open design decisions

1. **Higher-half vs identity-mapped kernel.** Targeted for M12. Until then, all
   new page-table access must go through `phys_to_virt()` (already the norm
   since M1.2) so the migration does not require rewriting access paths.
2. **Shared vs per-process PML4.** Decide whether the kernel keeps one PML4
   (shared with user space via high entries) or each process gets a private
   PML4 with duplicated kernel entries. Impacts `vmm_space_create_user`.
3. **Primary boot path.** Both UEFI and Multiboot2 are maintained. If the target
   is modern hardware, consider designating UEFI as the "golden" path and
   demoting Multiboot2 to legacy/CI-only to cut maintenance cost.
4. **Long-term physical allocator.** Bitmap → free-list or buddy (M9); the
   choice drives demand-paging alloc/free performance.
5. **Boot handoff ABI.** Keep the magic-based `kernel_main` signature, or move to
   a single typed `struct boot_params*` shared by both boot paths (issue #15).
