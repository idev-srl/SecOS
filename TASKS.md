# SECoS — Task List & Status

_Snapshot: realigned to the commit history through `ae2be37` (M39 + ath9k RX
bring-up). Last full self-test **182/182** (at `e66a57f`)._
_Last updated: 2026-11-06 (`integ/phase2b`: M40 init as PID 1 + GUI input + secwm compositor merged; Phase 3/4 plan approved)._

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
The work is organized in four phases (user decision, 2026-11-05: "a system that
really works"):

1. **Phase 1 — stability:** bug-finding harness (soak, sysfuzz), heap integrity,
   fix what they find. Done on `integ/phase1`; the soak and fuzzer keep running.
2. **Phase 2 — bash usability:** done on `integ/phase2` (real `execve`, raw termios +
   readline, ptys + sessions, `poll`/`select`, per-pid `/proc`, mid-path symlinks,
   BusyBox userland, Dropbear SSH with pty sessions). **Phase 2b** (remote + GUI) done
   on `integ/phase2b`: `/bin/init` as PID 1 with supervised sshd, GUI input/fb0, the
   secwm compositor, LVGL.
3. **Phase 3 — complete system** (approved 2026-11-06, in this order):
   1. persistent ext4 root by default;
   2. real multi-user management;
   3. more filesystems (exFAT RW, NTFS RO then RW);
   then init/services hardening, signed packages (keyring), more ports.
   Details in the "Phase 3" section below.
4. **Phase 4 — hardening:** SMAP by default, kernel KASLR, no identity map in user
   address spaces, the exec confinement rule, net stack locking, SMP (TLB shootdown,
   IPIs). Details in the "Phase 4" section below.

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
- [x] **Real in-place `execve`** — `SYS_EXECVE` (60) + `FD_CLOEXEC`/`fcntl` (61) +
      `pipe2`; libc's spawn+wait+exit emulation removed (`docs/BASH_PORT.md`).
- [x] **Raw-mode termios** + GNU readline in bash, VT100 console, ptys/sessions
      (`kernel/tty.c`, `kernel/pty.c`, `drivers/vt.c`; `docs/PTY.md`).
- [x] **`poll`/`select`** (`SYS_POLL` 78; bash/readline built with `HAVE_SELECT`).
- [x] **Wider `ioctl` coverage** (termios, winsize, FIONREAD/FIONBIO, job control, sessions, ptmx).
- [x] **Mid-path symlink components** (`vfs_walk`).
- [x] **Per-pid `/proc/<pid>`** + `/dev/kmsg`, wall clock, statfs, rename/link/truncate.
- [x] **BusyBox 1.36.1** on the real kernel APIs (`docs/BUSYBOX_PORT.md`), **Dropbear SSH**
      with pty sessions (`docs/SSH.md`). Integrated + validated on `integ/phase2`.
- [ ] Symlinks / hard links on the ramfs root (BusyBox `ln`, applet symlinks).
- [ ] Rare flake: M10/fat32 once ran `hello` a second time after its `SYS_EXIT` and
      faulted (1 of ~11 runs; 8/8 reruns clean) — zombie resumed? Needs a repro.
- [ ] Signal handler frames do not save the x87/SSE state (the scheduler does since
      integ/phase2): a handler using doubles can clobber the interrupted code's XMM.
- [ ] **Serial input drops the first char of a line** under bash in QEMU (PS/2 is fine) —
      cosmetic, but hurts serial-driven tests.
- [ ] pthreads subset (on M19 COW/fork), fuller stdio/locale.

### GUI (Phase 2b: compositor + LVGL terminal on a PTY)
- [x] **Kernel half (M40, `feat/gui-input`)**: PS/2 mouse (IRQ12 via IOAPIC), USB HID
      mouse + `usb-tablet` (report-descriptor parser), `/dev/input/eventN` (Linux evdev:
      EVIOCG*, grab, SYN_DROPPED, poll/O_NONBLOCK), `/dev/fb0` (fbdev ioctls + shared NX
      device mmap), `KDSETMODE` console handoff with auto-restore on exit/crash, keymaps
      `us`/`it` (`loadkeys`, `make KEYMAP=it`), explicit `CAP_INPUT`/`CAP_DISPLAY`
      manifest bits. API contract: `docs/GUI_INPUT.md`; devlog `docs/devlog/M40-gui-input.md`.
- [x] **Compositor + graphical terminal + LVGL (`feat/compositor`)**: `/bin/secwm` =
      `/bin/startgui` (fb0 + EVIOCGRAB, damage compositing, panel/launcher, windows),
      terminals = bash on locked ptys with job control, SWP client protocol over pipes
      with a confined-manifest client sandbox, LVGL 9.2.2 `/bin/sysmon`. `docs/GUI.md`,
      selftest "GUI compositor gate".
- [x] GUI autostart through init: `make iso GUI_AUTOSTART=1` adds `gui:once:/bin/startgui --auto`
      to the default `/etc/inittab` (never respawned; leaving the GUI returns to the console bash).
- [ ] GUI: clipboard (compositor-mediated, explicit user action only), terminal text selection.
- [ ] GUI: shared-memory buffers for clients (memfd-style object passed at spawn, syscalls
      90-94) if a client needs more than pipe bandwidth.
- [ ] GUI: mouse reporting in the terminal (`?1000/?1006`) for `vi`/`less` mouse support;
      wide (CJK) characters; blinking cursor.
- [ ] GUI: software key autorepeat for USB keyboards (no host-side repeat yet).
- [ ] GUI: client windows resizable (a CONFIGURE round trip); keyboard window move.
- [x] `/proc/meminfo` under UEFI reported MemFree > MemTotal: the PMM started all-free, so the
      holes between EFI regions (loader data, ACPI, runtime services) were allocatable. Fixed in
      `mm/pmm.c` (start all-used, free only the reported regions).
- [ ] USB keyboard host-side autorepeat; keyboard LEDs (Caps/Num Lock).
- [ ] fb0 double buffering / page flip (`yres_virtual > yres`), vsync; mode setting.
- [ ] VT switching (several consoles / graphics sessions).

### Phase 3 — complete system (approved 2026-11-06, in this order)
1. [ ] **Persistent ext4 root by default** for QEMU (`make run*`) and the ISO: users,
   keys, `/etc` and the SSH host key survive a reboot. Builds on `make sysdisk-ext4`,
   `ext2_mount_root` + `/.secosroot` (M23/M33) and the init `/etc/inittab` "install
   only when absent" rule. **Prerequisite (done):** the ext2/ext4 VFS inode-slot reuse
   fix from `feat/init-pid1` (a service log fd once wrote into `/etc/passwd`). Open:
   first-boot provisioning (create + format the root when no marked volume exists),
   ISO + data-disk layout, host-key generation on first boot, fsck-on-dirty policy.
2. [ ] **Real multi-user management.** Model: **the code signature still decides
   *which code* may run; the uid decides *who* may access which files.** This
   **revises the M26 decision** "POSIX metadata stored & exposed, not enforced":
   - [ ] enforce uid/gid + file modes (owner/group/other rwx, directory x/w, sticky
         `/tmp`) in the VFS; uid 0 (root) is exempt; per-process uid/gid/euid +
         supplementary groups, inherited across fork/exec; `setuid`/`setgid` syscalls
         (root only);
   - [ ] `login` with salted, hashed passwords in `/etc/shadow` (mode 0600; a slow KDF,
         e.g. PBKDF2-SHA-512 on the in-kernel/libc crypto, or yescrypt/bcrypt);
         `useradd`, `passwd`, `su` (sudo optional);
   - [ ] a home directory per user (`/home/<user>`, `/root` for root);
   - [ ] SSH logs in as the matching user (Dropbear multi-user mode,
         `~user/.ssh/authorized_keys`) instead of always root;
   - [ ] init's console runs `login` (or a getty) instead of a root bash, opt-out for dev.
3. [ ] **More filesystems:**
   - [x] **exFAT read-write** (the specification is public since 2019): boot-sector
         checksum, FAT + allocation bitmap, up-case table, directory entry sets with
         name hashes; validated host-side with `fsck.exfat` — **M41**, `fs/exfat.c`,
         `docs/devlog/M41-exfat.md` (open: one volume at a time, TexFAT RO, 4 KiB-sector
         volumes untested; found on the way: `SYS_MOUNT` hands fat32/ext2 a stack
         mount-point pointer, `fat32` page-cache staleness on inode-slot reuse,
         constant `st_dev` breaks `df <path>`);
   - [ ] **NTFS read-only first, read-write later** — probably by porting Linux `ntfs3`
         (GPLv2) through a compat shim, as done for ath9k (`drivers/ath9k_port/`).
4. [ ] **init / services hardening:** DHCP lease renewal, service dependencies beyond
   file order, per-service caps/limits, a GUI session that survives a console logout.
5. [ ] **Signed packages**: package keyring + revocation (see Kernel robustness).
6. [ ] **More ports** (see More source ports).

### Kernel robustness
- [ ] **Buddy / free-list heap** — `mm/heap.c` is still a single-lock first-fit list.
- [ ] **`MAP_SHARED` + shared read-only text** — retire the M14 per-process pinned ELF
      image via the M20 page cache (cross-core use needs TLB shootdown).
- [x] **PID-1 init wired into boot** (M40, `docs/devlog/M40-init.md`): `/bin/init` is PID 1,
      reads `/etc/inittab`, takes a DHCP lease via `SYS_NETCTL` (95), supervises Dropbear,
      respawns the console bash, reaps orphans, orderly poweroff; keys baked with
      `make iso SSH_AUTHORIZED_KEYS=...`. Follow-ups: DHCP lease renewal, service
      dependencies beyond file order, per-service caps/limits.
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

### Phase 4 — hardening (after Phase 3)
Collects the items below plus three that have no entry yet:
- [ ] SMAP by default and kernel KASLR (see Security depth).
- [ ] **No low identity map in user address spaces**: every user PML4 still carries
      the supervisor-only 0–512 MB identity map (uaccess rejects aliasing pointers since
      `c536c03`, but the mapping itself remains); move its users (virtio DMA, early frame
      access) to the physmap and drop it from user spaces.
- [ ] **Exec confinement rule**: a process's caps come only from its own image's
      manifest (`kernel/process.c`), so a `CAP_ENFORCE`-confined process holding
      `CAP_PROC` can exec or spawn an ambient program and leave its confinement. Rule
      to enforce: the new image's caps are bounded by the caller's (confinement is
      inherited).
- [ ] **Net stack locking**: `net/` assumes one core (no locks around sockets, TCP
      state, ARP/routing); lock it before SMP is turned on.
- [ ] SMP: TLB shootdown, IPI reschedule, per-CPU run queues (see SMP prep).

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

- [x] **Makefile: header-dep tracking** — kernel objects use `-MMD -MP`;
      `make check-user-progs` flags stale committed `crypto/user_*_elf.h`.
- [ ] **Makefile: `-D` (CFLAGS_EXTRA) changes still need `make clean`.** A flags
      stamp is easy, but `tools/selftest.sh` M25/M30 rely on the sticky
      behaviour (`make CFLAGS_EXTRA=…` then `smoke.sh`'s plain `make iso`) —
      fix those callers first.
- [ ] **`isodir/boot/grub/grub.cfg` is tracked but `make clean` deletes it** —
      consider untracking it (recurring papercut).
- [ ] **Prebuilt `drivers/ath9k_port/ath9k_hw.o` is committed** (`git add -f`) —
      regenerate with `tools/port-ath9k.sh` after touching the port.

---

## ⚠️ Process reminders (don't relearn the hard way)
- **`make clean` after any `.h` or `-D` change** — Makefile ignores those.
- **NIC RX only works in the shell/idle context** — test networking from the shell.
- **Don't build concurrently with `tools/selftest.sh`** (shared build dir + /tmp logs).
- **Never `pkill -f "qemu-system"`** in a command that also launches qemu.
- **SMP testing**: default boot is single-core; build with `-DSECOS_SMP_AT_BOOT` or run
  shell `smp`, then `-smp 2/4` + `stress [N]`.
- **Runtime kernel page-table edits must be locked** (the M5 kstack race, fixed in
  `28ff58a`): any new runtime kernel-VA map/unmap needs `kstack_lock` (or a shared
  kernel-PT lock) + an IPI shootdown if another core can touch the VA.
- Talk to the user in **Italian**; all project content in **English only**.
