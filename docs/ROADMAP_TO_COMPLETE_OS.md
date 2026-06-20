# SECoS — Roadmap to a complete OS (no GUI)

_Strategic plan from the current state to a complete, networked, multi-process,
secure operating system — headless (no GUI) for now._
_Written 2026-06-18; **realigned to actual state 2026-06-20** (HEAD `a358f42`,
selftest 165/165). Companion to `ROADMAP_SECoS.md` (technical) and
`docs/DEVELOPMENT_PLAN.md` (phases A–E)._

> **Reading note on milestone numbers.** This document was first written with
> *aspirational* milestone numbers (e.g. "M24 = SMP"). The project then shipped on
> a different numbering. This file now uses the **real** numbers: M0–M13 (A–E),
> M14 (demand paging), M15–M17 (F), M18–M20 (G), M21 (AHCI), M22 (NVMe+USB),
> M23 (POSIX FS), M24 (networking, pulled early), M25 (pipes+TTY), M26–M27 (H),
> M28–M29 (I = ACPI/APIC/TSC + SMP). Remaining future work is renumbered **M30+**.

## Definition of "complete OS (no GUI)"

A system that:
1. **survives misbehaving userspace** (a faulting/abusive process is killed, not
   the kernel); ✅ (M15)
2. has a **real process model** — argv/env, exec, fork-or-spawn, signals,
   blocking wait, pipes, a TTY with job control; **mostly ✅** (M16/M17/M19/M25) —
   **signals + job control still open** (M30);
3. has a **complete virtual-memory API** — mmap/mprotect/brk, COW, shared memory,
   a unified page cache; ✅ (M18–M20) — `MAP_SHARED` still future;
4. has a **mature storage stack** — permissions/ownership, symlinks, journaling /
   crash consistency, pseudo-filesystems (dev/proc); ✅ (M23/M26/M27);
5. uses **modern interrupts and runs on multiple cores** (APIC/IOAPIC, SMP);
   ✅ (M28/M29);
6. is **networked** — a driver, a TCP/IP stack, BSD sockets; ✅ (M24);
7. ships a **usable userland** — a real libc, a shell, ported tools, an init /
   service manager; **partial** — native shell + minimal libc; full libc / ported
   tools / init still open (Phase K, M31–M32);
8. keeps its **security identity central** — everything signed, capability-rooted
   permissions, exploit mitigations, an audit trail; **partial** — signing +
   driver caps in; generalized caps / mitigations / audit open (Phase L, M33–M35).

**Where we are:** points 1, 3, 4, 5, 6 are **done**; 2 is done except async
signals; 7 and 8 are the remaining frontier. SECoS boots dual UEFI/MB2, runs a
higher-half SMP kernel on multiple cores with demand paging, **mandatory Ed25519
code signing as the root of trust**, a capability-mediated driver space, RW
FAT32/ext2/ext4 with journaling, a full TCP/IP stack, and a multi-process signed
userland that survives faults.

## Guiding principles

- **Security is the through-line, not a phase.** Each milestone states how the
  new capability is gated by the signed manifest / capability model.
- **Custom minimal syscall ABI underneath; POSIX-friendly libc on top** — port
  open-source software *from source*, not Linux-binary compatibility (locked).
- **Microkernel bias where it pays** — drivers in ring 3 behind capabilities.
- **Every milestone ends green**: `tools/selftest.sh` grows, both boot paths PASS,
  0 warnings.
- **Dependency order over feature appeal.**

---

## ✅ Phase F — Robustness & process model — **DONE** (M15–M17)

An unhandled ring-3 fault **kills the offending process** and returns to the
scheduler instead of halting (M15, `sched_kill_current`, exit status `128+vector`).
argv/env on the demand-paged user stack + `SYS_SPAWN` + blocking `SYS_WAIT` (M16).
Wait queues + block/wake core + `SYS_SLEEP` + blocking `SYS_MSG_RECV` (M17).
_Deferred to Phase L:_ async signal delivery (Ctrl-C → SIGINT) — see M30.

## ✅ Phase G — Virtual memory completeness — **DONE** (M18–M20)

`mmap`/`munmap`/`mprotect` + `brk`/`sbrk` + libc `malloc` (M18). Copy-on-write
`fork` via PMM frame refcounts + `vmm_fork_space`/`vmm_cow_fault` (M19). Unified
page cache backing `read()` and file-backed `mmap` coherently (M20). _Future:_
`MAP_SHARED` + shared read-only text (retire the M14 per-process pinned image).

## ✅ Storage drivers (pulled early, for real hardware/VMware) — **DONE** (M21–M22)

**AHCI/SATA** (M21, `sda`..`sdd`) and **NVMe** (`nvme0n1`) + a polled **USB stack**
(xHCI core + HID boot keyboard + Bulk-Only Mass Storage `usb0`) (M22). In-kernel
for now; moving them into ring-3 Driver Space is future work (see "Driver Space
depth" in `TASKS.md`).

## ✅ POSIX filesystem personality — **DONE** (M23)

Persistent ext2 root (via `/.secosroot`), **devfs** (`/dev`), **procfs** (`/proc`),
**sysfs** (`/sys`), `SYS_LSEEK`/`SYS_STAT`. Trust = signature (a signed program
gets ambient `/dev` access; Driver Space is orthogonal).

## ✅ Phase J — Networking (pulled early) — **DONE** (M24)

NIC contract + 4 drivers (e1000/e1000e validated on real VMware; vmxnet3/igc
compile-clean), L2/L3/L4 stack (Ethernet/ARP/IPv4/ICMP/UDP/DHCP/DNS/full TCP),
**BSD sockets + `CAP_NET`** (a signed manifest bit), interrupt-driven MSI-X/NAPI RX.
Shell `dhcp`/`ping`/`nslookup`/`nettest`. _Future (was "M27"):_ network services +
capability-gated packet-filter firewall — folds into Phase L's generalized caps.

## ✅ Pipes + console TTY — **DONE** (M25)

Anonymous pipes (`SYS_PIPE`, blocking, EOF/EPIPE, fork-inherited), console cooked
line discipline (echo/backspace/Ctrl-D/Ctrl-C-as-line-cancel), TCP window 8K→64K.
_Open:_ **async Ctrl-C → SIGINT** and shell pipelines/job control — need signals
(M30).

## ✅ Phase H — Storage & filesystem maturity — **DONE** (M26–M27)

POSIX metadata stored & exposed (mode/uid/gid/timestamps), chmod/chown/utimes,
ext2 fast symlinks, mount/umount syscalls (M26). **JBD2 journal replay** — safely
mount a dirty ext3/4 left by a crash (M27a). **Write-side journaling** — SecOS's
own metadata writes are crash-atomic (transaction → commit → checkpoint;
read-your-writes; atomic rollback), proven with a fault-injection harness (M27b).
_Not implemented:_ `metadata_csum` (journals to `^metadata_csum` volumes).

## ✅ Phase I — Modern platform: ACPI, APIC, timekeeping, SMP — **DONE** (M28–M29)

- **M28 — ACPI + APIC/IOAPIC + TSC.** ACPI discovery (RSDP → XSDT/RSDT → MADT →
  CPU/LAPIC/IOAPIC topology, M28-1). **APIC switchover** (M28-2): retire the legacy
  8259 PIC + PIT IRQ0 onto the **LAPIC timer** (scheduler tick) and the **IOAPIC**
  (keyboard), with a PIC/PIT fallback. **TSC monotonic clock** (M28-3): `ktime_ns`
  calibrated against PIT ch2.
- **M29 — SMP (multicore).** Per-CPU data (`this_cpu()` by LAPIC id), AP bring-up
  (INIT-SIPI-SIPI trampoline), per-CPU GDT/TSS/idle, **CPU-pinned processes**
  (`cpu_affinity`, round-robin) with fine-grained spinlocks instead of a Big Kernel
  Lock — **user tasks run on multiple cores in parallel**. Verified on real VMware
  (shell `stress [N]`). The locking audit found one runtime shared-kernel-PT
  mutation (the M5 per-process kernel stack) → fixed with `kstack_lock`.
  _Open (polish):_ IPI reschedule, per-CPU run queues, task migration + TLB
  shootdown — see `docs/devlog/M29.md` and `TASKS.md`.

**Exit criteria I (met):** boots and schedules on multiple cores with modern
interrupts; monotonic timekeeping; runs on bare-metal/VMware storage. _Driver Space
for IRQ/DMA (the old "M25" payoff) remains future work alongside Phase L._

---

# ▶▶ REMAINING WORK (renumbered M30+)

## M30 — Signals & job control _(next; small, high-leverage)_

The process model's last gap. Build on the M16/M17 block/wake + trapframe core:
per-process pending mask + handler table, delivered on return-to-user. **Ctrl-C →
SIGINT** (the TTY raises it on the foreground process; default = terminate, reuse
M15's kill path), **SIGPIPE** (write to a pipe/socket with no reader; EPIPE already
exists), `SYS_KILL` + a minimal libc `signal`/`kill`/`sigaction`. Unblocks **shell
job control + pipelines** (`cmd1 | cmd2`, `&`). _Security:_ signal delivery respects
process ownership; no cross-trust-domain signalling.

## Phase K — Userland & (near) self-hosting (M31–M32)

- **M31 — libc completeness + ported software.** Round out libc: stdio, malloc
  (have), fuller string/math, time, errno, sockets (have), a **pthreads subset**
  (on M19/threads). Port real software *from source*: **dash** (or grow the native
  shell), **lua**, **sqlite**, a coreutils-like toolset. _Security:_ each ported
  tool is signed before it runs.
- **M32 — init + service manager + signed packages.** A real `init` (PID 1),
  service supervision, dependency ordering; a **signed package format** (install =
  verify + unpack into the FS — the natural extension of the M9 signing root); a
  package **keyring + revocation**.

**Exit criteria K:** boots to a multi-service userland; can install and run signed
third-party software built from source.

## Phase L — Security hardening (cross-cutting, dedicated pass) (M33–M35)

> _The differentiator._ Pieces land throughout; this phase makes SECoS's security
> claims rigorous and provable.

- **M33 — Generalized capability/permission model.** Lift the M11 driver-capability
  idea to **all** processes: the signed manifest declares FS scope, network scope
  (folding in the deferred firewall), IPC peers, and an allowed-syscall set; the
  kernel enforces them. Keyring with multiple keys + **revocation** + a structured
  **audit subsystem** (`[AUDIT]` → a queryable log). Also: real `DRIVER_OP_MAP_MEM`,
  IRQ-to-driver delivery, a DMA sandbox, driver auto-restart.
- **M34 — Exploit mitigations.** ASLR + KASLR; SMEP/SMAP; stack canaries; CFI;
  **W^X everywhere** (drop the 0–512 MB identity RWX huge-page map + the low
  identity map; guard pages on every kernel/user stack); spectre/meltdown-class
  mitigations as feasible.
- **M35 — Boot integrity.** A measured/secure boot chain (verify the kernel +
  initial userland against the signing root; TPM-backed measurement later);
  reproducible builds (extends the existing `BUILD_TS`/`GIT_HASH`).

**Exit criteria L:** every executable and the boot chain are verified; processes
run under least-privilege capabilities; common exploit primitives are mitigated;
security events are audited.

---

## Big rocks & risks (size honestly)

- **SMP (M29)** — _done._ Was the largest correctness effort; the per-CPU-pinned +
  fine-grained-lock design avoided a Big Kernel Lock. The `stress`-test found the
  one missed shared-kernel-PT race; budget for "the audit misses something" held.
- **TLB shootdown** — deliberately absent (pinned per-CPU spaces). A **hard
  prerequisite** for task migration / `MAP_SHARED` across cores / any cross-core
  kernel-VA unmap. Build it before those.
- **Generalized capability model (M33)** — touches every subsystem's permission
  checks; design the manifest schema once, enforce everywhere.
- **Journaling (M27)** — _done;_ crash-consistency proven with fault injection.

## Recommended near-term sequence (next few sessions)

1. **M30 — signals** (Ctrl-C→SIGINT, SIGPIPE, `kill`) → shell job control + pipelines.
2. **SMP polish** (IPI reschedule / per-CPU runqueues) *or* **NIC HW validation**,
   as opportunity allows.
3. **Phase K** — grow libc + port a first real tool (sqlite/lua) from source.
4. **Phase L** — the generalized capability model + audit, then mitigations.

A GUI/display server, window system, and desktop userland are explicitly **out of
scope here** and would be a later Phase M+.

## How this maps to the existing roadmap

Phases A–E (M0–M13) = the original mandatory plan. M14 = first stretch. Phases
**F–J + H + I are done** (M15–M29). What remains is **M30 (signals)** and Phases
**K (userland, M31–M32)** and **L (security hardening, M33–M35)** — the path from
"a complete headless OS kernel" to "a self-hosting, hardened, least-privilege OS".
