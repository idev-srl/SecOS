# SECoS — Task List & Status

_Snapshot: HEAD `5e471ac` on `main` (pushed to `origin/main`). Self-test **122/122**._
_Last updated: 2026-06-19._

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
| M15 | Fault-driven process kill (ring-3 fault kills only the process) | ✅ done |
| M16 | Exec model: argv/env + blocking `SYS_WAIT` | ✅ done |
| M17 | Blocking primitives: `SYS_SLEEP` + blocking `SYS_MSG_RECV` | ✅ done |
| M18 | Dynamic memory: `mmap`/`munmap`/`mprotect`/`brk` | ✅ done |
| M19 | Copy-on-write `fork` + PMM frame refcounts | ✅ done |
| M20 | Unified page cache backing `read()` and file-backed `mmap` | ✅ done |
| M21 | AHCI/SATA block driver (`sda`..`sdd`) | ✅ `M21_STABLE` |
| M22 | NVMe (`nvme0n1`) + USB stack (xHCI / HID keyboard / Mass Storage) | ✅ `M22_STABLE` |
| M23 | POSIX FS personality: persistent ext2 root + `/dev` + `/proc` + `/sys` | ✅ done |
| M24 | **Networking**: e1000/e1000e drivers, ARP/IPv4/ICMP/UDP/DHCP/DNS/TCP, BSD sockets + CAP_NET, MSI-X/NAPI RX | ✅ done, validated on real VMware |
| M25 | **Pipes + TTY + TCP throughput**: `SYS_PIPE` (blocking, EOF/EPIPE, fork-inherited), console cooked line discipline (echo/backspace/Ctrl-D/Ctrl-C), TCP window 8K→64K + deeper NIC RX rings | ✅ done (pipe demo verified; TTY needs interactive test; throughput needs VMware re-measure) |

---

## 🔶 DONE-BUT-UNVALIDATED (works in QEMU / logic only — needs real hardware)

- [ ] **vmxnet3 driver** — compile-clean, QEMU has no model → untested on real HW.
- [ ] **igc driver (2.5 GbE, I225/I226)** — compile-clean, needs the real adapter.
- [ ] **82574 (e1000e) MSI-X re-arm tuning** — IVAR/EIAC so the poll backstop can
      eventually be dropped (currently a hybrid: MSI-X + poll backstop).
- [ ] **USB hub support** (`drivers/usb_hub.c`) — logic-verified only; QEMU CI has
      no hub. DA TESTARE su hardware reale.
- [ ] **MSI/MSI-X for NVMe/xHCI** — additive, OFF by default (kernel still polled);
      gated behind `-DXHCI_USE_IRQ`/`-DNVME_USE_IRQ`. NVMe I/O CQ still IEN=0 →
      needs recreate with IEN=1 + vector to truly fire. Needs LAPIC + HW validation.
- [ ] **USB Mass Storage on real hardware** — verified in QEMU, not yet on a real stick.

---

## 🎯 CANDIDATE NEXT WORK (pick one to start)

### Option 1 — TCP throughput optimization ✅ DONE (M25)
- [x] Bump TCP window/buffer from 8 KB → **64 KB** (`net/tcp.c`).
- [x] Larger RX rings (e1000 64→256, e1000e 32→256; TX 16/32→64).
- [ ] **Re-measure on real VMware** (was ~270–460 Mbit/s; ceiling = `window / RTT`).
- Next lever if needed: TCP **window scaling** (>64 KB for high-BDP links).

### Option 2 — NIC hardware validation _(needs real adapters)_
- [ ] Validate vmxnet3 on real VMware.
- [ ] Validate igc (2.5 GbE) on real I225/I226.
- [ ] Tune 82574 MSI-X re-arm; try dropping the poll backstop.
- _Scope: blocked on physical hardware availability._

### Option 3 — Pipes + TTY ✅ DONE (M25, except async signals)
- [x] Anonymous pipes on the `fds[]` table (`SYS_PIPE`, blocking, EOF/EPIPE,
      fork-inherited refcounts). `kernel/pipe.{c,h}`.
- [x] Ring-3 TTY cooked line discipline (echo, backspace, Ctrl-D EOF, Ctrl-C
      line-cancel) on fd 0 + `/dev/tty`/`/dev/console`. `kernel/tty.c`.
      **Verified** 2026-06-19 via scripted serial input (`cat /dev/tty`): echo,
      line return, backspace (`abcXX`+2⌫+`YZ`→`abcYZ`), Ctrl-D EOF, Ctrl-C `^C`.
- [ ] **Async signal delivery (Ctrl-C → SIGINT)** still deferred — today Ctrl-C
      only interrupts an in-progress read; killing a compute-bound foreground
      process needs a real signal subsystem (SIGPIPE too). _Prerequisite for_
      _shell job control._
- [ ] **Shell pipelines** (`cmd1 | cmd2`) — now unblocked (pipes + fork + exec).

### Option 4 — Phase H: storage maturity  ✅ COMPLETE (M26 + M27a + M27b)
- [x] VFS permissions / ownership / timestamps — **M26** (stored & exposed, not
      enforced; signature = trust boundary). chmod/chown/utimes + extended stat.
- [x] `mount` / `umount` syscalls — **M26**.
- [x] Symlinks — **M26** (ext2 fast symlinks; `stat` follows final component).
      _Limitation: mid-path symlink components not followed yet._
- [x] **JBD2 journal replay (read-side)** — **M27a**: safely mount a dirty
      ext3/ext4 left by a crash on another OS. 3-pass recovery, e2fsck-validated.
- [x] **Write-side journaling** — **M27b**: SecOS's own metadata writes are
      crash-atomic (transaction → commit → checkpoint; read-your-writes; atomic
      rollback). Proven with a fault-injection harness (crash before commit → op
      absent; after publish → replayed; always e2fsck-clean). `metadata_csum`
      intentionally not implemented (journals to `^metadata_csum` volumes).
      **➡ Phase H COMPLETE.**

### Option 5 — Phase I: modern platform → **MULTICORE / SMP** _(biggest effort)_

Multicore is **not a standalone task** — it's the top of a prerequisite chain. The
kernel today is written assuming a single core (PMM, scheduler, heap, VFS, page
cache are all SMP-unsafe), and there is no way to even discover the other cores
yet. The chain below must be done roughly in order; SMP is the last and riskiest step.

- [x] **1. ACPI table parsing** — **M28-1 done**: RSDP (UEFI loader / MB2 scan) →
      XSDT/RSDT → MADT → CPU LAPIC IDs+count, LAPIC base, IOAPIC(s), overrides.
      `arch/x86/acpi.{c,h}`; verified both boot paths, scales with `-smp`.
- [ ] **2. APIC / IOAPIC** — retire the legacy 8259 PIC; route IRQs through the
      IOAPIC; per-core LAPIC (EOI, IPIs). Prerequisite for per-core interrupts.
- [ ] **3. TSC / HPET timekeeping** — reliable time source + per-core LAPIC timer
      (replaces the single PIT/IRQ0 tick the scheduler uses today).
- [ ] **4. AP bring-up** — start the Application Processors via the INIT–SIPI–SIPI
      sequence + a real-mode→long-mode trampoline per core; per-core GDT/IDT/TSS
      and per-core kernel stacks; per-core current-task pointer.
- [ ] **5. Locking audit + SMP-safe core** — the single biggest correctness effort.
      Make PMM, scheduler (run queues), heap (`kmalloc`/`kfree`), VFS, page cache,
      and IPC channels SMP-safe (spinlocks / per-CPU data / lock-free where it fits).
      Do this audit BEFORE flipping on multiple cores, not after.
- [ ] **6. SMP scheduler** — per-CPU run queues, load balancing, IPI-based
      reschedule/TLB shootdown.
- _Scope: very large; this is the long pole of the entire roadmap. Best tackled as
  several sequenced milestones (one per step above), not one big push._

---

## 🧹 Smaller open items / tech debt (independent of milestone choice)

- [ ] **Makefile: no header-dep tracking** — `.h`/`-D` changes need a manual
      `make clean`. Adding proper dependency tracking would remove a recurring
      "stale kernel" footgun.
- [ ] **`isodir/boot/grub/grub.cfg` is tracked but `make clean` deletes it** —
      consider untracking it (recurring papercut).
- [ ] **Heap allocator** (`mm/heap.c`) is simple first-fit — a buddy/free-list
      allocator was an old M12 wishlist item (the >4 KB bug is fixed, but it's still
      basic).
- [ ] **`MAP_SHARED` + shared read-only text** — retire the M14 per-process pinned
      ELF image via the M20 page cache.
- [ ] **W^X of the 0–512 MB identity huge-page map** — still RWX (deferred since M12).
- [ ] **Driver Space depth**: real `DRIVER_OP_MAP_MEM` (map device MMIO into the
      driver space — still a validating stub); IRQ-to-driver (`IRQ_SUBSCRIBE` + IPC
      queue); DMA sandbox; auto-restart of a crashed critical driver.
- [ ] **POSIX completeness** (for compiling more OSS from source): `ioctl`, fuller
      libc (musl/newlib), wider syscall surface (poll/select/fcntl), per-pid
      `/proc/<pid>`.

---

## ⚠️ Process reminders (don't relearn the hard way)
- **`make clean` after any `.h` or `-D` change** — Makefile ignores those.
- **NIC RX only works in the shell/idle context** — test networking from the shell.
- **Don't build concurrently with `tools/selftest.sh`** (shared build dir + /tmp logs).
- **Never `pkill -f "qemu-system"`** in a command that also launches qemu.
- **After worktree agents**: verify `git rev-parse --abbrev-ref HEAD` == `main`.
- Talk to the user in **Italian**; all project content in **English only**.
