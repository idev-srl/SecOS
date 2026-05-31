# SECoS — Roadmap

Single, linear milestone scheme (M0 → present → future). Earlier revisions of
this file mixed two conflicting numbering schemes (the executed git milestones
and an older pre-analysis plan); that has been collapsed into one timeline.

- **Done:** M0–M6
- **In progress:** M7 (ring-3 + cooperative scheduling — runtime demo not yet passing)
- **Planned:** M8+

Per-milestone implementation notes live in `docs/devlog/M*.md`.

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
| M7 | **WIP** | — | Ring-3 entry (`arch_enter_user_mode`) + `SYS_YIELD` cooperative scheduling — **runtime demo not yet passing** |

---

## 2. Current focus — finish M7

### Problem
At HEAD the boot path runs an M7 demo from `kernel_main_phase2`: it builds a
synthetic 512-byte ELF (`mov rax,0 / int 0x80 / jmp loop`), creates two
processes, and calls `arch_enter_user_mode(p1)` (marked `// NOT REACHED`).
The smoke test reports PASS (no triple fault), but debugcon output stalls at:

```
[M7] Creating two ring3 yield-loop processes
```

The follow-up lines (`[M7] p1 pid=...`, `[M7] Entering ring3`, `[SCHED] switch ...`)
never appear, so the stall is inside the two `process_create_from_elf()` calls,
before ring-3 is ever entered. Because the demo ends with `arch_enter_user_mode`,
it also short-circuits the rest of boot — RAMFS/VFS init and the interactive
shell are never reached. (Full detail in `docs/devlog/M7.md`.)

### Tasks
- Diagnose why `process_create_from_elf()` does not return for the synthetic
  ELF (trapframe allocation, address-space setup, ELF parse, or stack mapping).
- Confirm the ring-3 transition (`arch_enter_user_mode`) reaches user mode and
  emit at least one `[SCHED] switch` between the two processes.
- Gate the demo behind a build flag (e.g. `M7_RING3_DEMO`, like
  `M4_SELFTEST_ENABLE`) so normal boot still reaches the shell.

### Done when
- With the demo flag off: kernel boots to the interactive shell on both MB2 and
  UEFI paths (smoke PASS).
- With the demo flag on: debugcon shows `[SCHED] switch <a> -> <b>` alternating
  between the two ring-3 processes; no triple fault.
- Tag `M7_STABLE`.

---

## 3. Planned milestones

> Future milestones continue the single timeline (M8+). They are grounded in
> open work already visible in the codebase, not speculative features.

### M8 — Real multiprogramming
**Goal:** processes start, run, and exit cleanly under a preemptive scheduler.

- `SYS_EXIT`: release the address space (`vmm_space_destroy`), remove the PCB
  from the process table, schedule the next process. Verify no PMM leak across
  repeated spawn/exit (PMM stats stable).
- Timer-driven preemption: per-PCB tick budget; yield after N ticks (not every
  tick), driven from the IRQ0 handler on top of the M6/M7 trapframe machinery.
- `TSS.RSP0` update on every switch; `ps` shell command shows live RUNNING/READY
  states for multiple PIDs.

**Depends:** M7. **Done when:** two ELF processes produce interleaved output via
`SYS_WRITE`; `ps` reflects alternating states; clean exit leaves PMM stable.

### M9 — Memory scalability
**Goal:** the kernel manages all reported RAM and runs from the higher half.

- Higher-half kernel at `0xFFFFFFFF80000000`: keep identity map transiently for
  boot, drop it after switch; update linker script, bootloader PML4 entry, and
  any residual physical casts.
- PMM scalability: replace the linear bitmap scan (`find_free_frame`) with a
  free-list or buddy allocator; remove the early 128 MB / 512 MB clamps and
  handle the full memory map (test with QEMU `-m 2G`).
- Demand paging: complete `vmm_handle_page_fault` on-demand allocation for
  registered user regions, with validation and per-process limits.

**Depends:** M8. **Done when:** kernel runs higher-half with the low identity
map removed; PMM reports >512 MB free on a 2 GB VM; demand-paged user region
faults in correctly; a W+X page request is rejected (panic/#GP).

### M10 — UEFI handoff hardening
**Goal:** the UEFI path is deterministic and safe on real firmware.

- Implement real post-ExitBootServices ELF segment mapping with 4 KB pages and
  W^X (currently a placeholder that relies on the firmware identity map; see
  `uefi/boot.c`).
- Copy `secos_boot_info` and the memory-map descriptors into a kernel-owned
  frame before the PMM can recycle `EFI_LOADER_DATA`.
- Re-audit PMM bitmap placement at `_kernel_end` for collisions with loaded
  kernel segments.
- Add a serial (COM1 / 0x3F8) console driver for headless debug and post-EBS
  diagnostics (no serial driver exists yet).

**Depends:** M9 (higher-half makes mapping ownership clean). **Done when:**
boot is UB-free on OVMF and at least one real firmware; boot info survives PMM
init; serial output works.

### M11 — Driver Space enforcement
**Goal:** Driver Space becomes a verifiable security boundary.

- Add `proc_type` (`PROC_TYPE_USER` / `PROC_TYPE_DRIVER`) to the PCB, set by the
  loader from the ELF manifest (`MANIFEST_FLAG_DRIVER`).
- Reject `SYS_DRIVER` for `PROC_TYPE_USER` with `DRV_ERR_PERM` before touching
  the device registry.
- Implement `DRIVER_OP_MAP_MEM`: validate `mem_offset`/`mem_length` against the
  device descriptor; map into the driver process (USER=1, RW=1, NX=1); track for
  precise cleanup; unbind on process destroy.
- Automatic restart of a critical driver on abnormal exit (bounded N restarts in
  K ticks, then `DEV_FLAG_FAILED`); `DRIVER_OP_IRQ_SUBSCRIBE` delivering IRQ
  events to a driver via an IPC queue consumed through `SYS_READ`.

**Depends:** M8 (lifecycle/IPC), M9 (reliable MAP_MEM). **Done when:** a user
process gets `DRV_ERR_PERM` on `SYS_DRIVER`; a crashing driver auto-restarts;
MAP_MEM maps and frees correctly (PMM stable); a simulated IRQ reaches the
driver without a race.

---

## 4. Open architectural debt (re-audited against HEAD)

The original critical analysis (pre-M0) listed 15 issues. Most were closed by
M1–M7; remaining ones feed the milestones above. Line numbers from the original
analysis have drifted — locations below are indicative.

| # | Issue | Status | Where it lands |
|---|-------|--------|----------------|
| 1 | UEFI page tables never activated | **Fixed** | `AllocatePages` + `activate_page_tables()` called pre-handoff |
| 2 | Console calls after ExitBootServices | **Fixed** | explicit "no boot-services past this point" guard in `uefi/boot.c` |
| 3 | PMM clamp limits usable RAM | **Partial** | 512 MB map limit + a 128 MB early-region clamp remain → M9 |
| 4 | PMM bitmap may collide with loaded ELF segments | **Open (re-audit)** | M10 |
| 5 | `_bss_start` after `*(COMMON)` in linker script | **Fixed** | M1 |
| 6 | Scheduler without real context switch | **Implemented** | M6/M7 (demo still WIP) |
| 7 | VMM accesses page tables via raw physical cast | **Fixed** | `phys_to_virt` (M1.2) |
| 8 | `vmm_space_destroy` leaks page-table frames | **Fixed** | M1 |
| 9 | Kernel stack without guard page | **Fixed** | M2 |
| 10 | `AllocatePool` for page tables (mis-aligned) | **Fixed** | now `AllocatePages` |
| 11 | `secos_boot_info` in recyclable `EFI_LOADER_DATA` | **Partial** | still static, mitigated by identity-map assumption → copy to kernel frame in M10 |
| 12 | No kernel-owned GDT with TSS descriptor | **Fixed** | `arch/x86/tss.c` builds GDT + TSS |
| 13 | `find_free_frame` is O(n) bitwise | **Open** | acceptable now; buddy/free-list in M9 |
| 14 | UEFI ELF segment copy relies on identity map | **Open** | real 4 KB mapping in M10 |
| 15 | `kernel_main(uint32_t, uint64_t)` magic-based signature | **Open (by design)** | dual-boot dispatch is intentional; optionally formalize as typed `boot_params` |

---

## 5. Open design decisions

1. **Higher-half vs identity-mapped kernel.** Targeted for M9. Until then, all
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
