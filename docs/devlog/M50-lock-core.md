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
| `block_io_lock` (missing) | `LK_S42_BLOCK_IO` | was taken under `pc_lock`; since B2 it no longer nests there (see below), the level only keeps it classified until B3 deletes it |

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
no longer nests inside S40; it keeps `LK_S42_BLOCK_IO` only to stay
classified until B3 replaces it with the per-disk K40 `blkdev->lock` (B2 did
not move it). `kernel/lock_order.h` says so.

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
