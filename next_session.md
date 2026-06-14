# SECoS — Resume Here (session handoff)

_Last updated: end of M11 (Driver Space for real). Read this first._

## Where we are

- **Done through M11.** HEAD = **`c84d968`** on branch **`milestone/M7`**
  (historical name; actually holds everything through M10). Tag `M9_STABLE` is the
  last tag. **M11 is staged in the working tree, NOT yet committed** (commit/push
  when the user asks).
- Build is green: `make` clean (no warnings), and the full self-test harness
  `tools/selftest.sh` is **47/47** (M4 + crypto + M7 + M8 + M9 + M10×{fat32,ext2,ext4} + M11).
- Only `edk2/` is untracked (a large vendored tree, not part of the build) — leave it.
  Local build artifacts `secos-uefi.{img,vmdk}` and `disk.img` are gitignored.

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
| **M11** | **done** | Driver Space for real: `.note.secos` v2 manifest carries `proc_type`/`dev_id`/`dev_caps` (signature-rooted); loader auto-binds a `PROC_TYPE_DRIVER` to its device with the granted cap subset; `SYS_DRIVER` enforces driver-only + per-binding caps + `[DRV-AUDIT]`. Signed user-space driver demo + user-denied probe. Harness 47/47. See `docs/devlog/M11.md` |
| M12 | **next** | higher-half + **buddy/multi-frame allocator** (kmalloc >4 KB currently unreliable — see M11 gotcha) + demand paging + UEFI hardening |
| M13 | stretch | shell-launches-programs + full manifest enforcement; real `DRIVER_OP_MAP_MEM`, IRQ-to-driver, DMA sandbox, driver restart |

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

## Suggested first move next session (M12 — memory + higher-half)

The most load-bearing M12 task is a **real allocator**: `mm/heap.c` builds the
heap one PMM frame at a time, so `kmalloc` > ~4 KB is unreliable and can silently
return an undersized block (M11 hit this — see `docs/devlog/M11.md`). Fix:
1. A **buddy / multi-frame** allocator (or at least a contiguous-frame path in
   the PMM + a `kmalloc` that returns NULL instead of an undersized block).
2. Then large files/ELFs can go through the VFS/ramfs again (M11's driver demo
   currently loads the embedded signed ELF directly to dodge the heap limit).
3. Higher-half kernel + demand paging + UEFI hardening round out M12.

Open follow-ups (not blocking M12):
- **M11 driver space**: real `DRIVER_OP_MAP_MEM` (map device MMIO into the driver
  address space — still a validating stub); IRQ-to-driver (`IRQ_SUBSCRIBE` + IPC
  queue); DMA sandbox; automatic restart of a crashed critical driver.
- **M10 storage**: ext4 JBD2 journaling + `metadata_csum`; `SYS_WAIT` blocking
  semantics (currently poll-style: 0=done, 1=running).
