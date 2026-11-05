# SECoS — Task List & Status

_Snapshot: realigned to the commit history through `ae2be37` (M39 + ath9k RX
bring-up). Last full self-test **182/182** (at `e66a57f`)._
_Last updated: 2026-11-04._

This file tracks what is **done** vs **open**, and the planned order of work. The
narrative handoff lives in `next_session.md`; the long-term plan in
`docs/ROADMAP_TO_COMPLETE_OS.md`. This is the actionable checklist on top of those.

---

## 🧭 Process note (2026-11-04)

**Physical-hardware testing is paused as of 2026-11-04.** Current work targets
items that can be **validated in QEMU** (smoke tests + `tools/selftest.sh` +
serial). Real-hardware-only items are parked in their own section below and are
resumed only when hardware testing restarts.

### Planned order (QEMU-validatable)
1. **Selftest coverage M34–M39 + docs realignment** (in progress — this file,
   `ROADMAP_TO_COMPLETE_OS.md`, `CLAUDE.md`, `docs/BASH_PORT.md`, `docs/devlog/M39.md`).
2. **bash / shell usability:** real in-place `execve`, raw-mode termios in
   `kernel/tty.c`, `poll`/`select`, `ioctl` coverage, mid-path symlink following
   (`fs/vfs.c`), per-pid `/proc/<pid>`.
3. **Kernel robustness:** buddy heap, `MAP_SHARED`, PID-1 init in boot, package
   keyring + revocation.
4. **More source ports:** sqlite, dash.
5. **Security depth:** SMAP by default, kernel KASLR.
6. **SMP prep** (TLB shootdown, IPIs) validated in QEMU `-smp 4`.

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
| M29 | **SMP / multicore** (Phase I final): per-CPU data, AP bring-up, CPU-pinned scheduling; verified on real VMware. **⚠ AP bring-up OFF by default since `3f776dd`** (froze the real ASUS under `stress 8`); opt-in via `-DSECOS_SMP_AT_BOOT` or shell `smp`; no TLB shootdown yet | ✅ done, **opt-in** |
| M30 | **Signals + job control + shell pipelines**: POSIX signals for ring-3, Ctrl-C/Ctrl-Z, pipelines `a\|b`, `&`/`jobs`/`fg`/`bg`, signal-based `kill` | ✅ done |
| M31 | **Real C library + coreutils** (Phase K): standard headers + `user/libc.c` (printf/string/stdlib/ctype/stdio/dirent); file syscalls (getdents/create/mkdir/unlink); 26-applet busybox-style coreutils installed to `/bin` | ✅ done |
| M32 | **Signed packages + init** (Phase K): `.spkg` format (`tools/secos-pkg` + `kernel/pkg.c`, verify+unpack, tampered refused); supervised `init` service-manager + demo service | ✅ done |
| M33 | **Full ext4 root + metadata_csum (RW)**: crc32c for sb/gd/inode/dir/bitmaps + uninit_bg; default `mkfs.ext4` volume is the e2fsck-clean persistent root | ✅ done |
| M34 | **Phase K self-hosting tail**: libc float (`%f/%g`)/`strtod`/`setjmp`/`math.h`/SSE; **lua 5.4.7 compiled FROM SOURCE**, signed, runs ring-3 | ✅ done |
| M35 | **Generalized capability model** (Phase L): least-privilege opt-in caps in the signed manifest, kernel-enforced + `[AUDIT]` + key revocation | ✅ done |
| M36 | **Exploit mitigations** (Phase L): SMEP, stack canaries, W^X low identity map, SMAP infra (opt-in) | ✅ done |
| M37 | **Boot integrity** (Phase L): measured boot (kernel self-hash) + reproducible builds (`SOURCE_DATE_EPOCH`) | ✅ done |
| M38 | **WiFi AR9565 (ath9k) scaffold + WPA2 crypto**: PCI attach/identify + PBKDF2 PMK + AES (KAT-validated) | ✅ scaffold + crypto done |
| M39 | **Secure boot** (UEFI loader verifies kernel, `61f52c7`) + **full kernel W^X** + auditor (`021d445`) + **user ASLR** (`e5a991c`) + **WPA2 4-way supplicant** (`7ee71d1`) + **POSIX foundation** (syscalls 49–56: dup/dup2/chdir/getcwd/ioctl/getppid/waitany/fstat, cwd, termios, environ, emulated execve) + **GNU bash 5.2 from source = boot login shell** (`2881cf4`/`8762198`/`e66a57f`) + **ath9k_hw Linux driver port** (init/reset/cal/RX on real chip; RX CRC wall → parked). See `docs/devlog/M39.md` | ✅ done (WiFi radio parked) |

**Phases done:** A–E (M0–M13), F (M15–M17), G (M18–M20), H (M26–M27, + M33),
I (M28–M29, SMP opt-in), J/networking (M24), K (M31–M32, M34, bash in M39),
**L (M35–M37, extended in M39)**, M30 (signals). Remaining work is depth,
robustness and the paused real-hardware bring-up — see below.

### Previously-open checklist items now shipped (for the record)
- [x] Signal delivery + Ctrl-C→SIGINT + SIGPIPE + `SYS_KILL`/`sigaction` + shell job control/pipelines (M30)
- [x] `open(O_CREAT/O_TRUNC/O_APPEND)` + shell `>`/`>>` redirection (`0491f81`)
- [x] Per-process cwd + `getcwd`/`chdir` + a real `environ`/`getenv`/`setenv` (M39)
- [x] libc `printf` floats, `strtod`, `setjmp`/`longjmp`, `math.h` (M34)
- [x] Port a real program from source and sign it — lua 5.4.7 (M34), GNU bash 5.2 (M39)
- [x] Generalized capability model + `[AUDIT]` + **code-key** revocation (M35)
- [x] SMEP, stack canaries (M36); user ASLR (M39)
- [x] W^X of the 0–512 MB identity map (M36) + full high-half kernel W^X (M39) — W^X everywhere
- [x] Measured boot + reproducible builds (M37); UEFI secure boot of the kernel (M39)
- [x] `metadata_csum` read-write (M33)
- [x] `ioctl` (termios/winsize/pgrp subset) + `fcntl` (M39)
- [x] Fork/pipes under bash (Unix zombie semantics + `SYS_WAITANY`, `8762198`)

---

## 🟢 OPEN — QEMU-validatable (current focus)

### Shell / POSIX usability (bash)
- [ ] **Real in-place `execve`** — today emulated as spawn+wait+exit (`user/libsecos.c`):
      pid changes, env not inherited, job control under bash is approximate.
- [ ] **Raw-mode termios** — `TCSETS` sets `g_tty_canon`/`g_tty_echo`
      (`kernel/syscall.c`) but `kernel/tty.c` never consults them; console is always cooked.
      Prerequisite for readline in bash.
- [ ] **`poll`/`select`** syscalls (disabled in bash's `config.h` today).
- [ ] **Wider `ioctl` coverage** beyond TCGETS/TCSETS/TIOCGWINSZ/TIOCG/SPGRP.
- [ ] **Mid-path symlink components** — `vfs_lookup_follow` (`fs/vfs.c`) follows only
      the final component (M26 limitation).
- [ ] **Per-pid `/proc/<pid>`** — procfs has only `/proc/self` + global files.
- [ ] **Serial input drops the first char of a line** under bash in QEMU (PS/2 is fine) —
      cosmetic, but hurts serial-driven tests.
- [ ] pthreads subset (on M19 COW/fork), fuller stdio/locale.

### Kernel robustness
- [ ] **Buddy / free-list heap** — `mm/heap.c` is still a single-lock first-fit list.
- [ ] **`MAP_SHARED` + shared read-only text** — retire the M14 per-process pinned ELF
      image via the M20 page cache (cross-core use needs TLB shootdown).
- [ ] **PID-1 init wired into boot** — `/bin/init` is run on demand today (boot launches bash).
- [ ] **Package keyring + revocation** — `kernel/pkg.c` verifies against the single
      `secos_trusted_pubkey`; the M35 key-revocation list (`mm/elf_sign.c`) covers ELFs only.
- [ ] Dependency ordering for packages/services.

### More source ports
- [ ] **sqlite** from source (signed).
- [ ] **dash** from source (signed).

### Security depth
- [ ] **SMAP by default** — needs bounce-buffering in the read/write/send/recv fast paths
      (they pass user buffers straight to vfs/pipe/socket); opt-in `-DSECOS_SMAP` today.
- [ ] **Kernel KASLR** — deferred in M39 (relocation with `-mcmodel=kernel` vs measured
      boot, reproducible builds, both boot paths).
- [ ] ELF-base ASLR (needs PIE user binaries); CFI.
- [ ] **Driver Space depth**: real `DRIVER_OP_MAP_MEM`; IRQ-to-driver (`IRQ_SUBSCRIBE` +
      IPC queue); DMA sandbox; auto-restart. (Ported ath9k currently runs ring-0.)

### SMP prep (QEMU `-smp 4`)
- [ ] **TLB shootdown infrastructure** (IPI-based) — prerequisite for re-enabling SMP by
      default, task migration, `MAP_SHARED` across cores, cross-core kernel-VA unmap.
- [ ] **IPI-based reschedule** — a wake on another core preempts immediately (`lapic_ipi` exists).
- [ ] **Per-CPU run queues**; then load balancing / task migration.
- [ ] **debugcon line-locking everywhere** (only `switch_to`'s `[SMP]` line is atomic).

### Test coverage
- [ ] **Selftest coverage for M34–M39** (lua, caps, mitigations, measured boot, WPA2 KATs,
      W^X audit, ASLR, POSIX demo, bash) — in progress in `tools/selftest.sh`.

---

## 🔴 OPEN — requires real hardware (PAUSED since 2026-11-04)

- [ ] **ath9k radio (AR9565 on the ASUS E406S)** — RX frames fail CRC (over-gain / high
      NF); then TX, auth/assoc MLME, WPA2 4-way (supplicant ready), DHCP. PARKED; first
      step on resume = `wifi up; wifi chan 6; wifi regdump`. See `docs/devlog/M39.md`.
- [ ] **UEFI loader on real firmware — conflicting reports, needs re-verification on the
      ASUS** (possible regression from the secure-boot commit `61f52c7`). `afb7f3c` and the
      2026-06-21 real-HW hardening entry in `next_session.md` say it was fixed/validated on
      the ASUS; the newest `next_session.md` entry (WiFi, 2026-06-22) says "GRUB ISO only
      (UEFI loader triple-faults on real HW)". Do not assume either until re-tested.
- [ ] **SMP freeze on real HW** (`stress 8` on the ASUS, `3f776dd`) — must be root-caused
      and re-validated on hardware before AP bring-up is re-enabled by default.
- [ ] **vmxnet3 driver** — compile-clean, QEMU has no model → untested.
- [ ] **igc driver (2.5 GbE, I225/I226)** — compile-clean, needs the real adapter.
- [ ] **82574 (e1000e) MSI-X re-arm tuning** — IVAR/EIAC so the poll backstop can be
      dropped (currently MSI-X + poll backstop hybrid).
- [ ] **USB hub support** (`drivers/usb_hub.c`) — logic-verified only; QEMU CI has no hub.
- [ ] **MSI/MSI-X for NVMe/xHCI** — OFF by default (`-DXHCI_USE_IRQ`/`-DNVME_USE_IRQ`);
      NVMe I/O CQ still IEN=0 → needs recreate with IEN=1 + vector.
- [ ] **Async Ctrl-C/Ctrl-Z + fg/bg of a stopped job** on a PS/2 keyboard (M30; serial
      can't async-interrupt).

---

## 🌍 Post-roadmap: ARM64 port (DECIDED, deferred — see `docs/ARM64_PORT.md`)
- [ ] **AArch64 backend for RPi/SBC**, started **after the x64 roadmap is done**.
      Not a recompile — a second `arch/arm64` backend (boot/MMU/GIC/timer/context
      switch/syscall/MMIO drivers). First bring-up target: **QEMU `virt`**.
- [ ] **Forward discipline meanwhile** (cheap, only when touching those files):
      trapframe accessors over `tf->rdi`/`tf->rax`; no new port-I/O outside
      `drivers/`+`arch/`; page-table bits + higher-half constant stay in `mm/vmm.*`.

---

## 🧹 Build / tooling tech debt

- [x] **Makefile: header-dep tracking** — kernel objects use `-MMD -MP`, and a
      `.cflags_extra` stamp rebuilds them when `CFLAGS_EXTRA` changes.
      `make check-user-progs` flags stale committed `crypto/user_*_elf.h`.
- [ ] **`isodir/boot/grub/grub.cfg` is tracked but `make clean` deletes it** —
      consider untracking it (recurring papercut).
- [ ] **Prebuilt `drivers/ath9k_port/ath9k_hw.o` is committed** (`git add -f`) —
      regenerate with `tools/port-ath9k.sh` after touching the port.

---

## ⚠️ Process reminders (don't relearn the hard way)
- **Embedded programs are committed artifacts** — after a libc/ABI change run
  `make user-progs` (+ `port-bash`/`port-lua`) and commit; `make check-user-progs`.
- **NIC RX only works in the shell/idle context** — test networking from the shell.
- **Don't build concurrently with `tools/selftest.sh`** (shared build dir + /tmp logs).
- **Never `pkill -f "qemu-system"`** in a command that also launches qemu.
- **SMP testing**: default boot is single-core; build with `-DSECOS_SMP_AT_BOOT` or run
  shell `smp`, then `-smp 2/4` + `stress [N]`.
- **Runtime kernel page-table edits must be locked** (the M5 kstack race, fixed in
  `28ff58a`): any new runtime kernel-VA map/unmap needs `kstack_lock` (or a shared
  kernel-PT lock) + an IPI shootdown if another core can touch the VA.
- Talk to the user in **Italian**; all project content in **English only**.
