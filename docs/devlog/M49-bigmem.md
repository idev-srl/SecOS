# M49 — Much more usable RAM (physmap to 64 TiB, sectioned PMM)

Phase 4 (branch `feat/bigmem`). Before this milestone SecOS was tested up to
2 GiB and could not have used more than about 512 GiB:

| limit (before)            | cause                                                        |
|---------------------------|--------------------------------------------------------------|
| physmap 512 GiB           | one PDPT (one PML4 slot) of 2 MiB pages                      |
| PMM ~1 TiB                | defensive 32 MB bitmap cap                                   |
| PMM metadata in 512 MiB   | bitmap + `uint16` refcount per frame at `_kernel_end`, reached through the 512 MiB high-half window (2.125 B/frame = 544 MiB per TiB) |
| WB over MMIO              | the physmap was `0..total` linear, and `vmm_extend_physmap(end)` extended it linearly up to any BAR, write-back |

After it:

| limit (now)               | value                                                        |
|---------------------------|--------------------------------------------------------------|
| physmap window            | 64 TiB (`VMM_PHYSMAP_BASE` 0xFFFF888000000000 + `VMM_PHYSMAP_SIZE`, Linux's 4-level layout, 128 PML4 slots) |
| PMM                       | whatever the physmap maps (frame numbers are 64-bit; the section table is sized from the memory map) |
| PMM metadata              | 33 KiB per GiB of RAM (0.003 %), allocated only where RAM exists, in the physmap |
| largest RAM booted        | **62 TiB** (MB2, QEMU 9.2 TCG, lazily backed; 16 TiB on QEMU 6.2); 1 TiB on UEFI/OVMF |

## 1. Physmap

`mm/vmm.c` (`vmm_init_physmap`, `pm_map_range`):

- **Built from the memory map, RAM only.** The PMM parsers record two range
  lists: *usable* RAM (handed out) and *RAM-like* memory (usable + ACPI
  reclaim/NVS, and on UEFI loader/boot-services/runtime code+data and
  persistent memory). Only RAM-like ranges are mapped, write-back. Range
  edges are mapped with 4 KiB pages, so no WB mapping covers a PCI hole, the
  LAPIC/IOAPIC window or a BAR. The legacy low 1 MiB is always mapped (EBDA,
  BIOS ROM, the AP trampoline page at 0x8000), with the VGA window
  `[0xA0000, 0xC0000)` uncached.
- **1 GiB pages** when `CPUID.80000001h:EDX[26]` (pdpe1gb) is set, else 2 MiB.
  Measured: 1 TiB of RAM needs 132 page-table frames with 1 GiB pages
  (the 128 PDPTs + 4); with 2 MiB pages it would need one 4 KiB PD per GiB
  (256 MiB of tables for 64 TiB).
- **PML4 slots pre-populated at boot.** User CR3s copy the kernel's high-half
  PML4 entries once, at creation (`vmm_space_create_user`). So the PDPT of
  every slot that a physical address of this CPU can fall in
  (`CPUID.80000008h:EAX[7:0]`, capped at 64 TiB, at least the RAM top) is
  allocated before any process exists: 2 slots (8 KiB) for a 40-bit CPU, 128
  slots (512 KiB) for a 46-bit one. Later growth (a BAR mapped at runtime)
  lands in those shared PDPTs and every process sees it. A mapping outside
  them is refused (`[PHYSMAP][ERR]`) instead of silently being invisible to
  older address spaces.
- **NX everywhere.** Every physmap leaf is `PRESENT|RW|NX`, supervisor. The
  boot audit `vmm_physmap_audit()` walks every leaf:
  `[PHYSMAP] audit leaves=N all supervisor+NX`.
- **Device memory and firmware tables are mapped explicitly**, at their
  physmap address (`phys_to_virt` keeps working), with a cache type:
  - `vmm_map_mmio(phys, size, VMM_CACHE_UC|WC|WB)`: retags pages already
    mapped and splits a 1 GiB / 2 MiB page that straddles the range (the PAT
    bit moves from bit 12 to bit 7 on a 4 KiB split).
  - `vmm_map_phys(phys, size)`: WB, fills only unmapped pages (ACPI tables in
    reserved memory above the RAM top).
  - `pci_bar_size()` / `pci_bar_map()` (`drivers/pci.c`) size a memory BAR with
    the all-ones probe (memory decode off while probing) and map it UC. Used
    by AHCI, NVMe, xHCI, e1000, e1000e, igc, vmxnet3 and ath9k. ath9k used to
    rely on some earlier driver having extended the linear physmap past its
    BAR.
  - The framebuffer is mapped WC (`fb_finalize_mapping`). Only its own pages
    change, so a low framebuffer works too: the old code skipped the WC
    retag there because a 2 MiB page might also hold RAM.
  - ACPI: `pv()` maps 64 bytes (RSDP / a header), `pvt()` the whole SDT by its
    header length. The old code used a speculative 1 MiB WB margin.
  - The MSI-X table and the FADT reset register are mapped UC.
  - `vmm_extend_physmap(end)` is kept as a **deprecated** shim (UC, fills only
    unmapped pages of the 2 MiB block below `end`). `arch/x86/lapic.c` still
    uses it for the LAPIC and IOAPIC pages. That file belongs to the SMP work,
    so it was left alone. Each page is page-aligned inside its 2 MiB block,
    so the shim covers it.
- `kvirt_to_phys()` recognises the whole 64 TiB window (it stopped at 512 GiB).
- **KASLR:** the base is used only through `VMM_PHYSMAP_BASE`, and every
  index is derived from it. Randomizing it needs a 1 GiB-aligned base for
  1 GiB pages (2 MiB minimum, checked at boot).

TLB note: changing an attribute of a live physmap entry reloads CR3 on the
current CPU only. Device mappings are made at boot, before APs run. A runtime
retag under SMP will need the TLB-shootdown hook of `feat/smp`.

## 2. PMM

`mm/pmm.c`, rewritten:

- **Two phases.** `pmm_init*()` parses the map into sorted, merged lists and
  sets up an **early bump allocator** (memblock-style, per-range bottom-up
  cursor + top-down cursor). `vmm_init()` and `vmm_init_physmap()` draw their
  page-table frames from it, below 512 MiB, because they reach them through
  the boot identity map. `pmm_late_init()` (called right after
  `vmm_init_physmap()`) builds the real allocator. It then frees every usable
  frame the early allocator did not consume, and re-marks the reserved
  ranges.
- **1 GiB sections.** A section that holds usable RAM gets a descriptor:
  `bitmap[4096]` (1 bit/frame), `summary[64]` (bit *w* = bitmap word *w*
  full), `free`, `hint`, and 64 refcount-chunk pointers with their in-use
  counts. That is 33 936 bytes per GiB, allocated **top-down** from the
  highest RAM, so the DMA32 zone stays free. A section without RAM costs
  one NULL pointer in the section array (8 bytes per GiB of address space).
- **Allocation:** a rolling section hint, then the summary bitmap skips 64
  full words per probe, so a frame is found in at most 64 + 64 word probes
  per section. `pmm_alloc_contiguous()` stays a word-skipping run search
  inside one section (so a run is at most 1 GiB). The heap needs nothing more.
- **Zones:** frames below 4 GiB are DMA32. `pmm_alloc_frame()` /
  `pmm_alloc_contiguous()` prefer frames at or above 4 GiB and fall back to
  DMA32 only when that is exhausted. `pmm_alloc_below(limit)` and
  `pmm_alloc_contiguous_below(count, limit)` serve devices with a limited DMA
  reach.
- **COW refcounts:** one byte per frame (extra references), allocated per
  16 MiB chunk (4 KiB) on the first `pmm_share_frame()` in that chunk, and
  released when the chunk's last shared frame drops. So memory that is not
  shared costs nothing, and fork/exit cycles leave `MemFree` unchanged. A count that reaches 255 is
  **sticky**: the frame is never freed. That trades a leak for a
  use-after-free (the old `uint16` saturated, and then kept being
  decremented). `pmm_share_frame()` now returns `-1` when a chunk cannot be
  allocated. `vmm_fork_space()` then aborts the fork and destroys the
  partial child.
- **Reservations:** `[0, kernel end)` as before, plus the **MB2 info block**,
  which is now reserved explicitly. It is read again in phase 2
  (framebuffer, ACPI RSDP tags) but used to be protected only by luck.
- **meminfo:** `MemTotal` = usable RAM, `MemFree` = free frames (metadata,
  page tables and reserved frames count as used). Both stay exact: the boot
  self-test checks that the free count returns to its value after
  alloc/free.
- New API: `pmm_late_init`, `pmm_reserve`, `pmm_add_ram`,
  `pmm_ram_range{,_count}`, `pmm_alloc_below`, `pmm_alloc_contiguous_below`,
  `pmm_get_meta_bytes`, `pmm_free_frames_below`, `pmm_selftest`.

### Metadata overhead

The cost is 33 936 bytes per GiB of RAM (descriptor), plus 8 bytes per GiB of
physical address span (section array), plus 4 KiB per 16 MiB chunk that
currently holds a COW-shared frame. A chunk is released when its last shared
frame drops. Measured from the boot log (`[PMM] sections=… meta_KiB=…`, QEMU
TCG):

| guest RAM | sections (RAM / span) | PMM metadata | share of RAM | physmap tables | old design (bitmap + u16 refs) |
|-----------|-----------------------|--------------|--------------|----------------|--------------------------------|
| 256 MiB   | 1 / 1                 | 33 KiB       | 0.013 %      | 5 frames (2 MiB pages, qemu64) | 136 KiB  |
| 8 GiB     | 8 / 9                 | 265 KiB      | 0.0032 %     | 6 frames (1 GiB pages) | 4.25 MiB |
| 64 GiB    | 64 / 65               | 2.1 MiB      | 0.0032 %     | 6              | 34 MiB                         |
| 520 GiB   | 520 / 521             | 16.8 MiB     | 0.0032 %     | 6              | 276 MiB (past the 512 GiB physmap) |
| 1 TiB     | 1024 / 1025           | 33.1 MiB     | 0.0032 %     | 132 (128 PDPTs, 46-bit CPU) | 544 MiB (did not fit) |
| 4 TiB     | 4096 / 4097           | 132.6 MiB    | 0.0032 %     | 132            | 2.1 GiB                        |
| 16 TiB    | 16384 / 16385         | 530 MiB      | 0.0032 %     | 132            | 8.5 GiB                        |
| 32 TiB    | 32768 / 33789 ¹       | 1.04 GiB     | 0.0032 %     | 132            | 17 GiB                         |
| 62 TiB    | 63488 / 64509 ¹       | 2.0 GiB      | 0.0032 %     | 132            | 33 GiB                         |

¹ QEMU 9.2 puts the RAM above 4 GiB at 1 TiB (the AMD HyperTransport hole),
so the map has a ~1 TiB hole. It costs 1021 NULL pointers (8 KiB), not
metadata.

`pmm_late_init()` time (TSC, TCG; the bitmap fill and the free pass dominate):
1.2 ms at 256 MiB/8 GiB, 3.3 ms at 64 GiB, 24 ms at 520 GiB, 52 ms at 1 TiB,
0.2 s at 4 TiB, 0.82 s at 16 TiB, 1.6 s at 32 TiB, 3.5 to 8.3 s at 62 TiB. The 62 TiB
figure varies between runs because the host (23 GiB) has to back 2 GiB of
freshly touched guest metadata. Boot to the console bash (host wall clock from QEMU
start, BIOS + GRUB ≈ 3.9 s included): 5.3 s up to 64 GiB, 5.4 s at 520 GiB,
5.5 s at 1 TiB, 5.9 s at 4 TiB, 7.1 s at 16 TiB, 8.8 s at 32 TiB, 12.4 to 17.9 s
at 62 TiB. QEMU host RSS: 245 MB at 520 GiB, ~0.4 GB at 1 TiB, 0.7 GB at 4 TiB,
2.3 GB at 16 TiB.

## 3. DMA constraints

- **Zones** as above: ordinary allocations leave the DMA32 zone alone while
  higher RAM is free.
- **AP trampoline PML4** (loaded with a 32-bit `mov cr3`): now
  `pmm_alloc_below(4 GiB)`. It used to rely on the first frames being low.
  The trampoline code page itself is the fixed 0x8000, inside the reserved
  `[0, kernel end)`.
- **Per driver** (every DMA structure is a static buffer in the kernel image,
  so it sits below 4 GiB while the kernel is loaded low):
  - **AHCI**: reads `CAP.S64A`. Without it, the HBA refuses to start if the
    command lists or the 64 KB bounce sit above 4 GiB. All data already
    bounces through static buffers.
  - **xHCI**: reads `HCCPARAMS1.AC64`. Without it, a synchronous control or
    bulk transfer to a buffer above 4 GiB bounces through a static 64 KB
    buffer. This matters because callers pass heap and kernel-stack buffers,
    e.g. `usb_msc` `&maxlun`, and kernel stacks are now ordinary frames that
    can be anywhere. An async `xhci_submit` of such a buffer is refused.
  - **NVMe**, **e1000/e1000e/igc/vmxnet3**: 64-bit DMA by specification; static
    buffers.
  - **virtio-blk (legacy)**: the queue PFN register is 32-bit (reaches
    16 TiB); the queue is static.
  - **ath9k** (AR9565, 32-bit DMA): static RX pool in the kernel image.

## 4. Heap and large allocations

- `kmalloc` still backs a large block with `pmm_alloc_contiguous`. At boot a
  4 MiB `kmalloc` is checked: it is written and read back, and it comes from
  RAM above 4 GiB (`[HEAP] kmalloc(4M) phys=0x00000001…`).
- User space: the 12 GB mmap arena and `max_mem` are unchanged. A ring-3
  `lua` allocates and fills a 512 MiB string on an 8 GiB guest, and
  `MemFree` comes back after it exits (BIGMEM gate).

## 5. Tests

All on QEMU TCG. This host gives no access to `/dev/kvm`. QEMU backs guest
RAM lazily (an anonymous mapping, host overcommit), so a guest costs the host
only what the kernel touches.

- **Boot self-test** (`pmm_selftest`, every boot, `[BIGMEM]` lines). It
  touches one frame in up to 64 RAM sections spread over the map, plus the
  highest free frame. Every pattern is written before any is verified, so two
  physmap addresses aliasing the same frame would be caught. It also checks
  the zones (an ordinary frame comes from at least 4 GiB when such RAM is
  free; DMA32 single and 16-frame allocations come from below 4 GiB), COW
  refcounts (share/unref on the top frame, lazy chunk) and that the free
  count comes back. Then a 4 MiB `kmalloc` is checked, and the physmap leaf
  audit runs.
- **Largest RAM booted**: MB2 **62 TiB** with QEMU 9.2
  (`-machine pc,smbios-entry-point-type=64 -cpu max,phys-bits=46 -m 62T`;
  QEMU 6.2 refuses more than ~16 TiB because its 32-bit SMBIOS table
  overflows). The self-test passes and the system reaches the console bash.
  16 TiB also boots on the stock QEMU 6.2. UEFI/OVMF: 256 MiB, 8 GiB, 64 GiB
  and **1 TiB** boot with the self-test passing.
- **Frames above 512 GiB** (the old physmap limit) are allocated, written and
  read back through the physmap: on the 1 TiB guest the top frame is at
  1 049 566 MiB; on the 62 TiB guest, at 66 055 157 MiB.
- **BIGMEM gate** (`tools/selftest.sh`, ~100 s, four guests in parallel):
  `8g` (1 GiB pages, plus a ring-3 `lua` filling a 512 MiB string, and
  MemFree/MemTotal checks), `8g2m` (qemu64, no pdpe1gb: 2 MiB pages), `1t`
  (a frame above 512 GiB, 1024 sections, several PML4 slots) and `uefi` (8 GiB
  OVMF). Each also checks the self-test, the audit, a 4 MiB `kmalloc` above
  4 GiB, that the console bash starts, and that no exception or panic
  occurred. A guest whose QEMU cannot start with that much RAM is SKIPPED.
  The ESP is a per-run copy: QEMU's `fat:rw` wrote into `dist/kernel.elf`
  when a guest was killed, which then failed the secure-boot check on the
  next UEFI run.
- **Full suite** (`tools/selftest.sh`, private `/tmp`): **1077 passed, 1
  failed**. All 33 BIGMEM checks pass. The one failure is
  `M29-3 no PMM leak under multicore scheduling`, and it is **pre-existing
  and flaky**. The M8 workload on `-smp 2` compares free memory after the last
  two rounds, but a zombie pinned to the AP is reaped by the AP's own idle
  loop, so a sample can catch a transient dip of 16 to 26 frames. The next
  round recovers it, so nothing leaks. Repeated runs: `integ/phase4` (old
  PMM) failed 1 of 8 and this branch 2 of 8, with the same transient pattern.
  Waiting for the process table to drain did not remove it either: a vCPU
  can stall mid-reap under host load. It belongs to the SMP work.

## 6. 5-level paging (LA57): detection only

`pm_cpu_features()` reads `CPUID.7:ECX[16]` and logs `la57=0|1` in the
`[PHYSMAP]` line. Nothing else. To go past 64 TiB, SecOS would need:

- to enable `CR4.LA57` while paging is off (in the 32-bit MB2 path in
  `boot/boot.asm`, and in the UEFI loader before the handoff, or through a
  32-bit trampoline), and build a PML5;
- a level-generic walker: every walker in `mm/vmm.c` hard-codes 4 levels, the
  PML4 index from bits 39..47, and the 0xFFFF8… canonical boundary;
- a new layout: Linux uses a 32 PiB direct map at 0xFF11000000000000. The
  user/kernel split constants (`VMM_USER_TOP`, `0xFFFF800000000000` checks in
  `user_copy.c`, `selftest.c`) and the KASLR ranges would move;
- a 2-level section table in the PMM: the flat array is 8 bytes per GiB,
  which is 32 MiB for 4 PiB, so it still works, but it should be sparse;
- user ASLR and the mmap arena are unaffected (they stay below 128 TiB
  unless a process opts in, as on Linux).

## 7. Open issues

- Resolved in the Phase 4 integration (`integ/phase4`): the LAPIC/IOAPIC use
  `vmm_map_mmio(…, VMM_CACHE_UC)`, and `pm_map_range` ends every change to a
  live entry with a kernel TLB shootdown (`tlb_shootdown_kernel`), so MMIO
  mapped or retagged after the APs are up is safe on every CPU.
- `pmm_alloc_contiguous()` cannot return a run that crosses a 1 GiB section,
  and the run search is a linear word scan from the bottom of the zone. That
  is fine for the heap's multi-frame blocks, slow for very large runs on a
  fragmented multi-TiB machine. A buddy allocator would fix both.
- Early page tables (before the physmap) must come from below 512 MiB, the
  boot identity window. The old design had the same constraint.
- The DMA reasoning assumes the kernel image (and its static DMA buffers)
  sits below 4 GiB. AHCI and xHCI check it and refuse a 32-bit-only
  controller otherwise. Physical KASLR above 4 GiB would need
  `pmm_alloc_below()` for those buffers.
- Not NUMA-aware. No memory hotplug.
- Only tested under TCG. Not tested on real hardware (paused) or on KVM.
- More than 64 TiB needs 5-level paging (section 6).
