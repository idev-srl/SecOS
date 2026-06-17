# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

SECoS is a freestanding x86-64 (long mode) secure kernel written in C99 + NASM. It boots two ways from one binary: **UEFI** (primary, via an external bootloader) and **GRUB Multiboot2** (legacy). The security focus is memory isolation: W^X enforcement, NX, user/kernel separation, validated syscall boundaries, and guard-paged stacks.

Development is organized into milestones (M0–M7). `ROADMAP_SECoS.md` is the source of truth for the technical roadmap and includes a critical architectural-issues list worth reading before deep work (several of those issues — linker COMMON, `vmm_space_destroy`, kernel stack guard pages, scheduler context switch — have since been fixed in M1/M2/M6/M7). `docs/devlog/M0.md`–`M7.md` document each milestone.

**Current real state (HEAD):** M0–M7 done; M8 core done (verified N=2).
- **M7** (ring-3 + `SYS_YIELD` cooperative scheduling) works; demo gated behind `M7_RING3_DEMO`. See `docs/devlog/M7.md` for the four bugs it uncovered (notably: clear `EFLAGS.NT` before `iretq`; private `PML4[0]` PDPT per user space; ELF copy stride bug).
- **M8** (preemptive multitasking) works: trapframe `isr_timer` + quantum preemption (ring-3 only), kernel idle task, `SYS_EXIT`→zombie→reap, `vmm_space_destroy` frees the private PDPT, no PMM leak (verified N=4/N=6). Demo gated behind `M8_SCHED_DEMO`. Root-caused/fixed a heap bug along the way: `kfree` coalesced non-physically-contiguous free blocks, inflating a block across the gap between frames so a later `kmalloc` wrote over an unrelated live page table (invisible to physmap watches since the heap addresses blocks via the identity map). Fix in `mm/heap.c`: coalesce only when `current + HEADER + size == next`. See `docs/devlog/M8.md`.
- **M9** (real userland + mandatory ELF signing) done — tag `M9_STABLE`. In-kernel crypto (`crypto/`: SHA-256/512 + Ed25519 verify, KAT-tested at boot). **Every ELF must be signed to run**: `process_create_from_elf` calls `elf_signature_verify` (`mm/elf_sign.c`) and refuses unless valid, unless built `-DDEV_ALLOW_UNSIGNED` (the M7/M8 synthetic demos use this). Digest = SHA-256(whole file, 64 sig bytes zeroed); Ed25519 over the digest; QSIG note; embedded pubkey in `crypto/secos_pubkey.h`. Host tools in `tools/` (`secos-keygen`, `secos-sign`, `elf2h.py`); `make user-progs` builds+signs `user/hello.c` → `crypto/user_hello_elf.h`. The signed `hello` is loaded from the VFS, verified, run in ring-3 (prints via `SYS_WRITE` fd 1/2 → console+debugcon). See `docs/SIGNING.md`, `docs/devlog/M9.md`.
- **M10** (storage & persistence) done. virtio-blk driver (`drivers/pci.*`, `drivers/virtio_blk.*`: legacy/transitional PCI, single split virtqueue in a page-aligned static buffer — kernel is identity-mapped so virt==phys for DMA — polled, bounce-buffered) registers block device `vda`; `block_dev_t` gains `write()`. VFS goes single-root → **multi-mount** (`fs/vfs.c`): ramfs root `/`, disk FS at `/mnt`, longest-prefix routing. **FAT32, ext2 and ext4 read-write** through the VFS (`fs/fat32.c`, `fs/ext2.c` full rewrites): kernel-written files are host-readable (`mtools`/`debugfs`) and `e2fsck`-clean; ext4 is **no-journal v0** (image `mkfs.ext4 -O ^has_journal,^metadata_csum`; extents + 64-bit supported, JBD2/csum deferred). `SYS_SPAWN`/`SYS_WAIT` (8/9) + shell `run <path>`: the interactive shell runs as the **scheduler idle task** (one sched change: save idle context on preemption) so a spawned ring-3 program returns control to the shell on `SYS_EXIT`. Gate `M10_RUN_DEMO`: a signed ELF is written to disk, read back, verified and run in ring-3 (tampered copy refused). See `docs/devlog/M10.md`.
- **M11** (Driver Space for real) done. The driver privilege is now **rooted in the code signature**: the `.note.secos` manifest gains v2 fields (`proc_type`, `dev_id`, `dev_caps`; desc 24→40 bytes, `version` 2), covered by the Ed25519 signature. `process_create_from_elf` sets `proc_type` and, for a `PROC_TYPE_DRIVER` manifest, auto-binds the process to its device with the granted (device-intersected) capability subset. `SYS_DRIVER` now enforces `proc_type==DRIVER` (else `DRV_ERR_NOTDRV`) **and** the per-binding granted caps (a driver granted READ/WRITE/GET_INFO is refused MAP_MEM even on a device that supports it); every event mirrors to debugcon (`[DRV-AUDIT]`). `process_destroy` drops driver bindings first (fixed a latent stale-`process_t*` privilege leak). Demo gate `M11_DRIVER_DEMO` runs a signed user-space driver (`user/driver_demo`: mediated reg read/write round-trips, un-granted MAP_MEM refused) and a signed plain-user probe (`user/userprobe`: all `SYS_DRIVER` denied). **Gotcha found**: `mm/heap.c` reliably serves `kmalloc` only up to one frame (~4 KB) — `kmalloc` of a larger size can silently return an undersized block — so the demo loads the embedded **signed** ELF directly (still verified) rather than via the VFS; a buddy/multi-frame allocator is an M12 item. See `docs/devlog/M11.md`.
- **M12** (memory scalability + W^X hard gate + **higher-half kernel**) done. PMM now manages **all** RAM: the 512 MB `total_frames` clamp and the 128 MB per-region identity clamps are gone (frames above the identity window are reached via the physmap; a defensive 32 MB bitmap cap ≈ 1 TB RAM). `find_free_frame` skips full bitmap words from a rolling cursor; `pmm_alloc_contiguous`/`pmm_free_contiguous` back multi-frame allocations. The **heap** (`mm/heap.c`) now addresses memory through the physmap (`phys_to_virt`), `expand_heap` allocates enough contiguous frames for the request, and `kmalloc` returns NULL instead of a silently-undersized block — **fixes the M11 >4 KB gotcha** (the M11 demo loads from the VFS again). `vmm_map`/`vmm_map_in_space` add a **W^X hard gate** (reject RW-without-NX with `-5`); boot self-tests log `[WX] W+X mapping rejected` and `[HEAP] large kmalloc(64K) OK`; on `-m 2G` the PMM reports ~2045 MB free. **Higher-half**: the kernel is linked + runs at `KERNEL_VMA = 0xFFFFFFFF80000000` (`-mcmodel=kernel -fno-pic`). `linker.ld` keeps a low `.boot` section (Multiboot2 header + 32-bit setup + initial tables) and links the rest HIGH/loads LOW via `AT()`. `boot/boot.asm` builds page tables mapping BOTH low identity (0–512 MB) AND the high half, then jumps high (`long_mode_high`); the UEFI loader (`uefi/*`) loads segments at their LMA (`p_paddr`) and maps PML4[511]→high-half. `mm/vmm.c` kernel PML4 maps PML4[511]; `kvirt_to_phys` (`mm/vmm.h`) translates kernel/physmap VMAs → physical for the two symbol-as-physical spots (PMM bitmap frame math, virtio DMA `phys_of`). **The low identity map is deliberately kept** (virtio DMA + early frame access use it). Both `smoke.sh --mb2` and `--uefi` PASS. **Deferred**: full demand paging (`vmm_region` path dormant), W^X of the identity huge-page map. See `docs/devlog/M12.md`.
- Demos are off by default so normal boot reaches the shell (the signing gate is enforced but dormant at boot). Verify non-interactively with `tools/selftest.sh` (M4 + crypto KATs + M7 + M8 + M9 + M10×{fat32,ext2,ext4} + M11 + M12 = 51/51, from the debugcon log). CPU exceptions surface on debugcon as `[EXC] int=… rip=…`. Interactive shell over serial: `make run-serial`; with a disk: `make disk-fat32 && make run-disk` (note: `-boot d` so the BIOS boots the CD not the data disk). Execution plan: `docs/DEVELOPMENT_PLAN.md`; next is M13 (usability & policy: shell launches on-disk programs + end-to-end manifest enforcement).

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
