# SECoS — Roadmap to a complete OS (no GUI)

_Strategic plan from the current state (M0–M14) to a complete, networked,
multi-process, secure operating system — headless (no GUI) for now._
_Written 2026-06-18. Companion to `ROADMAP_SECoS.md` (technical) and
`docs/DEVELOPMENT_PLAN.md` (phases A–E)._

## Definition of "complete OS (no GUI)"

A system that:
1. **survives misbehaving userspace** (a faulting/abusive process is killed, not
   the kernel);
2. has a **real process model** — argv/env, exec, fork-or-spawn, signals,
   blocking wait, pipes, a TTY with job control;
3. has a **complete virtual-memory API** — mmap/mprotect/brk, COW, shared memory,
   a unified page cache;
4. has a **mature storage stack** — permissions/ownership, symlinks, journaling /
   crash consistency, pseudo-filesystems (dev/proc);
5. uses **modern interrupts and runs on multiple cores** (APIC/IOAPIC, SMP);
6. is **networked** — a driver, a TCP/IP stack, BSD sockets;
7. ships a **usable userland** — a real libc, a shell, ported tools, an init /
   service manager;
8. keeps its **security identity central** — everything signed, capability-rooted
   permissions, exploit mitigations, an audit trail.

SECoS already has a strong, unusual foundation: dual UEFI/MB2 boot, higher-half
kernel, full demand paging, **mandatory Ed25519 code signing as the root of
trust**, a capability-mediated driver space, RW FAT32/ext2/ext4, virtio-blk,
kernel IPC. The plan below builds the missing layers **without diluting the
security thesis** — the signed-manifest model is the spine that every new
subsystem hangs permissions off.

## Guiding principles

- **Security is the through-line, not a phase.** Each milestone states how the
  new capability is gated by the signed manifest / capability model.
- **Custom minimal syscall ABI underneath; POSIX-friendly libc on top** — port
  open-source software *from source*, not Linux-binary compatibility (locked
  decision).
- **Microkernel bias where it pays** — drivers in ring 3 behind capabilities; the
  kernel stays small and auditable.
- **Every milestone ends green**: `tools/selftest.sh` grows, both boot paths PASS,
  0 warnings. Same discipline as M0–M14.
- **Dependency order over feature appeal** — robustness and the process model
  unblock everything; SMP and networking are the big rocks and come after the
  foundation is solid.

---

## Phase F — Robustness & process model (make it survive userspace, give it a real one)

> _Why first:_ today an unhandled user fault **halts the kernel**, and spawned
> programs get no argv. Nothing else is worth building on a kernel that a buggy
> user program can take down. This phase is low-risk and high-leverage.

- **M15 — Fault-driven process termination + exception delivery.**
  An unhandled user fault (or fatal exception) **kills the offending process** and
  returns to the scheduler instead of halting. Thread the trapframe into the
  fault path; add a `sched_kill_current(reason)`. Skeleton for signal/exception
  delivery (at least fatal-signal semantics + exit status carrying the signal).
  _Security:_ a faulting process is contained; the kernel logs an audit line.

- **M16 — Exec model: argv/env/auxv + spawn.**
  Pass `argv`/`env` to ring-3 programs (stack setup at exec); `SYS_EXEC` /
  `posix_spawn`-style. Proper exit codes through `SYS_WAIT` (blocking). libc crt0
  consumes argv/env. _Security:_ the spawn path still enforces signing + manifest
  `max_mem`/caps; argv is bounce-buffered like all user data.

- **M17 — Blocking primitives, pipes, TTY.**
  Wait queues + a futex-like primitive; convert poll-style syscalls (`SYS_WAIT`,
  `SYS_MSG_RECV`) to **blocking**; `SYS_SLEEP`. Anonymous pipes. A real TTY line
  discipline so the shell does line editing / signals (Ctrl-C → SIGINT) — first
  step toward job control. _Security:_ pipe/tty endpoints are capabilities.

**Exit criteria F:** a buggy program can't crash the kernel; the shell runs
programs with arguments, pipes them, and Ctrl-C interrupts them.

---

## Phase G — Virtual memory completeness

> _Why here:_ the M14 VMA framework is the foundation; fork/COW and mmap are what
> "real" programs and a libc malloc expect.

- **M18 — mmap / munmap / mprotect + brk/sbrk + user malloc.**
  Anonymous and file-backed mappings on the M14 VMA path; `mprotect` honoring
  W^X; `brk`/`sbrk`; a real `malloc`/`free` in libc on top of mmap/brk.

- **M19 — Copy-on-write + fork + shared memory.**
  COW page sharing (refcounted frames); `fork` (or a deliberate
  spawn+COW-clone); `shm`. Needs a frame refcount in the PMM.

- **M20 — Unified page/buffer cache.**
  One cache backing file `read`/`write` **and** file-backed `mmap` coherently;
  `fsync`/writeback. Removes the per-process pinned-image copy from M14 (shared
  read-only text). _Security:_ cache respects file permissions/ownership.

**Exit criteria G:** programs `mmap` files and grow the heap; `fork`/`exec`
works; text pages are shared, not duplicated.

---

## Phase H — Storage & filesystem maturity

- **M21 — VFS maturity + pseudo-filesystems.**
  `mount`/`umount` syscalls; permissions, ownership, timestamps, symlinks; robust
  path resolution; **devfs** (`/dev`) and **procfs** (`/proc`). _Security:_
  per-file ACLs feed the capability model; signed programs declare FS scope.

- **M22 — Crash consistency.**
  ext4 **JBD2 journaling** + `metadata_csum` (finishing the M10 deferral), or a
  custom log-structured FS; block-cache writeback ordering; power-fail safety.

**Exit criteria H:** the FS survives power loss; `/dev` and `/proc` exist;
multi-user file permissions are enforced.

---

## Phase I — Modern platform: APIC, timekeeping, SMP, real drivers

> _Why here:_ SMP and IRQ-to-driver need APIC/IOAPIC and ACPI first. This is the
> heaviest phase; sequence it carefully.

- **M23 — ACPI + APIC/IOAPIC + timekeeping.**
  Parse ACPI tables; retire the legacy PIC for the Local APIC + IOAPIC; TSC/HPET
  monotonic + wall-clock time; `timerfd`-style timers. Prereq for SMP.

- **M24 — SMP (multi-core).**
  AP bringup; per-CPU data; a locking audit across the kernel (the single-CPU
  assumptions from M0–M14 must be found and fixed); an N-CPU scheduler with
  priorities and affinity. _The single biggest correctness effort in the plan._

- **M25 — Real driver space + more devices.**
  `DRIVER_OP_MAP_MEM` for real (MMIO into the driver address space); **IRQ →
  driver** delivery (on M13 IPC + M17 wait queues); a **DMA sandbox**; driver
  auto-restart. Add **AHCI/NVMe** (bare-metal / VMware storage) and
  **virtio-rng** (entropy). _Security:_ this is the payoff of the M11 capability
  model — drivers get exactly the MMIO/IRQ/DMA their signed manifest grants.

**Exit criteria I:** boots and schedules on multiple cores with modern
interrupts; user-space drivers handle real IRQs/DMA under capability limits; runs
on bare-metal/VMware storage.

---

## Phase J — Networking

- **M26 — virtio-net + TCP/IP + sockets.**
  virtio-net driver; a TCP/IP stack (port **lwIP** or a focused custom stack):
  ARP, IPv4, ICMP, UDP, TCP, DHCP, DNS, loopback; **BSD sockets** in libc.

- **M27 — Network services + firewall.**
  A demonstrator server (e.g. signed HTTP/echo daemon); packet-filter hooks tied
  to the **capability model** — a program's signed manifest declares its network
  scope (ports/hosts), enforced in-kernel. _Security:_ no unsigned program gets
  the network capability.

**Exit criteria J:** the OS gets an IP via DHCP, resolves names, serves and makes
TCP connections, with per-program network permissions.

---

## Phase K — Userland & (near) self-hosting

- **M28 — libc completeness + ported software.**
  Round out libc: stdio, malloc, string/math, time, errno, file I/O, sockets, a
  **pthreads subset** (on M19 threads). Port real software *from source* to prove
  the thesis: **dash** (or keep growing the native shell), **lua**, **sqlite**, a
  coreutils-like toolset. _Security:_ each ported tool is signed before it runs.

- **M29 — init + service manager + signed packages.**
  A real `init` (PID 1), service supervision, dependency ordering; a **signed
  package format** (the natural extension of the M9 signing root — install =
  verify + unpack into the FS). _Security:_ the package keyring + revocation is
  the distribution-level root of trust.

**Exit criteria K:** boots to a multi-service userland; can install and run signed
third-party software built from source.

---

## Phase L — Security hardening (cross-cutting, dedicated pass)

> _The differentiator._ Pieces land throughout, but this phase makes SECoS's
> security claims rigorous and provable.

- **M30 — Generalized capability/permission model.**
  Lift the M11 driver-capability idea to **all** processes: the signed manifest
  declares FS scope, network scope, IPC peers, and an allowed-syscall set; the
  kernel enforces them. Keyring with multiple keys, **revocation**, and a
  structured **audit subsystem** (`[AUDIT]` → a queryable log).

- **M31 — Exploit mitigations.**
  ASLR + KASLR; SMEP/SMAP; stack canaries; CFI; **W^X everywhere** (drop the
  0–512 MB identity RWX huge-page map and the low identity map; guard pages on
  every kernel/user stack); spectre/meltdown-class mitigations as feasible.

- **M32 — Boot integrity.**
  A measured/secure boot chain (verify the kernel + initial userland against the
  signing root; TPM-backed measurement later); reproducible builds so a running
  image is provably traceable to source (extends the existing `BUILD_TS`/`GIT_HASH`).

**Exit criteria L:** every executable and the boot chain are verified; processes
run under least-privilege capabilities; common exploit primitives are mitigated;
security events are audited.

---

## Big rocks & risks (size honestly)

- **SMP (M24)** — the largest correctness effort; touches every shared kernel
  structure. Budget generously; do the locking audit before, not after.
- **TCP/IP (M26)** — large; porting lwIP de-risks it vs. writing a stack.
- **Page cache / COW (M19–M20)** — subtle (refcounts, coherence); the M8 heap and
  M14 teardown bugs are a reminder to test frame accounting hard.
- **Journaling (M22)** — crash-consistency is easy to get subtly wrong; test with
  fault injection.

## Recommended near-term sequence (next few sessions)

1. **M15** — fault → kill process (stop halting the kernel). _Starting now._
2. **M16** — argv/env + exec + blocking wait.
3. **M17** — blocking primitives + pipes + TTY.

These three are low-risk, unblock everything, and turn SECoS from "runs signed
demos" into "runs a real interactive multi-process userland". SMP and networking
follow once the process/memory/IO model is solid.

## How this maps to the existing roadmap

Phases A–E (M0–M13) = the original mandatory plan, done. M14 = first stretch.
This document defines **Phases F–L (M15–M32)** as the path to a complete headless
OS. A GUI/display server, a window system, and desktop userland are explicitly
**out of scope here** and would be a later Phase M+.
