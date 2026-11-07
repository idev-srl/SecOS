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
| `block_io_lock` (missing) | `LK_S42_BLOCK_IO` | still taken under `pc_lock` until B2 moves FS reads out of it; B3 deletes it |

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
