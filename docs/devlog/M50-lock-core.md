# M50 — Lock core: klock, lockdep, the BKL on klock, `bkl=on|off`

Phase A of the BKL removal (`docs/BKL_REMOVAL_PLAN.md`, A1 `feat/lock-core`
with A2 folded in). Nothing is unlocked yet: the default boot behaves exactly
as before (one recursive kernel lock around every syscall body, user fault,
teardown and kernel-shell command). What changed is the machinery under it, so
that phases B–D can give each subsystem its own lock and have every mistake
reported on the first run instead of as a rare SMP deadlock.

## The klock (`kernel/klock.{c,h}`)

The BKL made generic. A klock belongs to a **task** (the CPU's current
process, or the CPU itself before its scheduler runs), spins with interrupts
**on** whenever the caller had them on (pause + `tlb_poll`, so shootdowns keep
working), and returns with the caller's RFLAGS.IF **exactly** as it was.
It never sleeps: ring 0 is not preemptible, disk I/O is polled and a blocked
syscall rewinds and re-runs, so no critical section has to block.

```c
static klock_t g_ext2_lock = KLOCK_INIT(LK_K30_FS, "ext2");   /* level from lock_order.h */

void klock_lock(klock_t*);          /* spin with the caller's IF            */
void klock_lock_irq_on(klock_t*);   /* spin with IF on (entry paths only)    */
int  klock_trylock(klock_t*);       /* 1 = taken                             */
void klock_unlock(klock_t*);
int  klock_held(const klock_t*);    /* by the calling task                   */
uint32_t klock_depth(const struct process*);   /* klocks held (NULL = me)   */
KLOCK_ASSERT_HELD(&g_ext2_lock);    /* lockdep builds; compiles away otherwise */
```

- **Ownership bookkeeping.** `process_t.klk` / `cpu_t.klk` (`klock_task_t`):
  the number of klocks held and their stack. Every ownership change happens
  with interrupts off on the local CPU, so the scheduler tick never sees a
  lock that is owned but not yet counted. Fork and process creation reset it
  (the child of a syscall must not inherit the parent's BKL hold).
- **No preemption while holding.** The tick and the reschedule IPI no longer
  ask "does the idle task hold the BKL?" but `cur->klk.depth > 0`
  (`kernel/sched.c`), so any klock protects the idle task from preemption.
- **IF fix.** The M48 `bkl_acquire` did `sti`, spun, then `cli`, and returned
  with IF=0 after a contended wait even when called from a syscall (trap gate,
  IF=1). Now an uncontended and a contended acquisition both return with the
  caller's IF; a syscall body therefore always runs with interrupts on.
- **Release-build checks** (one-shot `[LOCK] BUG: <what> (<lock>)` on
  debugcon, no panic): klock in IRQ context, a non-recursive klock taken twice,
  unlock by a non-owner, a klock held across `switch_to`, a klock held on the
  way back to ring 3 after a syscall. Costs: one `this_cpu()` per lock/unlock
  (the BKL already paid it), an `rdtsc` per acquisition, nothing per spinlock.
- **Interrupt nesting.** `cpu_t.irq_depth`, maintained by `irq_enter`/
  `irq_exit` in every hardware-interrupt stub (`arch/x86/idt_asm.asm`: timer,
  PS/2 keyboard and mouse, xHCI, NVMe, NIC, TLB and reschedule IPIs).
  `switch_to` resets it: a preemptive switch from the tick never returns
  through `irq_exit`.
- `cpu_wait_irq()` = `sti; hlt` with the lockdep checks. It replaces every raw
  `sti; hlt` / bare `hlt` wait loop of the kernel: `tty.c` (ESC wait, console
  spin), `shell.c` (`sh_wait_tick`), `keyboard.c` (`raw_wait`, ESC sequence),
  `timer.c` (`timer_sleep`), `smp.c` (AP idle loop, TLB selftest),
  `net/{net_core,dhcp,dns,tcp}.c` and the boot demos in `kernel.c` (42 sites).
  Halt-forever loops (`cli; hlt`, panic) stay raw.

## The BKL on klock (`kernel/bkl.{c,h}`)

`g_bkl = KLOCK_INIT_RECURSIVE(LK_K0_BKL, "bkl")`, the only recursive klock.
Same API and semantics as M48: `bkl_lock`, `bkl_lock_enable_irq`,
`bkl_unlock`, `bkl_suspend`/`bkl_resume` (the kernel shell's waits),
`bkl_drop_for` (`switch_to` drops it for a task that blocks, exits, is killed,
stops or yields), `bkl_held_by`. It is the only klock allowed across
`cpu_wait_irq()` and `copy_to/from_user` (lockdep enforces "only the BKL").

## `bkl=on|off` and `syscall_bkl_mask[]` (A2)

- `g_bkl_mode` (default 1). Boot word `bkl=off` / `bkl=on` on the Multiboot2
  command line, or in the **UEFI load options**: `uefi/boot.c` now copies the
  loader's load options (ASCII, ≤127 chars) into `secos_boot_info.cmdline`
  (flag bit 2), and the kernel uses it as its command line (so `nosmp` works
  on UEFI too). Verified with OVMF + `startup.nsh`: `\SECOS.EFI bkl=off
  nokaslr` → `[KASLR] off` and `[BKL] WARNING: bkl=off ...`.
- GRUB menu entry 3 "SecOS x64 (kernel lock: bkl=on)" (after the `nosmp`
  entry, whose index the M48 gate uses).
- `syscall_bkl_mask[4]` (256 syscall numbers, bit set = the syscall takes the
  BKL). **All bits are set**: `bkl=off` prints a loud warning and still
  serializes every syscall; phase D clears bits one subsystem at a time. Out
  of range numbers always take it. The fault, teardown and kernel-shell call
  sites take the BKL in both modes until phase D removes them.
- `syscall_handler` = `bkl_syscall_enter(num)` … `bkl_syscall_exit(taken)`; the
  exit unlocks and checks in one step that the task holds no klock any more.
- `/proc/smp` gains a last line `bkl on|off mask N/256 [lockdep on]`.

## Lock levels (`kernel/lock_order.h`)

The plan's §3.2 table as numbers (gaps left for later locks). Two deviations
from the plan table, both because the code already nests these locks:

| Plan | Here | Why |
|---|---|---|
| `tlb_lock` at S60 with `kstack_lock`, `g_pm_lock` | `LK_S62_TLB` | `vmm_selftest_remap` (and `TLB_KSTACK_EAGER`) shoot down under `kstack_lock` |
| kclock, csprng at S60 | `LK_S64_LEAF` | leaf locks; keeps S60 for page-table locks |
| `heap_lock → pmm_lock` both "S70" | `LK_S70_HEAP` < `LK_S75_PMM` | `expand_heap` calls the PMM under `heap_lock` |
| `block_io_lock` (missing) | `LK_S42_BLOCK_IO` | was taken under `pc_lock` until B2 (which removed that nesting); deleted by B3 (see below), so the level no longer exists |

Every existing spinlock is classified with `SPINLOCK_INIT_CLASS(level,
"name")`: tlb, proc, pty, heap, pipe, kclock, pagecache, kstack, physmap,
block-io, procfs, kbd, sysfs, input, mount, csprng, ipc, pmm, and the M47 net
locks (net-tick, nic-rx, net-stack, nic-tx = `LK_N0..N3`). The raw output
locks — `con_lock` (`drivers/vt.c`, recursive per CPU, bounded) and the klog /
debugcon line locks (`kernel/klog.c`, bounded) — are S50 by position but are
deliberately not tracked: lockdep reports through them.

## Lockdep (`kernel/lockdep.{c,h}`, `-DSECOS_LOCKDEP`)

With the define, `spinlock_t` grows a class, owner and counters and
`spin_lock/trylock/unlock` go out of line through `lockdep_spin_*`; without it
`spinlock_t` is one word and the operations stay the inline xchg loop (zero
overhead). `-DSECOS_LOCKDEP_LITE` (M47, net only) is now an alias; the NETSMP
selftest gate builds with `-DSECOS_LOCKDEP`.

- Per CPU: a stack of held spinlocks (with IF at acquisition and the interrupt
  frame). Per task: the klock stack.
- **Order** (levels strictly increasing; two different locks of one level
  never nest) and **recursion**, for spinlocks and klocks. Spinlock order is
  checked within one interrupt frame (an IRQ handler starts a new one);
  recursion across all frames.
- **IRQ safety:** a spinlock taken both with IF on (outside IRQ context) and
  from IRQ context is reported (it can deadlock its own CPU).
- **IF re-enabled** while holding a spinlock taken with interrupts off.
- **klock** taken under a spinlock, in IRQ context, or out of order.
- **Held across:** syscall exit, every return to ring 3 (`signal_dispatch`,
  the block re-run path), `switch_to`, `cpu_wait_irq` (BKL excepted), an IRQ
  handler's return, and `copy_to/from_user`/`strncpy_from_user` (spinlocks,
  and klocks other than the BKL).
- `SPIN_ASSERT_HELD(l)` / `KLOCK_ASSERT_HELD(l)`.
- **Watchdog:** a wait longer than 2 s (TSC) prints `[LOCKDEP] stuck on
  <spinlock|klock> <name> owner=<cpu|pid> N held_ms=… mine:` + the waiter's
  held locks, once per wait.
- **Panic dump:** `panic_enter` prints `[LOCK] held by pid N: …` (klocks, every
  build) and the CPU's spinlocks (lockdep).
- **Heap poisoning:** `kfree` fills freed memory with `0x6B` (the M12
  `SECOS_HEAP_POISON` machinery: poison verified when the block is reused).
- **`/proc/lockstat`:** acquisitions, contended acquisitions and the longest
  wait (µs) per lock; klocks in every build, spinlocks with lockdep, plus
  `lockdep reports N`.
- A report prints `[LOCKDEP] <what>: <lock> [vs <lock>] caller=<link addr>`
  and the held locks, then panics; boot word `lockdep=warn` prints each
  distinct report once and continues.
- **Boot negative controls** (`lockdep_selftest`, right after `idt_init`):
  ten cases on private test locks — ordered nesting is silent, spinlock order
  inversion, trylock recursion, klock under a spinlock, klock order inversion,
  spinlock held at return to ring 3, non-BKL klock across a user copy, both
  ASSERT_HELD macros, IF re-enabled under an irqsave lock — each must be
  caught exactly once: `[LDTEST] all 0xA/0xA negative controls`. (The prefix
  deliberately avoids the string `LOCKDEP`, which the gates grep for.)

## What lockdep found in existing code

Run on the BKL-on system (SMP stress -smp 4 and 8, network stress, GUI
compositor and GUI input gates):

- **`pty_ioctl` copied termios / winsize / FIONREAD to and from user memory
  under `g_pty_lock`** (irqsave): `tty_ldisc_ioctl` did the copies, and a
  fault there would demand-page (possibly from disk) with interrupts off on
  the CPU holding the pty lock. Fixed: `tty_ldisc_ioctl_payload()` gives the
  size and direction of each line-discipline request, `pty_ioctl` copies the
  payload in before the lock and out after it, and the line discipline works
  on the kernel buffer (`tty_ldisc_ioctl_kbuf`). A bad pointer still returns
  -EFAULT at the same point as before. (Caught by the GUI compositor gate:
  terminals run bash on ptys.)

Everything else stayed silent.

## Verification

- `-Wall -Wextra`: the warnings of a clean build are identical to `main`'s
  (41 pre-existing) in both the normal and the `-DSECOS_LOCKDEP` build.
- Every commit of the branch builds, normal and `-DSECOS_LOCKDEP`.
- Lockdep build (`make lockdep-check` = `tools/lockdep_check.sh`, 15/15):
  SMP stress -smp 4 and -smp 8 (17/17 each), network stress -smp 4 e1000e
  MSI-X (24/24), the same image booted `bkl=off` through the SMP stress
  `--quick` (12/12), boot negative controls 10/10. Also on lockdep builds:
  the GUI compositor (`tools/gui_wm_drive.py`, 57/57) and GUI input
  (`tools/gui_drive.py`) drivers, UEFI/OVMF boot with and without load
  options. No `[LOCKDEP]`, `[LOCK] BUG`, panic or unexpected `[EXC]`.
- Full `tools/selftest.sh` on the normal build: **1153 passed, 0 failed**
  (same as the d062882 baseline; the M48 x2APIC check is skipped as usual).
  Its NETSMP gate now runs the full lockdep and stays silent too.

## For phases B–D

- Declare: `static klock_t x = KLOCK_INIT(LK_K30_FS, "name");` or
  `klock_init(&x, level, "name", 0)`; spinlocks:
  `static spinlock_t y = SPINLOCK_INIT_CLASS(LK_S35_MM, "mm");` or
  `spin_init_class(&p->mm_lock, LK_S35_MM, "mm")` for one embedded in an
  object (instances of one class share the level; nesting two of them is
  reported).
- Internal helpers that expect a lock: `KLOCK_ASSERT_HELD(&x)` /
  `SPIN_ASSERT_HELD(&y)`.
- Never a user copy under a spinlock or a non-BKL klock: bounce buffers.
- Clearing a syscall from the BKL: clear its bit in `syscall_bkl_mask`
  (`kernel/bkl.c`) — the mask only matters with `bkl=off`.
- The host fuzz harnesses (`tools/{exfat,ntfs}_fuzz`) compile the drivers
  unchanged against `stub/`: an FS that starts using `klock.h` needs a stub
  `klock.h` there.
- Run `make lockdep-check` (≈25 min) on every phase B/C/D branch.

## B2 — page-cache fill without `pc_lock` (`feat/pagecache-fill`)

Plan §4 Phase B2. Before: `pagecache_get_phys_nolock` called
`ino->ops->read` with the irqsave `pc_lock` held — every cache miss ran the
filesystem and the (polled) block driver with interrupts off on that CPU and
put `pc_lock` *above* every FS and block lock, which would deadlock as soon as
the FS locks of phase C exist (a write path takes K30 and then invalidates
under S40). `pagecache_get_phys` also handed its caller a cache frame that
another CPU could evict and free while the caller (the file-mmap fault) was
still copying it.

### Design (`mm/pagecache.{c,h}`)
- **Slot states** `FREE` / `FILLING` / `VALID`, plus a per-slot `seq`
  (bumped on every state change), `owner` and `stale` bit. `pc_lock` guards
  only the index, the FIFO hand and the counters.
- **Miss:** reserve a `FILLING` slot for (inode, offset) owned by the calling
  task (the CPU before its scheduler runs), drop the lock, allocate a private
  frame, fill it through `ops->read`, copy the caller's bytes out of it, re-lock
  and publish (`VALID`) — unless the slot went stale, in which case the frame
  is freed and the slot released. A failed `ops->read` is returned as an error
  and never cached (it used to be cached as a zero page).
- **Invalidation** (`pagecache_invalidate`, `pagecache_invalidate_fs`): frees
  overlapping `VALID` pages (frames freed after unlocking, in batches of 16)
  and sets `stale` on overlapping `FILLING` slots. Every write, O_TRUNC,
  ftruncate, rename, unlink and inode-slot reuse already invalidates *after*
  the change, so: a fill reserved after the invalidation reads the new bytes;
  a fill reserved before it is doomed and not published; a page published
  before it is dropped by it. No stale page can survive a completed write.
  The plan asked for a per-inode generation; it is kept per in-flight fill
  instead, which is the same check with no field in `vfs_inode_t` (shared
  with C1) and range-precise: a fill only exists as a `FILLING` slot, so the
  slot is the one place an invalidation must reach, and a write to page 7
  does not throw away a concurrent fill of page 0.
- **Eviction** skips `FILLING` slots (the frame belongs to the filler). With
  all 128 slots filling the miss reads directly, uncached.
- **Second misser** for a page being filled: drops the lock and spins on the
  slot's `seq` (caller's IF, `cpu_relax_poll` = pause + `tlb_poll`), then
  looks again. The wait is bounded (`PC_WAIT_US` = 20 ms total per call):
  past it the misser reads the page itself without caching. A task that misses
  *its own* fill (re-entry) reads directly at once. So no wait can deadlock,
  whatever the filler does (an IF-off waiter on the BSP stops the tick, but
  the filler needs neither the tick nor that CPU, and the bound covers the
  rest).
- **No frame leaves the cache:** bytes are copied out under `pc_lock`
  (`pagecache_read` page by page, new `pagecache_copy_page` for one page).
  `pagecache_get_phys` is gone; its only caller, the M20 file-backed mmap
  fault (`mm/vma.c` `vma_fault_in`), now calls `pagecache_copy_page` into the
  new private frame. The fault path and `read()` still take their bytes from
  the same cache pages, so M20 coherence is unchanged.
- `pagecache_get_stats()` — hits, misses, published, discarded, waits,
  wait_timeouts, direct, evictions, invalidations, doomed, and the current
  valid/filling census (used by the self-test).

### Lock order after B2
`pc_lock` (S40) has only the PMM (S75) inside it and never an FS/block call,
so it nests *inside* the FS (K30) and block (K40) locks — the plan's
"write: K30 → K40, invalidate (S40) inside K30" is now legal. `block_io_lock`
no longer nests inside S40 (B2 did not move it; B3 deleted it and the
`LK_S42_BLOCK_IO` level in favour of the per-disk K40 `blkdev->lock`, see
below). `kernel/lock_order.h` says so.

### Tests
- **Forced interleavings** (`mm/pagecache_test.c`, `-DSECOS_PAGECACHE_TEST`,
  23 checks, `[PCTEST] DONE ok=0x17 fail=0`): `pagecache_test_fill_hook` runs
  between a fill's FS read and its publication. On the BSP: a write +
  invalidate (fill discarded, next read sees the new bytes), the negative
  control (`pagecache_test_nostale`: the stale page *is* served, proving the
  test sees the bug), `invalidate_fs` during a fill, a recursive miss on the
  own fill (direct read, no self-wait), a 140-page read through the full
  cache during a fill (the FILLING slot survives eviction). Against an AP,
  driven from its idle loop (`kernel/smp.c`, only with the define): a remote
  write + invalidate, a second misser that really waits and then hits the
  published page, a waiter that times out and reads directly, a remote
  `invalidate_fs`.
- **`/bin/pcstress`** (`user/pcstress.c`, signed) + `tools/pc_stress_drive.py`:
  two files per directory, one writer each (overwrite in place, O_TRUNC +
  rewrite, O_APPEND, ftruncate shrink, ftruncate grow), lock-step checkers
  that verify every completed write through `read()` *and* a fresh
  MAP_PRIVATE mmap from their own processes, and unsynchronised hammers that
  keep the cache full of the previous version (every page they see must be a
  whole, well-formed page of the right file and index). Run on the ramfs
  `/tmp` and a default `mkfs.ext4` `/mnt` at once, -smp 4, then umount and
  host `e2fsck -fn`. With the BKL on this mostly checks serialized coherence
  (invalidate-after-write vs a hot cache); the hook test covers the races.
- Selftest gate **"M50 PAGECACHE"** (`tools/selftest.sh`, last gate): one
  `CONSOLE=root -DSECOS_PAGECACHE_TEST -DSECOS_LOCKDEP` image, the driver
  with `--pctest`.

### What the stress found
- **ramfs inode size lagged behind writes** (`fs/ramfs_vfs.c`, pre-existing,
  independent of B2): `ramfs_write` grew the entry but the cached
  `vfs_inode_t.size` only changed at the next lookup. A file-mmap fault in that
  window filled the page by the old size (zero-padded) and cached it; the
  write's invalidation had already run, so read() and mmap both kept serving
  it. read() clamps to the same stale size, which is why M20 never saw it.
  pcstress caught it on the first run (a writer's own check after an O_TRUNC
  rewrite); fixed by updating `inode->size` in `ramfs_vfs_write`. fat32,
  exfat and ext2 already update the inode size on write.
- The first driver version ran out of process slots (`MAX_PROCESSES` 32,
  init/bash included): a checker fork failed and its writer waited forever.
  pcstress now fails cleanly on a fork error, and the gate sizes its runs to
  fit (2 × 9 processes in parallel, then 15 at a time).

### Verification (B2)
- `-Wall -Wextra`: 41 warnings, identical to the baseline, in the normal and
  the `-DSECOS_LOCKDEP -DSECOS_PAGECACHE_TEST` builds; none in the touched
  files.
- `[PCTEST]` 23/23 on -smp 4 (lockdep build), lockdep silent.
- `tools/pc_stress_drive.py --pctest --rounds 200` on -smp 4: 10/10 (both
  pcstress runs on ramfs + ext4, host `e2fsck -fn` clean).
- `make lockdep-check`: 15/15.
- From `tools/selftest.sh`: M10 × {fat32, ext2, ext4}, M20, the M41
  persistent-root gate (`persist_test.sh`, incl. kill -9 crash runs) and the
  M41 exFAT gate: 127/127.

## B3 — block and USB locks (`feat/block-locks`)

Phase B step 3 of the plan. The BKL stays on by default, so nothing changes
in behaviour. The block layer and the USB stack are now safe without it.

### Lock design, per driver

| Lock | Level | Type | Covers |
|---|---|---|---|
| `block_dev_t.lock` | K40 | klock, one per **base** disk | everything its driver does for that disk |
| `xhci_enum_lock` | K35 (new) | klock | one USB enumeration (port reset, slot setup, descriptors, class bind, hubs) |
| `hid_lock` | S12 (new) | spinlock irqsave, **trylock only** in the poll | HID slots, report delivery |
| `xhci_lock` | S45 | spinlock irqsave | one xHCI transfer or command |
| `g_devs_lock` | S64 | leaf spinlock | the block-device registry |

- **Block layer** (`fs/block.{c,h}`). `block_dev_t` embeds a klock that is
  initialized lazily on first use (`lock_state` 0→1→2), so drivers keep
  their static initializers. A partition records its `parent`, and
  `block_base()` resolves it, so `sdb1` and `sdb` share one lock. Every
  caller goes through `block_read`/`block_write`. The partition callbacks
  call `block_*_locked` on the parent, which asserts the lock. Multi-I/O
  sections use `block_lock` + `block_*_locked`. The global irqsave
  `block_io_lock` (S42) is deleted.
- **AHCI.** The ports are independent: each has its own command list, FIS
  area and PxIS/PxCI/PxTFD registers. Only the command table and the 64 KiB
  bounce were shared, so they are now per port, and each SATA disk is
  serialized only by its own lock: two CPUs drive `sda` and `sdb` at once.
  HBA-global registers are touched only by `ahci_init`.
- **NVMe.** One namespace, one I/O queue pair, one shared bounce. The
  controller is a single base disk, and the admin queue is used only before
  registration.
- **virtio-blk.** One device, one virtqueue, one header/bounce/status byte.
  The driver supports only `vda` (a singleton `g_vblk_dev`).
- **USB Mass Storage.** A BOT command (CBW, data, CSW) is serialized by the
  device's block lock. Each transfer takes `xhci_lock` on its own, so a HID
  poll can run between the phases. CBW/CSW/data buffers are now per device
  (the data buffer is aligned to its 32 KiB size, so a TRB never crosses
  64 KiB). Stall recovery reads `xhci_dev_last_cc(dev)`. The global
  `xhci_last_cc()` can be overwritten by another CPU's transfer.
- **xHCI.** `xhci_lock` is held for exactly one transfer or command:
  submit plus a polled `evt_wait`, bounded by an iteration count, never by an
  interrupt or the tick. It covers the command/event rings, the stash, the
  transfer rings, the contexts, the DCBAA and the 32-bit bounce. It is never
  held across a multi-transfer exchange or across a port reset (10–200 ms of
  TSC time). Those run under `xhci_enum_lock`, a klock, so the CPU keeps IF on
  and is not preempted. `usb_attach` runs after the slot setup has released
  `xhci_lock`. The gated MSI-X handler drains the event ring only if it gets
  the lock (it never spins in an IRQ).
- **HID.** `usb_hid_poll()` is called from syscall context on any CPU
  (console TTY wait, every evdev reader). It only *tries* `hid_lock`: if
  another CPU is already polling, it returns at once, and the other CPU
  delivers the reports. `xhci_submit`/`xhci_poll_transfer` only try
  `xhci_lock` (busy: -2 / "nothing yet"), so a keyboard poll never spins
  behind a long MSC transfer. Reports are delivered under `hid_lock`
  (`input_event` → S15, `keyboard_key_event` → S50 and S30 for ISIG), which
  is why it sits at S12, below all of them.
- **devfs raw I/O** (`/dev/<blk>`). It now goes through the block layer and
  holds the disk lock for the whole request, so a partial-sector
  read-modify-write is atomic against every other user of the disk and of its
  other partitions. The static one-sector bounce became a per-call `kmalloc`,
  and whole sectors move up to 128 at a time (it was one sector per driver
  call).
- **Polled waits under a K40 klock.** Every driver wait (AHCI PxCI/TFD, NVMe
  CQ phase, virtio used ring, xHCI event ring) is a busy-poll of memory or
  MMIO bounded by an iteration count, and xHCI port resets use the TSC. None
  needs an IRQ that only arrives when the holder yields, and none needs the
  scheduler tick. A klock holder is never preempted (`klk.depth`).

### Order on the paths

- Cache miss on a disk FS: (K30 FS, C3/C4) → K40 disk → S45 xhci (USB only).
- devfs: K40 for the whole request.
- Enumeration: K35 → K40 (partition probe of a new usbN) → S45; S12 at HID
  attach.
- HID poll: S12 (try) → S45 (try), then S15/S30/S50 for delivery.

**On B3 alone a cache miss still runs `ops->read` under `pc_lock` (S40), so a
disk-backed FS takes the K40 klock under a spinlock.** A release build does
not mind (no contention while the BKL is on). A `-DSECOS_LOCKDEP` build
reports "klock taken while holding a spinlock" as soon as a disk FS is read
through the page cache. The ramfs-root lockdep-check images do not do this.
B3 is therefore only merged together with B2 (`integ/bkl`, below).

### Stress gate (`-DSECOS_B3_BLKSTRESS`, selftest "M50 B3")

The syscall path still takes the BKL even with `bkl=off` (every mask bit is
set), so a userland `dd` test serializes on the BKL and cannot show a missing
block lock. The gate therefore has two phases:

1. **Kernel, real concurrency** (`fs/blkstress.c`). At boot, the BSP and
   every AP (from its idle loop) pull jobs from one queue outside the BKL:
   SHA-256 of regions of `sda`, `sdb1`, `nvme0n1` and `usb0` with varying
   chunk sizes for ≥3 rounds (every round must give the same digest), a
   pattern written to `nvme0n1` and read back, and two jobs writing
   interleaved 100-byte stripes of one `/dev/sdb1` region through devfs
   (partial-sector RMW) while another job reads `sdb1`. All of this runs
   while every CPU polls the USB keyboard between chunks, and the host types
   a line over QMP.
2. **Userland.** From bash: parallel `dd` reads of the four devices piped to
   `md5sum`, plus two unaligned disk-to-disk copies (`bs=1000` into
   `nvme0n1`, `bs=700` from `usb0` into `sdb1`), while a second line is typed
   into a foreground `read`.

`tools/blk_stress_drive.py` (q35 `-smp 4`: AHCI ×2 with an MBR on `sdb`,
NVMe, usb-storage + usb-kbd on one qemu-xhci) compares every digest and md5
with the image files. After QEMU quits, it also checks every byte the guest
wrote and that the bytes around the writes are untouched. A
`usb_hid_keydowns` counter proves the keys came over USB HID, not PS/2.
QEMU 6.2's `input-send-event` rejects a non-display `device`, so the events
go to the active keyboard, which is the usb-kbd.

Plan inaccuracies found on the way:
- `/dev/vdb` cannot exist: virtio-blk supports a single device, and with
  virtio present `kernel.c` skips the AHCI/NVMe probes. The gate uses
  sda/sdb/nvme0n1/usb0.
- AHCI was not "one per port" until the shared command table/bounce were
  split.
- `usb_msc` shared its CBW/CSW/data buffers across devices, and its stall
  recovery read the controller-wide completion code.
- devfs raw I/O also used a static shared bounce (race between two disks
  even with per-disk locks).
- The fuzz harnesses needed a `stub/klock.h`.

### Verification (B3)

- `-Wall -Wextra`: 41 warnings in both the normal and the `-DSECOS_LOCKDEP`
  build, the same as `feat/lock-core`, none in a touched file. The exFAT and
  NTFS fuzz harnesses build.
- Block-stress gate (normal build): 31/31. Kernel phase: 4 workers, ~90–130
  jobs in ~4.3 s, ≥10 rounds per digest; 55/55 keys over USB HID.
- The same gate on a `-DSECOS_LOCKDEP` build: 31/31 and silent, both alone
  and merged with `feat/pagecache-fill` (B2).
- Lockdep with a FAT32 disk on AHCI, `cat /mnt/*` from bash: on B3 alone it
  panics with `klock taken while holding a spinlock: sda vs pagecache`, as
  expected before B2. With B2 merged it is silent.
- Gates (a selftest subset: M10×{fat32,ext2,ext4}, M21, M22 nvme+usb, M50
  B3, M41 persistent root, M41 exFAT incl. corrupt suite, MNT, the read-only
  mount gate, M42 NTFS): **240 passed, 0 failed**.
- `make lockdep-check`: 15/15 (ssd4, ssd8, nsd4, off4). A first run gave
  13/15 (ssd8 step + marker). It ran while other QEMU runs and builds loaded
  the host, its logs were lost in a private /tmp, and a clean rerun passed.
