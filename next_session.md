# SECoS — Resume Here (session handoff)

_Last updated: M20 + consolidation to `main`. **Phases F+G complete.** Read this first._

## Where we are

- **Done through M20** — M0–M13 (A–E) + M14 (demand paging) + Phase F (M15
  fault-kill, M16 exec, M17 blocking) + **Phase G COMPLETE: M18 (mmap/brk/malloc),
  M19 (COW fork), M20 (page cache + file-backed mmap)**. **Everything is now
  consolidated on `main`** (the old `milestone/M7`, `milestone/M0`,
  `debug/uefi-unsupported` branches were merged/removed). `origin/main` is the
  single source of truth; develop on `main`.
- **Long-term roadmap: `docs/ROADMAP_TO_COMPLETE_OS.md`** — Phases F–L (M15–M32).
  **Next: pipes + TTY** (now unblocked by fork), then **Phase H** (storage
  maturity: VFS perms/ownership/mount syscalls, devfs/procfs, ext4 journaling) and
  **Phase I** (ACPI + APIC/IOAPIC + SMP — the big one).
- Build is green: `make` clean (0 warnings), `tools/selftest.sh` **96/96** (+9
  M18, +7 M19, +5 M20), both boot paths higher-half (`smoke.sh --mb2`/`--uefi`).
- **Self-test runner note:** the full suite takes ~12–15 min (≈14 variant builds).
  Run it as a SINGLE background process and wait for its completion — running
  several `selftest.sh` at once shares the build dir and corrupts/truncates logs.
- **M19 gotcha (important):** intermediate page-table entries are now always
  `RW|PRESENT` (USER still propagated). x86-64 ANDs the RW bit across ALL paging
  levels (CR0.WP=1), so a non-writable intermediate makes every leaf below it RO —
  which broke COW. The **leaf PTE is the sole write/exec authority** now.
- Only `edk2/` is untracked (a large vendored tree, not part of the build) — leave it.
  `.gitignore` shows a stale local modification from before this work — harmless,
  not committed. Build artifacts `secos-uefi.{img,vmdk}`, `disk.img`, `*.o`,
  `user/*.elf`, `uefi_loader.elf` are gitignored/untracked.

## What SECoS is (identity)

A minimal, **security-first x86-64 OS with a microkernel bias**. Not POSIX, not
general-purpose. Three execution modes (`docs/DRIVER_SPACE.md`):
**kernel** (ring 0), **driver** (ring 3 + capability-mediated HW access — a
*software* privilege, not a HW ring), **user** (ring 3, no HW).
**Every executable must be Ed25519-signed to run** — the signature is the root of
trust for the `.note.secos` manifest claims (mode, capabilities, limits).
Full mission + plan: `docs/DEVELOPMENT_PLAN.md`; high-level list: `ROADMAP_SECoS.md`.

## Locked decisions (do not relitigate without the user)

- Full secure OS; phases A–E mandatory, F–G stretch.
- **Custom minimal syscall ABI** underneath; libc exposes a **POSIX-friendly**
  surface so open-source C ports *from source* (NOT Linux-binary compatibility).
- **Mandatory signing**: every ELF, Ed25519 + SHA-256, **refuse-by-default** with
  `-DDEV_ALLOW_UNSIGNED` for bootstrap. Single project key for v0 (`QSIG` note has
  `key_id`, keyring-ready). Details: `docs/SIGNING.md`.
- Platform: **UEFI golden**, Multiboot2 = CI; storage = **virtio-blk** (M10).

## Milestone status

| M | State | Notes |
|---|-------|-------|
| M0–M6 | done | boot, paging/W^X, isolation, trapframe syscall, context switch |
| M7 | done (`M7_STABLE`) | ring-3 + `SYS_YIELD` cooperative; fixed 4 bugs (notably clear `EFLAGS.NT` before `iretq`; private `PML4[0]` PDPT per space; ELF copy stride) |
| M8 | done | preemptive sched + `SYS_EXIT`/reap + idle task + no leak (N=4/6); fixed `kfree` coalescing of non-contiguous blocks |
| M9 | done (`M9_STABLE`) | crypto (SHA-256/512 + Ed25519 verify), signing format+tools+gate, userland (crt0+libc), signed `hello` runs from VFS |
| M10 | done | storage & persistence: virtio-blk (`vda`) + RW **FAT32/ext2/ext4** via VFS multi-mount (`/mnt`); `SYS_SPAWN`/`SYS_WAIT` + shell `run`; signed ELF written to disk → read → verify → ring-3. See `docs/devlog/M10.md` |
| M11 | done | Driver Space for real: `.note.secos` v2 manifest carries `proc_type`/`dev_id`/`dev_caps` (signature-rooted); loader auto-binds a `PROC_TYPE_DRIVER` to its device with the granted cap subset; `SYS_DRIVER` enforces driver-only + per-binding caps + `[DRV-AUDIT]`. See `docs/devlog/M11.md` |
| M12 | done | Memory scalability + W^X hard gate + **higher-half kernel**: PMM manages all RAM (clamps gone; word-skip+cursor; `pmm_alloc_contiguous`); heap on physmap + multi-frame + NULL-on-fail; `vmm_map` rejects W+X (`-5`); `-m 2G` → ~2045 MB free. Kernel runs at `0xFFFFFFFF80000000` (both MB2 and UEFI). See `docs/devlog/M12.md` |
| **M13** | **done** | usability & policy: **manifest `max_mem` enforced at load** (leak-free abort, signature-rooted); `SYS_GETTICKS` + **kernel IPC channels** (`SYS_MSG_SEND`/`RECV`, `kernel/ipc.c`); shell `run` = on-disk launcher; producer/consumer demo exchange a message over channel 0. See `docs/devlog/M13.md` |
| **M14** | **done (stretch)** | **full demand paging**: per-process VMAs (`mm/vma.{c,h}`) replace eager mapping; `elf_load_image_lazy` reserves FILE/ANON regions; `vmm_handle_page_fault` materializes pages on first touch (`[PF] demand page`); pinned per-process ELF image; syscall buffers fault in via kernel-mode #PF (no `user_copy` change); stack guard = absence of a VMA; `max_mem` vs reserved footprint. See `docs/devlog/M14.md` |
| **M15** | **done (Phase F)** | **fault-driven process kill**: a ring-3 CPU fault kills only the offending process and returns to the scheduler instead of halting. `exception_handler` decides from saved `cs` (ring3⇒`sched_kill_current`, ring0⇒panic). `process_t.exit_code` (128+vec). Demo `M15_KILL_DEMO`. See `docs/devlog/M15.md` |
| **M16** | **done (Phase F)** | **exec model**: argv/env on the demand-paged user stack (`process_create_from_elf_args`, SysV `rdi/rsi/rdx`; crt0 unchanged); `SYS_SPAWN(path, char**)`; **blocking `SYS_WAIT`** returns exit status (rip-rewind re-run; status delivered into the waiter via `sched_wake_waitpid`); `SYS_EXIT` carries user status; shell `run <path> args`. libc `spawn`/`waitpid`. Demo `M16_EXEC_DEMO`. See `docs/devlog/M16.md` |
| **M17** | **done (Phase F)** | **blocking primitives** on the M16 block/wake core: `SYS_SLEEP` (13); **blocking `SYS_MSG_RECV`** (pre-queued msg returns immediately so M13 poll stays green). libc `sleep_ticks`. **Pipes/TTY deferred**. See `docs/devlog/M17.md` |
| **M18** | **done (Phase G)** | **dynamic memory**: `SYS_MMAP/MUNMAP/BRK/MPROTECT` (14–17) on the M14 VMA framework; `brk`/`sbrk` heap, anon `mmap` arena, `mprotect` (`vmm_protect_in_space`+invlpg); W^X-enforced + bounded by manifest `max_mem` at runtime; VMA tombstones + `VMA_MAX`→64. libc `malloc`/`free`/`mmap`. Demo `M18_MEM_DEMO`. See `docs/devlog/M18.md` |
| **M19** | **done (Phase G)** | **copy-on-write fork**: PMM frame refcounts (`pmm_share`/`pmm_unref`); `vmm_fork_space` (COW bit 9), `vmm_cow_fault`; `SYS_FORK` (18) / `process_fork`; libc `fork()`. **Fix**: intermediate PT entries always `RW|PRESENT` (leaf is sole protection authority). Demo `M19_FORK_DEMO`. See `docs/devlog/M19.md` |
| **M20** | **done (Phase G)** | **unified page cache** (`mm/pagecache.c`, 128 pages keyed by inode+offset) backs file `read()` AND **file-backed `mmap`** (MAP_PRIVATE via `vma_add_file`/`file_inode`; fault copies cached page into a private frame). `SYS_MMAP` +`fd` arg; `ksys_read`→cache, `ksys_write` invalidates. libc `open`/`read`/`close`/`mmap_file`. Demo `M20_MMAP_DEMO`: mmap+read of a VFS file return identical (coherent) bytes. See `docs/devlog/M20.md` |
| next | **Pipes/TTY → H → I** | **pipes + TTY** (unblocked by fork): anon pipes on the fd table, a ring-3 TTY line discipline (Ctrl-C→signal needs a signal mechanism). Then **Phase H** (VFS perms/ownership/mount syscalls, devfs/procfs, ext4 journaling) and **Phase I** (ACPI + APIC/IOAPIC + SMP). Full plan: `docs/ROADMAP_TO_COMPLETE_OS.md` |

## Build / test / run

```bash
make                       # build kernel.bin (default; signing gate ENFORCED, dormant at boot)
make iso                   # myos.iso (GRUB Multiboot2)
make user-progs            # build + SIGN user/hello.c -> crypto/user_hello_elf.h (needs python3 + `cryptography`)
tools/selftest.sh          # THE gate: builds M7/M8/M9/M10 variants, asserts from debugcon. Expect 38/38.
make disk-fat32            # 64MB test disk (also disk-ext2 / disk-ext4); make run-disk attaches it (-boot d!)
tools/smoke.sh --mb2 -t 8 --log /tmp/x.log   # single boot; exit 124->0 = alive = PASS
make run-serial            # interactive shell IN THE TERMINAL over COM1 (WSLg GUI is broken here; serial is the way)
```

Demos are gated and OFF by default (normal boot reaches the shell):
`-DM7_RING3_DEMO=1`, `-DM8_SCHED_DEMO=1`, `-DM9_USER_DEMO=1`. The M7/M8 synthetic
ELFs are unsigned, so build them with `-DDEV_ALLOW_UNSIGNED` (the harness does this).

Verification is **non-interactive** via debugcon markers (`[CRYPTO]`, `[SCHED]`,
`[M8]`, `[M9]`, `[SEC]`, `[hello]`, `[EXC]`). The interactive shell renders to the
framebuffer/serial, not debugcon — so harness assertions read debugcon only.

## Gotchas / notes

- **`make` does not track `CFLAGS_EXTRA` changes.** Toggling a `-D…` demo flag
  (e.g. `-DM10_RUN_DEMO=1`) does NOT recompile if the `.o` is newer than the
  source → you boot a STALE kernel with the wrong flags. Always `make clean`
  first (or `touch` the source) when changing `-D` flags. The harness does this.
- **`isodir/boot/grub/grub.cfg` is tracked** but `make clean` deletes it; restore
  with `git checkout -- isodir/boot/grub/grub.cfg` before committing (a recurring
  papercut — consider untracking it).
- **DEV signing key**: `crypto/secos_pubkey.h` is the fixed-dev-seed public key;
  `tools/secos-sign --dev` uses the matching seed in `tools/secos_signlib.py`.
  Production swaps in a real key (private half kept offline). Generated headers
  (`secos_pubkey.h`, `signed_test_elf.h`, `user_hello_elf.h`) are committed so the
  kernel build needs no python.
- **User ELFs need `-mcmodel=large`** (code at 4 GB+ → 64-bit relocations).
- **Heap allocator** (`mm/heap.c`) is a simple first-fit; its `kfree` only
  coalesces physically-contiguous blocks (a past bug). A buddy/free-list allocator
  is an M12 item.
- Talk to the user in **Italian**; all project content (code, comments, commits,
  docs) in **English only**.

## M13 recap (done)

Usability & policy enforcement (`docs/devlog/M13.md`):
- **Manifest `max_mem` enforced at load** (headline): a signed program whose
  mapped footprint exceeds its `.note.secos` `max_mem` (0 = unlimited) is refused
  by `process_create_from_elf`, with a leak-free teardown (`elf_unload_process` +
  `vmm_space_destroy`). Signature-rooted: a program can't raise its own ceiling.
- **New syscalls**: `SYS_GETTICKS` (10, uptime); `SYS_MSG_SEND` (11) /
  `SYS_MSG_RECV` (12) over kernel IPC channels (`kernel/ipc.c`: 4 ring buffers,
  non-blocking, user pointers validated + bounce-buffered). libc
  `getticks`/`msg_send`/`msg_recv`.
- **Minimal IPC**: two separately-spawned ring-3 programs talk over a
  kernel-owned channel (no fork/fd inheritance). Demo: producer sends
  `M13-IPC-OK` on channel 0 + reports uptime; consumer reads it. 56/56.
- Shell `run <path>` (M10) is the first-class on-disk launcher.

## M12 recap (core done)

Memory scalability + W^X (`docs/devlog/M12.md`):
- **PMM**: removed the 512 MB `total_frames` clamp and the 128 MB per-region
  identity clamps — the kernel manages all RAM (frames above the identity window
  are reached via the physmap; 32 MB bitmap cap ≈ 1 TB). `find_free_frame` skips
  full bitmap words from a rolling cursor; `pmm_alloc_contiguous`/`_free_contiguous`.
- **Heap**: addresses memory through the physmap (`phys_to_virt`); `expand_heap`
  allocates enough **contiguous** frames; `kmalloc` returns NULL instead of an
  undersized block. Fixes the M11 gotcha — the M11 demo loads from the VFS again.
- **W^X**: `vmm_map`/`vmm_map_in_space` reject RW-without-NX (`-5`); boot tests
  `[WX]` + `[HEAP] large kmalloc(64K) OK`. `-m 2G` → ~2045 MB free. 51/51.
- **Higher-half**: kernel linked + runs at `KERNEL_VMA=0xFFFFFFFF80000000`
  (`-mcmodel=kernel -fno-pic`). `linker.ld` keeps a low `.boot` (MB2 header +
  32-bit setup + initial tables) and links the rest HIGH/loads LOW via `AT()`.
  `boot.asm` maps low identity + high half then jumps high; the UEFI loader loads
  segments at their LMA (`p_paddr`) and maps PML4[511]. `kvirt_to_phys` fixes the
  two symbol-as-physical spots (PMM bitmap math, virtio DMA). **Low identity map
  kept** (virtio DMA + early frame access). Both `--mb2` and `--uefi` PASS.
- **Deferred** (rationale in devlog): full demand paging (`vmm_region` path is
  dormant), W^X of the 0–512 MB identity huge-page map (still RWX).

## M11 recap (done)

Driver Space is now a **signature-rooted** boundary (`docs/devlog/M11.md`):
- `.note.secos` manifest **v2** adds `proc_type`/`dev_id`/`dev_caps` (desc 24→40,
  `version` 2), covered by the Ed25519 signature → the driver claim is trusted.
- `process_create_from_elf` sets `proc_type`; a `PROC_TYPE_DRIVER` manifest
  auto-binds the process to its device with the **granted cap subset**
  (intersected with device-supported). `process_destroy` drops bindings first.
- `SYS_DRIVER` enforces `proc_type==DRIVER` (else `DRV_ERR_NOTDRV`) **and** the
  per-binding granted caps; every event mirrors to debugcon `[DRV-AUDIT]`.
- Demo (`M11_DRIVER_DEMO`): signed `user/driver_demo` does mediated reg
  read/write (value round-trips) and is refused the un-granted `MAP_MEM`; signed
  `user/userprobe` (PROC_TYPE_USER) is denied every `SYS_DRIVER`. Harness 47/47.
- **Heap gotcha**: `mm/heap.c` reliably serves `kmalloc` only ≤ one frame (~4 KB)
  and can silently return an undersized block for larger requests (ramfs copies
  files with one `kmalloc`). The demo therefore loads the embedded signed ELF
  directly (still verified). Real fix = M12 buddy/multi-frame allocator.

## M10 recap (done)

Storage & persistence is complete (`docs/devlog/M10.md`): virtio-blk (`vda`),
VFS multi-mount (`/mnt`), FAT32 + ext2 + ext4 read-write (ext4 no-journal v0),
`SYS_SPAWN`/`SYS_WAIT` + shell `run <path>` with the shell running as the
scheduler idle task. Kernel writes are host-readable and `e2fsck`-clean; a signed
ELF is written to disk, read back, verified and run in ring-3 (tampered refused).

## Running it interactively (WSLg GUI is broken → use serial)

```bash
# UEFI boot (OVMF), interactive shell over serial. Exit QEMU: Ctrl-A then X.
qemu-system-x86_64 -machine q35 -m 256M -bios /usr/share/ovmf/OVMF.fd \
  -drive file=secos-uefi.img,format=raw,if=ide \
  -serial stdio -display none -no-reboot

# Full M10 storage (BIOS/ISO path) — /mnt mounts here; try: vls /mnt, vcat /mnt/HELLO.TXT
make iso && make disk-fat32        # or disk-ext2 / disk-ext4
qemu-system-x86_64 -cdrom myos.iso -drive file=disk.img,if=virtio,format=raw \
  -boot d -serial stdio -display none -no-reboot -m 256M
```

## UEFI bootable image (for VMware / bare-ish UEFI)

- `secos-uefi.img` = a **GPT + ESP (FAT32)** disk built from `dist/` after
  `make && make uefi` (ESP root holds `/EFI/BOOT/BOOTX64.EFI` + `/kernel.elf`;
  the loader reads `kernel.elf` from the volume root). Built by hand this session
  (sgdisk + mkfs.fat + mcopy + dd; converted to `secos-uefi.vmdk` via qemu-img).
  Copied to Windows `C:\Users\Luigi\SecOS\` with a `README-VMware.txt`.
  **TODO (offered, not yet done):** add `make uefi-disk` / `make uefi-vmdk`
  targets so this is reproducible (currently ad-hoc shell steps).
- **Verified booting in QEMU+OVMF** (single disk → reaches the shell, 12/12).
- **Quirk:** with a *second* disk attached (e.g. a virtio data disk), OVMF using
  the read-only `-bios OVMF.fd` drops to the UEFI shell / fails our ESP with
  "Bad Buffer Size" (non-persistent NVRAM boot entries). A `startup.nsh` on the
  ESP didn't fix it. So UEFI+virtio-disk together is flaky under OVMF — give the
  two commands above separately, or use split OVMF_CODE/OVMF_VARS for persistence.
- **On VMware Workstation:** UEFI boot + shell will work; **storage `/mnt` will
  NOT** — VMware has no virtio-blk (offers SATA/NVMe/PVSCSI). An AHCI/NVMe driver
  would be needed (future work). The user was going to try the VMDK in VMware;
  if it didn't boot, ask for the serial log (loader prints `[OK]/[ERR]` lines).

## M14 recap (done, committed `c9b8a4d`)

Full demand paging (`docs/devlog/M14.md`): user pages are no longer eagerly
mapped. `process_create_from_elf` pins a private copy of the signature-verified
ELF image and registers **per-process VMAs** (`mm/vma.{c,h}`, in `process_t`):
FILE (lazy-copied from the image) for ELF segments, ANON (zero-fill) for
stack/BSS. `elf_load_image_lazy` validates + reserves, touches no frame;
`vmm_handle_page_fault` materializes pages on first touch via `vma_fault_in`
(frame filled through the physmap RW alias → RX code fills with no W^X
violation). Syscall buffers fault in via **kernel-mode #PF on a user address**
(the handler keys on address+VMA, not the U/S bit) — `copy_from/to_user`
dereferences directly, so no `user_copy.c` change. Teardown is automatic
(`vmm_space_destroy` frees present leaves; `mapped_pages` removed, no leak). Stack
**guard** = absence of a VMA below it. `max_mem` checked vs reserved footprint.
Demo `M14_DEMAND_DEMO`: signed `hello` shows `mapped at load=0` for a `0x9000`
reserved footprint, 2 pages fault in as it runs. Harness **61/61**. _(Historical
recap — M14 is long merged into `main`.)_

## Suggested first move next session (Phases F+G done → next)

Phases F and G are complete (M15–M20). Work on **`main`** (everything is
consolidated there; old milestone/* and debug/* branches are gone). Per
`docs/ROADMAP_TO_COMPLETE_OS.md`, the next steps:
- **Pipes + TTY** (now unblocked by fork): anonymous pipes on the `fds[]` table; a
  ring-3 TTY line discipline. Ctrl-C→SIGINT needs a signal-delivery mechanism
  (only fatal termination exists today — see M15).
- **Phase H — storage maturity**: VFS permissions/ownership/timestamps, `mount`/
  `umount` syscalls, symlinks, devfs + procfs; ext4 JBD2 journaling.
- **Phase I — modern platform**: ACPI + APIC/IOAPIC (retire the PIC), TSC/HPET
  timekeeping, then **SMP** (the single biggest correctness effort — do a locking
  audit first).
- Other open stretch: `MAP_SHARED` + shared read-only text (retire the M14
  per-process pinned image via the M20 page cache); W^X of the 0–512 MB identity
  map; real `DRIVER_OP_MAP_MEM`/IRQ-to-driver; argv populated env.
- **Driver space**: real `DRIVER_OP_MAP_MEM` (map device MMIO into the driver
  address space — still a validating stub); IRQ-to-driver (`IRQ_SUBSCRIBE` + IPC
  queue, building on M13's IPC channels); DMA sandbox; auto-restart of a crashed
  critical driver.
- **IPC/usability**: blocking `SYS_SLEEP` + channel wait (current recv is
  poll+yield); `argv`/env to spawned programs; richer pipe semantics (EOF).
- **Storage**: ext4 JBD2 journaling + `metadata_csum`; `SYS_WAIT` blocking
  semantics (currently poll-style: 0=done, 1=running).
