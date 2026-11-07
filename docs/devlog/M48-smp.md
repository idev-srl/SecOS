# M48 — SMP for real, on by default (Phase 4)

**Goal:** turn the M29 multicore prototype (INIT-SIPI-SIPI, per-CPU idle, CPU-
pinned processes, off by default since `stress 8` froze the ASUS laptop) into a
multicore kernel that is safe to run by default: TLB shootdown, reschedule
IPIs, task migration with load balancing, more CPUs (x2APIC), and the three
known SMP flakes fixed.

Branch `feat/smp` (from `integ/phase4`). Kernel side: `arch/x86/tlb.{c,h}`
(new), `arch/x86/{percpu,lapic,acpi,tss,spinlock,idt*}`, `kernel/{sched,
process,smp,signal,syscall*,klog,kernel,shell}.c`, `mm/vmm.{c,h}`,
`drivers/vt.c`, `lib/{terminal.c,debugcon.h}`, `fs/procfs.c`. Tests:
`user/smptest.c`, `tools/smp_stress_drive.py`, selftest M48 gate.

## Design

### Run state and the switch

M29 pinned each process to one CPU so the context switch needed no lock. M48
lets tasks move, which needs two guarantees: no two CPUs ever run (or reap)
the same task, and a CPU must not pick up a task whose previous CPU is still
executing on its kernel stack.

- **Claiming.** A CPU chooses its next task in `process_claim_next()` under the
  process-table lock and *claims* it there: `state = RUNNING`, `on_cpu = 1`,
  home CPU = this CPU. Only `READY` tasks with `on_cpu == 0` are eligible.
- **Releasing.** `on_cpu` stays 1 until the CPU has completely left the task:
  `switch_to()` saves the FPU image, loads the next CR3 and TSS.rsp0, then
  `arch_switch_finish` (asm) moves to a private per-CPU switch stack, *then*
  stores `prev->on_cpu = 0`, then restores the next trapframe and `iretq`s.
  From that store on, any CPU may run the previous task on its own kernel
  stack. The reaper (`process_reap_one`, any CPU) also requires `on_cpu == 0`,
  so a zombie's stack/space/PCB is never freed while a CPU is on it.
- **The kernel is not preemptible in ring 0** (unchanged): a blocked syscall
  saves its trapframe with `rip` rewound and re-runs from scratch when woken,
  so a READY task's whole state is its trapframe + FPU image — migration
  needs no kernel-stack continuation.
- **NEW is never picked.** `process_create_*` and `process_fork` return a
  `PROC_NEW` task; only `sched_make_ready()` (CAS NEW -> READY + kick)
  publishes it. This removes the NEW->READY race fixed in M44 only for the
  kernel shell: the kernel demo spawns in `kernel/kernel.c`, `ksys_spawn` and
  fork all set READY on a task another CPU could already have run (and, for
  fork, read `child->pid` after the child could already be reaped).

### Scheduling policy: home CPU + pull + push

Per-CPU run queues are logical: every task carries a *home* CPU
(`cpu_affinity`, soft) and an optional hard pin (`cpu_pin`, -1 = none; kernel
API `process_set_pin`). The table is still one array (32 entries) under one
lock — at this size a scan is cheaper than maintaining real queues.

- A CPU picks READY tasks homed on it first (round-robin after the current
  task), so a woken task stays on its warm CPU.
- **Pull:** with nothing of its own (idle, or quantum expired) a CPU pulls any
  other eligible task — unless that task's home CPU is idle and the task became
  READY less than 2 ticks ago (the home CPU was kicked and takes it itself;
  this stopped tasks ping-ponging across idle CPUs).
- **Push:** a new task (create/fork) is homed on the least-loaded online CPU.
- An idle CPU's tick first peeks at the table without the lock (stale reads
  of a just-freed entry are harmless), so many idle CPUs no longer contend for
  the lock 1000 times a second.

### Reschedule IPI and lost wakeups

- **Kick:** when a task becomes READY (wake, make_ready, SIGCONT) and its home
  CPU — or else any CPU — is idle, that CPU gets a `VEC_IPI_RESCHED` (0xF1)
  IPI; its handler claims the task at once instead of at the next tick.
  `resched_pending` suppresses duplicate IPIs.
- **Time-driven wake-ups** (sleepers, console readers, debugcon tail) run once
  per tick on the BSP (they ran on every CPU's tick: N table scans per ms).
- **Lost wakeups.** Every BLOCKED -> READY transition is a compare-and-swap
  (signal_post races the table-scanning wakers from outside the lock). The
  race that remains is check-then-block across CPUs: task A finds the pipe
  empty, B writes and wakes "every BLOCKED reader" before A is BLOCKED.
  `g_wake_seq` is bumped by every event-driven wake (pipe, pty, IPC, child
  exit, wait-any, signals) after its condition became true; `syscall_handler`
  snapshots it into `wake_snap`; `sched_block_current()` compares the two under
  the table lock (which every waker's scan takes) and, if they differ, re-runs
  the rewound syscall at once instead of sleeping. The M25 "re-check the pipe
  after 5 ticks" workaround is gone. Time-based conditions need no bump (the
  next tick re-checks).

### TLB shootdown (`arch/x86/tlb.c`)

- Each `vmm_space_t` has `active_cpus` (256-bit set) maintained by
  `vmm_switch_space()`: the new space gains this CPU with a locked RMW
  *before* the CR3 load, the old one loses it after. A shootdown samples the
  set after a full barrier, so either the target is in the set or its CR3 load
  observes the new PTE.
- `tlb_shootdown(space, va, npages)`: local `invlpg` (or CR3 reload above 32
  pages / npages = 0), then for every other online CPU in the set (kernel
  space: every online CPU) a request: one global request slot serialized by
  `tlb_lock`, a per-CPU `tlb_pending` flag, a `VEC_IPI_TLB` (0xF0) IPI, and a
  spin until every target cleared its flag (bounded: 200 ms, then logged as
  `[TLB] shootdown timeout` and counted).
- **No deadlock with locks held / IF = 0:** a target services pending
  requests from the IPI handler *or* from any lock-wait loop:
  `spin_lock()` (and the klog/debugcon/console bounded locks) call
  `tlb_poll()` while `g_tlb_inflight` is set. So an initiator holding lock L
  with interrupts off, waiting for a CPU that spins on L with interrupts off,
  still gets its acknowledgement. Handlers take no lock.
- Hooked: `vmm_unmap` (kernel), `vmm_unmap_in_space` (munmap/exit teardown),
  `vmm_protect_in_space` (mprotect), `vmm_cow_fault` (COW break),
  `vmm_fork_space` (parent write-protect, full flush), `kset_flags` (kernel
  W^X), `vmm_set_physmap_wc` (framebuffer caching retag), and the boot
  selftest. `vmm_space_destroy` logs `[TLB] BUG` if a CPU still has the space.
- **Kernel stack slots** are freed without an IPI: a slot is only touched by
  the CPU running its owner and every switch reloads CR3 before the next
  task's stack is used; the kernel maps nothing global and uses no PCID, so
  that reload drops the translation everywhere it matters. The rule is
  documented at the call site and `-DTLB_KSTACK_EAGER` restores the broadcast
  (required if global pages or PCID are ever enabled). Before this, with 64
  CPUs every process exit IPI'd 63 CPUs.
- Single-threaded processes mean a user space is loaded on at most one CPU, so
  user shootdowns stay local in practice; the mechanism is exact for when
  threads arrive. PCID was not added (optional).
- **Proof it works:** at boot with >= 2 CPUs, `smp_tlb_selftest()` maps a
  scratch kernel page, lets every AP read (cache) it, remaps it with a
  shootdown — every AP must read the new frame — and, as a negative control,
  without one: under QEMU every AP keeps reading the *old* frame
  (`without stale=3` on -smp 4), i.e. the test does detect a missing
  invalidation. `[M48] TLB shootdown selftest PASS`.

### FPU/SSE

Eager save/restore (unchanged in principle): `fxsave` into the PCB at switch-
out (before `on_cpu` is released), `fxrstor` at switch-in on whichever CPU
claims the task; fork copies the parent's live registers. New: the signal
frame carries the interrupted 512-byte `fxsave` image and `sigreturn`
restores it (MXCSR masked with the CPU's MXCSR_MASK so a forged frame cannot
#GP the kernel) — before M48 a handler that used SSE clobbered the
interrupted code's registers. `smptest fpu` checks both: with either restore
removed it fails (verified), with the kernel as shipped it passes.

### More CPUs, x2APIC

- `SMP_MAX_CPUS` 8 -> **256**. `cpu_t` blocks, AP idle PCB + trapframe, AP
  GDT/TSS/IST and AP switch stacks are allocated per CPU from the MADT count;
  only a 256-entry pointer table and a 4096-entry APIC-ID -> index map are
  static. `this_cpu()` is a map lookup (was a linear scan) and returns CPU 0
  directly while only one CPU exists.
- **x2APIC:** chosen at `lapic_enable()` when CPUID.01H:ECX[21] says so (or the
  firmware already enabled it — then xAPIC MMIO would not work at all);
  `-DSECOS_NO_X2APIC` keeps xAPIC. All LAPIC accesses go through
  `lapic_rd/lapic_wr` (MMIO or MSR 0x800 + reg/16), the ID is the full 32-bit
  MSR 0x802, IPIs are one 64-bit ICR MSR write (with `mfence`), INIT/SIPI too.
  Each AP switches its LAPIC to the BSP's mode first thing in `ap_entry` (the
  APIC ID read by `this_cpu()` depends on it). The MADT parser takes type-9
  (Processor Local x2APIC) entries and 32-bit IDs; xAPIC fallback unchanged.
  IOAPIC/MSI destinations stay the BSP (8-bit field, APIC ID 0 on QEMU).
- ICR writes are done with interrupts off (an IPI sent from an ISR on the same
  CPU must not split xAPIC's two-register sequence).

### Kernel log and console

- **Debugcon (flake a):** with more than one CPU each CPU assembles its
  current kernel debugcon line in its `cpu_t` (interrupts off while
  appending) and emits it whole, with the kernel log record, under the
  debugcon line lock at `'\n'`. A partial line is flushed by the owning CPU's
  tick after 20 ms and by `debugcon_kflush()` on the panic/kill paths. Single
  core output is byte-for-byte immediate as before. The lock is bounded (a
  CPU that died holding it cannot silence the others).
- **Console:** the vt emulator (grid, cursor, escape parser, fb/VGA backends)
  had no lock at all; two CPUs printing at once could corrupt the parser and
  push the cursor out of the grid. `con_lock()` (recursive per CPU, bounded,
  IRQs off) wraps every vt mutation; `terminal.c` holds it across a whole
  kernel string and a whole TTY write, so serial output no longer interleaves
  inside a string. The blink tick only tries it.
- A kernel panic parks every other CPU (`VEC_IPI_STOP`, 0xF2) so the report
  is the last thing printed.

### The kernel lock (found by the first multicore selftest run)

Running the whole selftest with two CPUs per guest exposed the real cost of
"SMP on": the kernel had been implicitly serialized because a syscall ran
with interrupts off on the only CPU executing user code. VFS and every
filesystem, the TTY layer, credentials, the spawn/exec staging buffers, procfs
snapshots and driver bindings have no locks of their own. Two Dropbear
processes appending to `/var/log/sshd.log` on two CPUs corrupted the heap
(`[HEAP] corrupted block header`, owner `ramfs_write`, SSH gate), and the
persistent ext4 root came out e2fsck-dirty (PERSIST gate).

`kernel/bkl.c` adds one recursive kernel lock, owned by a *task* (the CPU's
current process; the CPU itself before the scheduler runs), taken around:
every syscall body (`syscall_handler`), user-address page faults (demand
paging, COW, file-backed mmap; recursive when the fault comes from a
syscall's user access), process teardown (exit, kill, stop, reap) and
kernel-shell commands. User code still runs in parallel on every CPU; the
subsystems with their own locks (PMM, heap, page cache, pipes, ptys, IPC,
block I/O, input, network, process table) nest inside it; interrupt handlers
never take it. Rules: `switch_to()` drops it for a task that leaves the CPU
mid-syscall (blocked syscalls re-run from the start, so nothing is held
across a sleep); the block re-run path drops it before returning to ring 3;
the tick and the reschedule IPI never preempt an idle task that holds it;
syscall and ring-3 fault entry spin for it with interrupts on, so a CPU that
waits for a long syscall elsewhere keeps its timer ticks; kernel-shell waits
(children, the pager key) release it. Fine-grained locking of the
filesystems and the TTY layer is the follow-up that removes it.

With it the SSH gate (13/13) and the persistent-root suite (30/30) pass on
`-smp 2`; the pending-signal set is now updated atomically as well
(`signal_post` runs on any CPU and from interrupt handlers).

### Defaults and opt-outs

SMP is on by default: `smp_init()` runs at boot unless the kernel was built
with `-DSECOS_NO_SMP` or the Multiboot2 command line contains `nosmp`
(the ISO's GRUB menu has a second entry "SecOS x64 (single CPU: nosmp)").
The shell `smp` command still brings APs up later. `-DSECOS_SMP_AT_BOOT` is
now a no-op. `/proc/smp` reports CPUs, x2APIC mode, shootdown counters and
per-CPU switches, migrations and IPI counts.

## The three known flakes

1. **Interleaved debugcon/console lines** (`[SCHED]` inside `[INIT]`/`[UA-
   TEST]` lines). Root cause: kernel debug output was written byte by byte to
   port 0xE9 (and the klog line buffer, shared) from every CPU; multi-call
   lines from two CPUs merged. Fixed by per-CPU line assembly (above); the
   console got its first lock at the same time.
2. **M29-3 PMM leak check flaky** ("PMM MISMATCH across rounds", also on
   main). Root cause: the M8 demo measured free memory as soon as no process
   was *alive*, but on SMP the zombies were only reaped by their own CPU's
   idle loop after its next `hlt` wake-up, so a round could be measured while
   some zombies still held their address space, kernel stack and PCB (a dip of
   16-26 frames, recovered next round). Waiting for an empty table is not
   enough by itself either: `process_reap_one` detaches a zombie *before*
   `process_destroy` frees it, outside the lock and possibly on another CPU.
   Fix: any CPU reaps a zombie once `on_cpu == 0`; the reaper counts detached
   zombies until their destroy has finished and `process_count_all()`
   includes them, so "table empty" is a real quiesce point; the demo samples
   only then. 30/30 runs clean on -smp 2 and -smp 4.
3. **NEW -> READY race in the kernel demo spawns.** Root cause: a NEW task was
   pickable, so another CPU could run it (even to exit) before the creator set
   READY over its state, resurrecting it. Fixed for every creator by making
   NEW unpickable and publishing with `sched_make_ready()` (CAS); fork and
   spawn read the child pid before publishing.

Found on the way: `ksys_spawn`/fork returned `child->pid` read after the child
was published (use-after-free if it already exited and was reaped on another
CPU); fork children could be killed by a signal posted before their own
handler setup (test-side); the console race above, which — together with the
NEW-state race (the kernel `stress` command relied on NEW being picked) and
lost pipe wake-ups — is the most likely cause of the ASUS `stress 8` freeze
(eight spinners printing their start line at once onto the unlocked
framebuffer console).

## Results (QEMU TCG, this host, 24 cores)

| Test | Result |
|---|---|
| Boot -smp 1/4/8/16/64 (QEMU 6.2, xAPIC) | init + bash prompt, no `[EXC]` |
| Boot -smp 4/8 x2APIC (QEMU 9.2, `-cpu max,+x2apic`) | `mode=x2apic`, all online, shootdown selftest PASS |
| Boot 256 CPUs x2APIC, sparse APIC IDs up to 0x1BC (`-smp 260,sockets=2,dies=2,cores=65`, intel-iommu eim) | 256 online (the cap), bash prompt |
| TLB shootdown boot selftest (-smp 4) | fresh 3/3 with shootdown, stale 3/3 without |
| `smp_stress_drive.py` -smp 4 / 8 / 16 / 8+LOCKDEP_LITE | 16/16 each |
| kernel `stress 8/16/32` on -smp 4/8/16 | all done; 32 x 5 s spinners: 45.8 s / 25.7 s / 16.5 s |
| NETSMP netstress (LOCKDEP_LITE) -smp 8 e1000e MSI-X, -smp 16 e1000 polled | PASS 24/24 each |
| soak (`tools/soak_drive.py` + syscall fuzzer) -smp 8, 22 sessions x 600 cmds over 35 min | 22/22 PASS |
| soak, one continuous session -smp 8, 15000 cmds + fuzzer, 38 min | PASS |
| SSH gate / persistent-root suite on -smp 2 (with the kernel lock) | 13/13, 30/30 |
| M8/M29-3 leak check, 15 runs each on -smp 2 and -smp 4 | 30/30 NO LEAK |
| selftest M48 gate (incl. x2APIC with SECOS_QEMU_X2APIC = QEMU 9.2) | 46/46 |
| full `tools/selftest.sh`, every guest -smp 2 unless the gate sets it (run 1, before the kernel lock) | 1074 pass / 13 fail: M7 (needs one CPU), SSH (heap corruption), PERSIST (count + dirty ext4) |
| full `tools/selftest.sh` (run 2, final kernel) | 1060 pass / 3 fail: all NETSMP smp4-msix, whose host driver crashed before starting QEMU (UDP port race between the three parallel drivers, `Address already in use`) |
| NETSMP gate re-run after fixing the driver (3 parallel: smp4-msix, smp4-poll, smp1-msix) | 24/24 x 3, MSI-X armed, 4 CPUs online |

## Must be re-validated on real hardware (paused since 2026-11-04)

- `stress 8` / `stress 32` on the ASUS: the original freeze. Expected fixed
  (console lock, NEW race, lost wake-ups) but never run on hardware.
- **x2APIC on real firmware**: QEMU's TCG only gained x2APIC in 9.x, and the
  QEMU on the build host (6.2) has none. Real laptops usually expose x2APIC
  and some firmwares hand over with it already enabled (before M48 that would
  have broken the xAPIC-only kernel). If anything misbehaves, boot an
  `-DSECOS_NO_X2APIC` build to compare.
- INIT-SIPI-SIPI delays (10 ms / 200 µs) and AP bring-up on real firmware with
  many CPUs; the `nosmp` GRUB entry is the fallback.
- TLB shootdown on real silicon (speculative page walks, real IPI latency):
  check `/proc/smp` `shootdown_timeouts 0` after a GUI session (the physmap WC
  retag of the framebuffer is a real cross-CPU shootdown).
- PAT is now programmed on every AP (`vmm_pat_init`), because a task using the
  WC framebuffer (secwm) migrates; verify GUI rendering speed on hardware.
- MSI/IOAPIC destinations assume the BSP has APIC ID 0 (true on QEMU and most
  boards).
- The kernel lock serializes syscalls: check that a GUI session (secwm +
  terminals) and SSH stay responsive on the 4/8-thread laptop.

## Integration notes

- feat/bigmem: the LAPIC/IOAPIC still map through `vmm_extend_physmap()`
  (TODO in `arch/x86/lapic.c`: switch to `vmm_map_mmio()`); a physmap cache
  attribute change after the APs are up (`vmm_set_physmap_wc`) already goes
  through `tlb_shootdown_kernel()`, keep that in the new physmap code.
- CLAUDE.md's M29 line ("AP bring-up is OFF by default") is now outdated.
