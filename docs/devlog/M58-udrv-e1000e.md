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

- 2 control pages (`struct udrv_net_ctrl`: constants, kernel→driver and
  driver→kernel indices on separate cache lines, rings `tx[128]`, `txc[128]`,
  `rxf[256]`, `rx[256]` of 8-byte `{slot, len}` entries), zeroed, mapped
  RW+NX into the driver (`VMA_VF_DONTFORK|PINNED`, munmap refused). Not in the
  device's IOMMU domain;
- 384 packet slots of 2 KiB (128 TX + 256 RX, 192 pages) from
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
  slot (none: the frame is dropped, `tx_full` counted, the driver nudged),
  copy, publish `tx[]`, set `UDRV_EV_TX_KICK` on the claim fd.
- **RX** (`px_poll`, under `rx_lock`): reap TX completions, read `link`,
  consume `rx[]`, repost each slot on `rxf[]`, then `net_rx`. Runs on the
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
DMA page each for 256 RX / 128 TX legacy descriptors, `NET_ATTACH`, one
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

## Tests

(see "Test record" below)

## VMware recipe (prepared for the user; not yet run)

TBD
