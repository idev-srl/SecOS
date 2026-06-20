# SECoS — Task List & Status

_Snapshot: HEAD `7d45b38` on `main` (pushed to `origin/main`). Self-test **173/173**._
_Last updated: 2026-06-20._

This file tracks what is **done** vs **open**, and the candidate work for the next
session. The narrative handoff lives in `next_session.md`; the long-term plan in
`docs/ROADMAP_TO_COMPLETE_OS.md`. This is the actionable checklist on top of those.

---

## ✅ DONE (milestones shipped on `main`)

| M | Area | State |
|---|------|-------|
| M0–M6 | Boot, paging/W^X, isolation, trapframe syscall, context switch | ✅ done |
| M7 | Ring-3 + `SYS_YIELD` cooperative scheduling | ✅ `M7_STABLE` |
| M8 | Preemptive multitasking + `SYS_EXIT`/reap + idle task | ✅ done |
| M9 | In-kernel crypto (SHA-256/512 + Ed25519) + mandatory ELF signing + userland | ✅ `M9_STABLE` |
| M10 | Storage: virtio-blk + FAT32/ext2/ext4 RW + VFS multi-mount + `run` | ✅ done |
| M11 | Driver Space rooted in code signature (`.note.secos` v2, per-binding caps) | ✅ done |
| M12 | Memory scalability + W^X hard gate + **higher-half kernel** | ✅ done |
| M13 | `max_mem` enforced at load + `SYS_GETTICKS` + kernel IPC channels | ✅ done |
| M14 | Full demand paging (per-process VMAs, lazy fault-in) | ✅ done |
| M15 | Fault-driven process kill (ring-3 fault kills only the process) — **Phase F** | ✅ done |
| M16 | Exec model: argv/env + blocking `SYS_WAIT` — Phase F | ✅ done |
| M17 | Blocking primitives: `SYS_SLEEP` + blocking `SYS_MSG_RECV` — Phase F | ✅ done |
| M18 | Dynamic memory: `mmap`/`munmap`/`mprotect`/`brk` — **Phase G** | ✅ done |
| M19 | Copy-on-write `fork` + PMM frame refcounts — Phase G | ✅ done |
| M20 | Unified page cache backing `read()` and file-backed `mmap` — Phase G | ✅ done |
| M21 | AHCI/SATA block driver (`sda`..`sdd`) | ✅ `M21_STABLE` |
| M22 | NVMe (`nvme0n1`) + USB stack (xHCI / HID keyboard / Mass Storage) | ✅ `M22_STABLE` |
| M23 | POSIX FS personality: persistent ext2 root + `/dev` + `/proc` + `/sys` | ✅ done |
| M24 | **Networking** (Phase J, pulled early): e1000/e1000e, ARP/IPv4/ICMP/UDP/DHCP/DNS/TCP, BSD sockets + CAP_NET, MSI-X/NAPI RX | ✅ done, validated on real VMware |
| M25 | Pipes (`SYS_PIPE`) + console TTY cooked mode + TCP window 8K→64K | ✅ done |
| M26 | **VFS maturity** (Phase H pt1): POSIX metadata, chmod/chown/utimes, symlinks, mount/umount | ✅ done |
| M27a | JBD2 journal **replay** (mount a dirty ext3/4 left by a crash) | ✅ done |
| M27b | **Write-side journaling** (SecOS's own metadata writes crash-atomic) — **Phase H complete** | ✅ done |
| M28 | **Modern platform** (Phase I pt1–3): ACPI discovery + **APIC switchover** (LAPIC timer + IOAPIC retire the 8259) + **TSC monotonic clock** | ✅ done |
| M29 | **SMP / multicore** (Phase I final): per-CPU data, AP bring-up, CPU-pinned scheduling — **user tasks run on multiple cores**; verified on real VMware | ✅ done |
| M31 | **Real C library + coreutils** (Phase K): standard headers + `user/libc.c` (printf/string/stdlib/ctype/stdio/dirent); file syscalls (getdents/create/mkdir/unlink); 26-applet busybox-style coreutils installed to `/bin` | ✅ done |
| M32 | **Signed packages + init** (Phase K): `.spkg` format (`tools/secos-pkg` + `kernel/pkg.c`, verify+unpack, tampered refused); supervised `init` service-manager + demo service | ✅ done |

**Phases done:** A–E (M0–M13), F (M15–M17), G (M18–M20), H (M26–M27), I (M28–M29),
J/networking (M24), **K (M31–M32)**. Foundation, process model, VM, storage
maturity, modern platform + multicore, networking, and a real signed userland are
all in. **Remaining: M30 (signals) and Phase L (security hardening).**

---

## 🔶 DONE-BUT-UNVALIDATED (works in QEMU / logic only — needs real hardware)

- [ ] **vmxnet3 driver** — compile-clean, QEMU has no model → untested on real HW.
- [ ] **igc driver (2.5 GbE, I225/I226)** — compile-clean, needs the real adapter.
- [ ] **82574 (e1000e) MSI-X re-arm tuning** — IVAR/EIAC so the poll backstop can
      eventually be dropped (currently a hybrid: MSI-X + poll backstop).
- [ ] **USB hub support** (`drivers/usb_hub.c`) — logic-verified only; QEMU CI has
      no hub. DA TESTARE su hardware reale.
- [ ] **MSI/MSI-X for NVMe/xHCI** — additive, OFF by default (storage still polled);
      gated behind `-DXHCI_USE_IRQ`/`-DNVME_USE_IRQ`. NVMe I/O CQ still IEN=0 →
      needs recreate with IEN=1 + vector to truly fire. Needs HW validation.
- [ ] **USB Mass Storage on real hardware** — verified in QEMU, not yet on a real stick.

---

## 🎯 CANDIDATE NEXT WORK (pick one to start)

### Option 1 — **Signals** _(recommended — the biggest usability gap)_
The process model is complete *except* async signals. Today a ring-3 fault is the
only way a process dies asynchronously (M15); Ctrl-C only interrupts an in-progress
read, and there is no SIGPIPE. This blocks real interactive use.
- [ ] **Signal delivery mechanism** — per-process pending mask + handler table;
      deliver on return-to-user (build on the M16/M17 block/wake + trapframe core).
- [ ] **Ctrl-C → SIGINT** — the TTY (`kernel/tty.c`) raises SIGINT on the
      foreground process; default action = terminate (reuse M15's kill path).
- [ ] **SIGPIPE** — a write to a pipe/socket with no reader raises it (pipes/EPIPE
      already exist in `kernel/pipe.c`).
- [ ] **`SYS_KILL` + libc `signal`/`kill`/`sigaction`** (minimal subset).
- [ ] **Shell job control + pipelines** (`cmd1 | cmd2`, Ctrl-C, `&`) — unblocked by
      this + the existing pipes/fork/exec.

### Option 2 — SMP hardening / polish _(multicore works; make it better)_
- [ ] **IPI-based reschedule** — a wake on another core preempts immediately
      instead of at the next tick (currently up to 1 ms latency). `lapic_ipi` exists.
- [ ] **Per-CPU run queues** — today one shared proc table under `proc_lock`; fine
      at 2–8 cores, but a per-CPU queue scales better and cuts contention.
- [ ] **Load balancing / task migration** — affinity is static round-robin; a
      migrating scheduler needs an **IPI TLB shootdown** first (see `docs/devlog/M29.md`).
- [ ] **debugcon line-locking everywhere** — only `switch_to`'s `[SMP]` line is
      atomic now; other concurrent boot/runtime debug can still interleave.

### Option 3 — NIC hardware validation _(needs real adapters)_
- [ ] Validate vmxnet3 on real VMware; igc (2.5 GbE) on real I225/I226.
- [ ] Tune 82574 MSI-X re-arm; try dropping the poll backstop.

### Option 4 — finish Phase K self-hosting _(libc done; ports remain)_
Phase K core is **done** (M31 libc+coreutils, M32 signed packages + supervised
init). Remaining toward true self-hosting:
- [ ] **`open(O_CREAT)` creates** (today use `creat_file`); `getcwd`/per-process cwd
      + an environment (so `pwd`/`getenv` work in userland, not just the shell).
- [ ] **libc gaps** — `printf` floats (`%f/%g`), fuller stdio/locale, `setjmp`/
      `longjmp` (needed by lua/sqlite), a `math.h`, a pthreads subset (on M19).
- [ ] **Port a real program from source** (lua or sqlite) and sign it — the
      headline proof of the "compile OSS from source" thesis.
- [ ] **Package keyring + revocation**; a real PID-1 `init` wired into boot
      (today `/bin/init` is run on demand); dependency ordering.

### Option 5 — Phase L: security hardening _(the differentiator, dedicated pass)_
- [ ] **Generalized capability model** — lift M11's driver-caps to all processes:
      the signed manifest declares FS/network/IPC/syscall scope, kernel-enforced.
      Keyring + revocation + a queryable `[AUDIT]` subsystem.
- [ ] **Exploit mitigations** — ASLR/KASLR, SMEP/SMAP, stack canaries, CFI,
      **W^X everywhere** (drop the 0–512 MB identity RWX huge-page map).
- [ ] **Boot integrity** — measured/secure boot chain; reproducible builds.

---

## 🌍 Post-roadmap: ARM64 port (DECIDED, deferred — see `docs/ARM64_PORT.md`)
- [ ] **AArch64 backend for RPi/SBC**, started **after the x64 roadmap is done**.
      Not a recompile — a second `arch/arm64` backend (boot/MMU/GIC/timer/context
      switch/syscall/MMIO drivers). First bring-up target: **QEMU `virt`**.
- [ ] **Forward discipline meanwhile** (cheap, only when touching those files):
      trapframe accessors over `tf->rdi`/`tf->rax`; no new port-I/O outside
      `drivers/`+`arch/`; page-table bits + higher-half constant stay in `mm/vmm.*`.

---

## 🧹 Smaller open items / tech debt (independent of milestone choice)

- [ ] **Makefile: no header-dep tracking** — `.h`/`-D` changes need a manual
      `make clean`. Proper dependency tracking would remove a recurring footgun.
- [ ] **`isodir/boot/grub/grub.cfg` is tracked but `make clean` deletes it** —
      consider untracking it (recurring papercut).
- [ ] **Heap allocator** (`mm/heap.c`) is simple first-fit (+ a global lock since
      M29) — a buddy/free-list allocator would scale better under SMP contention.
- [ ] **`MAP_SHARED` + shared read-only text** — retire the M14 per-process pinned
      ELF image via the M20 page cache.
- [ ] **TLB shootdown infrastructure** — not needed today (pinned per-CPU spaces;
      see `docs/devlog/M29.md`) but a **prerequisite** for task migration,
      `MAP_SHARED` across cores, or any cross-core kernel-VA unmap.
- [ ] **W^X of the 0–512 MB identity huge-page map** — still RWX (deferred since M12).
- [ ] **Mid-path symlink components** — `vfs_lookup_follow` follows only the final
      component (M26 limitation).
- [ ] **`metadata_csum`** — M27b journals to `^metadata_csum` volumes only; csum
      support is an orthogonal follow-up.
- [ ] **Driver Space depth**: real `DRIVER_OP_MAP_MEM` (still a validating stub);
      IRQ-to-driver (`IRQ_SUBSCRIBE` + IPC queue); DMA sandbox; auto-restart.
- [ ] **POSIX completeness**: `ioctl`, fuller libc, wider syscalls (poll/select/
      fcntl), per-pid `/proc/<pid>`.

---

## ⚠️ Process reminders (don't relearn the hard way)
- **`make clean` after any `.h` or `-D` change** — Makefile ignores those.
- **NIC RX only works in the shell/idle context** — test networking from the shell.
- **Don't build concurrently with `tools/selftest.sh`** (shared build dir + /tmp logs).
- **Never `pkill -f "qemu-system"`** in a command that also launches qemu.
- **SMP testing**: `-smp 2/4`; shell `stress [N]` saturates the vCPUs. The user
  validates on **real VMware** (2+ vCPUs), not QEMU.
- **Runtime kernel page-table edits must be locked** (the M5 kstack race, fixed in
  `28ff58a`): any new runtime kernel-VA map/unmap needs `kstack_lock` (or a shared
  kernel-PT lock) + an IPI shootdown if another core can touch the VA.
- Talk to the user in **Italian**; all project content in **English only**.
