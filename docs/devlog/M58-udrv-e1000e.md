# M58 — the e1000e NIC driver in ring 3

Phase 5 step M58 (`feat/udrv-e1000e`, plan `docs/PHASE5_PLAN.md` §3 "M58",
design D6/D10/D11). The Intel 82574L (QEMU `e1000e`, `8086:10d3`) is driven
by a signed ring-3 program, `/sbin/udrv-e1000e`, on the M57 Driver Space v2
interface. The kernel keeps the interface (`eth0`, IP config, DHCP,
sockets, CAP_NET) through a proxy network device. Opt-in: nothing changes
unless the boot command line names the NIC in `udrv=`.

## Using it

```
make iso UDRV=8086:10d3        # adds "iommu=on udrv=8086:10d3" to the boot entry
```

On that boot: the in-kernel e1000e never sees the function (M57
`pci_registry_reserve`), `eth0` is registered as a proxy with link down,
`/etc/inittab` (the default text) gains `net0:driver:/sbin/udrv-e1000e`,
init starts it before `ifup`, and DHCP runs through it. Without an IOMMU
with interrupt remapping the claim has no DMA rights: the driver says so
and exits 0 (init does not restart it). Boot without `udrv=` to use the
in-kernel driver.

## Kernel side

### The proxy (`net/net_udrv.{c,h}`)

`net_init` registers one `net_dev_t` per network-class function given to
ring 3, after the in-kernel probes (so in-kernel NICs keep their names), and
adds it to the timer-tick poll list (`net_poll_add`, new in
`net/net_core.c`). It is never passed to `net_request_irq` (that would
program the device's MSI-X from the kernel).

`UDRV_OP_NET_ATTACH` (op 11, ABI `user/include/secos/udrv_net.h`) needs a
claim of a network function with DMA rights and the manifest flag
`UDRV_DRV_NET` (v3 `udrv_flags`). It allocates:

- 3 control pages (`struct udrv_net_ctrl`: constants, kernel→driver and
  driver→kernel indices on separate cache lines, rings `tx[256]`, `txc[256]`,
  `rxf[256]`, `rx[256]` of 8-byte `{slot, len}` entries), zeroed, mapped
  RW+NX into the driver (`VMA_VF_DONTFORK|PINNED`, munmap refused). Not in the
  device's IOMMU domain;
- 512 packet slots of 2 KiB (256 TX + 256 RX, 256 pages) from
  `pmm_alloc_contiguous`, mapped only into the claim's IOMMU domain (RW). The
  driver gets their IOVA, never a mapping. They come from the PMM directly,
  not `dma_alloc_coherent`: those are frames of a ring-3 claim's own domain,
  and the coherent table (64 entries, kernel strict domain) is for kernel
  drivers.

The kernel reaches the region only through the physmap. It keeps its own
copy of every index and the owner of every slot (kernel / driver-TX /
driver-RX), and reads each shared field once:

- an index may be ahead of the kernel's copy by at most the ring size;
- a slot must be in range and owned by the driver for that direction;
- a received length must be 14..1536.

The checks are pure functions (`udrv_net_check_index/rx/txc`,
`udrv_net_reject_count`). A rejected entry is dropped and logged as
`[UDRV] eth0 bad ring entry: ...`; the 8th one per claim calls
`udrv_net_kill` (bus mastering off, SIGKILL, `[UDRV] ... network driver
pid=N killed: ...`). Received frames are copied out of the slot into a
kernel buffer (batches of 16) before `net_rx`, outside the proxy lock.

- **TX** (`px_transmit`, under `tx_lock`): reap completions, take a free TX
  slot, copy, publish `tx[]`, set `UDRV_EV_TX_KICK` on the claim fd. When
  every slot is out the frame waits in a bounded kernel queue (256 frames,
  `tx_queued`) and goes out as slots come back; only a full queue drops
  (`tx_full`). See "Bugs found" for why.
- **RX** (`px_poll`, under `rx_lock`): reap TX completions, read `link`,
  consume `rx[]`, repost each slot on `rxf[]`, then `net_rx`. If the pass
  was not the driver's own and the driver held fewer than half of the RX
  slots, it is woken (its NIC ring was running dry). Runs on the
  driver's kick (`write()` of `struct udrv_ack` with `UDRV_ACK_NET_KICK`, in
  the driver's own syscall, no lock held) and from the 1 kHz tick.
- **Stall watchdog**: TX frames queued with no completion for 5 s →
  `[UDRV] eth0 stalled ...`; with `udrv.watchdog=kill` the driver is killed.
- **Detach** (claim teardown step 5, after the device is detached from its
  IOMMU domain and before the frames are freed): link down, queued frames
  dropped, `[UDRV] eth0 driver died pid=N (exit|reap)` or `detached
  (release)`. The interface stays registered with its IP configuration, so a
  restarted driver re-attaches to the same `eth0`; sockets survive (TCP
  retransmits across the gap).

Lock: `px->lk` = `LK_S22_UDRV_NET` (122): inside `tx_lock` N3 and `rx_lock`
N1, never held across `net_rx` (stack N2); inside it the udrv event lock S25
(TX kick, kill) and proc S30. Attach/detach run under `g_udrv_lock` (K45).

### Driver Space changes (`kernel/udrv.c`)

- `op_net_attach`, the teardown step 5 and the region frees (after the IOMMU
  domain is freed);
- `udrv_fd_write`: `UDRV_ACK_NET_KICK` → `net_udrv_kick` after the event
  lock is dropped;
- `udrv_net_notify` / `udrv_net_kill` / `udrv_assigned` / `udrv_has_net`;
- `/proc/udrv`: `live_net_pages=` and `net_waiting=` on line 1, `net_kills=`
  per device, one `net ethN ...` line per proxy (attached/waiting, pid, link,
  attaches, deaths, tx/rx frames, tx_inflight, tx_full, bad entries, stalls),
  and on any boot with `udrv=` the line *"note: blocking of
  compatibility-format and DMA-injected interrupt messages is enforced by the
  IOMMU hardware and verifiable only on real hardware (QEMU does not block
  them)"*;
- boot word `udrv.watchdog=kill`.

### `dma_dev_release` (`kernel/dma.{c,h}`)

There was no way for a kernel driver to give a function back. 
`dma_dev_release(dd)` moves a function in the strict kernel domain back to
the passthrough domain (blocked if there is none), then frees the handle;
`dma_dev_peek(pd)` returns an existing handle without creating one. Users:
every failure path of the in-kernel `e1000e_init` after `dma_dev()` (bus
mastering off first), and `udrv` `assign()` (defensive: no kernel driver has
a handle at that point, since probes run after `udrv_init`).

## The driver (`user/udrv/e1000e.c`)

A port of `drivers/e1000e.c`: claim by id, INFO, command register
MEM|BUSMASTER, `MAP_BAR 0`, device reset, link up, MAC from RAL0/RAH0, one
DMA page each for 256 RX / 256 TX legacy descriptors, `NET_ATTACH`, one
MSI-X vector (IVAR: RxQ0, TxQ0 and Other → entry 0; IMS: TXDW, LSC, RXO,
RXT0, RxQ0, TxQ0, Other). Without a vector (or with `--poll`) it polls every
millisecond. Main loop: service the rings (TX done → `txc[]`, new `tx[]` →
descriptors + TDT, RX done → `rx[]`, free slots → descriptors + RDT, link),
write one ack (unmask + kick), wait in `poll()` on the claim fd (10 ms
backstop), read the events, clear ICR. A frame the NIC flags bad keeps its
slot (reused for the next descriptor). Exit 0 when there is nothing to drive,
2 when the claim ends or the kernel sets `UDRV_EV_STOP`.

Manifest `user/udrv/note_e1000e.S`: v3 DRIVER, confined (`CAP_ENFORCE |
FS_READ | PROC | TIME`, so `no_new_privs`), match `8086:10d3` only,
`UDRV_DRV_NET`, BAR 0, one vector, 64 KiB DMA, `cfg_write_mask = COMMAND`.

## init and sysroot

- `user/init.c`: action `driver` — like `respawn` (own session, log
  `/var/log/<id>.log`), but started before the `sysinit` entries; init then
  waits up to 5 s until `/proc/udrv` says `net_waiting=0` (or every driver
  exited); restart backoff capped at 2 s; exit status 0 = nothing to drive,
  not restarted.
- `kernel/sysroot.c`: installs `/sbin/udrv-e1000e` (new `/sbin`, 0755); four
  default inittab texts (login / root console × with / without the driver
  line). An untouched default follows this boot's console policy and `udrv=`;
  an edited one is left alone, with a boot line saying which entry to add.
- `Makefile`: `UDRV=` → `iommu=on udrv=...` on the default GRUB entry;
  `udrv-progs` builds and signs the driver (`crypto/user_udrv_e1000e_elf.h`).
- `user/udrv_negtest.c` row A02 uses op 12 (11 is NET_ATTACH now).
- `tools/persist_test.sh`: 40 programs.

## Gate (`tools/net_udrv_gate.sh`, `tools/net_udrv_check.py`)

Every guest: q35, `-device intel-iommu,intremap=on` (named explicitly: QEMU
turns IR on by itself under TCG), `kernel-irqchip=split`, e1000e on QEMU
user networking, MB2 ISO `-boot d`, CONSOLE=root, a throwaway SSH key baked
in. Host peers on 127.0.0.1 with kernel-chosen ports (never 2222/8000): an
HTTP server with random 8/32 MiB payloads, a TCP sink that hashes what it
gets, a one-shot echo server; SSH through a hostfwd port.

| guest | image | checks |
|---|---|---|
| udrv-smp2 | `UDRV=8086:10d3` | claim translated + DMA, MSI-X, proxy attached, no in-kernel e1000e; DHCP; 8 MiB download and upload with MD5; DNS (`nslookup` via QEMU's resolver); SSH; perf; 20× `kill -9` of the driver during a download loop: new driver attached with link within 5 s, traffic resumes, `driver died` lines, MemFree+Slab drift of the kill phase within 1 MiB of a control phase with the same commands and no kill, SUnreclaim within 2 MiB, ring-3 page counters (`live_dma/bar/net_pages`) equal, 0 bad entries, download after the loop |
| udrv-smp4 | same | the same with 5 kills at -smp 4 |
| kern-smp2 | `KERNEL_CMDLINE=iommu=on` (no `udrv=`) | fallback: in-kernel e1000e (MSI-X NAPI), no `[UDRV]`, the same data checks, perf |
| netstress-smp2/-smp4 | udrv | `tools/net_stress_drive.py` (the NETSMP workload: TCP echo, firehose closes, UDP floods, DNS, a server with resets, parallel ssh; socket/TCP/UDP leak checks) through the ring-3 driver |
| lockdep-smp4 | udrv + `-DSECOS_LOCKDEP` | the udrv checks (5 kills), `[LDTEST] all 0xA/0xA`, then netstress on it |

Every guest also fails on `[EXC]`, panic, lockdep report, nested fault,
stack smashing, `[SMAP] violation`, `[DMA] BUG`, TLB/invalidation timeouts.
`ping`: BusyBox `ping` needs raw sockets, which SecOS does not have (for any
driver); ICMP echo is not in the gate. The TCP connect+echo round trips
stand in for latency.

## Test record (2026-11-09, `feat/udrv-e1000e`)

`tools/net_udrv_gate.sh`: **7 passed, 0 failed** (udrv-smp2, udrv-smp4,
kern-smp2, netstress-smp2, netstress-smp4, lockdep-smp4,
lockdep-netstress-smp4). Builds: 0 warnings in all three images (base
`0f5ba0d`: 0 normal, 0 lockdep with `make iso CONSOLE=root`).

- kill loop: 20/20 (smp2), 5/5 (smp4, lockdep); respawn 0.1–2.1 s (init's
  driver backoff is at most 2 s after a short run);
- memory, udrv-smp2: kill phase MemFree+Slab drift 940 kB vs control phase
  1480 kB; SUnreclaim 32855 → 31005 kB; ring-3 pages 2 DMA / 32 BAR / 259 net
  before and after; lockdep-smp4: drift −892 kB vs control 984 kB;
- netstress: 25/25 checks in each of the three runs (no socket, TCP, UDP or
  memory leak).

### Performance (QEMU 6.2 TCG, -smp 2, same host, not a gate)

| | ring-3 e1000e | in-kernel e1000e |
|---|---|---|
| 32 MiB download (host → guest, wget) | 237–295 Mbit/s | 253–289 Mbit/s |
| 16 MiB upload (guest → host, nc) | 25.6–26.8 Mbit/s | 45.0–46.1 Mbit/s |
| 200 TCP connect + echo round trips (one nc each) | 60.6–60.9 ms each | 59.1–59.9 ms each |

Download is the same within noise (receive is bounded by the guest stack
and the copy out of the slot). Upload is ~0.55× the in-kernel driver: every
frame is copied into a slot and handed over by a wakeup of a separate
process, and TX completions come back through it too. The round trips are
dominated by starting `nc` (process creation under TCG), so they do not show
the per-frame latency. Ranges are over two runs.

## Bugs found

- **TX drops stalled TCP and lost UDP** (proxy, fixed in `e0bfebd` /
  `192d9cc`). With 128 TX slots and drop-on-full, an 8 MiB upload stalled
  (`tx_full=176`) and NETSMP lost UDP datagrams and DNS replies. The
  in-kernel driver never drops on TX (its transmit waits for the
  descriptor). Two things made drops expensive: the driver is an ordinary
  process and can be off-CPU for a while, and the kernel TCP
  (`net/tcp.c`, unchanged) retransmits only the oldest unacked segment per
  RTO with backoff, so several lost segments take many timeouts. Fix: 256 TX
  slots and the bounded TX queue. **For later:** TCP go-back-N or SACK-less
  fast retransmit would make any lossy NIC (or a real lossy link) behave much
  better; it is a stack change outside M58.
- **The driver must reset the NIC before enabling bus mastering**
  (`becf34d`). The claim teardown's reset is the PM D3hot→D0 cycle (82574L
  in QEMU has no FLR), which does not clear QEMU's descriptor base
  registers. The first version turned bus mastering on, then reset the NIC:
  in the window between the two the device resumed DMA at the old driver's
  IOVAs, which do not exist in the new claim's domain → `[IOMMU] DMA fault`
  → the new driver killed; three in a row quarantined the device (no
  network until reboot). Seen once in the third kill of a 3-kill run.
  For M59: a quarantine after kills that are really the previous claim's
  in-flight DMA is harsh; a device-specific reset hook in the kernel (CTRL.RST
  for 8086:10d3) would make the teardown complete regardless of the driver.
- `nc` with stdin from `/dev/urandom` is very slow under TCG (csprng in
  256-byte chunks); the gate uploads a downloaded file instead.

## Not covered

The proxy's ring checks are separate pure functions (`udrv_net_check_index`,
`udrv_net_check_rx`, `udrv_net_check_txc`, `udrv_net_reject_count`, commit
`9828299`), but M58 has no dedicated test for them: the gate exercises only
well-formed traffic from the real driver (`bad_total=0` is checked).

## VMware recipe (prepared for the user; not yet run)

Goal: the same image on VMware Workstation with a virtual IOMMU, the NIC
driven from ring 3. Report the `[IOMMU]`, `[UDRV]` and `[udrv-e1000e]` lines
and whether SSH works.

1. Build: `make iso UDRV=8086:10d3 SSH_AUTHORIZED_KEYS=~/.ssh/id_ed25519.pub`
   (optionally `CONSOLE=root`). The ISO is a GRUB hybrid (BIOS + UEFI).
2. VM settings (Workstation 15.5+, hardware version 14 or later):
   - Options → Advanced → Firmware type **UEFI** (the virtual IOMMU needs
     it); Secure Boot off;
   - Processors → **Virtualize IOMMU (IO memory management unit)** on;
   - CD/DVD → the ISO, connected at power on;
   - Network adapter: edit the `.vmx` so it is an 82574L:
     `ethernet0.virtualDev = "e1000e"` (NAT or bridged);
   - Serial port → output to a file (the debugcon port does not exist on
     VMware; the console shows the same lines).
3. Boot the first GRUB entry ("SecOS x64"). Expected:
   - `[IOMMU] ...` DMAR found, translation on, `[IOMMU] IR: on`;
   - `[UDRV] udrv= 0x:00.0 8086:10d3 assigned to ring 3 (iommu: blocked)`;
   - `[NET] eth0: ring-3 driver proxy for ...`;
   - `[init] started net0 (/sbin/udrv-e1000e)`, then
     `[UDRV] claim ... iommu=translated dma=yes`, `[UDRV] eth0 driver
     attached`, `[UDRV] irq_setup ... msix nvec=1`;
   - `ifup: eth0: lease ...`.
4. Check in the shell: `cat /proc/udrv` (device claimed, `net eth0 ...
   attached link=1`, `faults=0`), `cat /var/log/net0.log`, `ifconfig`,
   `wget -O /dev/null http://<host>/<file>`; SSH in from the host.
5. If the claim says `dma=no` (no interrupt remapping on this VMware
   version), the driver logs "no DMA rights" and exits: boot the second
   build without `UDRV=` to get the in-kernel driver back, and send the
   `[IOMMU]` lines.
6. `kill -9 $(pidof udrv-e1000e)` once: `[UDRV] eth0 driver died`, a new
   driver attaches within ~2 s, SSH reconnects (or the session survives).
