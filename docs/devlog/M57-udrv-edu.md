# M57 — the edu sample driver, the argument-validation table and the Driver Space v2 gate

Part of Phase 5 step M57 (`feat/udrv-core`, plan `docs/PHASE5_PLAN.md` §3
"M57", design D5–D10). The kernel side (`kernel/udrv.{c,h}`, ABI
`user/include/secos/udrv.h`) came first; this part adds the first ring-3
driver that uses it, the gate that runs it, and the kernel changes the gate
needed, and the SYS_UDRV argument-validation table (`user/udrv_negtest.c`).
The hostile-driver modes of the plan (`udrv_evil`: DMA/MSI injection faults,
MSI-X table hole, DONTFORK touch, IRQ flood, kill during DMA) are not in this
step.

## `/bin/udrv_edu` (`user/udrv_edu.c`)

A driver for QEMU's `edu` educational device (`1234:11e8`,
QEMU `docs/specs/edu.rst`), in the role of a Linux UIO/VFIO sample driver.
Signed with the v3 DRIVER manifest `user/note_udrv.S` (confined:
`CAP_ENFORCE|FS_READ|PROC|TIME`, so `no_new_privs` too).

1. `CLAIM` by vendor:device with `dma_mask = 0x0FFFFFFF` (edu's default 28-bit
   mask; edu silently clamps a wider address) and `INFO`, printed.
2. `MAP_BAR 0` (1 MiB, UC): identification register `0x010000ed`, 64 liveness
   values (`0x04` reads back inverted), factorials 0!..12! (`0x08`, polling
   status `0x20` bit 0).
3. `CFG_WRITE` command = MEM|BUSMASTER (QEMU sends an MSI as a bus-master
   write, so it needs BM), `IRQ_SETUP 1` → MSI (edu has one MSI vector, no
   per-vector mask: the kernel masks with the MSI enable bit). The vector
   starts masked; the first ack on the fd arms it. Then N rounds of: write
   `0x60`, block in `read()` of the claim fd, check `irq_count` advanced by
   exactly one, `vec_pending_mask` bit 0 and the interrupt status `0x24`,
   ack the device (`0x64`) and write `struct udrv_ack` to the fd. One more
   interrupt through `poll()` (POLLIN), and `INFO.irq_count` must match.
4. `DMA_ALLOC` 2 pages (zeroed, checked). edu DMA 2 KiB from the buffer to
   edu's buffer (device address `0x40000`), then from there into the second
   page, each completed by interrupt (status `0x100`, RUN bit clear). The
   bytes must match and the rest of the page stay zero. Copies are 2 KiB
   because QEMU's `edu_check_range` treats the end of the 4 KiB buffer as
   exclusive and aborts QEMU on a DMA that ends exactly there.
5. `DMA_FREE` (and `INFO.dma_used == 0`), `RELEASE`; the fd then reads
   `-ENODEV`.

Without DMA rights (no IOMMU) it checks the MMIO-only contract instead:
`DMA_ALLOC` -ENODEV, bus mastering and `IRQ_SETUP` -EPERM, BAR works.
`--hold` takes a second claim with everything (BAR, BM, armed vector,
16 KiB DMA) and exits without `RELEASE`; `--serve` does the same and then
sleeps in `read()` until killed. Output: `[udrv_edu] <check> ok|FAIL`,
`RESULT pass=N fail=M`, `PASS|FAIL pid=N` on stdout (mirrored to debugcon when
stdout is the console; as an init service it goes to `/var/log/<id>.log`,
because a confined program cannot open `/dev/kmsg` for writing).

Installed with `udrv_negtest` and `udrv_user` by `kernel/sysroot.c`.

## Kernel changes

- **EFAULT after the op acted** (`udrv: fail an op with EFAULT before it
  acts, not after`). Result structs were copied back only after the op ran:
  a struct readable but not writable passed `copy_from_user`, then `MAP_BAR`
  mapped the BAR / `DMA_ALLOC` allocated and IOMMU-mapped frames /
  `IRQ_SETUP` armed vectors, and the call returned -EFAULT. The driver never
  learnt the addresses, the resources stayed charged to the claim, and a
  retry got -EBUSY. The syscall now probes the write-back first (writes the
  same bytes; one thread per process, nothing changes them in between).
- **Live counters** (`udrv, vtd: live page counters`): `/proc/udrv` line 1
  gains `live_dma_pages=` and `live_bar_pages=`, `/proc/iommu` a
  `tables live N` line (all VT-d table pages). The leak checks need exact
  numbers: MemFree+Slab drifts by ~50 kB per program run, and a control run
  of `uaccess_test` instead of `udrv_edu` drifted just as much, so a frame
  leak of one claim (8–20 KiB) is invisible in MemFree.

Nothing in the kernel side broke under the positive tests: 10 000 MSI round
trips, DMA through the translated domain, exit-path teardown of a held claim,
`kill -9` of a driver blocked in `read()` and its respawn, lockdep at -smp 4
and `bkl=on` all passed on the first try.

## Gate (`tools/udrv_gate.sh` + `tools/udrv_check.py`)

Replaces the M11 section of `tools/selftest.sh` (its demo programs were
removed in `109efc5`). Every guest is q35, MB2 ISO with `-boot d`,
`-device edu,addr=04.0`, CONSOLE=root.

| guest | image cmdline | machine | checks |
|---|---|---|---|
| run | `iommu=on udrv=00:04.0 udrv.noir` | intel-iommu, -smp 2 | udrv_edu 10 000 rounds PASS; `[UDRV]` claim (translated, dma=yes, ir=no(udrv.noir)), irq_setup msi, release lines; 8 more runs, every other `--hold`; counters exact, MemFree+Slab / SUnreclaim within 1 MiB / 2 MiB; `/proc/udrv` device free, claims counted, faults 0, nothing pending/masked; `/proc/iommu` device blocked, 0 faults |
| service | same | + fresh ext4 root on vda (`tools/mkroot.sh`, `/etc/inittab` written with `debugfs`) | `edu:respawn:/bin/udrv_edu -n 1000 --serve` passes and holds the claim; 3× `kill -9` of the pid from `/proc/udrv`: `[UDRV] release pid=<old> (exit)`, init restarts it, the new pid passes and serves; counters after kill 3 equal those after kill 2; only SIGKILL `[KILL]` lines |
| nommu | same | plain q35 | assigned "MMIO only", claim `iommu=none dma=no`, id/liveness/factorial ok, DMA_ALLOC -19, BM and IRQ_SETUP -1, refusals logged |
| default | (none) | intel-iommu | no `[UDRV]` line, `/proc/udrv` "udrv devices 0", edu listed in `/proc/bus/pci/devices` |
| noir | `iommu=on udrv=00:04.0` | intel-iommu (no interrupt remapping before M56) | claim `iommu=translated dma=no`; udrv_edu MMIO-only path passes; BM and DMA_ALLOC refused "no interrupt remapping" |
| unsafe | `udrv=00:04.0 udrv.unsafe` | plain q35 | boot warning; udrv_edu 100 rounds + DMA pass with iova = phys; claim `TAINTED(unsafe)`, taint line in the log and in `/proc/udrv` |
| lockdep-smp4 | same + `-DSECOS_LOCKDEP` | intel-iommu, -smp 4 | as run (2000 rounds, 4 leak runs), `[LDTEST] all 0xA/0xA`, no lockdep report |
| bkl-on | same + `bkl=on` | intel-iommu | as run (4 leak runs), `[BKL] kernel lock mode: bkl=on` |

Every guest also runs the argument-validation table (next section) for its
environment, and fails on `[EXC]`, panic, lockdep, nested fault, stack
smashing, `[SMAP] violation`, TLB/shootdown/invalidation timeouts.

## Argument-validation table (`user/udrv_negtest.c`)

A conformance table: each row is one call with one invalid (or, for the
stateful rows, valid) argument and its expected raw return value (0 or
-errno). `/bin/udrv_negtest --env E` carries the v3 DRIVER manifest;
`/bin/udrv_user` is the same source with a plain manifest. Output: one
`[udrv_negtest] <id> <what>: PASS|FAIL|SKIP rc=... want=...` line per row,
`RESULT rows= pass= fail= skip=`, `PASS|FAIL pid=`.

Expectation columns: claim with DMA rights (`translated`, `unsafe`), MMIO-only
claim (`noir`, `nommu`), no `udrv=` device (`nodev`), non-driver manifest
(`user`).

- **Table A** (30 rows, every environment, no claim): op 0 / 11 / 2^32+1 / -1;
  CLAIM size ±1, 0, ~0; CLAIM struct at NULL, an unmapped user page, the
  kernel half, a non-canonical address, straddling the top of user space;
  unknown claim flags; BDFs/ids not in `udrv=` (incl. one the manifest
  matches); euid 65534 (real/saved uid 0, restored afterwards); a fork child
  (USER after fork); INFO/MAP_BAR/DMA_ALLOC/RELEASE on stdin, fd 31, 32, -1,
  2^40; RESET with a size.
- **Table B** (88 rows, on a claim of edu, in order): second claim by id and
  by BDF (-EBUSY); INFO size/NULL/kernel/read-only result; MAP_BAR bar 6,
  0xffffffff, absent BARs, flags; MAP_BAR into a read-only struct (-EFAULT)
  followed by MAP_BAR 0 succeeding (nothing was left mapped) and a third one
  (-EBUSY); CFG_READ widths 0/3/8, offset 0x1000, unaligned; CFG_WRITE header,
  BAR 0/5, ROM, interrupt line, inside the MSI capability (found by walking
  the list), 0x100, 0xffc, I/O-space bit, bus mastering (0 / -EPERM), status
  bytes (0, ignored); DMA_ALLOC size 0, 4 MiB+4 KiB, 2^64-4096 (round-up
  overflow), 2^64-1, dir 0/4, flags, 128 KiB over `dma_max`, read-only result,
  then 64 KiB (all of `dma_max`) and 4 KiB more (-ENOMEM); DMA_FREE of 0, of a
  page inside the buffer, size+1, double free; munmap/mprotect of a DMA page
  and of a BAR page (-EINVAL); mmap of the claim fd (-ENODEV); IRQ_SETUP nvec
  0/5/33/0xffffffff/2, read-only result, then 1 and again (-EBUSY); fd
  read/write short lengths, NULL/unmapped/kernel buffers, O_NONBLOCK with
  nothing pending (-EAGAIN), a pending interrupt read into a read-only page
  (-EFAULT); RESET (-EOPNOTSUPP on edu); INFO/read/write/RELEASE from a fork
  child (-EPERM); RELEASE size 4, RELEASE, every op after it (-ENODEV), INFO on
  the closed fd (-EBADF). Afterwards a fresh claim + RELEASE must succeed.
- MMIO-only environments: every DMA_ALLOC row is -ENODEV, IRQ_SETUP -EPERM,
  bus mastering -EPERM; the rows that need a DMA buffer are SKIP (3 rows).

The checker also requires the `refused(not-root)`, `refused(not-a-driver)` and
`refused(busy)` log lines, the device free afterwards with no live DMA/BAR
pages, and in the `run` guests unchanged resources (vectors, domains, VT-d
table pages, PMM/heap within slack) and no IOMMU fault.

Found while writing it: `read()`/`write()` of length 0 return -EFAULT from the
generic `SYS_READ`/`SYS_WRITE` entry (`kernel/syscall.c`, the M3 rule
`len <= 0 || !user_range_valid` → -EFAULT, for every fd) before the claim fd
is looked at, so the udrv "len < 16 → -EINVAL" path only sees 1..15. Linux
returns 0 for a zero-length read; that is a pre-existing difference of every
fd, not changed here. The same entry rejects kernel-half buffers, so the
table also uses an unmapped user page and a read-only page to reach the
claim fd's own copy paths.

## Error codes

Error codes as implemented (kernel `-errno`, Linux numbers; libc `read`/`write`
map them to -1/errno, so use raw `secos_syscall` for the claim fd):

- **dispatcher**: unknown op, or `size != sizeof(struct)` (RESET/RELEASE: 0)
  → -EINVAL; unreadable struct → -EFAULT; unwritable result struct →
  -EFAULT before acting; no `udrv=` device at all → CLAIM -ENODEV.
- **handle** (every op but CLAIM): not a claim fd / fd ≥ 32 → -EBADF; fd of
  an ended claim → -ENODEV; caller not the owner (fork child) → -EPERM; claim
  hit by a DMA fault → -EIO for all ops except INFO and RELEASE.
- **CLAIM**: unknown flag → -EINVAL; not `PROC_TYPE_DRIVER` → -EPERM; euid ≠ 0
  → -EPERM; manifest not v3 or no match entries → -EPERM; no such assigned
  function → -ENODEV; BY_ID with every match claimed, or BDF already claimed
  → -EBUSY; manifest match fails → -EACCES; quarantined → -EIO; domain
  allocation → -ENOMEM; attach → -EIO; fd pool/table full → -EMFILE.
  (The proc_type/euid/manifest checks run after the device lookup: the
  refusal line names the device when found.)
- **MAP_BAR**: flags ≠ 0, bar ≥ 6 or ≥ nbars, BAR absent or an upper half,
  size < 4 KiB / unaligned / > 256 MiB → -EINVAL; not in `bar_mask` or an I/O
  BAR → -EPERM; mapped already → -EBUSY; map slots → -ENOSPC; VA/map → -ENOMEM.
- **CFG_READ**: width not 1/2/4, off > 0xFFF or unaligned → -EINVAL.
- **CFG_WRITE**: same -EINVAL; command register without `UDRV_CFGW_COMMAND`,
  I/O space bit, BUSMASTER without DMA rights → -EPERM; status bytes
  0x06/0x07 → **0, nothing written**; faulted → -EIO; 0x40..0xFF without
  `UDRV_CFGW_DEVSPEC` or inside a capability → -EPERM; anything else (header,
  BARs 0x10–0x27, ≥ 0x100) → -EPERM.
- **DMA_ALLOC**: no DMA rights → -ENODEV; faulted → -EIO; flags ≠ 0, dir not
  1..3, size 0 or > 4 MiB → -EINVAL; over `dma_max` or `max_mem` → -ENOMEM;
  32 buffers → -ENOSPC; frames / IOVA / VA → -ENOMEM.
- **DMA_FREE**: uva not the start of a live buffer → -EINVAL; IOMMU unmap
  failure → -EIO (buffer deliberately leaked, counter shows it).
- **IRQ_SETUP**: set up already → -EBUSY; no DMA rights → -EPERM; nvec 0, >
  `irq_max`, > 32, > MSI-X table, MSI not a power of two or > 1 → -EINVAL;
  vector allocation → -ENOSPC; enable / no CPU → -EIO; no MSI/MSI-X →
  -EOPNOTSUPP.
- **RESET**: no FLR / AF FLR / PM reset → -EOPNOTSUPP (edu has none: always).
- **fd read**: len < 16 → -EINVAL; ended → -ENODEV; not owner → -EPERM;
  O_NONBLOCK and nothing new → -EAGAIN; signal → -EINTR; bad buffer → -EFAULT.
- **fd write**: len < 4 → -EINVAL; bad buffer → -EFAULT; ended → -ENODEV; not
  owner → -EPERM; faulted → -EIO; else returns len (bits beyond nvec ignored).
- **fd poll**: POLLOUT always, POLLIN|POLLRDNORM when read would not block,
  POLLHUP|POLLERR after the claim ended. `mmap` of the fd → -ENODEV.

Also worth knowing: closing the claim fd does **not** end the claim (it ends
at RELEASE, exit or exec); edu has no reset, so `release` logs `reset=none`.

## Test record (2026-11-09, HEAD of `feat/udrv-core`)

- `tools/udrv_gate.sh` (QEMU 6.2): **8 passed, 0 failed** — run, service,
  nommu, default, noir, unsafe, lockdep-smp4, bkl-on. Table results: 118 rows
  (30 + 88) all PASS in translated / unsafe (run, lockdep-smp4, bkl-on,
  unsafe); 115 PASS + 3 SKIP in noir / nommu; 30/30 in nodev (default guest,
  both binaries) and for `udrv_user` everywhere.
- `tools/smoke.sh --mb2` and `--uefi`, normal and `-DSECOS_LOCKDEP` builds:
  all PASS, no `[EXC]`, `[LDTEST] all 0xA/0xA` on the lockdep build.
- `-Wall -Wextra` warnings: 0 in every gate image (normal, lockdep, bkl=on,
  four cmdlines) and in both smoke builds, as on the base (`cba08ac`: 0 normal,
  0 lockdep, 0 UEFI loader).

The first rerun after the host reboot failed only on rows of the new table
whose expectation was wrong (zero-length read/write, see above); every other
check of every guest passed on that run.
