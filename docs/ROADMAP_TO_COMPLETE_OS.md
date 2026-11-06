# SECoS — Roadmap to a complete OS

_Strategic plan from the current state to a complete, networked, multi-process,
secure operating system. Written for a headless system; a GUI (secwm compositor,
LVGL) shipped in M40 anyway (`docs/GUI.md`)._
_Written 2026-06-18; realigned 2026-06-20; **realigned again 2026-11-04** to the
commit history through M39 (selftest 182/182 at `e66a57f`). Companion to `ROADMAP_SECoS.md` (technical) and
`docs/DEVELOPMENT_PLAN.md` (phases A–E)._

> **Reading note on milestone numbers.** This document was first written with
> *aspirational* milestone numbers (e.g. "M24 = SMP"). The project then shipped on
> a different numbering. This file now uses the **real** numbers: M0–M13 (A–E),
> M14 (demand paging), M15–M17 (F), M18–M20 (G), M21 (AHCI), M22 (NVMe+USB),
> M23 (POSIX FS), M24 (networking, pulled early), M25 (pipes+TTY), M26–M27 (H),
> M28–M29 (I = ACPI/APIC/TSC + SMP), M30 (signals), M31–M32 (K), **M33 (full
> ext4 + metadata_csum)**, **M34 (lua from source)**, **M35 (capabilities)**,
> **M36 (mitigations)**, **M37 (measured boot)**, **M38 (WiFi scaffold + WPA2
> crypto)**, **M39 (secure boot, full kernel W^X, user ASLR, POSIX foundation,
> bash from source + boot-into-bash, ath9k_hw port)**. An earlier draft of this
> file used M33–M35 for caps/mitigations/boot integrity — those shipped as
> M35–M37. Open work is tracked in `TASKS.md` (no pre-assigned numbers).

## Definition of "complete OS (no GUI)"

A system that:
1. **survives misbehaving userspace** (a faulting/abusive process is killed, not
   the kernel); ✅ (M15)
2. has a **real process model** — argv/env, exec, fork-or-spawn, signals,
   blocking wait, pipes, a TTY with job control; ✅ (M16/M17/M19/M25/M30, plus
   in-place `execve`, raw-mode TTY, ptys and poll/select on `integ/phase2`);
3. has a **complete virtual-memory API** — mmap/mprotect/brk, COW, shared memory,
   a unified page cache; ✅ (M18–M20) — `MAP_SHARED` still future;
4. has a **mature storage stack** — permissions/ownership, symlinks, journaling /
   crash consistency, pseudo-filesystems (dev/proc); ✅ (M23/M26/M27);
5. uses **modern interrupts and runs on multiple cores** (APIC/IOAPIC, SMP);
   ✅ APIC/TSC (M28); **SMP implemented but OFF by default** (M29 — AP bring-up
   opt-in since `3f776dd` after a real-HW freeze; needs TLB shootdown + audit);
6. is **networked** — a driver, a TCP/IP stack, BSD sockets; ✅ (M24);
7. ships a **usable userland** — a real libc, a shell, ported tools, an init /
   service manager; ✅ (M31/M32/M34/M39: libc, coreutils, signed packages,
   supervised init, lua + **GNU bash compiled from source**, bash as the boot
   shell; M40: `/bin/init` as PID 1 with supervised services) — _open:_
   multi-user login, more ports (sqlite, dash);
8. keeps its **security identity central** — everything signed, capability-rooted
   permissions, exploit mitigations, an audit trail; ✅ (M35–M37 + M39: caps +
   `[AUDIT]` + key revocation, SMEP/canaries/full W^X, measured + UEFI secure boot,
   user ASLR) — _open:_ SMAP by default, kernel KASLR, package keyring.

**Where we are:** all eight points are met at a first level; the remaining work
is depth (POSIX completeness for ports, robustness, SMP hardening, SMAP/KASLR) and
real-hardware bring-up (WiFi radio, NIC/UEFI validation). SECoS boots dual
UEFI/MB2 (UEFI path secure-booted), runs a higher-half fully-W^X kernel with
demand paging, **mandatory Ed25519 code signing as the root of trust**,
capability-confined processes, RW FAT32/ext2/ext4 (journaling + metadata_csum), a
full TCP/IP stack, and a signed userland whose login shell is GNU bash compiled
from source.

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
Async signal delivery (Ctrl-C → SIGINT) shipped later in M30.

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
Async Ctrl-C → SIGINT and shell pipelines/job control shipped in M30.

## ✅ Phase H — Storage & filesystem maturity — **DONE** (M26–M27)

POSIX metadata stored & exposed (mode/uid/gid/timestamps), chmod/chown/utimes,
ext2 fast symlinks, mount/umount syscalls (M26). **JBD2 journal replay** — safely
mount a dirty ext3/4 left by a crash (M27a). **Write-side journaling** — SecOS's
own metadata writes are crash-atomic (transaction → commit → checkpoint;
read-your-writes; atomic rollback), proven with a fault-injection harness (M27b).
`metadata_csum` RW followed in M33 (see below).

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
  (shell `stress [N]`). **⚠ Since `3f776dd` AP bring-up is OFF by default**:
  `stress 8` froze the real ASUS (2 cores) — likely a missing TLB shootdown or a
  lock race the QEMU/VMware paths never exposed. Default boot is single-core (LAPIC
  timer kept); opt in with `-DSECOS_SMP_AT_BOOT` or shell `smp`. The locking audit found one runtime shared-kernel-PT
  mutation (the M5 per-process kernel stack) → fixed with `kstack_lock`.
  _Open (polish):_ IPI reschedule, per-CPU run queues, task migration + TLB
  shootdown — see `docs/devlog/M29.md` and `TASKS.md`.

**Exit criteria I (met in QEMU/VMware; multicore opt-in on real HW):** boots and schedules on multiple cores with modern
interrupts; monotonic timekeeping; runs on bare-metal/VMware storage. _Driver Space
for IRQ/DMA (the old "M25" payoff) remains future work alongside Phase L._

---

# Shipped after Phase I (M30–M39)

## ✅ M30 — Signals & job control — **DONE**

Per-process pending/blocked masks + handler table delivered at the three
return-to-ring-3 chokepoints; custom handler frames via a libc `__sigreturn`
trampoline; `SIGACTION/SIGRETURN/KILL/SIGPROCMASK/SETPGID` (44–48). Async sources:
Ctrl-C/Ctrl-Z → SIGINT/SIGTSTP to the foreground pgid, SIGPIPE, SIGCHLD; blocking
syscalls return `-EINTR`. Shell pipelines, `&`, `jobs`/`fg`/`bg`, `kill`. See
`docs/devlog/M30.md`.

## ✅ Phase K — Userland & self-hosting — **DONE** (M31, M32, M34; bash in M39)

- **M31 — real libc + coreutils.** Standard headers + `user/libc.c` (printf
  family, FILE layer, string/stdlib/ctype, malloc/realloc/qsort); file syscalls
  (getdents/create/mkdir/unlink); a busybox-style 26-applet coreutils ELF in
  `/bin`. See `docs/devlog/M31.md`.
- **M32 — signed packages + supervised init.** `.spkg` (`tools/secos-pkg` +
  `kernel/pkg.c`): **install = verify the Ed25519 signature, then unpack**. Shell
  `pkg install`. A supervised `init` (restart-on-exit, bounded). See
  `docs/devlog/M32.md`.
- **M34 — lua 5.4.7 from source.** libc `%f/%e/%g`, `strtod`, `setjmp/longjmp`,
  `math.h`; SSE enabled for ring-3; lua compiled from source, signed, runs ring-3.
  See `docs/devlog/M34.md`.
- **M39 (part) — GNU bash 5.2 from source**, now the boot login shell (see M39).

_Open:_ PID-1 init wired into boot, package keyring + revocation, more ports
(sqlite, dash), real in-place `execve`.

## ✅ M33 — Full ext4 root + metadata_csum RW — **DONE**

crc32c for superblock / group descriptors / bitmaps / inodes / directory tails +
uninit_bg bookkeeping, validated byte-for-byte against `mkfs.ext4`: a default
`mkfs.ext4` volume is the e2fsck-clean persistent root. See `docs/devlog/M33.md`.

## ✅ Phase L — Security hardening — **DONE** (M35–M37, extended in M39)

- **M35 — Generalized capability model.** Opt-in least privilege rooted in the
  signed manifest (`CAP_ENFORCE` + `CAP_FS_READ/FS_WRITE/PROC/IPC/SIGNAL/TIME/NET`),
  `kernel/cap.{c,h}`, `[AUDIT]` ring + shell `audit`, key revocation. See
  `docs/devlog/M35.md`.
- **M36 — Exploit mitigations.** SMEP, stack canaries, W^X on the 0–512 MB low
  identity map, SMAP infrastructure (opt-in `-DSECOS_SMAP`). See
  `docs/devlog/M36.md`.
- **M37 — Boot integrity.** Measured boot (`boot_measure()` SHA-256 of
  `.text+.rodata`) + reproducible builds (`SOURCE_DATE_EPOCH`). See
  `docs/devlog/M37.md`.
- **M39 — extensions:** UEFI-loader **secure boot** (Ed25519-verifies the kernel),
  **full high-half kernel W^X** + boot auditor (the M36 routine had never been
  called), **user-space ASLR**.

_Not done:_ **kernel KASLR** (deferred — relocation + measured boot + two boot
paths), **SMAP by default** (fast-path read/write/send/recv need bounce
buffers), CFI, PIE user binaries (ELF-base ASLR), real `DRIVER_OP_MAP_MEM` /
IRQ-to-driver / DMA sandbox, TPM-backed measurement.

## 🔶 M38–M39 — WiFi (Atheros AR9565) — **scaffold + crypto done; radio PARKED**

M38: PCI attach + identify + WPA2 crypto (PBKDF2, AES) KAT-validated. M39: WPA2
4-way supplicant (KAT'd with a synthetic handshake) + the upstream Linux
**ath9k_hw** layer ported as-is via a compat shim (`drivers/ath9k_port/`).
init/reset/calibration/EDMA RX work on the real chip, but received frames fail CRC
(over-gain) — **parked**; real-hardware-only work. See `docs/devlog/M38.md`,
`docs/devlog/M39.md`.

---

# ▶▶ REMAINING WORK

Physical-hardware testing is **paused as of 2026-11-04**; current work targets
items that can be validated in QEMU. Since 2026-11-05 the work is organized in
four phases toward "a system that really works"; the actionable checklist is in
`TASKS.md`.

1. **Phase 1 — stability** (done, `integ/phase1`): soak harness, ring-3 syscall
   fuzzer, heap integrity checks, and the bugs they found.
2. **Phase 2 — bash usability** (done, `integ/phase2`): real in-place `execve`,
   raw termios + readline, ptys + sessions, `poll`/`select`, per-pid `/proc`,
   mid-path symlinks, BusyBox, Dropbear SSH. **Phase 2b** (done,
   `integ/phase2b`): `/bin/init` as PID 1 (inittab, DHCP from user space,
   supervised sshd, key baking, poweroff through init), GUI input + `/dev/fb0`,
   the secwm compositor with terminals on ptys, LVGL 9.2.2.
3. **Phase 3 — complete system.** Approved 2026-11-06, in this order:
   1. **Persistent ext4 root by default** for QEMU and the ISO, so users, keys
      and the SSH host key survive a reboot. The ext4 inode-slot reuse fix from
      `feat/init-pid1` is its prerequisite (done). **Done for QEMU in M41**
      (`docs/devlog/M41-persistent-root.md`); a real-disk installer for the ISO
      is still open.
   2. **Real multi-user management:** enforce uid/gid and file modes (root
      exempt); `login` with salted, hashed passwords in `/etc/shadow`;
      `useradd`, `passwd`, `su` (sudo optional); a home directory per user; SSH
      logs in as the matching user instead of always root. The model: **the code
      signature still decides *which code* may run; the uid decides *who* may
      access which files.** This **revises the M26 decision** ("POSIX metadata
      stored and exposed, not enforced", because the signature was the only
      trust boundary): the signature stays the boundary for code, and file
      access gains a second, uid-based one.
   3. **More filesystems:** exFAT read-write (the specification is public since
      2019); NTFS read-only first, read-write later, probably by porting Linux
      `ntfs3` (GPLv2) through a compat shim, as done for ath9k.

   Phase 3 also keeps: init/services hardening (DHCP renewal, dependencies,
   per-service limits), signed packages with a keyring + revocation, more ports
   (sqlite, dash).
4. **Phase 4 — hardening:** SMAP by default, kernel KASLR, removing the low
   identity map from user address spaces, the exec confinement rule (a confined
   process must not gain caps by exec'ing an ambient program), net stack
   locking, and SMP (TLB shootdown, IPI reschedule — prerequisites for SMP by
   default, which also needs real-HW re-validation).

**Real-hardware-only (paused):** ath9k RX/TX/assoc, UEFI loader on real firmware
(conflicting reports — re-verify), the SMP freeze, vmxnet3/igc/e1000e-MSI-X.

## Big rocks & risks (size honestly)

- **SMP hardening** — M29 works in QEMU/VMware but froze real hardware under load.
  **TLB shootdown** is absent (pinned per-CPU spaces) and is a hard prerequisite
  for re-enabling SMP by default, task migration, `MAP_SHARED` across cores, or
  any cross-core kernel-VA unmap.
- **In-place `execve`** — replaces the spawn+wait emulation; touches process
  identity, fd inheritance, signals and job control. Needed for correct bash
  semantics.
- **Kernel KASLR** — relocation with `-mcmodel=kernel` vs measured boot,
  reproducible builds and two boot paths.
- **WiFi radio** — blind register debugging on one real chip; parked.

A GUI was out of scope for this roadmap; a first one shipped in M40 anyway
(`/bin/secwm` + terminals + LVGL clients, `docs/GUI.md`). A fuller desktop
userland would be a later phase.

## How this maps to the existing roadmap

Phases A–E (M0–M13) = the original mandatory plan. M14 = first stretch. Phases
**F–L are done** (M15–M37), plus M38–M39 (WiFi scaffold, secure boot, full W^X,
ASLR, bash). What remains is depth and hardening (see REMAINING WORK) and the
paused real-hardware bring-up — the path from "a signed, hardened OS with a
from-source userland" to "a robust, self-hosting, multicore-on-real-HW OS".
