# M53 follow-up — "stuck on spinlock net-stack owner=cpu 0xFFFF"

Branch `fix/netstack-stuck`, base `b5af912` (`integ/phase5`).

## The report

The M53 integrator saw, once, on three `-DSECOS_LOCKDEP` guests at the same
time:

    [LOCKDEP] stuck on spinlock net-stack owner=cpu 0xFFFF ...

and the guests did not reach init within 60 s. The three guests were the ones
of `tools/acpi_dmar_gate.sh` that have NICs (`vtdbr`, `amd`, `amd9`: an e1000e
on a `pcie-root-port`, an e1000 behind a `pcie-pci-bridge`, `-net none`);
plain q35, i440fx and the intel-iommu guest without bridges have no NIC at
all in that gate (`-net none` drops QEMU's default one). It did not come back
at low host load and the logs were lost.

## Reproduction

Not reproduced as written. With the base lockdep image:

- 55 boots of the three DMAR-gate bridge guests (18 at a time, `-smp 2`/`4`,
  host load average up to 18) and 36 + 36 boots of the `tools/pci_gate.sh`
  Q35 setup (two e1000e, e1000 behind the bridge; plain / intel-iommu /
  amd-iommu): every guest reached init in 5–7 s, no `[LOCKDEP]` line.
- 16 guests at a time with real traffic: bash loops of `/dev/udp` sends and
  `netstress stats` on every CPU plus a host UDP flood into the bridged
  e1000e (MSI-X RX on CPU 0), host load average 49–107: no `[LOCKDEP]`.

What did reproduce is the condition behind the message. With a temporary
probe that logged every spinlock wait over 50 ms together with the lock's
`owner_cpu` sampled at the 50 ms mark, the base image showed, under that
load, 80 such waits in 8 guests, and in most of them the lock was **held
with `owner_cpu == 0`**, i.e. exactly what the watchdog prints as
`owner=cpu 0xFFFF` (heap 28×, proc 7×, pty 6×, pmm 6×, ...). Some of those
waits saw 1022 acquisitions by other CPUs during the wait (starvation),
others none.

## Why the owner is missing

`lockdep_spin_lock` wins the lock word with `xchg`, then `ld_acquired` does
`l->owner_cpu = ld_idx() + 1`. `ld_idx()` is `this_cpu()->index`, and
`this_cpu()` reads the LAPIC ID register. Under QEMU TCG that read is an MMIO
exit that takes QEMU's global lock (BQL). The vCPUs spinning on the lock take
the BQL too: every `pause` in `cpu_relax_poll()` ends the translation block
and returns to the MTTCG CPU loop, which locks and unlocks the BQL. So the
CPU that has just won the lock can wait a long time for the BQL **before it
has recorded itself as the owner**, and the longer the host is loaded the
longer the BQL handoff to a descheduled thread takes. A waiter that hits its
2 s limit at that moment prints owner 0xFFFF. Every lockdep lock and unlock
does at least one more LAPIC read (`ld_me()`), so lockdep images are far more
exposed than normal ones.

None of the other candidates holds:

- trylock paths record the owner like the blocking path (`ld_acquired`);
- `this_cpu()` is valid in IRQ context and the net IRQ runs on a registered
  CPU;
- the e1000e MSI-X handler and `net_tick` both run on CPU 0 in IRQ context
  with IF off; every other holder takes the stack lock with
  `spin_lock_irqsave` (`netlk_acquire`), and the RX path only *trylocks*
  `rx_lock`, so the IRQ handler cannot interrupt a holder on its own CPU
  (lockdep's IRQ-safety rule would also have fired);
- the unlock path clears `owner_cpu` immediately before releasing the word,
  with nothing in between;
- the only untracked acquisitions are the ones made while the CPU is printing
  a lockdep report (`in_report`); for klock waits that print ran with IF on,
  so an interrupt during the report took its locks untracked too.

And no path holds the net stack lock for long: TX waits for the descriptor
are bounded and measured at under 20 ms even at load average 18 (temporary
probe around `net_dev_xmit`), and during boot nothing outside CPU 0 takes
the lock at all.

## Verdict

Not a deadlock or lock-ordering bug in the net stack: a false watchdog alarm.
Two separate weaknesses of the watchdog combined:

1. the owner was recorded after an MMIO access, so a holder stalled by the
   emulator looked ownerless;
2. the 2 s limit was plain elapsed time since the wait started, so a waiter
   that lost every race for 2 s on a lock that kept changing hands (unfair
   test-and-set under heavy contention), or a waiter whose own vCPU was
   descheduled, was reported as stuck.

The "no init within 60 s" was the host load: the same images reach init in
5–7 s on a quiet host.

## The fix (`kernel/lockdep.c`, `kernel/lockdep_wait.h`, `kernel/klock.c`)

- **CPU index without the LAPIC.** `ld_idx()` reads the index from
  `IA32_KERNEL_GS_BASE`, tagged (`0xFFFFEC1D00000000 | index`, canonical as
  the MSR requires). Nothing else uses that MSR: the kernel never executes
  `swapgs`, and ring 3 cannot read or write it. A CPU stores its index only
  once `this_cpu()` has demonstrably resolved it (the `cpu_t`'s APIC ID is
  the one the CPU reads), because `this_cpu()` answers "the BSP" on an AP that
  is not registered yet; until then the slow path is used. Lockdep images
  only.
- **Owner first.** `ld_acquired` stores `owner_cpu` from the `ld_cpu` slot it
  already has, before anything else; the untracked `in_report` paths record
  the owner too.
- **Reports with interrupts off.** `ld_stuck` and `ld_report2` clear IF while
  `in_report` is set, so an interrupt can no longer run untracked lock
  operations in the middle of a report.
- **Progress-aware watchdog** (`lockdep_wait.h`, pure code shared with the
  host test). A waiter reports `stuck` only when the lock has **not changed
  hands** for 2 s of time the waiter itself observed. The hand-over counter
  is the spinlock's `n_acq` or the klock's `serving` ticket. Samples are
  taken every 1024 spins, as before; a gap of more than 250 ms between two
  samples means the waiter's vCPU did not run, and the window restarts.
  A lock that keeps changing hands while one waiter loses every race is
  reported as `spinlock (starved)` / `klock (starved)` after 30 s. The klock
  watchdog had one global sample counter shared by every CPU; it now uses
  the same per-wait state.

What still gets reported: a lock held without a hand-over for 2 s (a
deadlock, a lost unlock, an over-long critical section), 2 s after the
waiter last ran without a gap; the owner and held time are printed as
before. What no longer gets reported as stuck: a busy lock changing hands
(reported only after 30 s, as starvation), and time the waiter spent
descheduled. The host itself can still deschedule the holder for over 2 s;
that is indistinguishable from a hang in guest time and is still reported,
now with the real owner.

## Tests

- `tools/ldwatch_host.c` (host, the `lockdep_wait.h` logic on synthetic
  time): never-released lock reported once at 2 s; hand-overs every 50 ms for
  10 s never reported; hand-overs that stop at 3 s reported at 5 s; a waiter
  that loses every race for 30 s reported once as starved; a 2.5 s stall of
  the waiter not reported, and a lock still held after it reported 2 s after
  the resume; a 200 ms pause does not delay detection; 5 ms sampling still
  detects. 8/8 PASS.
- `-DSECOS_LDWATCH_TEST` (`kernel/ldwatch_test.c`, lockdep image, `-smp 2`):
  the BSP holds a test spinlock with IF off, an IPI makes the AP wait for it
  in IRQ context; 3 s of simulated hand-overs → no report; 3 s without →
  exactly one report naming cpu 0 as the owner; after the release the AP gets
  the lock. Reports are counted (`lockdep_stuck_expect`) and printed as
  `[LDTEST]   caught: ...`, so the image has no `[LOCKDEP]` line. 4/4 PASS.
  With the old watchdog phase 1 would have been reported at 2 s.
- The base-vs-fix comparison above: 8 + 8 guests at the same time under
  load average ~107: base 80 waits over 50 ms, most with no owner recorded;
  fix 59, every one with an owner recorded.
- Gate runs with the fixed lockdep image (all after a clean build; the
  normal and the `-DSECOS_LOCKDEP` builds both have 0 `-Wall -Wextra`
  warnings, as the base does):
  - NETSMP (the selftest's three configs at once: `-smp 4` e1000e MSI-X,
    `-smp 4` e1000 polled, `-smp 1` e1000e): 25/25 each, no `[LOCKDEP]`,
    panic or exception, MSI-X NAPI armed.
  - DMAR-gate bridge guests (`vtdbr`, `amd`, `amd9` setups, 512 MiB,
    `-smp 2`/`4`, 9 at a time): 36/36 reached init (5 s), no `[LOCKDEP]`.
  - `tools/pci_gate.sh` Q35 guests (plain / intel-iommu / amd-iommu, two
    e1000e + an e1000 behind the bridge): 36/36 reached init, no `[LOCKDEP]`.
  - 8 contention guests (bash `/dev/udp` + `netstress stats` loops on every
    CPU, host UDP flood into the bridged e1000e) for 120 s at load ~31: all
    at the prompt, no `[LOCKDEP]`.
  - `make lockdep-check` subset: `smp_stress_drive.py -smp 4` 19/19,
    `proc_race_drive.py -smp 8` 9/9, logs clean.
  - `tools/smoke.sh --mb2` and `--uefi`: PASS, both reach init.
- An earlier round of the same loops ran at host load average ~200 (other
  agents' guests at the same time). Most guests did not reach init within
  60 s there, but none printed a `[LOCKDEP]` line. That fits the reading
  that "no init in 60 s" in the original report was host overload.

## For later

- `this_cpu()` reads the LAPIC on every call. That is slow under TCG
  everywhere, not only in lockdep. A per-CPU pointer (GS base with
  `swapgs`, or `RDPID`/`TSC_AUX`) would fix it for the whole kernel. If
  `swapgs` is ever introduced, lockdep's use of `IA32_KERNEL_GS_BASE` must
  move.
- The test-and-set spinlock is unfair: under contention a waiter can lose
  for tens of milliseconds even on KVM-free emulation. The watchdog now
  calls that starvation, not a hang. A ticket spinlock would bound it.
