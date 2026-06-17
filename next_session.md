# SECoS — Resume Here (session handoff)

_Last updated: M15 (fault-driven process kill, Phase F start). Read this first._

## Where we are

- **Done through M15** — M0–M13 (phases A–E) + **M14 (demand paging)** + **M15
  (fault-kill, first of Phase F)**. Branch **`milestone/M7`** (historical name).
  **HEAD ≈ M15 commit, committed but NOT yet pushed** to `origin/milestone/M7`
  (the M14 commit `19e10f5` + tag `M14_STABLE` are already pushed). Tag
  `M14_STABLE` is the last pushed tag.
- **Long-term roadmap to a complete headless OS:
  `docs/ROADMAP_TO_COMPLETE_OS.md`** — Phases F–L (M15–M32): robustness/process
  model, VM completeness, storage maturity, APIC+SMP+real drivers, networking,
  userland, security hardening. Next: **M16** (argv/env + exec + blocking wait),
  **M17** (blocking primitives + pipes + TTY).
- Build is green: `make` clean (0 warnings), the full self-test harness
  `tools/selftest.sh` is **66/66** (+5 M14, +5 M15), both boot paths run
  higher-half at `0xFFFFFFFF80000000` (`smoke.sh --mb2` and `--uefi` PASS).
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
| **M15** | **done (Phase F)** | **fault-driven process kill**: a ring-3 CPU fault kills only the offending process and returns to the scheduler instead of halting. `vmm_handle_page_fault`→int (no halt); `exception_handler` decides from saved `cs` (ring3⇒`sched_kill_current`, ring0⇒panic); kill reuses the SYS_EXIT path (ZOMBIE+`pick_user`+`switch_to`, reaped later, no leak). `process_t.exit_code` (128+vec). Demo `M15_KILL_DEMO`: signed `crashtest` NULL-writes→killed→`hello` still runs. Harness **66/66**. See `docs/devlog/M15.md` |
| next | **Phase F cont.** | **M16**: argv/env/auxv + `SYS_EXEC`/spawn + blocking `SYS_WAIT` carrying exit status. **M17**: wait queues + futex-like + blocking `SYS_SLEEP`/recv + anonymous pipes + TTY line discipline (Ctrl-C→SIGINT). Then Phase G (mmap/COW/page cache). Full plan: `docs/ROADMAP_TO_COMPLETE_OS.md` |

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
reserved footprint, 2 pages fault in as it runs. Harness **61/61**. **Still to
do:** `git push origin milestone/M7`, maybe tag `M14_STABLE`.

## Suggested first move next session (M0–M15 done → Phase F)

Mandatory plan (A–E) complete; M14 (demand paging) + M15 (fault-kill) done.
**Push first** if the M15 commit isn't on origin yet (`git push origin
milestone/M7`). Then continue Phase F per `docs/ROADMAP_TO_COMPLETE_OS.md`:
- **M16 — exec model**: pass `argv`/`env`/`auxv` to ring-3 programs (stack setup
  at exec), `SYS_EXEC`/`posix_spawn`, blocking `SYS_WAIT` returning the
  `exit_code` M15 already records (incl. the `128+vec` killed encoding).
- **M17 — blocking + pipes + TTY**: wait queues + a futex-like primitive; make
  `SYS_WAIT`/`SYS_MSG_RECV` blocking; `SYS_SLEEP`; anonymous pipes; a TTY line
  discipline so the shell does line editing + Ctrl-C→SIGINT.
- Later stretch still open: COW/page-out, shared-text page cache, W^X of the
  0–512 MB identity map, dropping the low identity map.
- **Driver space**: real `DRIVER_OP_MAP_MEM` (map device MMIO into the driver
  address space — still a validating stub); IRQ-to-driver (`IRQ_SUBSCRIBE` + IPC
  queue, building on M13's IPC channels); DMA sandbox; auto-restart of a crashed
  critical driver.
- **IPC/usability**: blocking `SYS_SLEEP` + channel wait (current recv is
  poll+yield); `argv`/env to spawned programs; richer pipe semantics (EOF).
- **Storage**: ext4 JBD2 journaling + `metadata_csum`; `SYS_WAIT` blocking
  semantics (currently poll-style: 0=done, 1=running).
