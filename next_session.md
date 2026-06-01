# SECoS — Resume Here (session handoff)

_Last updated: end of M9. Read this first to pick up exactly where we left off._

## Where we are

- **Done through M9.** Latest tag **`M9_STABLE`**, HEAD commit **`ccb192e`**.
- Branches: work is on **`milestone/M7`**; **`main`** mirrors it (both at the same
  commit, pushed to `origin`). The branch name is historical — it actually holds
  everything through M9. Don't be confused by the name.
- Build is green: `make` clean, default smoke PASS, and the full self-test
  harness is **14/14**.
- Only `edk2/` is untracked (a large vendored tree, not part of the build) — leave it.

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
| **M9** | **done (`M9_STABLE`)** | crypto (SHA-256/512 + Ed25519 verify), signing format+tools+gate, userland (crt0+libc), signed `hello` runs from VFS |
| M10 | **next** | storage & persistence: virtio-blk + RW FAT32/ext2/ext4 via VFS; load programs from disk |
| M11 | planned | Driver Space for real (user-space driver; ties `proc_type` + manifest DRIVER flag to the signature trust root — already designed) |
| M12–M13 | stretch | higher-half + buddy PMM + demand paging + UEFI hardening; shell-launches-programs + manifest enforcement |

## Build / test / run

```bash
make                       # build kernel.bin (default; signing gate ENFORCED, dormant at boot)
make iso                   # myos.iso (GRUB Multiboot2)
make user-progs            # build + SIGN user/hello.c -> crypto/user_hello_elf.h (needs python3 + `cryptography`)
tools/selftest.sh          # THE gate: builds M7/M8/M9 variants, asserts from debugcon. Expect 14/14.
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

## Suggested first move next session (M10)

Storage & persistence. Concrete starting steps:
1. A **virtio-blk** driver (MMIO/PCI) exposing `block_read/block_write` (QEMU
   `-drive ...,if=virtio`); wire into `fs/block.c`.
2. Make **FAT32, ext2 and ext4** **read-write** through the VFS over the block
   device (all three are SECoS target filesystems).
3. Mount a data FS at boot; load a **signed** program from disk (the signing gate
   already verifies whatever `process_create_from_elf` is handed).
4. Add `SYS_SPAWN <path>` / `SYS_WAIT` and a shell `run <path>` (loader/VFS/sig
   plumbing is already in place from M9).
Gate idea: write a file, reboot the VM, read it back identical; run a signed
program loaded from the disk image — all harness-asserted.
