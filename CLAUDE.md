# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

SECoS is a freestanding x86-64 (long mode) secure kernel written in C99 + NASM. It boots two ways from one binary: **UEFI** (primary, via an external bootloader) and **GRUB Multiboot2** (legacy). The security focus is memory isolation: W^X enforcement, NX, user/kernel separation, validated syscall boundaries, and guard-paged stacks.

Development is organized into milestones (M0–M7). `ROADMAP_SECoS.md` is the source of truth for the technical roadmap and includes a critical architectural-issues list worth reading before deep work (several of those issues — linker COMMON, `vmm_space_destroy`, kernel stack guard pages, scheduler context switch — have since been fixed in M1/M2/M6/M7). `docs/devlog/M0.md`–`M7.md` document each milestone.

**Current real state (HEAD):** M0–M7 done; M8 core done (verified N=2).
- **M7** (ring-3 + `SYS_YIELD` cooperative scheduling) works; demo gated behind `M7_RING3_DEMO`. See `docs/devlog/M7.md` for the four bugs it uncovered (notably: clear `EFLAGS.NT` before `iretq`; private `PML4[0]` PDPT per user space; ELF copy stride bug).
- **M8** (preemptive multitasking) works: trapframe `isr_timer` + quantum preemption (ring-3 only), kernel idle task, `SYS_EXIT`→zombie→reap, `vmm_space_destroy` frees the private PDPT, no PMM leak (verified N=4/N=6). Demo gated behind `M8_SCHED_DEMO`. Root-caused/fixed a heap bug along the way: `kfree` coalesced non-physically-contiguous free blocks, inflating a block across the gap between frames so a later `kmalloc` wrote over an unrelated live page table (invisible to physmap watches since the heap addresses blocks via the identity map). Fix in `mm/heap.c`: coalesce only when `current + HEADER + size == next`. See `docs/devlog/M8.md`.
- Demos are off by default so normal boot reaches the shell. Verify non-interactively with `tools/selftest.sh` (M4 12/12 + M7 ring-3 + M8 preempt/exit/no-leak, all from the debugcon log). CPU exceptions surface on debugcon as `[EXC] int=… rip=…`. Interactive shell over serial: `make run-serial`. Execution plan: `docs/DEVELOPMENT_PLAN.md`; next is M9 (real userland).

## Build & run

```bash
make            # build kernel.bin (default target)
make uefi       # build UEFI bootloader -> dist/EFI/BOOT/BOOTX64.EFI (also copies kernel.bin to dist/kernel.elf)
make iso        # build myos.iso (GRUB Multiboot2 bootable)
make run        # build iso + run in QEMU with -debugcon stdio
make clean      # remove objects, kernel.bin, iso, dist/
```

There is no separate lint step. `CONTRIBUTING.md` asks contributors to build clean under `-Wall -Wextra` locally.

Toolchain (Debian/Ubuntu): `nasm gcc binutils gnu-efi ovmf qemu-system-x86 mtools grub-common grub-pc-bin xorriso`.

## Testing (smoke tests)

There is no unit-test framework. Correctness is validated by **deterministic QEMU smoke tests** plus an **in-kernel selftest suite**.

```bash
tools/smoke.sh --mb2  --timeout 20 --log /tmp/secos_mb2.log    # Multiboot2/GRUB path
tools/smoke.sh --uefi --timeout 25 --log /tmp/secos_uefi.log   # UEFI/OVMF/q35 path
```

PASS/FAIL criterion (counterintuitive): a triple fault resets the CPU and, with `-no-reboot`, QEMU exits **before** the timeout (exit 1 = FAIL). A healthy kernel keeps running until `timeout(1)` kills it (exit 124, which the script remaps to **exit 0 = PASS**). So "the process getting killed by timeout" is the success signal.

The kernel writes crash-signature markers to ISA debugcon port `0xE9`, forwarded by QEMU to the `--log` file. Expect lines like `SECoS build <BUILD_TS> git:<GIT_HASH>` and `[M2] Stack switch ok. RSP=...`.

The isolation selftest (12 cases for `user_range_valid`/`copy_from_user`/`copy_to_user`) runs at boot when `M4_SELFTEST_ENABLE=1` (the default) and logs `[M4][SELFTEST]` lines via debugcon. Disable with `make iso CFLAGS_EXTRA=-DM4_SELFTEST_ENABLE=0`. To exercise other subsystems, use the interactive shell commands (`memtest`, `memstress`, `elfload`/`elfunload`, `drvtest`, RAMFS `rf*` and VFS `v*` commands) — these are the de-facto test harness.

## Boot architecture (the part that needs multiple files to understand)

One binary supports both boot paths; `kernel_main(magic, info)` disambiguates at runtime:
- `magic == 0` → **UEFI**; `info` is a `struct secos_boot_info*` (see `kernel/bootinfo.h`), populated by the external bootloader in `uefi/boot.c` (`efi_main`) which ELF-loads the kernel and calls `_uefi_start` (64-bit) in `boot/boot.asm`.
- `magic == 0x36d76289` → **Multiboot2**; `info` is the MB2 info pointer. `_start` (32-bit) in `boot/boot.asm` sets up long mode, then jumps to `long_mode` (64-bit) → `kernel_main`.

`kernel_main` saves the boot magic/info into globals (`g_multiboot_magic`, `g_multiboot_info`), does early init, then **switches to the hardened virtual kernel stack** and tail-calls `kernel_main_phase2()` via `trampoline_switch_stack`. Phase 2 re-reads the globals to finish framebuffer/PMM/etc. init. This two-phase split exists because the stack pointer changes mid-init, so don't assume locals survive across the trampoline — pass state through globals or the boot-info struct.

Build defines `BUILD_TS` and `GIT_HASH` (from the Makefile) are compiled in and surfaced in the debugcon banner and the RAMFS `VERSION` file; they are how a running image is traced back to a build.

## Code layout

- `boot/` — MB2 (`_start`, 32-bit) and UEFI (`_uefi_start`, 64-bit) entry, in `boot.asm`.
- `arch/x86/` — IDT, TSS, syscall entry (`syscall_asm.asm`), context switch (`context_switch.c` + `context_switch_asm.asm`). Trapframe-based.
- `kernel/` — `kernel.c` (init), `process.c` + `sched.c` (PCB/scheduler), `syscall.c` + `syscall_trap.c` (dispatch), `driver_if.c` (Driver Space mediation), `shell.c`, `selftest.c`, `panic.c`.
- `mm/` — `pmm.c` (frame allocator), `vmm.c` (paging, kernel-owned page tables, W^X/NX), `heap.c` (kmalloc/kfree), `elf*.c` (ELF64 loader, `.note.secos` manifest parsing, precise per-process unload), `user_copy.c` (`user_range_valid`, `copy_from/to_user`).
- `fs/` — `vfs.c` abstraction over `ramfs` (root), `fat32`, `ext2` (+ `ext2ramdev`), `block.c`.
- `drivers/` — framebuffer (`fb`, `fb_console`), keyboard, timer (PIT/IRQ0), RTC.
- `uefi/` — the external UEFI bootloader (`boot.c`, `elf_load.c`, `crt0.s`, `efi.h`), built separately by `make uefi`.
- `config.h` — compile-time feature toggles (`ENABLE_*`) and `CONFIG_MULTIBOOT`/`CONFIG_UEFI` (both can be 1 for dual-boot; at least one is required).

`edk2/` is a large untracked vendored tree (alternative `edk2_bootloader/` path); it is not part of the default build.

## Security & style invariants (enforced by review, not tooling)

- **W^X**: never map a page writable and executable at once; W|X ELF segments are rejected unconditionally by the loader.
- **NX** on all data/stack regions; **never set the USER bit on kernel pages** (`vmm_map`/`vmm_map_in_space` enforce supervisor; PML4[256+] is asserted kernel-only).
- All syscall user pointers must go through `user_range_valid` + `copy_from/to_user` — never dereference a user pointer directly in the kernel (this is the M3 hardening contract).
- ELF segments must have `p_align` of 0 or 0x1000; track allocated pages per process for precise unload/accounting; user stacks get a guard page.
- C99 freestanding, 4-space indent (no tabs), K&R braces, explicit fixed-width types (`uint64_t`), domain-prefixed static helpers (`vmm_`, `pmm_`, `drv_`).
- Code, comments, and docs in **English only** — no mixed Italian/English (existing files still contain some Italian; don't add more).
- Topic branches (`feat/...`, `fix/...`, `docs/...`), imperative commit subjects, one logical change per commit.
