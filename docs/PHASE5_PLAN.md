# Phase 5 plan: isolated drivers (M53–M64)

Written from main `c8d41db` (Phase 4 + M50–M52 done, default `bkl=off`) by a
planning agent. File and line references are for that commit. The design
decisions are recommendations. The ones marked **DECIDE** need the user's
answer before the step that depends on them starts. Implementers must check
every claim against the code.

What shapes this plan:
- **Driver Space today is a mock.** M11 made the *privilege* real: it is rooted
  in the signed manifest, enforced, and audited. But nothing behind it touches
  hardware. Registers are a kmalloc'd shadow buffer and `MAP_MEM` is a
  validation-only stub. Phase 5 builds the hardware path from scratch, next to
  the M11 path, which keeps working.
- **Every DMA buffer in the kernel is a static array in the kernel image**
  (`.bss`, physical load at 2 MiB, unaffected by KASLR). So "IOVA = physical
  address, mapped only for the image's DMA buffers" is a cheap, strong first
  confinement for the in-kernel drivers.
- **The user's only real laptop (ASUS E406S, Braswell) probably has no VT-d.**
  The no-IOMMU path is the one real hardware will take, so it must be safe and
  must change nothing. The real-hardware validation target for the IOMMU path
  is VMware with "Virtualize IOMMU" enabled.
- **Zero regressions.** Every step is opt-in (`iommu=on`, `udrv=`) until its
  gates are green. Each step keeps a boot-time fallback, like `bkl=on` did.

---

## 1. Current state

### 1.1 Driver Space (M1/M11)
| Piece | Where | State |
|---|---|---|
| `SYS_DRIVER` (struct `driver_call_t`: opcode/target/value/flags/device_id) | `kernel/syscall.c:1769`, `kernel/driver_if.c:176-206` | Works; M3-hardened copy in/out |
| Device registry | `driver_if.c:23-35` | **Two synthetic seed devices** (ids 0, 1) with made-up `reg_base`/`mem_base`; no PCI link |
| READ_REG / WRITE_REG | `driver_if.c:217-231` | Read and write a **kmalloc'd shadow buffer**, not hardware |
| MAP_MEM / UNMAP_MEM | `driver_if.c:234-237` | **Stub**: range check only, `// TODO: use vmm to map` |
| Driver identity | manifest v2 `proc_type/dev_id/dev_caps` (`mm/elf_manifest.h:71-107`, `user/note_driver.S`); auto-bind in `process_create_from_elf` (`kernel/process.c:846-860`, `:1008`) | Real, signature-rooted |
| Teardown | `driver_remove_all_bindings` first in `process_destroy` (`process.c:969`, `:1027`); fork drops the privilege (`process.c:1038`) | Real |
| Rate limit, audit ring, `[DRV-AUDIT]` | `driver_if.c:144-174`, `:240-256` | Real; `driver_if_lock` S15 |
| Caps | `kernel/cap.h`: `CAP_AUDIT_DRIVER` (bit 31) is audit-only; no `CAP_*` gates drivers | — |
| Demo/gate | `user/driver_demo.c` (granted reg R/W, MAP_MEM refused), `user/userprobe.c` (all refused), `M11_DRIVER_DEMO` | 9 selftest assertions |

**Missing:**
- binding to real PCI functions;
- MMIO BAR mapping into the driver;
- mediated PCI config access;
- IRQ delivery to ring 3;
- DMA memory and any DMA isolation;
- a way for a ring-3 driver to feed the network stack, input core or block layer;
- supervision and restart of a driver;
- a policy for "this device belongs to ring 3, the in-kernel driver must not probe it".

### 1.2 PCI, MSI and vectors
- `drivers/pci.c`:
  - Config access is legacy CF8/CFC only, with **no lock**. A CF8 write and the CFC access are two port operations, so concurrent config accesses on SMP race. This is latent today because probing happens at boot.
  - No ECAM/MCFG, so extended config space (ACS, ATS, AER, the PCIe FLR capability) is unreachable.
  - No device table: each driver brute-scans buses 0..255 (`pci_find`, `pci_find_class`, `:54`, `:78`).
  - `pci_bar_map` (M49) maps BARs UC into the physmap.
- MSI/MSI-X (`pci.c:182-287`):
  - Programs **MSI-X table entry 0 only**, to LAPIC id 0 (the BSP).
  - No per-vector mask/unmask API.
  - MSI is forced to one message.
- Vectors are **hard-coded per subsystem** (`arch/x86/idt.h:81-83`): `0x40` xHCI, `0x41` NVMe, `0x42` NET. Each has its own NASM stub (`isr_net`, `arch/x86/idt_asm.asm:417`). There is no vector allocator and no generic dispatch.
  - `idt_install_msi_vector` (`idt.c:51`) installs a handler.
  - `net_request_irq` (`net/net_core.c:94`) wires only the first NIC. MSI-X with a poll backstop is the default RX path. QEMU's legacy `e1000` has no MSI capability, so it stays polled.
  - xHCI and NVMe IRQs are compile-time opt-in (`-DXHCI_USE_IRQ`/`-DNVME_USE_IRQ`).
- IOAPIC routing: `ioapic_route_irq(irq, vector, uint8_t dest)` (`arch/x86/lapic.c:179`), used for IRQ1 and IRQ12 only. The destination is 8-bit, which is a limit once interrupt remapping or x2APIC IDs above 255 matter.

### 1.3 DMA users (all assume bus address == physical address)
| Driver | DMA memory | Address source |
|---|---|---|
| virtio-blk (legacy/transitional) | static `g_vq_mem`, `g_bounce`, `g_hdr` (`virtio_blk.c:100-102`) | `kvirt_to_phys` |
| AHCI | static `g_clb/g_fis/g_ctba/g_dma` per port (`ahci.c:66-70`), 4 GiB check `:203` | `phys_of()` = `kvirt_to_phys` |
| NVMe | static queues + `g_dma` 64 KB bounce (`nvme.c:57-63`) | same |
| xHCI + USB core/HID/hub/MSC | static rings, DCBAA, scratchpad, contexts, `g_bounce32` (`xhci.c:113-132`), class buffers static (`usb.c:19`, `usb_hid.c:76-78`, `usb_hub.c:68`, `usb_msc.c:50-52`) | same |
| e1000 / e1000e / igc / vmxnet3 | static rings + per-slot buffers (`e1000.c:76-79`, `e1000e.c:113-116`, `igc.c:141-144`, `vmxnet3.c:229-239`) | same |
| ath9k port | static `g_rxpool` (`drivers/ath9k_port/ath9k_secos.c:169`), `ath9k_host_v2p` (`ath9k.c:150`) | same (TX path to verify) |

No driver DMAs into heap, page-cache or user frames. Storage bounces through static buffers. This is the property M55 exploits.

DMA32 is served by `pmm_alloc_below`/`pmm_alloc_contiguous_below` (`mm/pmm.c:630`, M49). Device memory can be mapped into user space through `VMA_TYPE_PHYS` + `VMM_FLAG_DEVICE` (M40 fb0: `mm/vma.c:51/:156`, `vmm.c:964/:1065/:1590`). Such frames are never freed by unmap/exit, and **fork shares them verbatim** (`vmm.c:1065`), which is wrong for driver mappings.

### 1.4 Waiting and events
- `poll()` (`kernel/kiface.c:452-540`) knows pipes, sockets, evdev, ptys and the console through `ki_fd_revents`, with immediate wake sources (`wait_pty`, `wait_tty`) and a 10 ms re-check otherwise. A new fd kind slots in there.
- The fd table is `struct proc_fd_entry` (`kernel/process.h:192`): `is_pipe`, `is_sock`, inode-tagged kinds.
- Supervision exists: `/bin/init` respawns `/etc/inittab` entries with backoff (`user/init.c`). It has no "start before sysinit" ordering.

### 1.5 QEMU on this host (checked)
| Item | QEMU 6.2 (`qemu-system-x86_64`) | QEMU 9.2 (`~/qemu-local/bin/qemu-system-x86_64`) |
|---|---|---|
| `intel-iommu` options | `aw-bits`(39), `caching-mode`, `device-iotlb`, `dma-drain`, `eim`, `intremap`, `pt`, `version`, `x-buggy-eim`, `x-scalable-mode` | + `dma-translation`, `snoop-control`, `stale-tm`, `x-pasid-mode` |
| `intel-iommu,intremap=on` (TCG) | boots | boots |
| `eim=on` (needed for interrupt remapping with x2APIC) | **refused**: "eim=on requires accel=kvm,kernel-irqchip=split". TCG 6.2 has no x2APIC anyway (M48 devlog), so xAPIC + IR is what 6.2 tests | **works** under TCG with `-cpu max,+x2apic` |
| KVM | `/dev/kvm` exists, but **this user cannot open it** (not in group 993): TCG only, so `kernel-irqchip=split` cannot be tested | same |
| `amd-iommu` | `device-iotlb`, `intremap`, `pt`; boots on q35 TCG | + `xtsup` (x2APIC) |
| `-machine pc` + intel-iommu | refused (q35 only) | — |
| virtio-blk `iommu_platform=on` | **refused on a legacy/transitional device** ("VIRTIO_F_IOMMU_PLATFORM was supported by neither legacy nor transitional device"); accepted with `disable-legacy=on` (our driver cannot drive that) | — |
| `edu` test device (DMA engine + MSI) | present (`dma_mask` option) | present |

Consequences:
- **Legacy virtio-blk DMA bypasses the vIOMMU in QEMU.** Without `ACCESS_PLATFORM`, QEMU uses plain guest memory, so the `vda` root keeps working with translation on, but it is never isolated. AHCI (q35's built-in ich9-ahci), NVMe, xHCI, e1000 and e1000e all go through the vIOMMU.
- Every Phase 5 gate runs on **q35** (MB2 ISO: `-cdrom … -boot d`, as M21 does). The default `make run*` and most selftest guests are i440fx, which cannot have an IOMMU. Put `-device intel-iommu` first on the command line, before any PCI device.
- `edu` is the ideal device for the IOMMU and user-IRQ gates:
  - its DMA engine copies between an internal buffer and an arbitrary bus address we choose;
  - its MSI is raised by a register write;
  - its default 28-bit `dma_mask` also exercises IOVA-allocator limits.

---

## 2. Design decisions

### D1. IOMMU abstraction: vendor-neutral ops, VT-d first
**Interface** (`kernel/iommu.h`):

```c
struct iommu_domain;                       /* one I/O address space */
int  iommu_present(void);                  /* a DMA-remapping unit is active */
struct iommu_domain* iommu_domain_new(int kind);   /* IDOM_BLOCKED/IDENTITY/PT/TRANSLATED */
int  iommu_map(struct iommu_domain*, uint64_t iova, uint64_t phys, uint64_t len, int prot);
int  iommu_unmap(struct iommu_domain*, uint64_t iova, uint64_t len);   /* returns after IOTLB flush completes */
int  iommu_attach(struct iommu_domain*, const pci_dev_t*);   /* context entry; flush */
void iommu_detach(const pci_dev_t*);       /* -> blocked domain */
int  iommu_irq_compose(const pci_dev_t*, int vec, int cpu, uint64_t* addr, uint32_t* data); /* IR or plain */
```

- VT-d (`arch/x86/vtd*.c`) implements it in M54–M56. AMD-Vi (M64) implements the same ops, so nothing above the interface changes.
- **Rejected:** an Intel-only API used directly by the drivers. It would force a rewrite for AMD.

### D2. Domain model and what in-kernel drivers get
Options:
- **(a) Passthrough** (context `TT=10b`, needs `ECAP.PT`): kernel devices DMA untranslated. Zero risk, zero protection.
- **(b) One shared "kernel DMA domain", identity-mapped (IOVA == phys) over only the registered DMA regions.** No driver-visible address change. A misbehaving or compromised kernel-driven device can no longer write kernel code, the heap, the page cache or user memory.
- **(c) A private translated domain per device for every kernel driver.** Linux-strict. Real IOVA allocation in every driver and an unmap/flush on every free. Little extra value over (b) while all DMA buffers are static.

Recommendation:
- **Ring-3 drivers always get (c)**: a private translated domain with IOVA ≠ phys on purpose, so a driver that assumes physical addresses fails loudly.
- **Kernel drivers: (a) in M54, (b) from M55** (`iommu.kernel=strict`), with `iommu.kernel=pt` kept as the fallback boot word.
- **Unknown or undriven PCI functions:** passthrough in M54, so turning VT-d on changes nothing for them. M59 moves them to the blocked domain in strict mode. Exception: devices named by an RMRR always keep their RMRR identity maps.

### D3. DMA API shape (`kernel/dma.{c,h}`)
- `dma_dev_t* dma_dev(const pci_dev_t*)`: carries the DMA mask and the domain.
- `dma_addr_t dma_static(dma_dev_t*, const void* kva, size_t len)`: for the existing static buffers. Returns `phys` (identity) and asserts that the range lies in the `.dmabss` region.
- `void* dma_alloc_coherent(dma_dev_t*, size_t, dma_addr_t* out, int flags)` / `dma_free_coherent`: page granular, zeroed, honours the mask (DMA32 through `pmm_alloc_below`). With an identity domain it maps phys→phys; with a translated domain it allocates an IOVA.
- `dma_map_single`/`dma_unmap_single`: defined now for the future (nothing uses them yet). `dma_sync_*` are no-ops on x86 (coherent). The IOMMU's own page tables need `clflush` when `ECAP.C=0`; that is IOMMU-internal.
- **Linker:** a new page-aligned `.dmabss` output section (`linker.ld`, inside the `.bss` area, start/end symbols). Driver buffers are tagged `__dma` (`#define __dma __attribute__((section(".dmabss")))`, keeping each buffer's own alignment). The kernel DMA domain identity-maps exactly `[__dma_start, __dma_end)` plus `dma_alloc_coherent` frames. `.text`/`.rodata`/`.data` and the rest of `.bss` are invisible to devices: W^X extended to devices.
- **Debug:** `-DSECOS_DMA_DEBUG` checks every address handed to hardware against the registered regions, even with no IOMMU (i440fx). It catches an unconverted buffer before an IOMMU fault would.
- **Rejected:** converting every driver to real dynamic `dma_map_single` now. It is a large churn for no isolation gain while the buffers are static.

### D4. Interrupt remapping: yes, as its own milestone (M56), required by default for ring-3 bus-master drivers
- **Why it matters:** without IR, a device that can DMA can also write any `{address 0xFEEx_xxxx, data}` pair. That is an MSI, so the device can raise **any vector on any CPU** (exception vectors, `0x80`, the IPI vectors `0xF0`–`0xF2`). This is true even when the kernel keeps the MSI-X table pages out of the driver's BAR mapping: the device's DMA engine can write to the MSI window directly.
- **What IR gives:** each source is pinned to an IRTE (with source-id validation, SVT=1 for the owning BDF), and anything else is blocked and logged.
- **Cost:**
  - every interrupt source must switch to remappable format at once: IOAPIC RTEs for IRQ1/IRQ12, every MSI/MSI-X. The IOMMU's own fault interrupt is not remapped;
  - x2APIC needs EIM (honour the DMAR `X2APIC_OPT_OUT` flag);
  - QEMU 6.2 can only test xAPIC + IR; QEMU 9.2 tests x2APIC + EIM.
- **Policy:** with DMA remapping but no IR, a ring-3 bus-master claim is refused unless the boot word `udrv.allow_no_ir` is set (logged, shown in `/proc/udrv`). **DECIDE (Q4).**

### D5. IRQ delivery to ring 3: a per-claim **IRQ fd with UIO semantics**
Options:
- (a) The M13 IPC channels. Rejected: only 4 global channels, message-oriented, not pollable together with other fds.
- (b) A new blocking `udrv_wait()` syscall. Simple, but the driver cannot also wait on its other fds.
- (c) **An fd** in the style of Linux UIO/VFIO, pollable. Recommended.

Semantics of the fd:
- `read()` returns a fixed `struct udrv_events { u64 irq_count; u32 vec_pending_mask; u32 flags; }`. `flags` carries `UDRV_EV_TX_KICK` (the kernel queued frames for the driver) and `UDRV_EV_STOP` (the kernel wants the driver to quiesce). `read()` blocks, or returns `EAGAIN` when non-blocking.
- `write()` of `struct udrv_ack { u32 vec_mask; }` acknowledges and **unmasks** those vectors.
- `poll()`: `POLLIN` when anything is pending. It gets a new immediate wake source in `ki_fd_revents`/`sys_poll`, not the 10 ms re-check.

Kernel side of each interrupt (generic dynamic-vector stub, M53c):
1. Look up the binding.
2. **Mask the source.** The mechanisms, in order of preference: MSI-X entry mask bit (through the kernel's own mapping of the table); MSI per-vector mask (when the device has `PVM`); the IRTE present bit (when IR is on); MSI enable off as a last resort.
3. Count it and mark it pending.
4. Wake the driver.
5. LAPIC EOI.

Masking until the driver acks bounds the interrupt rate to the driver's service rate: a slow or hostile driver cannot storm a CPU. MSI-X latches the pending bit, so nothing is lost.

**INTx is out of scope** until M61, because the lines are shared and level-triggered. It would follow the VFIO model (mask with the PCI 2.3 `INTx Disable` command bit, unmask on ack) and only for DisINTx-capable functions.

### D6. Driver ↔ kernel subsystem data path: kernel-owned shared rings, zero copy on the device side
The kernel stays the owner of `eth0`: IP config, sockets, DHCP, CAP_NET. The ring-3 driver is an L2 device.
- **`net/net_udrv.c`**: a proxy `net_dev_t` whose `transmit`/`poll` talk to a shared region.
- **The shared region:**
  - allocated by the kernel (zeroed PMM frames);
  - mapped into the driver RW+NX as a `VMA_TYPE_PHYS` DONTFORK region;
  - the packet-buffer part is also mapped into the device's IOMMU domain;
  - **reached by the kernel only through the physmap**, never through the user VA. This keeps it SMAP-clean and immune to munmap races.
  - Layout: header + TX ring + TX completion index + RX ring + RX free ring + N 2 KB packet slots.
- **TX:** `transmit()` (under `tx_lock` N3, maybe in IRQ context):
  1. copies the frame into a free slot;
  2. publishes the producer index;
  3. sets `TX_KICK` and wakes the driver.

  A full ring makes `transmit()` return an error and the frame is dropped; TCP retransmits. The driver points its NIC TX descriptors at the slot's IOVA, with no copy.
- **RX:** the driver posts free slots as NIC RX buffers. On completion it writes `{slot, len}` to the RX ring and kicks through the IRQ fd (`write` with a `RX_KICK` bit). That is `net_dev_poll(proxy)`, which validates and calls `net_rx` (which already copies the frame out).
- **Hostile-input rules** (the driver is untrusted; the kernel reads shared memory once per field):
  - indices are masked to the ring size and compared against the kernel's own copy;
  - `len ≤ NET_FRAME_MAX`;
  - a slot must be one the kernel handed out;
  - violations are counted, logged as `[UDRV] eth0 bad ring entry`, and dropped. Repeated violations kill the driver.
- **The same pattern later for:**
  - input: `drivers/input_udrv.c`, a proxy `input_dev_t`; events are a ring of `{type, code, value}` checked against the declared capability bits;
  - block (M63): request/completion rings, data in kernel-owned bounce slots.

### D7. Assignment, identity and capabilities
**Manifest v3** (QSEC `version 3`, descriptor grows; v1/v2 still parse; the signature covers it as before):

```
proc_type = DRIVER
match:     vendor, device (0xFFFF = any), class/subclass (0xFF = any), up to 4 entries
resources: bar_mask (MMIO BARs it may map), irq_max, dma_max (bytes),
           cfg_write_mask (which whitelisted config fields it may write)
flags:     DRV_NET | DRV_INPUT | DRV_BLOCK (which kernel proxy it may attach)
```

**Two keys are needed to drive a device from ring 3:**
1. The **signed** driver claims the device in its manifest match.
2. The **local boot policy** gives that device to ring 3: `udrv=8086:10d3[,…]` or `udrv=00:03.0` on the MB2 cmdline / UEFI load options. Phase 5 has no runtime unbind.

**DECIDE (Q5).** Devices listed in `udrv=` are skipped by the in-kernel probe: `pci_registry_is_udrv(dev)` is checked in `net_init`, `usb_init`, etc. They wait unclaimed, with bus mastering off.

**Claim rules:**
- euid 0 at claim time. init starts drivers as root; the driver then `setresuid`s to a dedicated `drv` user.
- At most one claimant.
- The claimant should be `CAP_ENFORCE`-confined (`note_udrv.S`: no FS write, no sockets, no signals to others), and `no_new_privs` follows automatically (M46).
- Each CLAIM is audited (`[UDRV] claim pid=… bdf=… ok|refused(reason)`), plus `CAP_AUDIT_DRIVER`.

**The M11 path stays.** `SYS_DRIVER` keeps the synthetic devices and the M11 gate. The new API is a separate syscall, so nothing regresses. **DECIDE (Q9)** whether to retire M11 later.

### D8. New syscall, not more `SYS_DRIVER` opcodes
`SYS_UDRV` = **92** (free; 92–94 are unused). It takes `(op, handle, struct* arg, size)`. Every struct is copied in and out with `copy_from/to_user`.

| Op | Effect |
|---|---|
| `CLAIM {match or bdf}` | → handle fd (the IRQ/event fd) |
| `INFO` | ids, BARs, MSI/MSI-X counts, IOMMU/IR state |
| `MAP_BAR {bar}` | `VMA_TYPE_PHYS` UC NX shared DONTFORK. **Holes** where the MSI-X table and PBA live (those pages are never mapped; the kernel keeps them). I/O-port BARs refused. |
| `CFG_READ` | any offset |
| `CFG_WRITE` | whitelist only: the command register (MEM, BUSMASTER; BUSMASTER refused without an IOMMU), device-specific config above 0x40 outside capability structures, as `cfg_write_mask` allows. Never BARs, MSI/MSI-X, PM or the capability chain. |
| `DMA_ALLOC {size, dir}` | → `{uva, iova}`. Zeroed frames, user RW NX, device R/W/RW per `dir`; charged to `dma_max` and `max_mem` |
| `DMA_FREE` | device unmap + IOTLB flush **before** user unmap and frame free. `munmap` of a DMA VMA is refused (`EINVAL`) |
| `IRQ_SETUP {nvec}` | allocates vectors, programs MSI-X/MSI (remapped when IR is on), all masked |
| `RESET` | FLR if the function has it, else the PM D3hot→D0 cycle, else `EOPNOTSUPP` |
| `NET_ATTACH {ring region}` / `INPUT_ATTACH` | creates or re-attaches the kernel proxy |

`bkl.c`: add `SYS_UDRV` to the right list (a D1-style "never takes the BKL" entry, forced under `bkl=on`).

### D9. No IOMMU present: refuse DMA, allow MMIO+IRQ
- **Without an active DMA-remapping unit**, `CLAIM` succeeds, but `DMA_ALLOC` returns `ENODEV` and `CFG_WRITE` of BUSMASTER returns `EPERM`. A PCI function cannot initiate DMA with bus mastering off, by spec. So a ring-3 driver can never be a DMA hole. Every real driver needs DMA, so in practice it cannot run, and the in-kernel driver is the one used: do not list the device in `udrv=`.
- **`udrv.unsafe`** (boot word, development only) allows DMA without an IOMMU: identity allocation, loud warning, `/proc/udrv` "TAINTED: ring-3 DMA without IOMMU". **DECIDE (Q2).**
- On the ASUS this means Phase 5 changes nothing unless VT-d exists. The in-kernel drivers stay the default there.

### D10. Crash, teardown and restart
**Kernel teardown of a claim.** It runs from `driver_remove_all_bindings`, which is already first in `process_destroy`, for every exit path: kill, fault, `SYS_EXIT`, exec. Order is strict:
1. Clear BUSMASTER (stops new DMA).
2. Mask every vector, free them, invalidate the IRTEs (IEC).
3. FLR / PM reset when available.
4. Detach to the blocked domain and wait for the context-cache + IOTLB invalidation to complete.
5. Proxy link down; the proxy stays registered.
6. Unmap the user VMAs.
7. Only now free the DMA frames and the shared region.

Freeing frames before step 4 completes would let the device write into reused memory. That is the one ordering bug that must never land; it has its own gate.

**Fork, exec and attacks:**
- Driver VMAs are **DONTFORK**: the child sees nothing. This is new behaviour; today `VMA_TYPE_PHYS` is shared.
- `execve` drops the claim.
- A DMA fault recorded for a ring-3 domain kills the owning driver (`SIGKILL`, `[UDRV] dma fault → kill pid`) and quiesces the device. This is the default; **DECIDE (Q6)** log-only as the alternative.
- A blocked-interrupt fault (IR) does the same.

**Restart:**
- init respawns the driver, with its existing backoff.
- The new driver reclaims the device (claim waits for teardown to finish, or returns `EBUSY`), runs `RESET`, and calls `NET_ATTACH`. The proxy reconnects to the same `eth0` with its IP config, and sockets survive.
- **init change:** a new inittab action `driver` (respawn semantics, started *before* the `sysinit` entries), so `ifup -t 10` finds `eth0` at boot. Today sysinit runs first.

**Stall watchdog:** if TX is pending with no progress for 5 s, log `[UDRV] eth0 stalled`, and with `udrv.watchdog=kill` kill the driver.

### D11. First real driver: **e1000e (82574L)**, not USB HID
Reasons for e1000e:
- It has **MSI-X** in QEMU: edge-triggered, unshared, per-vector maskable. The test case for D5.
- It is a self-contained PCI function with simple descriptor rings. The in-kernel `drivers/e1000e.c` is a ready reference, and it is the NIC already validated on real VMware (M24).
- It works behind `intel-iommu` in QEMU, and VMware offers it with a virtual IOMMU. So it is the one driver that can be validated on "real" hypervisor hardware.
- A crash costs connectivity only; the console and the disk survive.

Against USB HID:
- HID is a class driver *on top of* xHCI. Moving HID alone is impossible; the whole xHCI controller has to move.
- That controller also carries USB mass storage (`usb0`) and the console keyboard. A ring-3 xHCI therefore needs both the input proxy and a block proxy.
- A crash takes the keyboard away.

QEMU's `e1000` (82540EM, the `make run-ssh`/`run-gui` NIC) has **no MSI**. Its ring-3 port needs either poll mode or INTx (M61).

---

## 3. Milestones

Size: S ≈ 1–3 agent-days, M ≈ 1 week, L ≈ 2+ weeks.

The dependency graph:
```
M53a ─┐
M53b ─┼─ M54 ─┬─ M55 (DMA API; kernel strict domain)
M53c ─┘       ├─ M56 (interrupt remapping)
              ├─ M57 (udrv core; IR enforcement wired when M56 lands) ── M58 (e1000e ring 3) ── M59 (defaults)
              └─ M64 (AMD-Vi, any time after M54's ops are stable)
M58 ─┬─ M60 (xHCI + HID ring 3)
     ├─ M61 (other NICs, INTx)
     ├─ M62 (ath9k ring 3)
     └─ M63 (storage, stretch)
```

**Parallelism:**
- M53a, M53b and M53c run in parallel.
- After M54: M55, M56 and M57 run in parallel (three agents). M57's IR policy is a stub until M56 merges.
- M64 can take a fourth agent after M54.

**Merge order on `integ/phase5`:** M53a → M53b → M53c → M54 → M55 → M56 → M57 → M58.

### M53 — groundwork (no behaviour change; default boot identical)

**M53a `feat/pci-registry`** (S–M)
- `drivers/pci.c`/`pci.h`:
  - one boot enumeration into a table (`pci_dev_t`: BDF, ids, class, header type, BARs with sizes, capability offsets for MSI/MSI-X/PM/PCIe, bridge secondary/subordinate buses, parent);
  - `pci_find*` reimplemented over it (same API, so callers are unchanged);
  - **ECAM** from the ACPI MCFG table (`arch/x86/acpi.c` gains MCFG; MMIO through `vmm_map_mmio(UC)`), CF8 kept as the fallback;
  - extended capability walk (ACS, ATS, FLR through PCIe DevCap);
  - a `pci_cfg_lock` (new leaf level, `LK_S64_LEAF`) around CF8/CFC pairs (fixes the latent SMP race);
  - `/proc/bus/pci/devices`-style listing (`fs/procfs.c`), plus an `lspci`-like line in the kernel shell `pci`.
- **Isolation groups:** a function is "assignable" only if it is a root-complex integrated endpoint, or sits below a PCIe root port, with ACS P2P controls on every upstream bridge. Conventional PCI behind a bridge is never assignable; it shares the bridge's requester ID. M57 uses this.
- **Tests:**
  - every existing gate (unchanged behaviour);
  - boot logs on i440fx, q35, q35+intel-iommu and q35+amd-iommu list the same devices as QMP `query-pci`;
  - ECAM vs CF8 reads compared for every function at boot (`[PCI] ecam self-check OK`).

**M53b `feat/acpi-dmar`** (S)
- Parse DMAR (`arch/x86/acpi.c`, new `arch/x86/acpi_dmar.c`): DRHD (register base, segment, INCLUDE_PCI_ALL, device scopes), RMRR, ATSR, RHSA, SATC, and the flags `INTR_REMAP`, `X2APIC_OPT_OUT`, `DMA_CTRL_PLATFORM_OPT_IN`.
- Parse IVRS headers (IVHD types 10h/11h/40h, IVMD) for M64.
- Device scopes are resolved to BDFs by walking bridges in the M53a registry.
- **Read-only.** It logs `[IOMMU] DMAR: n DRHD haw=… flags=…`, each RMRR, and which units cover graphics only.
- Every length, count and offset is untrusted firmware input: bounded walks.
- **Host fuzz harness** `tools/acpi_fuzz/` (the parser compiled unchanged, ASan+UBSan, like `ntfs_fuzz`) with synthesized tables from `tools/acpi_tables.py`.
- **Tests:**
  - boot on q35+intel-iommu (one DRHD, INCLUDE_PCI_ALL), q35+amd-iommu (IVRS), and plain q35 (`[IOMMU] none`);
  - fuzz: 50k mutated tables clean.

**M53c `feat/irq-vectors`** (M; touches live IRQ paths, so the riskiest of M53)
- Dynamic vector allocator over `0x50–0xDF`, kept apart from the fixed `0x20–0x2F`, `0x40–0x42` (compat), `0x80` and `0xF0–0xF2`:
  - `irq_vec_alloc(cpu, handler, ctx)`;
  - generic NASM stubs generated by a macro (`arch/x86/idt_asm.asm`);
  - one C `irq_dispatch(vec)` with `irq_enter/irq_exit`.
- MSI moves to `drivers/pci_msi.c` (moved from `pci.c`, so M53a and M53c do not conflict; merge M53a first):
  - `pci_msix_enable(dev, nvec)`, `pci_msix_set(dev, entry, vec, cpu)`, `pci_msix_mask/unmask(entry)`;
  - MSI multi-message where supported, `pci_msi_mask` when PVM is available;
  - **one** `msi_compose(dev, vec, cpu, &addr, &data)` hook, which M56 replaces with remappable format.
- The net/xHCI/NVMe IRQ users move to allocated vectors.
- **Tests:**
  - NETSMP (MSI-X NAPI on e1000e);
  - `-DXHCI_USE_IRQ` and `-DNVME_USE_IRQ` builds boot and do I/O;
  - M48 SMP gate;
  - a lockdep build (`make lockdep-check` subset).

### M54 — VT-d core: DMA remapping, opt-in (`feat/vtd-core`, M–L; after M53a–c)
**Files:** new `kernel/iommu.{c,h}` (ops, policy, `/proc/iommu`), new `arch/x86/vtd.{c,h}`, `arch/x86/vtd_pt.c` (second-level tables), `kernel/kernel.c` (init call after ACPI+PCI and before driver probes, plus the `iommu=` boot word), `kernel/lock_order.h`.

**Content:**
- Capability checks: SAGAW/MGAW (3-level for `aw-bits=39`, 4-level for 48), ND (domain IDs), QI required, PT, C (clflush when 0), CM, RWBF, SLLPS (use 2M/1G superpages only after the 4 KB path is proven).
- **Firmware handoff.** If firmware left TE or PMRs on (DMA-protection laptops), program the new root table, switch over, then disable the PMRs. Log every step.
- Root and context tables in legacy mode (scalable mode is a non-goal).
- Domain-ID allocator.
- Second-level page tables (R/W bits, no ATS: ATS is never enabled on endpoints).
- Queued invalidation: context-cache, IOTLB (domain and page-selective), and the **wait descriptor** with status write. `iommu_unmap` returns only after completion.
- Fault reporting: FSTS/FRCD, a fault-event interrupt on an allocated vector (FEDATA/FEADDR), `[IOMMU] DMA fault bdf=… iova=… reason=… R|W`, a rate limit, attribution of the fault to a binding (hook for D10), and counters in `/proc/iommu`.

**Policy in M54:**
- `iommu=off` is the **default**.
- `iommu=on`: every enumerated function gets a passthrough context entry (an identity domain over all RAM, using 1 GiB pages, when `ECAP.PT` is missing), with RMRRs honoured.
- `iommu=on` + `-DSECOS_IOMMU_TEST` adds the in-kernel edu tests below.

**Locks** (new levels in `lock_order.h`):
- `LK_S66_IOMMU_DOM` (166): one per domain, around page-table edits. Heap S70 and PMM S75 nest inside for table pages.
- `LK_S68_IOMMU_QI` (168): one per unit, around the invalidation queue and register access.
- The fault ISR takes only the QI/unit lock.
- `iommu_map/unmap` must be callable under `tx_lock` (N3) and `xhci_lock` (S45). The levels above allow that.

**Tests** (gate "M54 VT-d", q35 MB2 ISO, `-smp 2`):
1. `iommu=off`: the full selftest, unchanged (no regression by construction).
2. `-device intel-iommu` + `iommu=on`, in the variants `aw-bits=39` / `48`, `caching-mode=off` / `on` (CM=on makes the hardware cache not-present entries, so a missing flush-after-map fails). Checks:
   - boot to login;
   - ext4 root on AHCI (`-drive if=ide` on q35, so it is translated) survives `fs_stress_drive` + host `e2fsck`;
   - NVMe and xHCI+usb-storage+usb-kbd I/O;
   - e1000e DHCP+ping;
   - 0 `[IOMMU] DMA fault`.
3. `-DSECOS_IOMMU_TEST` with `-device edu` (the kernel drives edu directly):
   - **positive:** map one page at IOVA X, edu DMA → data correct;
   - **negative 1:** DMA to an unmapped IOVA → exactly one fault with edu's BDF, that IOVA and reason "not present"; the kernel continues;
   - **negative 2 (proves the IOTLB flush):** map, DMA OK, unmap, DMA again → must fault;
   - **negative 3:** a read-only mapping plus a device write → a write fault;
   - **negative 4:** a `dma_mask` 28-bit edu + an IOVA above 256 MiB → refused by the allocator;
   - all in a loop of 1000 with MemFree steady.
4. UEFI/OVMF q35 + intel-iommu boot (the loader passes RSDP, DMAR is found).
5. The same as 2 on QEMU 9.2 (`SECOS_QEMU_X2APIC`).

### M55 — DMA API + strict kernel domain (`feat/dma-api`, M; after M54; parallel with M56/M57)
**Step 1 (one agent, first):**
- `kernel/dma.{c,h}` (D3);
- `.dmabss` in `linker.ld` and the KASLR reloc tool check (`tools/kaslr-relocs.py` must accept the new section; it is `.bss`-like, with no relocations);
- the `__dma` macro;
- `iommu.kernel=pt|strict` (strict = one shared identity domain over `.dmabss` + `dma_alloc_coherent`);
- `-DSECOS_DMA_DEBUG`.

**Step 2 — driver conversions, one commit per driver, parallelizable** (separate files, no conflicts):

| Driver | Files |
|---|---|
| virtio-blk | `virtio_blk.c` |
| AHCI | `ahci.c` |
| NVMe | `nvme.c` |
| USB | `xhci.c`, `usb.c`, `usb_hid.c`, `usb_hub.c`, `usb_msc.c` |
| e1000 | `e1000.c` |
| e1000e | `e1000e.c` |
| vmxnet3 | `vmxnet3.c` |
| igc | `igc.c` |
| ath9k | `ath9k.c`, `ath9k_port/ath9k_secos.c` (check its TX path) |

For each: tag the static buffers `__dma` and replace `phys_of()` with `dma_static(dev, …)`. The 4 GiB checks become the DMA mask.

**Tests:**
- every storage/USB/net gate on i440fx with `SECOS_DMA_DEBUG` (no IOMMU; catches unconverted buffers);
- the M54 gate 2 matrix with `iommu.kernel=strict`: 0 faults;
- **negative:** `-DSECOS_IOMMU_TEST` points one AHCI PRD at a `.text` page → an IOMMU write fault, the command errors, `.text` hash (`boot_measure`) unchanged, the kernel alive;
- MemFree steady over `fsstress`.

### M56 — interrupt remapping (`feat/vtd-intremap`, M–L; after M54; parallel with M55/M57)
**Files:** new `arch/x86/vtd_ir.c`; `drivers/pci_msi.c` (the `msi_compose` hook); `arch/x86/lapic.c` (IOAPIC RTE remappable format, 32-bit destinations); `kernel/iommu.c`.

**Content:**
- IRTE table (64K entries max; start with 256);
- IEC invalidation through QI;
- SVT=1 source-id checks (SID = requester BDF; for an IOAPIC, the DMAR device scope's IOAPIC ID);
- EIM with x2APIC, honouring `X2APIC_OPT_OUT`;
- compatibility format disabled (`GCMD.CFI` clear);
- `iommu.ir=off` boot word;
- ordering: IR is enabled only after every live source has been reprogrammed, under one switchover function like `apic_switchover`.

**Tests:**
- full boot + keyboard (QMP `sendkey`), PS/2 mouse, NET MSI-X, `-smp 4`, on QEMU 6.2 (`intremap=on`, xAPIC) and QEMU 9.2 (`intremap=on,eim=on`, `-cpu max,+x2apic -smp 8`);
- **negative (the injection test):** edu DMA-writes `{0xFEE00000, vector 0x80}` and `{…, 0xF0}` → blocked, an IR fault is logged, no stray syscall or IPI handler runs (counters unchanged), the kernel alive;
- a stale IRTE after `irq_free` + device MSI → fault, not delivery.

### M57 — Driver Space v2 core (`feat/udrv-core`, L; after M54, IR policy after M56)
**Files:**
- new `kernel/udrv.{c,h}` (claim, bindings, IRQ fd, teardown, `/proc/udrv`);
- `kernel/syscall.{c,h}` (`SYS_UDRV` 92 dispatch only), `kernel/kiface.c` (`ki_fd_revents` + poll wake);
- `kernel/process.{c,h}` (fd kind `is_udrv`, claim pointer, DONTFORK on fork, drop on exec);
- `mm/vma.{c,h}` + `mm/vmm.c` (`VMA_FLAG_DONTFORK`, a UC PHYS variant, munmap refusal);
- `mm/elf_manifest.{c,h}` (v3);
- `kernel/driver_if.c` (`driver_remove_all_bindings` calls `udrv_release_all`);
- `kernel/bkl.c` (list entry), `kernel/lock_order.h` (`LK_S14_UDRV` = 114: per-claim IRQ/event state, taken in IRQ context, irqsave; proc S30 for the wake nests inside);
- in-kernel probes skip `udrv=` devices (`net/net_core.c`, `drivers/usb.c`, `ahci.c`, `nvme.c`: one `pci_registry_is_udrv` check each);
- `user/libsecos.{c,h}` (wrappers), `user/note_udrv.S`, `user/include/secos/udrv.h` (ABI).

**Tests** (gate "M57 UDRV", q35 + intel-iommu(intremap) + `-device edu`, `udrv=1234:11e8`):
- **`user/udrv_edu.c` positive:**
  - claim;
  - `MAP_BAR 0`: identification register and factorial round trip;
  - `IRQ_SETUP 1` + edu raise-irq → `read(fd)` count 1 → write ack → repeat 10 000 times;
  - `DMA_ALLOC` 2 pages + edu DMA to/from → data matches, the IOVA ≠ phys.
- **`user/udrv_evil.c` negative modes** (each one: kernel alive, the expected log line, MemFree steady):
  - DMA to an unmapped IOVA → `[IOMMU] DMA fault` attributed → driver SIGKILLed → device quiesced → re-claim works;
  - MSI injection via DMA → IR fault → kill;
  - `CFG_WRITE` BAR0 / MSI cap → `EPERM`;
  - `MAP_BAR` covering the MSI-X table → the hole is not mapped (SIGSEGV on touch);
  - `fork()` + child touches the BAR / DMA VA → SIGSEGV (DONTFORK);
  - `munmap` of a DMA VMA → `EINVAL`;
  - a second claimant → `EBUSY`;
  - a non-root claimant → `EPERM`;
  - an unsigned binary or one with a foreign manifest match → refused;
  - a claim of a device not in `udrv=` → refused;
  - an IRQ flood without ack → one pending count, no storm (`/proc/interrupts`-style counter bounded);
  - `kill -9` in the middle of a DMA loop × 200 → no fault after teardown, no frame leak (MemFree+Slab), no `[EXC]`.
- **No IOMMU (plain q35):** claim OK, `DMA_ALLOC` → `ENODEV`, BUSMASTER write → `EPERM`. With `udrv.unsafe`: works, and the taint is shown.
- **No IR** (`intremap=off`): bus-master claim refused unless `udrv.allow_no_ir`.
- **Lockdep build:** all of the above silent.
- **`bkl=on`:** the same gate passes.

### M58 — e1000e in ring 3 (`feat/udrv-e1000e`, L; after M57)
**Files:**
- new `net/net_udrv.c` + `net/net.h` (proxy registration/reattach);
- `user/udrv/e1000e.c` (port of `drivers/e1000e.c`: MSI-X through the IRQ fd, poll fallback with `sleep`);
- `user/include/secos/udrv_net.h` (ring ABI);
- `user/init.c` (the `driver` action);
- the sysroot table (`kernel/sysroot.c`: install `/sbin/udrv-e1000e`);
- the default inittab gains `net0:driver:/sbin/udrv-e1000e` only when `udrv=` names a NIC;
- the Makefile (`make iso UDRV=8086:10d3` → cmdline).

**Tests** (gate "M58 NET-UDRV", q35 + intel-iommu(intremap) + `-device e1000e`, `udrv=8086:10d3`, `-smp 2` and `-smp 4`):
- boot → `ifup` DHCP → ping → DNS → `wget`-style TCP download, checksum checked;
- the NETSMP `netstress` gate on the ring-3 driver;
- an SSH session through it (host port **not** 2222/8000);
- **restart:** `kill -9` the driver during `netstress` → `[UDRV] eth0 driver died` → init respawns within ~1 s → link up again → ping resumes; the TCP stream completes (retransmit) or fails cleanly with no kernel error. 20 kill loops, MemFree+Slab steady;
- **hostile driver build** (`-DUDRV_E1000E_EVIL`, modes: bad RX len, slot out of range, index junk, TX completion of a slot never given) → dropped and logged, repeated → killed, no `[EXC]`;
- **performance report** (not a gate): throughput and latency vs in-kernel e1000e on the same guest (`tools/bkl_bench.py`-style);
- **fallback:** the same image booted without `udrv=` uses the in-kernel e1000e (the existing gates).
- **Real validation (user):** VMware with "Virtualize IOMMU" + e1000e NIC. Give him the `.vmdk`/ISO. Report the `[IOMMU]`/`[UDRV]` lines and SSH.

### M59 — defaults + docs (`integ/phase5`, S)
- **If the M54–M58 gates are green and VMware is validated:**
  - default `iommu=on` when DMAR is present, `iommu.kernel=strict`, IR on;
  - unknown functions go to the blocked domain, except RMRR devices and graphics-only DRHDs, which are left untranslated (Linux `igfx_off` behaviour: the iGPU scans out the firmware framebuffer from stolen memory).
- GRUB fallback entries: "IOMMU off (iommu=off)" and "no ring-3 drivers" (drops `udrv=`).
- `udrv=` stays opt-in per image.
- **DECIDE (Q3):** the default timing.
- Devlogs M53–M59, CLAUDE.md, TASKS.md.

### M60 — xHCI + HID in ring 3 (`feat/udrv-xhci`, L; after M58)
- The whole xHCI controller moves (`user/udrv/xhci/`, from `drivers/xhci.c`, `usb.c`, `usb_hub.c`, `usb_hid.c`).
- The input proxy `drivers/input_udrv.c`: the ring of events is validated against the declared capability bits. `/dev/input/eventN` is unchanged for secwm.
- When xHCI is in ring 3, **USB mass storage is unavailable** until M63 (documented).
- **Console safety:** PS/2 and serial still work; the kernel shell is reachable over serial.
- **Tests:**
  - the M40 GUI gates (`tools/gui_drive.py`: usb-tablet + usb-kbd) with ring-3 xHCI;
  - `kill -9` the driver → input resumes after the respawn, secwm survives (its evdev fds see `ENODEV` then reopen; secwm needs a reopen path);
  - a hostile event ring → dropped.

### M61 — other NICs + INTx (`feat/udrv-nics`, M)
- e1000 (QEMU's default NIC, no MSI), first in poll mode.
- `feat/udrv-intx`: VFIO-style INTx (DisINTx mask/unmask, refuse functions without DisINTx or sharing the line with a kernel device).
- vmxnet3 and igc (compile-tested; real-hardware only).

### M62 — ath9k in ring 3 (`feat/udrv-ath9k`, M)
- Move the ported Linux code and its compat shim (`drivers/ath9k_port/`) into a ring-3 driver, so its faults stop being kernel faults. The radio itself stays **parked** (M39 CRC wall), so this is containment only.
- Needs MSI or INTx (M61) and the WiFi side of the net proxy (management frames).
- Validation is real-hardware only, and the ASUS has no IOMMU, so it would run under `udrv.unsafe` there.
- **DECIDE (Q10).**

### M63 — storage in ring 3 (stretch, likely Phase 6)
- A block proxy (`fs/block_udrv.c`: request/completion rings, kernel-owned bounce slots, timeouts → `-EIO`).
- Order: data disks first (NVMe, AHCI port), then a modern virtio-blk 1.0 driver with `ACCESS_PLATFORM` (the legacy one cannot be isolated in QEMU).
- The root disk is last. It needs the driver to come from the kernel image (an embedded signed ELF, as `elf2h.py` already does) before the root is mounted.
- **Risks:** JBD2 ordering across a driver restart, and in-flight requests at a crash.
- **DECIDE (Q7)** whether this is in Phase 5.

### M64 — AMD-Vi (`feat/amdvi`, L; after M54's ops are frozen, parallel with M55–M58)
**Files:** `arch/x86/amdvi.{c,h}`, `amdvi_ir.c`; IVRS from M53b.

**Content:**
- device table (up to 64K × 32 B; size it to the max BDF present);
- command buffer + `COMPLETION_WAIT`;
- event log + its MSI (the IOMMU is itself a PCI function: `AMDVI-PCI` in QEMU);
- v1 I/O page tables (levels from `HATS`);
- `INVALIDATE_DEVTAB_ENTRY` / `INVALIDATE_IOMMU_PAGES`;
- IR with 32-bit IRTEs (128-bit GA + `xtsup` for x2APIC on QEMU 9.2).

**Tests:** the M54/M56/M57 gate matrix with `-device amd-iommu[,intremap=on]` (QEMU 6.2 and 9.2), edu included.

There is no known AMD real hardware, so this is QEMU-only validation. **DECIDE (Q8)** its priority.

---

## 4. Gates summary (new selftest sections)
| Gate | Guest | Pass criteria |
|---|---|---|
| M53 | i440fx/q35/q35+vtd/q35+amdvi | PCI listing = QMP `query-pci`; ECAM self-check; DMAR/IVRS logs; `acpi_fuzz` clean |
| M54 VT-d | q35+intel-iommu × {aw 39/48} × {CM off/on}, 6.2 + 9.2, MB2 + UEFI | 0 DMA faults in normal use; edu fault/flush tests exact |
| M55 DMA | i440fx (`SECOS_DMA_DEBUG`) + q35 strict | 0 faults; the `.text` PRD negative faults; boot measure unchanged |
| M56 IR | 6.2 intremap (xAPIC), 9.2 intremap+eim (x2APIC, -smp 8) | keyboard/mouse/NET work; MSI injection blocked |
| M57 UDRV | q35+vtd+edu | positive + 12 negative modes; no leak; lockdep silent; `bkl=on` too |
| M58 NET-UDRV | q35+vtd+e1000e | DHCP/TCP/SSH/netstress; 20 kill-restart loops; hostile ring modes |

All of them on top of the unchanged full `tools/selftest.sh`, which keeps `iommu=off` and no `udrv=` until M59. Each new gate is a separate script (`tools/iommu_gate.sh`, `tools/udrv_gate.sh`) called from `selftest.sh`, so they can also run alone (`make iommu-gate`). Every gate uses a private `/tmp` (`unshare -rm`), pid files, and never `pkill -f`.

## 5. Risks and open questions (sized honestly)
| Risk | Size | Mitigation |
|---|---|---|
| A free-before-flush teardown bug (device writes reused frames): silent memory corruption | high impact | Strict order in D10; the edu "unmap then DMA must fault" test; kill-loop gate with heap poisoning |
| Enabling IR breaks every interrupt at once (keyboard, NET, timer unaffected) | medium | One switchover function; `iommu.ir=off`; gates on two QEMU versions |
| Firmware DMAR bugs on real hardware (bogus RMRRs, scopes for absent devices, broken IR + x2APIC opt-out) | medium, **real-hardware only** | Bounded parser + fuzz; log and ignore invalid entries; `iommu=off` GRUB entry; default stays off until VMware is validated |
| iGPU / USB legacy emulation needing RMRR identity maps; firmware-enabled TE/PMR at handoff | medium, real-hardware only | Honour RMRRs; never translate a graphics-only DRHD by default; explicit handoff sequence |
| QEMU's vIOMMU is more forgiving than hardware (no real IOTLB timing, CM semantics) | medium | Run CM=on as the stricter mode; real validation on VMware |
| A ring-3 driver too slow (a syscall + wake per IRQ, a copy per packet) | low–medium | Batching (one kick per ring drain), masked IRQs; measured, not gated |
| The shared rings as a new attack surface | medium | Kernel reads through the physmap, one read per field, bounded indices, owner tracking per slot; hostile-driver gates |
| Peer-to-peer DMA bypassing the IOMMU on real PCIe topologies | medium, real-hardware only | Isolation-group rule in M53a (ACS or integrated endpoint only) |
| Device reset quality (no FLR on many devices; QEMU e1000e FLR support to be checked) | low–medium | Fall back to the PM reset; BUSMASTER off + blocked domain is the real guarantee; RESET is best-effort |
| CF8 config race already latent on SMP | low | Fixed in M53a (lock; ECAM is atomic) |
| init ordering (the driver must exist before `ifup`) | low | New `driver` inittab action |
| Legacy virtio-blk can never be isolated in QEMU | accepted | Documented; a modern virtio driver is part of M63 |

**What only real hardware can validate:**
- DMAR quirks, RMRR and iGPU behaviour, firmware handoff (TE/PMR), IR with x2APIC + EIM on silicon, real IOTLB invalidation timing, FLR, ACS topologies;
- VMware's virtual IOMMU (the realistic target for the user).

The **ASUS E406S (Braswell)** most likely has **no VT-d**, which needs checking:
- the M53b boot line `[IOMMU] none` / `DMAR: …`, or `acpidump` under Linux on the same laptop.

If there is none, Phase 5 is inert there by design (D9), and the M62 ath9k move could only run under `udrv.unsafe`.

## 6. Decisions for the user
- **Q1.** First ring-3 driver: e1000e (recommended, D11) or USB HID?
- **Q2.** No IOMMU: refuse ring-3 DMA (recommended), with `udrv.unsafe` as a development override?
- **Q3.** Kernel-driver domain default (strict identity domain recommended, PT fallback), and when `iommu=on` becomes the default: at M59, after VMware validation (recommended)?
- **Q4.** Interrupt remapping mandatory for ring-3 bus-master drivers (recommended; `udrv.allow_no_ir` override)?
- **Q5.** Device assignment by boot cmdline `udrv=` (recommended) vs a runtime unbind/bind interface?
- **Q6.** A DMA/IR fault from a ring-3 device: kill + restart the driver (recommended) or log only?
- **Q7.** Storage in ring 3 inside Phase 5, or deferred to Phase 6 (recommended: defer)?
- **Q8.** AMD-Vi priority: no AMD machine is known; QEMU-only validation?
- **Q9.** Keep the M11 `SYS_DRIVER` mock API, or retire it once `SYS_UDRV` lands?
- **Q10.** Move ath9k to ring 3 while its radio is parked (containment only)?
