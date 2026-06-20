# SECoS — Resume Here (session handoff)

## ✅ M30 (signals + job control + shell pipelines) DONE this session
Full POSIX-style signals for ring-3 + shell job control. See `docs/devlog/M30.md`
and the M30 bullet in `CLAUDE.md`. Highlights:
- `kernel/signal.{c,h}`: per-process pending/blocked masks + `sig_handler[32]` +
  `pgid`/`ppid` + `PROC_STOPPED`. `signal_dispatch(tf)` at the syscall/timer/
  exception return-to-ring3 asm chokepoints (default term/stop/ignore, or a
  custom handler frame returning via the `__sigreturn` trampoline in `crt0.S`).
  Syscalls 44–48 (SIGACTION/SIGRETURN/KILL/SIGPROCMASK/SETPGID).
- Async: keyboard Ctrl-C/Ctrl-Z → SIGINT/SIGTSTP to `foreground_pgid`; SIGPIPE;
  SIGCHLD; EINTR on blocking syscalls.
- Shell: pipelines `a|b|c`, background `&`, `jobs`/`fg`/`bg`, signal-based `kill`.
- Validated non-interactively: m30_sig demo (handler+raise+sigreturn, SIGPIPE) +
  pipelines/bg over serial (`seq 1 100 | wc`→`100 100 292`). **Self-test 179/179.**
- ✅ **VALIDATED INTERACTIVELY on VMware AND real hardware** (user, this session):
  async Ctrl-C (kills a foreground job), Ctrl-Z→Stopped + `jobs`/`fg`/`bg`,
  pipelines, background `&` — all working on the PS/2 console.
- **Bug fixed during bring-up**: `job_wait_foreground` returned with IF=0 (a `cli`
  with no matching `sti` on the job-done path) → the shell froze at its
  keyboard `hlt` after the first foreground command. Fixed in `0aec6da`.

_Last updated: **2026-06-20 — M30 (signals + job control + pipelines) DONE,
validated on VMware + real HW, pushed.** Phases F–K + I done (kernel, process
model, VM, storage+journaling, SMP, networking, real libc/coreutils, signed
packages, init) + **M30**. Self-test **179/179**. HEAD `8041267` on `main`,
pushed to origin. Read the M30 block above first, then "NEXT SESSION TASK"._

_Images on `C:\Users\Luigi\SecOS\` rebuilt at `git:0aec6da`: `secos-uefi.vmdk`
(VMware) + `secos.iso` (real HW). Rebuild + recopy after changes; check the
banner `git:<hash>`._
_Actionable checklist: `TASKS.md`; long plan: `docs/ROADMAP_TO_COMPLETE_OS.md`;
per-milestone: `docs/devlog/M28..M32.md`. Memories: `real-hardware-boot`,
`user-tests-on-vmware`, `working-style-checkpoints`._

## ✅ RUNS ON REAL HARDWARE (2026-06-20) — via the GRUB ISO
Confirmed on a real ASUS E406S laptop (Braswell N3060, 2 cores, 4 GB, UEFI-only,
eMMC). The shell, file commands, `poweroff` all work; console is fluid.
**Boot path that works: the GRUB hybrid ISO** (`make iso` → `secos.iso`, flashed
with balenaEtcher, UEFI mode + Secure Boot OFF). See `memory/real-hardware-boot.md`.
Images live in `C:\Users\Luigi\SecOS\` (`secos.iso` for real HW, `secos-uefi.vmdk`
for VMware) — rebuild + recopy after changes; check the banner `git:<hash>`.

## ⚠️ TWO OPEN BUGS found by testing on real hardware (high-value to fix)
1. **Custom UEFI loader triple-faults on real firmware.** ⏳ **FIX READY — needs
   ASUS validation.** Found TWO root causes (see `docs/devlog/UEFI_REALHW.md`):
   (a) **the real killer** — `acpi.c:pv()` reads ACPI tables via the physmap for
   addresses > 512 MiB, but `vmm_init_physmap` sizes the physmap to *usable* RAM
   only, and firmware puts the tables in *reserved* memory above the usable top →
   kernel `#PF` in `acpi_init` (reproduced at `-m 2G`; certain on the 4 GB ASUS).
   Fixed: `pv()` calls `vmm_extend_physmap`. (b) the loader installed a 512 MiB-only
   identity map before the kernel jump → triple fault if firmware placed the loader
   image/stack/bootinfo high. Fixed: copy the live firmware PML4 (inherit full
   identity) + add the high half; kernel copies `bootinfo` into its own memory in
   phase1. **Validated in QEMU+OVMF at `-m 2G/4G/8G` (clean boot, ACPI parsed, no
   `[EXC]`), smoke `--uefi`/`--mb2` PASS, selftest 179/179.** GOP stage-color bars
   added for blind debug on the ASUS (blue→cyan→yellow→magenta→green; vanish on
   success). **NEXT: boot the new `secos-uefi.img`/`.vmdk` on the ASUS.** If it
   still stops, the last bar color localizes the stage. (NOT yet committed.)
2. **SMP freezes on real hardware under load.** `stress 8` with real cores brought
   up froze the machine — a race / **missing TLB shootdown** that QEMU (interleaved
   cores) and VMware never exposed. **FIX SHIPPED this session: AP bring-up is now
   opt-in** (commit `3f776dd`) — default boot is single-core + stable (LAPIC timer
   still on). Bring cores up on demand with the shell `smp` command, or build
   `-DSECOS_SMP_AT_BOOT`. **Real fix (open): IPI-based TLB shootdown on shared
   kernel-PT edits (the M5 kstack reuse) + memory barriers + a real-HW lock
   audit.** Hard to debug blind (freezes, no output) — needs real-HW diagnostics
   (USB-serial adapter, or an on-screen heartbeat/watchdog).

## ▶▶ NEXT SESSION TASK: pick one
- **A. Fix the UEFI loader for real HW** (open bug #1) — so it boots without GRUB.
  Likely the loader's GOP/memory-map/page-table handoff or pre-IDT kernel init.
  Add an early fault-catching IDT + on-screen markers to diagnose (output isn't
  visible on real UEFI until the FB is mapped; the loader's GOP is the only early
  display). High value: a self-contained bootable OS.
- **B. ~~Signals (M30)~~ DONE** — see the M30 block at the top. Remaining:
  interactive VMware validation of async Ctrl-C/Ctrl-Z + fg/bg; optional
  SA_RESTART / `sigaction` flags / `WIFSTOPPED` waitpid status / sessions.
- **C. Finish Phase K self-hosting** — `setjmp`/`longjmp` + `printf %f` + `math.h`,
  `open(O_CREAT)`/cwd/env, then **port lua or sqlite from source** and sign it
  (headline "compile OSS from source" proof). See `TASKS.md` Option 4.
- **D. SMP hardening** (open bug #2) — TLB shootdown IPI + barriers + real-HW audit.
  Needs real-HW diagnostics first.
- **E. Phase L** — generalized capability model + audit, exploit mitigations.

### Build / test / artifacts cheat-sheet
- `make iso` → `myos.iso` (GRUB hybrid UEFI+BIOS; the **real-HW** path).
- `make uefi-vmdk` → `secos-uefi.vmdk` (VMware) / `make uefi-disk` → `.img`.
- `make user-progs` → rebuilds + signs user programs, regenerates `crypto/user_*_*.h`
  (needs python3 `cryptography`). **Run this BEFORE referencing a new embedded
  header in the kernel**, else the kernel build fails.
- `tools/selftest.sh` → 173/173 (~12–15 min; ONE background job; don't build
  concurrently; don't edit sources while it runs — both bit me this session).
- After changes: rebuild ISO+vmdk, `cp` to `/mnt/c/Users/Luigi/SecOS/`, tell the
  user the `git:<hash>` to expect in the banner. User tests on **VMware / real HW**.
- Quick QEMU UEFI boot of the ISO: `qemu-system-x86_64 -machine q35 -m 256M -bios
  /usr/share/ovmf/OVMF.fd -cdrom myos.iso -serial stdio -display none -no-reboot`.
- Headless FB screenshot: QEMU `-monitor stdio` → `screendump /tmp/fb.ppm`, then
  `python3 -c "from PIL import Image; Image.open('/tmp/fb.ppm').save('/tmp/fb.png')"`
  and Read the PNG (verified the console renders correctly this way).

### Recent commits (this session, newest first)
`3f776dd` SMP opt-in (default single-core, `smp` cmd) · `b6756f0` fast console
scroll (RAM back buffer + dirty rect) · `8cddda1` `poweroff` (ACPI _S5) · `5869fa0`
clean help (columns + `-l`) + quiet init.rc · `a13d1bc` verbosity gate + run-by-name
· `7d45b38` M32 signed packages + init · `758fdfb`/`28fc50b` M31 coreutils + libc ·
`84a0678`/`c390fb4` M29 SMP · `377db8a`/`eb29d94` M28 TSC/APIC.

### Shell commands worth knowing
`help` / `help -l`, run-by-name (`ls`, `hexdump f`, `uname -a`, `init`, `seq 1 5`),
`run /bin/<x>`, `pkg install <f.spkg>`, `stress [N]`, `smp` (opt-in multicore),
`verbose on|off`, `poweroff` / `halt` / `reboot`. Coreutils live in `/bin`
(installed at boot, idempotent). `uname`/`ls`/`cat`/`echo` are also shell builtins
(take precedence over `/bin/`).

### Phase K facts (don't relearn)
- libc: `user/include/*.h` (standard headers) + `user/libc.c` (impl). `libc.o`
  links into every user prog. malloc/free/calloc/realloc live in libc.c now.
  **No GCC nested functions/statement-exprs** in user code — they need an
  exec-stack trampoline → W^X kill.
- New syscalls: **40 GETDENTS, 41 CREATE, 42 MKDIR, 43 UNLINK** (over vfs_*).
  libc wrappers in libsecos.c; `<dirent.h>` opendir/readdir over GETDENTS.
- **coreutils** = one signed binary (`user/coreutils.c` + `cu_text/file/misc.c`),
  installed to `/bin/<applet>` at boot (`coreutils_install` in kernel.c,
  idempotent). `RAMFS_MAX_FILES` 32→96 to fit. Applet = basename(argv[0]).
- **`.spkg`**: `tools/secos-pkg create OUT SRC:/dest [--mkdir /d]`; digest = SHA-256
  with the 64 sig bytes zeroed (same as ELF QSIG); `kernel/pkg.c pkg_install`
  verifies vs `secos_trusted_pubkey` then vfs_create/mkdir. Shell `pkg install`.
- **Gotcha that bit this session**: editing `kernel.c` (adding `#include
  crypto/user_*_elf.h`) while a selftest was mid-run broke its later builds —
  generate embedded headers (`make user-progs`) BEFORE referencing them, and
  don't edit sources during a selftest run.

### M29/SMP facts (don't relearn)
- VMware-verified: `stress [N]` saturates the vCPUs. `stress 16` once exposed a
  race in the M5 per-process kernel stack → fixed with `kstack_lock` (`28ff58a`).

### M29 facts (don't relearn)
- **Architecture**: per-CPU run state (`arch/x86/percpu.{c,h}`, `this_cpu()` by
  LAPIC id) + **CPU-pinned processes** (`process_t.cpu_affinity`, round-robin) +
  fine-grained spinlocks (`arch/x86/spinlock.h`, irqsave) on shared subsystems.
  NO Big Kernel Lock across the context switch (deliberate — that's the fragile
  point). Each core schedules only its own affinity set → switch is lock-free.
- **AP bring-up**: `boot/ap_trampoline.asm` (flat `org 0x8000`, objcopy-wrapped),
  `kernel/smp.c smp_init()` INIT-SIPI-SIPI (one AP at a time; ktime_us delays),
  `ap_entry()` → per-CPU GDT/TSS (`tss_setup_ap`), idle task, LAPIC timer, scheduler.
- **TLB shootdown is intentionally absent**: a user space runs on one core, so its
  page-table edits (fault/COW/mmap/fork) are core-local; the only growing shared
  kernel map (`vmm_extend_physmap`) is boot/driver-bind only on the BSP. If you add
  cross-core page-table sharing (e.g. MAP_SHARED across cores, task migration), you
  MUST add an IPI shootdown first.
- **Reaping**: `process_reap_one(affinity)` detaches a ZOMBIE under `proc_lock`;
  only its affinity core reaps it, from idle (off its stack). Don't reap across cores.
- Locks added M29-1: PMM, heap, pagecache, ipc, pipe, block I/O, vfs mounts,
  keyboard, proc table. All irqsave (safe vs this core's own ISRs).
- Observability: `[SMP] cpu=<idx> run pid=<pid>` in `switch_to` (user task on a core).

### M28-3 facts (don't relearn)
- `arch/x86/tsc.{c,h}`: `tsc_init` calibrates rdtsc vs **PIT channel 2** (50 ms,
  port 0x61 gate + OUT2 bit5) — independent of 8259/APIC, works in either mode.
  `ktime_ns/us/ms()` + `tsc_hz()`. Calibrated ~2.5 GHz on QEMU/VMware.
- **No 128-bit divide**: libgcc isn't linked (`-nostdlib`), so `(__uint128_t)/hz`
  → `__udivti3` link error. Hot path is `ns=(cycles*mult)>>32` with `mult`
  precomputed via one 64-bit divide; the 64x64→128 multiply is a native `mulq`.
- `SYS_GETTICKS` ABI unchanged (still the 1 kHz tick). The ns clock is
  kernel-internal + exposed via `/proc/uptime` (sub-second) and `/proc/cpuinfo`
  (`tsc_mhz`). `net_tsc_per_us()` now prefers `tsc_hz()`.

### M28-2 facts (don't relearn)
- Everything is in `arch/x86/lapic.c` (it already owned the LAPIC MMIO for MSI).
  `apic_switchover(hz)` orchestrates; `apic_mode_active()` + `irq_eoi()` are the
  public surface. `g_apic_mode` flips the EOI source atomically.
- The **vector stays 0x20** for the timer (reuses isr_timer + the IDT gate) — only
  the EOI moves from `out 0x20,0x20` to `irq_eoi()` (LAPIC). Same for kbd 0x21.
- LAPIC timer **calibrated against PIT channel 2** (same trick M28-3 reuses).
- Fallback: no MADT/IOAPIC or calibration==0 → revert the PIC mask, stay on 8259.
- **VMware-verified** (user, this session): keyboard via IOAPIC + Ctrl+C work.

### M28-2 facts (don't relearn)
- Everything is in `arch/x86/lapic.c` (it already owned the LAPIC MMIO for MSI).
  `apic_switchover(hz)` orchestrates; `apic_mode_active()` + `irq_eoi()` are the
  public surface. `g_apic_mode` flips the EOI source atomically.
- The **vector stays 0x20** for the timer (reuses isr_timer + the IDT gate) — only
  the EOI moves from `out 0x20,0x20` to `irq_eoi()` (LAPIC). Same for kbd 0x21.
- LAPIC timer is **calibrated against PIT channel 2** (port 0x61 gate, poll OUT2
  bit5; ch2 is independent of the 8259 so it counts with the PIC masked). QEMU
  LAPIC ≈ 1 GHz, /16 → ~62.5 M ticks/s → init ≈ 62.5 k for 1 kHz.
- The asm `call irq_eoi` sits exactly where the old inline EOI was (same RSP as
  the working `call timer_handler`) so stack alignment is unchanged.
- Fallback path is real: no MADT/IOAPIC or calibration==0 → revert the PIC mask,
  `irq_eoi` stays in 8259 mode, kernel runs as before.

### Other open fronts (lower priority than finishing Phase I)
- **Signals** — async Ctrl-C→SIGINT + SIGPIPE → shell job control & pipelines
  (`cmd1 | cmd2`; pipes already exist). M15 `sched_kill_current` exists; no async delivery.
- **NIC hardware validation** — vmxnet3 / igc (2.5 GbE) need the real adapter.
- **FS follow-ups**: mid-path symlink following; `metadata_csum`; ctime auto-bump.

### M28-1 facts (don't relearn)
- ACPI tables are read via the **low identity map (0–512 MiB)** (`pv()` in acpi.c),
  NOT the physmap — the physmap may not cover the ACPI region on UEFI (#PF otherwise).
- RSDP: UEFI loader passes it (`secos_boot_info.acpi_rsdp`, via the EFI config
  table for the ACPI 2.0/1.0 GUID); MB2 scans the BIOS area. `acpi_init(rsdp_hint)`.
- `efi.h` `EFI_SYSTEM_TABLE` now includes `ConfigurationTable` (was truncated).

### M27 (a+b) facts (don't relearn)
- M27b is **ordered mode**: metadata journalled, file **data** written direct
  (`blk_write_direct`). A crash can lose *recent data* but never corrupts FS
  structure. The key correctness invariant is **read-your-writes** in `blk_read`
  (serves buffered txn blocks) — without it shared metadata (gd/sb/bitmaps) drift.
- Write-journaling only engages on a **`has_journal`, csum-free, non-async** ext4
  volume (`jt_init`). `make disk-journal` builds a dirty-journal test image;
  `tools/mk_dirty_journal.py` is the e2fsck-validated synthesizer.
- Crash test: gates `M27B_CRASH=1` (before commit→absent) / `=2` (after publish→
  replayed) for run1, `M27B_VERIFY` for run2; `g_blk_dead` drops writes.

## (superseded) earlier NEXT TASK list
4. **NIC hardware validation** — vmxnet3 / igc (2.5 GbE) need the real adapter
   (QEMU models neither). Tune the 82574 MSI-X re-arm.

### This session's deltas (don't relearn)
- **M25**: pipes (`kernel/pipe.{c,h}`, `SYS_PIPE` 31, blocking/EOF/EPIPE,
  fork-inherited) + console **TTY cooked mode** (`kernel/tty.c`: echo/backspace/
  Ctrl-D/Ctrl-C) + **PS/2+USB-HID Ctrl-key support** (was missing entirely — that's
  why Ctrl-C/D did nothing on the console) + shell-prompt Ctrl-C. **fds 0/1/2 are
  now reserved as stdin/stdout/stderr at process creation** (else a pipe end
  collided with the console fd specials). TCP buffer 8K→64K (helps WAN, not the
  sub-ms-RTT LAN — that's processing-bound; see `memory/m25-tcp-throughput-findings`).
- **M26**: POSIX metadata **stored & exposed, NOT enforced** (signature = trust;
  user decision). `struct secos_stat` grew (st_size@0/st_mode@8 stable). chmod/
  chown/utimes/lstat/readlink/symlink/mount/umount syscalls + libc. **Mid-path
  symlink components not followed yet** (final component only).
- **M27a**: JBD2 **replay only** (read-side). `tools/mk_dirty_journal.py` +
  `make disk-journal` build an e2fsck-validated dirty-journal test image.
- **selftest gotcha hit this session**: it's `set -euo pipefail`, so a single
  failing `grep -q` **aborts the whole suite** (and can look like a premature
  "exit 0"). Keep selftest grep patterns exact. Don't hardcode file sizes.
- **Process gotcha (bit me again)**: never `pkill -f "...selftest..."`/`qemu` in a
  command whose own line matches the pattern → kills the shell (exit 144). Same
  for a `pgrep` wait-loop that matches itself.

### M24 final state (all live-verified on real VMware against a bridged LAN)
- **Waves 1–4 done**: UDP/DHCP/DNS (`net/udp.c,dhcp.c,dns.c`), TCP
  (`net/tcp.c`: full state machine, sliding window, retransmit, active+passive),
  BSD sockets + **CAP_NET** (`net/socket.c`; syscalls 21–30; `CAP_NET` = bit 4 of
  the signed manifest `flags`, `process_t.cap_net`; demo gate `M24_NET_DEMO`),
  **MSI-X/NAPI RX now the DEFAULT** (`net_request_irq` tries MSI-X/MSI, falls back
  to polling; opt out `-DNET_NO_MSIX`; e1000/e1000e `irq_enable`/`irq_ack`, e1000e
  programs the 82574 IVAR; hybrid keeps a poll backstop).
- **Drivers**: e1000 binds **both** 82540EM (`100E`, QEMU) and 82545EM (`100F`,
  VMware "e1000"); e1000e binds 82574L (`10D3`, VMware "e1000e" — the validated
  one). vmxnet3/igc compile-clean, untested.
- **Shell**: `netinfo` (shows `rx msix(irq)`/`poll`), `dhcp`, `ping <ip> [count]`
  (RTT min/avg/max via rdtsc), `nslookup`, `udpsend`, `tcptest <ip> <port> [path]`,
  `nettest <ip> <port> [path]` (TCP throughput → KB/s + Mbit/s).
- **Three bugs fixed post-completion** (don't reintroduce): (a) DHCP `xid` written
  BE / read LE → every OFFER rejected; (b) **`ping` ARPed the destination IP
  instead of the next hop** → any off-subnet/internet ping timed out *without
  sending the echo* (LAN worked, TCP worked → looked like a network/ICMP issue but
  wasn't); fix = resolve next hop (gateway for off-subnet) like `ipv4_send`;
  (c) `tcp_connect` now warms the next-hop ARP before the SYN (no transient
  "connect failed").

### Build/run the networking (interactive — needs a NIC + the shell context)
```bash
make iso && qemu-system-x86_64 -cdrom myos.iso -boot d \
  -netdev user,id=n0 -device e1000e,netdev=n0 \
  -serial stdio -display none -no-reboot -m 256M
# shell: dhcp / netinfo / ping 8.8.8.8 10 / nslookup example.com / nettest <ip> <port> /big.bin
# throughput test: serve a BIG file on the host (python3 -m http.server 8000) then
#   nettest <hostip> 8000 /big.bin   (nettest WITHOUT a path GETs / = tiny = noise)
# CAP_NET demo (non-interactive): make iso CFLAGS_EXTRA=-DM24_NET_DEMO=1
# VMware: set ethernet0.virtualDev="e1000e", NAT/Bridged; images via `make uefi-vmdk`
#   (copied to C:\Users\Luigi\SecOS\ ; verify banner GIT_HASH matches HEAD)
```
**RX gotcha still applies:** test from the shell/idle context. **`make clean`
after any `-D`/header change** (no header-dep tracking — a no-clean `make iso`
after a selftest run links stale demo `.o` files; this bit me this session).
**Never `pkill -f "qemu-system"`/`"http.server"` in a command that also runs
them — the pattern matches the command's own line and kills the shell (exit 144).**

### Key facts for the next session (don't relearn the hard way)
- **NIC RX needs the shell/idle context**: the idle task `hlt`s between timer
  ticks → yields the vCPU so QEMU delivers RX DMA; `net_tick` polls. A busy-spin
  in early `kernel_main` never yields (`GPRC=0`, looks like an RX bug — it isn't).
  Test networking from the **shell** (`ping`/`netinfo`), not early boot.
- **Makefile has NO header-dep tracking + ignores CFLAGS_EXTRA changes**: after
  editing any `.h` or toggling a `-D` flag, **`make clean` first** or you run a
  stale kernel (this bit me twice this session — stale demo flag, stale struct).
- **Don't build concurrently with `tools/selftest.sh`** (shared build dir + /tmp
  logs → corruption). Run it as a single background job and wait.
- **Worktree agents can contaminate the main checkout's branch** (HEAD got left
  on a `feat/*` branch twice). After spawning worktree agents, verify
  `git rev-parse --abbrev-ref HEAD` == `main` before committing, and that
  `git rev-parse main` == `origin/main`.
- Net code is **inert without a NIC** → self-tests unaffected (still **122/122**).
  Test ping: `qemu-system-x86_64 -cdrom myos.iso -boot d -netdev user,id=n0
  -device e1000,netdev=n0 ...` then shell `ping`. On VMware use an **e1000** NIC.
- Locked decisions: all 4 NICs (incl **igc=2.5GbE**), **MSI-X+NAPI** target,
  **CAP_NET** single manifest capability. See `memory/m24-networking-decisions.md`.

## ✅ Status: M24 networking L2/L3 done — `ping` WORKS
Networking foundation landed and verified: NIC contract `net/net.h`, **4 NIC
drivers** (e1000 + e1000e + vmxnet3 + igc/2.5GbE), L2/L3 stack (Ethernet/ARP/
IPv4/ICMP), `ping`/`netinfo` shell. **`ping` works host↔VM** in QEMU
(`-netdev user -device e1000`): `ping` → `[NET] ARP resolved` → `[ICMP] echo
reply` → `[NET] PING OK`. Self-test **122/122** (net is inert without a NIC).
- **Gotcha (resolved):** RX only works in the **shell/idle context** — the idle
  task `hlt`s between ticks, yielding the vCPU so QEMU delivers RX DMA; `net_tick`
  polls. An early-boot busy-spin self-test never yielded → looked like an RX bug
  (`GPRC=0`) but wasn't. Auto-ping removed; use the `ping` shell command.
- **e1000e/vmxnet3/igc**: compile-clean, **untested** (no QEMU model / need real
  HW). **igc = 2.5GbE** (I225/I226).
- **Next waves**: UDP+DHCP+DNS → TCP → sockets + `CAP_NET` → MSI-X/NAPI RX
  (current RX is timer-tick polling). See `docs/devlog/M24.md`.

## ✅ Status: M23 done + pushed (HEAD `04eb1f9`, self-test 122/122)
M23 (POSIX FS personality: persistent ext2 root + /dev /proc /sys + POSIX shell)
is implemented, **self-test 122/122**, and pushed. VMware images rebuilt
(`GIT_HASH=04eb1f9`) and copied to `C:\Users\Luigi\SecOS\`: `secos-uefi.vmdk`
(boot) + `sysdisk.vmdk` (persistent ext2 root — attach as a 2nd SATA disk). See
the "M23 — POSIX FS personality: DONE" section below.

## (earlier) Status: M22 + follow-ups pushed (HEAD `6f7e0c6`)
M21 (`M21_STABLE`) and **M22 (tag `M22_STABLE` = `e970af2`)** are pushed to
`origin/main`. Post-tag additions also pushed: `blk`/`mountdev` shell
diagnostics, **USB MSC BOT STALL recovery + Get Max LUN**, plus the two M22
follow-ups merged into `main`:

- **USB hub support** (`drivers/usb_hub.c` + xHCI routing): enumerate devices
  behind a hub (route string, parent hub slot/port, TT for FS/LS). **⚠️ DA
  TESTARE su hardware reale** — QEMU CI ha no hub, quindi dormiente lì (compile-
  + logic-verified only). Emits `[HUB]` markers when a hub is found.
- **MSI / MSI-X plumbing** (`drivers/pci.c`, `arch/x86/lapic.c`, IDT vectors
  0x40/0x41, `xhci_enable_irq`/`nvme_enable_irq`): **⚠️ DA TESTARE** — additive,
  **OFF by default** (kernel still 100% polled). Real interrupt path gated behind
  `-DXHCI_USE_IRQ`/`-DNVME_USE_IRQ` and needs the LAPIC + hardware validation.
  NVMe I/O CQ is still created with IEN=0 (recreate with IEN=1 + vector to truly
  fire). Default build is byte-for-byte behaviourally identical (no `[MSIX]`).

Self-test **112/112** on the merged tree. NVMe **confirmed on real VMware**; USB
stick (MSC) verified in QEMU, not yet on hardware. `edk2/` stays untracked.
Latest VMware image last built at `GIT_HASH=d30db54` (pre-merge) in
`C:\Users\Luigi\SecOS\` — rebuild (`make uefi-vmdk`) for the hub/MSI-X code.

## Where we are

- **Done through M22** — M0–M13 (A–E) + M14 (demand paging) + Phase F (M15–M17) +
  Phase G (M18–M20) + M21 (AHCI/SATA) + **M22 (NVMe + USB: xHCI / HID keyboard /
  Mass Storage)**. Consolidated on **`main`**.
- **🎉 NVMe CONFIRMED ON REAL VMWARE** (user, 2026-06-18): boot disk on SATA
  (`sda`, GPT skipped), data disk moved to a **VMware NVMe** controller → `blk`
  shows `nvme0n1 sectsz=512 sectors=131072 (64 MB) fs=FAT` and `vls /mnt` lists
  the files. So the full NVMe path (PCI probe → IDENTIFY → I/O queue → polled DMA
  → FAT32 mount) works on real VMware, not just QEMU. USB stick (MSC) not yet
  tried on VMware. Diagnose storage on VMware with the new **`blk`** / **`mountdev
  <dev> [mp]`** shell commands (debugcon markers aren't visible there).
- **M22 highlights** (`docs/devlog/M22.md`): `drivers/nvme.c` registers `nvme0n1`
  (NVMe-only laptops / VMware NVMe). A polled `drivers/xhci.c` host controller +
  `usb.c` core enumerate USB devices; `usb_hid.c` (HID **boot keyboard** → injects
  into the shared keyboard buffer via `keyboard_inject_char`, polled from
  `keyboard_has_char`, never an ISR) and `usb_msc.c` (Bulk-Only/SCSI → `usb0`).
  `pci_bar_mem64` for 64-bit BARs. Boot mount tries `vda,sda..sdd,nvme0n1,usb0`.
  Verified in QEMU: NVMe FAT32 RW; xHCI+usb-kbd+usb-storage (keystrokes reach the
  shell, `usb0` FAT32 RW). **Gotcha:** SeaBIOS hangs trying to boot an NVMe/USB
  *data* disk → ISO tests use `-boot d`.
- **🎉 RUNS ON VMWARE WITH WORKING DISKS** (user-confirmed this session): boot
  from `secos-uefi.vmdk`, data on `data.vmdk` (both **SATA**) → `/mnt` mounts
  FAT32 read-write. Interactive via the VM console (PS/2) AND a serial console
  (com0com virtual COM pair → PuTTY @ 115200 8N1). Write a file from the shell:
  `vcreate /mnt/note.txt <text>` (FAT32 upcases to 8.3 → `NOTE.TXT`).
- **Run on real hardware / VMware:** `docs/RUN_ON_HARDWARE.md`
  (`make uefi-vmdk` boot disk + `make data-vmdk` SATA data disk). Built images are
  in `~/secos/*.vmdk` and copied to the Windows host `C:\Users\Luigi\SecOS\`.
- **Serial fix (`8dd0b7a`) — important gotcha:** `serial_init()` used to set
  `ready=1` unconditionally; on a host with **no COM port** (VMware default, many
  PCs) every UART register reads `0xFF`, so the LSR Data-Ready bit looks stuck and
  the shell's polled input (`keyboard_getchar`→`serial_poll_char`) **flooded the
  console with `0xFF` chars**, drowning PS/2 keystrokes. Fixed with the standard
  16550 **loopback presence test** (send `0xAE` in MCR loopback, require it back);
  if absent, `ready=0`. The PS/2 keyboard path itself was fine.
- **Long-term roadmap: `docs/ROADMAP_TO_COMPLETE_OS.md`** — Phases F–L (M15–M32).
  **Next (post-M22):** USB **hubs** (only root-port devices enumerate today) +
  NVMe/xHCI **interrupts** (MSI-X; everything is polled now). Then **pipes/TTY**,
  Phase H (storage maturity), Phase I (ACPI+APIC+SMP).
- Build is green: `make` clean (0 warnings), `tools/selftest.sh` **112/112**
  (+4 M22 NVMe, +6 M22 USB), both boot paths higher-half.
- **M22 notes:** everything USB/NVMe is **polled, one op at a time** (matches
  AHCI/virtio-blk). The HID keyboard is polled from `keyboard_has_char()` (the
  input-wait path), **never an ISR**, so the single shared xHCI event ring is
  never drained underneath an in-flight MSC/control transfer. NVMe/xHCI are
  testable in QEMU (`-device nvme`, `-device qemu-xhci -device usb-kbd -device
  usb-storage`). **`-boot d` is required** on the ISO path: SeaBIOS otherwise
  hangs trying to boot the NVMe/USB *data* disk (FS but no boot record). OVMF 6.2
  + `-device nvme` + `fat:rw:dist/` won't launch our EFI app — use a real GPT/ESP
  disk image or the ISO path for NVMe.
- **M21 note:** AHCI is testable in QEMU because `q35` has a built-in AHCI
  controller (`-machine q35 -drive ...,if=ide` routes to it). The driver registers
  multiple disks (sda..sdd) and the boot mount tries each, so VMware's "boot ESP +
  data disk both SATA" works (the GPT ESP is skipped, the raw-FS data disk mounts).
- **Self-test runner note:** the full suite takes ~12–15 min (≈14 variant builds).
  Run it as a SINGLE background process and wait for its completion — running
  several `selftest.sh` at once shares the build dir and corrupts/truncates logs.
- **M19 gotcha (important):** intermediate page-table entries are now always
  `RW|PRESENT` (USER still propagated). x86-64 ANDs the RW bit across ALL paging
  levels (CR0.WP=1), so a non-writable intermediate makes every leaf below it RO —
  which broke COW. The **leaf PTE is the sole write/exec authority** now.
- Only `edk2/` is untracked (a large vendored tree, not part of the build) — leave it.
  `.gitignore` shows a stale local modification from before this work — harmless,
  not committed. Build artifacts `secos-uefi.{img,vmdk}`, `disk.img`, `*.o`,
  `user/*.elf`, `uefi_loader.elf` are gitignored/untracked.

## What SECoS is (identity)

A minimal, **security-first x86-64 OS with a microkernel bias**. Not POSIX, not
general-purpose. Three execution modes (`docs/DRIVER_SPACE.md`):
**kernel** (ring 0), **driver** (ring 3 + capability-mediated HW access — a
*software* privilege, not a HW ring), **user** (ring 3, no HW).
**Every executable must be Ed25519-signed to run** — the signature is the root of
trust for the `.note.secos` manifest claims (mode, capabilities, limits).
Full mission + plan: `docs/DEVELOPMENT_PLAN.md`; high-level list: `ROADMAP_SECoS.md`.

## Locked decisions (do not relitigate without the user)

- Full secure OS; phases A–E mandatory, F–G stretch.
- **Custom minimal syscall ABI** underneath; libc exposes a **POSIX-friendly**
  surface so open-source C ports *from source* (NOT Linux-binary compatibility).
- **Mandatory signing**: every ELF, Ed25519 + SHA-256, **refuse-by-default** with
  `-DDEV_ALLOW_UNSIGNED` for bootstrap. Single project key for v0 (`QSIG` note has
  `key_id`, keyring-ready). Details: `docs/SIGNING.md`.
- Platform: **UEFI golden**, Multiboot2 = CI; storage = **virtio-blk** (M10).

## Milestone status

| M | State | Notes |
|---|-------|-------|
| M0–M6 | done | boot, paging/W^X, isolation, trapframe syscall, context switch |
| M7 | done (`M7_STABLE`) | ring-3 + `SYS_YIELD` cooperative; fixed 4 bugs (notably clear `EFLAGS.NT` before `iretq`; private `PML4[0]` PDPT per space; ELF copy stride) |
| M8 | done | preemptive sched + `SYS_EXIT`/reap + idle task + no leak (N=4/6); fixed `kfree` coalescing of non-contiguous blocks |
| M9 | done (`M9_STABLE`) | crypto (SHA-256/512 + Ed25519 verify), signing format+tools+gate, userland (crt0+libc), signed `hello` runs from VFS |
| M10 | done | storage & persistence: virtio-blk (`vda`) + RW **FAT32/ext2/ext4** via VFS multi-mount (`/mnt`); `SYS_SPAWN`/`SYS_WAIT` + shell `run`; signed ELF written to disk → read → verify → ring-3. See `docs/devlog/M10.md` |
| M11 | done | Driver Space for real: `.note.secos` v2 manifest carries `proc_type`/`dev_id`/`dev_caps` (signature-rooted); loader auto-binds a `PROC_TYPE_DRIVER` to its device with the granted cap subset; `SYS_DRIVER` enforces driver-only + per-binding caps + `[DRV-AUDIT]`. See `docs/devlog/M11.md` |
| M12 | done | Memory scalability + W^X hard gate + **higher-half kernel**: PMM manages all RAM (clamps gone; word-skip+cursor; `pmm_alloc_contiguous`); heap on physmap + multi-frame + NULL-on-fail; `vmm_map` rejects W+X (`-5`); `-m 2G` → ~2045 MB free. Kernel runs at `0xFFFFFFFF80000000` (both MB2 and UEFI). See `docs/devlog/M12.md` |
| **M13** | **done** | usability & policy: **manifest `max_mem` enforced at load** (leak-free abort, signature-rooted); `SYS_GETTICKS` + **kernel IPC channels** (`SYS_MSG_SEND`/`RECV`, `kernel/ipc.c`); shell `run` = on-disk launcher; producer/consumer demo exchange a message over channel 0. See `docs/devlog/M13.md` |
| **M14** | **done (stretch)** | **full demand paging**: per-process VMAs (`mm/vma.{c,h}`) replace eager mapping; `elf_load_image_lazy` reserves FILE/ANON regions; `vmm_handle_page_fault` materializes pages on first touch (`[PF] demand page`); pinned per-process ELF image; syscall buffers fault in via kernel-mode #PF (no `user_copy` change); stack guard = absence of a VMA; `max_mem` vs reserved footprint. See `docs/devlog/M14.md` |
| **M15** | **done (Phase F)** | **fault-driven process kill**: a ring-3 CPU fault kills only the offending process and returns to the scheduler instead of halting. `exception_handler` decides from saved `cs` (ring3⇒`sched_kill_current`, ring0⇒panic). `process_t.exit_code` (128+vec). Demo `M15_KILL_DEMO`. See `docs/devlog/M15.md` |
| **M16** | **done (Phase F)** | **exec model**: argv/env on the demand-paged user stack (`process_create_from_elf_args`, SysV `rdi/rsi/rdx`; crt0 unchanged); `SYS_SPAWN(path, char**)`; **blocking `SYS_WAIT`** returns exit status (rip-rewind re-run; status delivered into the waiter via `sched_wake_waitpid`); `SYS_EXIT` carries user status; shell `run <path> args`. libc `spawn`/`waitpid`. Demo `M16_EXEC_DEMO`. See `docs/devlog/M16.md` |
| **M17** | **done (Phase F)** | **blocking primitives** on the M16 block/wake core: `SYS_SLEEP` (13); **blocking `SYS_MSG_RECV`** (pre-queued msg returns immediately so M13 poll stays green). libc `sleep_ticks`. **Pipes/TTY deferred**. See `docs/devlog/M17.md` |
| **M18** | **done (Phase G)** | **dynamic memory**: `SYS_MMAP/MUNMAP/BRK/MPROTECT` (14–17) on the M14 VMA framework; `brk`/`sbrk` heap, anon `mmap` arena, `mprotect` (`vmm_protect_in_space`+invlpg); W^X-enforced + bounded by manifest `max_mem` at runtime; VMA tombstones + `VMA_MAX`→64. libc `malloc`/`free`/`mmap`. Demo `M18_MEM_DEMO`. See `docs/devlog/M18.md` |
| **M19** | **done (Phase G)** | **copy-on-write fork**: PMM frame refcounts (`pmm_share`/`pmm_unref`); `vmm_fork_space` (COW bit 9), `vmm_cow_fault`; `SYS_FORK` (18) / `process_fork`; libc `fork()`. **Fix**: intermediate PT entries always `RW|PRESENT` (leaf is sole protection authority). Demo `M19_FORK_DEMO`. See `docs/devlog/M19.md` |
| **M20** | **done (Phase G)** | **unified page cache** (`mm/pagecache.c`, 128 pages keyed by inode+offset) backs file `read()` AND **file-backed `mmap`** (MAP_PRIVATE via `vma_add_file`/`file_inode`; fault copies cached page into a private frame). `SYS_MMAP` +`fd` arg; `ksys_read`→cache, `ksys_write` invalidates. libc `open`/`read`/`close`/`mmap_file`. Demo `M20_MMAP_DEMO`: mmap+read of a VFS file return identical (coherent) bytes. See `docs/devlog/M20.md` |
| **M21** | **done (`M21_STABLE`)** | **AHCI/SATA block driver** (`drivers/ahci.c`) — disk path for **VMware + physical PCs**. PCI class probe (01/06/01, ABAR=BAR5), IDENTIFY, polled DMA read/write (READ/WRITE DMA EXT). Registers sda..sdd; boot mount tries vda,sda..sdd (skips GPT ESP). `make data-vmdk`; `docs/RUN_ON_HARDWARE.md`. Tested in QEMU q35. See `docs/devlog/M21.md` |
| **M22** | **done** | **NVMe + USB stack**. `drivers/nvme.c` → `nvme0n1` (admin+I/O queues, IDENTIFY, polled DMA, PRP1+PRP-list). Polled **xHCI** (`drivers/xhci.c`) + USB core (`usb.c`) + **HID boot keyboard** (`usb_hid.c` → `keyboard_inject_char`) + **Bulk-Only Mass Storage** (`usb_msc.c` → `usb0`). `pci_bar_mem64`. Mount tries vda,sda..sdd,nvme0n1,usb0. Verified in QEMU (NVMe + usb-kbd + usb-storage). See `docs/devlog/M22.md` |
| next | **USB hubs / IRQs → pipes/TTY → Phase H/I** | USB hubs (only root-port devices today) + NVMe/xHCI **interrupts** (MSI-X, retire polling). Then pipes/TTY, Phase H (storage maturity), Phase I (ACPI+APIC+SMP). Full plan: `docs/ROADMAP_TO_COMPLETE_OS.md` |

## Build / test / run

```bash
make                       # build kernel.bin (default; signing gate ENFORCED, dormant at boot)
make iso                   # myos.iso (GRUB Multiboot2)
make user-progs            # build + SIGN user/hello.c -> crypto/user_hello_elf.h (needs python3 + `cryptography`)
tools/selftest.sh          # THE gate: builds M7/M8/M9/M10 variants, asserts from debugcon. Expect 38/38.
make disk-fat32            # 64MB test disk (also disk-ext2 / disk-ext4); make run-disk attaches it (-boot d!)
tools/smoke.sh --mb2 -t 8 --log /tmp/x.log   # single boot; exit 124->0 = alive = PASS
make run-serial            # interactive shell IN THE TERMINAL over COM1 (WSLg GUI is broken here; serial is the way)
```

Demos are gated and OFF by default (normal boot reaches the shell):
`-DM7_RING3_DEMO=1`, `-DM8_SCHED_DEMO=1`, `-DM9_USER_DEMO=1`. The M7/M8 synthetic
ELFs are unsigned, so build them with `-DDEV_ALLOW_UNSIGNED` (the harness does this).

Verification is **non-interactive** via debugcon markers (`[CRYPTO]`, `[SCHED]`,
`[M8]`, `[M9]`, `[SEC]`, `[hello]`, `[EXC]`). The interactive shell renders to the
framebuffer/serial, not debugcon — so harness assertions read debugcon only.

## Gotchas / notes

- **`make` does not track `CFLAGS_EXTRA` changes.** Toggling a `-D…` demo flag
  (e.g. `-DM10_RUN_DEMO=1`) does NOT recompile if the `.o` is newer than the
  source → you boot a STALE kernel with the wrong flags. Always `make clean`
  first (or `touch` the source) when changing `-D` flags. The harness does this.
- **`isodir/boot/grub/grub.cfg` is tracked** but `make clean` deletes it; restore
  with `git checkout -- isodir/boot/grub/grub.cfg` before committing (a recurring
  papercut — consider untracking it).
- **DEV signing key**: `crypto/secos_pubkey.h` is the fixed-dev-seed public key;
  `tools/secos-sign --dev` uses the matching seed in `tools/secos_signlib.py`.
  Production swaps in a real key (private half kept offline). Generated headers
  (`secos_pubkey.h`, `signed_test_elf.h`, `user_hello_elf.h`) are committed so the
  kernel build needs no python.
- **User ELFs need `-mcmodel=large`** (code at 4 GB+ → 64-bit relocations).
- **Heap allocator** (`mm/heap.c`) is a simple first-fit; its `kfree` only
  coalesces physically-contiguous blocks (a past bug). A buddy/free-list allocator
  is an M12 item.
- Talk to the user in **Italian**; all project content (code, comments, commits,
  docs) in **English only**.

## M13 recap (done)

Usability & policy enforcement (`docs/devlog/M13.md`):
- **Manifest `max_mem` enforced at load** (headline): a signed program whose
  mapped footprint exceeds its `.note.secos` `max_mem` (0 = unlimited) is refused
  by `process_create_from_elf`, with a leak-free teardown (`elf_unload_process` +
  `vmm_space_destroy`). Signature-rooted: a program can't raise its own ceiling.
- **New syscalls**: `SYS_GETTICKS` (10, uptime); `SYS_MSG_SEND` (11) /
  `SYS_MSG_RECV` (12) over kernel IPC channels (`kernel/ipc.c`: 4 ring buffers,
  non-blocking, user pointers validated + bounce-buffered). libc
  `getticks`/`msg_send`/`msg_recv`.
- **Minimal IPC**: two separately-spawned ring-3 programs talk over a
  kernel-owned channel (no fork/fd inheritance). Demo: producer sends
  `M13-IPC-OK` on channel 0 + reports uptime; consumer reads it. 56/56.
- Shell `run <path>` (M10) is the first-class on-disk launcher.

## M12 recap (core done)

Memory scalability + W^X (`docs/devlog/M12.md`):
- **PMM**: removed the 512 MB `total_frames` clamp and the 128 MB per-region
  identity clamps — the kernel manages all RAM (frames above the identity window
  are reached via the physmap; 32 MB bitmap cap ≈ 1 TB). `find_free_frame` skips
  full bitmap words from a rolling cursor; `pmm_alloc_contiguous`/`_free_contiguous`.
- **Heap**: addresses memory through the physmap (`phys_to_virt`); `expand_heap`
  allocates enough **contiguous** frames; `kmalloc` returns NULL instead of an
  undersized block. Fixes the M11 gotcha — the M11 demo loads from the VFS again.
- **W^X**: `vmm_map`/`vmm_map_in_space` reject RW-without-NX (`-5`); boot tests
  `[WX]` + `[HEAP] large kmalloc(64K) OK`. `-m 2G` → ~2045 MB free. 51/51.
- **Higher-half**: kernel linked + runs at `KERNEL_VMA=0xFFFFFFFF80000000`
  (`-mcmodel=kernel -fno-pic`). `linker.ld` keeps a low `.boot` (MB2 header +
  32-bit setup + initial tables) and links the rest HIGH/loads LOW via `AT()`.
  `boot.asm` maps low identity + high half then jumps high; the UEFI loader loads
  segments at their LMA (`p_paddr`) and maps PML4[511]. `kvirt_to_phys` fixes the
  two symbol-as-physical spots (PMM bitmap math, virtio DMA). **Low identity map
  kept** (virtio DMA + early frame access). Both `--mb2` and `--uefi` PASS.
- **Deferred** (rationale in devlog): full demand paging (`vmm_region` path is
  dormant), W^X of the 0–512 MB identity huge-page map (still RWX).

## M11 recap (done)

Driver Space is now a **signature-rooted** boundary (`docs/devlog/M11.md`):
- `.note.secos` manifest **v2** adds `proc_type`/`dev_id`/`dev_caps` (desc 24→40,
  `version` 2), covered by the Ed25519 signature → the driver claim is trusted.
- `process_create_from_elf` sets `proc_type`; a `PROC_TYPE_DRIVER` manifest
  auto-binds the process to its device with the **granted cap subset**
  (intersected with device-supported). `process_destroy` drops bindings first.
- `SYS_DRIVER` enforces `proc_type==DRIVER` (else `DRV_ERR_NOTDRV`) **and** the
  per-binding granted caps; every event mirrors to debugcon `[DRV-AUDIT]`.
- Demo (`M11_DRIVER_DEMO`): signed `user/driver_demo` does mediated reg
  read/write (value round-trips) and is refused the un-granted `MAP_MEM`; signed
  `user/userprobe` (PROC_TYPE_USER) is denied every `SYS_DRIVER`. Harness 47/47.
- **Heap gotcha**: `mm/heap.c` reliably serves `kmalloc` only ≤ one frame (~4 KB)
  and can silently return an undersized block for larger requests (ramfs copies
  files with one `kmalloc`). The demo therefore loads the embedded signed ELF
  directly (still verified). Real fix = M12 buddy/multi-frame allocator.

## M10 recap (done)

Storage & persistence is complete (`docs/devlog/M10.md`): virtio-blk (`vda`),
VFS multi-mount (`/mnt`), FAT32 + ext2 + ext4 read-write (ext4 no-journal v0),
`SYS_SPAWN`/`SYS_WAIT` + shell `run <path>` with the shell running as the
scheduler idle task. Kernel writes are host-readable and `e2fsck`-clean; a signed
ELF is written to disk, read back, verified and run in ring-3 (tampered refused).

## Running it interactively (WSLg GUI is broken → use serial)

```bash
# UEFI boot (OVMF), interactive shell over serial. Exit QEMU: Ctrl-A then X.
qemu-system-x86_64 -machine q35 -m 256M -bios /usr/share/ovmf/OVMF.fd \
  -drive file=secos-uefi.img,format=raw,if=ide \
  -serial stdio -display none -no-reboot

# Full M10 storage (BIOS/ISO path) — /mnt mounts here; try: vls /mnt, vcat /mnt/HELLO.TXT
make iso && make disk-fat32        # or disk-ext2 / disk-ext4
qemu-system-x86_64 -cdrom myos.iso -drive file=disk.img,if=virtio,format=raw \
  -boot d -serial stdio -display none -no-reboot -m 256M
```

## UEFI bootable image (for VMware / bare-ish UEFI)

- `secos-uefi.img` = a **GPT + ESP (FAT32)** disk built from `dist/` after
  `make && make uefi` (ESP root holds `/EFI/BOOT/BOOTX64.EFI` + `/kernel.elf`;
  the loader reads `kernel.elf` from the volume root). Built by hand this session
  (sgdisk + mkfs.fat + mcopy + dd; converted to `secos-uefi.vmdk` via qemu-img).
  Copied to Windows `C:\Users\Luigi\SecOS\` with a `README-VMware.txt`.
  **TODO (offered, not yet done):** add `make uefi-disk` / `make uefi-vmdk`
  targets so this is reproducible (currently ad-hoc shell steps).
- **Verified booting in QEMU+OVMF** (single disk → reaches the shell, 12/12).
- **Quirk:** with a *second* disk attached (e.g. a virtio data disk), OVMF using
  the read-only `-bios OVMF.fd` drops to the UEFI shell / fails our ESP with
  "Bad Buffer Size" (non-persistent NVRAM boot entries). A `startup.nsh` on the
  ESP didn't fix it. So UEFI+virtio-disk together is flaky under OVMF — give the
  two commands above separately, or use split OVMF_CODE/OVMF_VARS for persistence.
- **On VMware Workstation:** UEFI boot + shell will work; **storage `/mnt` will
  NOT** — VMware has no virtio-blk (offers SATA/NVMe/PVSCSI). An AHCI/NVMe driver
  would be needed (future work). The user was going to try the VMDK in VMware;
  if it didn't boot, ask for the serial log (loader prints `[OK]/[ERR]` lines).

## M14 recap (done, committed `c9b8a4d`)

Full demand paging (`docs/devlog/M14.md`): user pages are no longer eagerly
mapped. `process_create_from_elf` pins a private copy of the signature-verified
ELF image and registers **per-process VMAs** (`mm/vma.{c,h}`, in `process_t`):
FILE (lazy-copied from the image) for ELF segments, ANON (zero-fill) for
stack/BSS. `elf_load_image_lazy` validates + reserves, touches no frame;
`vmm_handle_page_fault` materializes pages on first touch via `vma_fault_in`
(frame filled through the physmap RW alias → RX code fills with no W^X
violation). Syscall buffers fault in via **kernel-mode #PF on a user address**
(the handler keys on address+VMA, not the U/S bit) — `copy_from/to_user`
dereferences directly, so no `user_copy.c` change. Teardown is automatic
(`vmm_space_destroy` frees present leaves; `mapped_pages` removed, no leak). Stack
**guard** = absence of a VMA below it. `max_mem` checked vs reserved footprint.
Demo `M14_DEMAND_DEMO`: signed `hello` shows `mapped at load=0` for a `0x9000`
reserved footprint, 2 pages fault in as it runs. Harness **61/61**. _(Historical
recap — M14 is long merged into `main`.)_

## M23 — POSIX FS personality: DONE

Persistent ext2 root + `/dev` + `/proc` + `/sys` + `lseek`/`stat`. See
`docs/devlog/M23.md`. Highlights:
- **Trust = signature** (signed program → ambient `/dev` access; Driver Space
  orthogonal). **Persistent ext2 root** via the `/.secosroot` marker (data disks
  not grabbed; CI keeps RAMFS root). `make sysdisk-ext2` builds the root image.
- `fs/devfs.c` (char + byte-addressed block nodes), `fs/procfs.c` (generated),
  `fs/sysfs.c` (block tree). `VFS_FS_NOCACHE`; `VFS_MAX_MOUNTS`→12.
  `SYS_LSEEK`/`SYS_STAT` + libc `lseek`/`stat`.
- Verified: signed `user/m23_fs` uses `/dev`+`/proc`+`stat`+`lseek` from ring 3
  (`M23_FS_DEMO`); **two-boot persistence** confirmed (`vcreate` → reboot →
  `vcat` survives). Run a persistent-root VM: `make sysdisk-vmdk`, attach
  `sysdisk.vmdk` as a SATA/NVMe data disk.
- **Foundation, not full Linux source-compat**: `ioctl`, a fuller libc
  (musl/newlib) + wider syscall surface (poll/select/fcntl/pipe/signals), per-pid
  `/proc/<pid>` are the follow-ups.
- **Shell cleanup (M23)**: added POSIX-style commands over the VFS with a real
  working directory — `ls cd pwd cat touch mkdir rm df free uname` (`kernel/shell.c`,
  `cwd_resolve` handles `.`/`..`). The prompt now shows the cwd (`secos:/home$`).
  Legacy `v*`/`rf*` commands kept (init.rc + selftests use `vls`/`vcat`). Verified
  interactively against the persistent ext2 root + /dev + /proc.

## (historical) Next milestone: M23 — POSIX FS personality (AGREED with the user)

The user wants a **Linux-style filesystem** so that open-source Linux projects can
be **compiled-from-source (and signed) for SecOS**, including apps that access
`/dev`. Agreed design:
- **Trust model**: the **signature IS the trust boundary**. Only signed code runs,
  so a signed app gets ambient authority over `/dev` like a normal Linux process.
  **Driver Space is orthogonal** (it governs how a *driver* gets privileged HW
  access; a `/dev/sda` read still goes through the kernel block driver). Caveat to
  keep in mind: with "signed = full trust" the signing key is the whole perimeter
  (a signed binary can `dd` a raw disk) — a conscious trade the user accepts.
- **Root `/` PERSISTENT on disk (ext2)** — not the volatile ramfs. (User confirmed.)
- Scope reality: FHS + `/dev` is the *foundation*; "all Linux projects compile" is
  a long road (libc completeness, full syscall surface incl. `ioctl`, `/proc`).
  Treat M23 as the foundation that grows program-by-program.

M23 build list:
1. Persistent root: mount ext2 as `/` (boot ordering: root before init).
2. FHS skeleton: `/bin /etc /dev /proc /sys /tmp /usr /opt /home /lib /mnt /root`.
3. **devfs** (`/dev`): char (`null,zero,full,random,urandom,console,tty`) + block
   (`sda,nvme0n1,usb0`) → `block_dev_t` byte-addressed. Signed apps open freely.
4. **procfs** (`/proc`): `self`, `<pid>/{status,cmdline,maps}`, `meminfo`,
   `uptime`, `mounts`, `cpuinfo`.
5. minimal **sysfs** (`/sys/block`, `/sys/class`).
6. syscalls/libc: wire `open/read/write/close/lseek` (partly from M20) through VFS
   to these backends; add `ioctl`/`stat`/`fstat` + libc wrappers.

## Other options later (per docs/ROADMAP_TO_COMPLETE_OS.md)
- **USB/IRQ maturity** (M22 follow-ups already landed, DA TESTARE): validate hub +
  MSI-X on hardware; wire NVMe CQ IEN=1 for real interrupt-driven completion;
  multi-LUN MSC; USB3 warm-reset.
- **Pipes + TTY** (now unblocked by fork): anonymous pipes on the `fds[]` table; a
  ring-3 TTY line discipline. Ctrl-C→SIGINT needs a signal-delivery mechanism
  (only fatal termination exists today — see M15).
- **Phase H — storage maturity**: VFS permissions/ownership/timestamps, `mount`/
  `umount` syscalls, symlinks, devfs + procfs; ext4 JBD2 journaling.
- **Phase I — modern platform**: ACPI + APIC/IOAPIC (retire the PIC), TSC/HPET
  timekeeping, then **SMP** (the single biggest correctness effort — do a locking
  audit first).
- Other open stretch: `MAP_SHARED` + shared read-only text (retire the M14
  per-process pinned image via the M20 page cache); W^X of the 0–512 MB identity
  map; real `DRIVER_OP_MAP_MEM`/IRQ-to-driver; argv populated env.
- **Driver space**: real `DRIVER_OP_MAP_MEM` (map device MMIO into the driver
  address space — still a validating stub); IRQ-to-driver (`IRQ_SUBSCRIBE` + IPC
  queue, building on M13's IPC channels); DMA sandbox; auto-restart of a crashed
  critical driver.
- **IPC/usability**: blocking `SYS_SLEEP` + channel wait (current recv is
  poll+yield); `argv`/env to spawned programs; richer pipe semantics (EOF).
- **Storage**: ext4 JBD2 journaling + `metadata_csum`; `SYS_WAIT` blocking
  semantics (currently poll-style: 0=done, 1=running).
