● SECoS M1 Phase 1 — Memory Architecture Deep Analysis

  ---
  A) Page Table Ownership

  Current state: kernel inherits bootloader CR3

  vmm_init() at mm/vmm.c:137:
  kernel_space.pml4_phys = read_cr3();
  The kernel reads whatever CR3 the bootloader loaded. It never allocates or builds its own tables.

  UEFI path (uefi/boot.c lines 121–139):
  - Three frames allocated via AllocatePages (PML4, PDPT, PDT)
  - activate_page_tables(pml4) (lines 15–37): enables PAE + NXE, loads CR3, enforces PG
  - Frames live in EFI_LOADER_DATA or EFI_BOOT_SERVICES_DATA memory — firmware may mark these as available after
  ExitBootServices

  Multiboot2 path (boot/boot.asm lines 36–57):
  - pml4: resb 4096, pdpt: resb 4096, pdt: resb 4096 in kernel .bss — tables embedded in the ELF binary itself
  - _start zeroes, builds, and loads CR3 = physical address of pml4 symbol

  Problem: both table sets are ephemeral.
  - UEFI frames: PMM may eventually mark AllocatePages pages as free RAM and reallocate them
  - MB2 frames: in .bss, not tracked by PMM as reserved page table frames, though their physical pages are within the
  kernel segment (safe for now)

  ---
  B) Identity Mapping

  Structure (both paths)

  3-level table, 2MB huge pages — no PT level:

  PML4[0]      → PDPT phys        [Present|Write, 0x03]
  PDPT[0]      → PDT  phys        [Present|Write, 0x03]
  PDT[0..255]  → index * 2MB      [Present|Write|PS, 0x83]

  Coverage: 0x0000_0000 – 0x1FFF_FFFF (512MB physical → virtual, 1:1).

  Kernel loads at 0x200000 (2MB), well inside the range.

  What is NOT mapped at boot

  The physmap (VMM_PHYSMAP_BASE = 0xFFFF888000000000) does NOT exist in the boot tables. It is established later by
  vmm_init_physmap() / vmm_extend_physmap(), which walk PML4[272] downward. These walks also use identity casts (see
  §C).

  Any physical address ≥ 512MB (0x20000000) is completely inaccessible until vmm_init_physmap runs.

  ---
  C) VMM Access Pattern

  Pervasive identity assumption — four critical sites

  get_pt_space (mm/vmm.c:104):
  uint64_t* pml4 = (uint64_t*)pml4_phys; // Identity assumption

  get_or_create_table (mm/vmm.c:96):
  return (uint64_t*)phys; // Identity assumption

  vmm_init_physmap (mm/vmm.c:376-377):
  uint64_t* pml4 = (uint64_t*)pml4_phys; // identity assumption

  vmm_extend_physmap (mm/vmm.c:40):
  uint64_t* pdpt = (uint64_t*)(pml4[pml4_i] & ADDRESS_MASK); // identity cast

  Only zero_frame has a physmap-aware path

  if (physmap_initialized) {
      p = (uint8_t*)phys_to_virt(phys);
  } else {
      p = (uint8_t*)phys;            // identity fallback
  }

  This is the only function with a guarded transition. Every page table walker is permanently identity-dependent.

  Critical conclusion for M1: Identity mapping must be kept alive for the entire lifetime of the kernel until every page
   table walk function is updated to use phys_to_virt. M1 must NOT remove identity mapping — it must extend it by adding
   a kernel-owned layer on top.

  ---
  D) PMM Limits

  128MB hard cap — both UEFI and MB2 paths

  pmm_init_uefi (mm/pmm.c:278-283) clips each EFI memory region to:
  uint64_t identity_limit = 128ULL * 1024 * 1024;

  pmm_init [MB1] (mm/pmm.c:188-196) applies the same 128MB clip before calling pmm_build_from_regions.

  pmm_build_from_regions (mm/pmm.c:91-94) then applies a second cap:
  uint64_t mapped_limit = 512ULL * 1024 * 1024;
  total_frames = MIN(total_frames, mapped_limit / PAGE_SIZE);

  Net result: min(128MB, 512MB) = 128MB. The PMM ignores all physical RAM above 128MB, regardless of how much the
  machine has.

  Bitmap placement risk

  frame_bitmap = (uint32_t*)((uint64_t)&_kernel_end);

  Placed immediately after the kernel ELF image. For 128MB / 4KB pages = 32768 frames → bitmap = 4KB (one frame). Small,
   but there is no check that this page doesn't overlap:
  - UEFI page table frames (at AllocatePages addresses — could theoretically be near _kernel_end)
  - secos_boot_info (EFI_LOADER_DATA at an unknown physical address)

  find_free_frame is O(n)

  mm/pmm.c:46-53: bit-by-bit linear scan from frame 0. With 128MB / 4KB = 32768 frames, worst case = 32768 iterations.
  Acceptable now, not at 1GB+.

  ---
  E) Stack Layout

  Kernel stack (boot.asm lines 36–42)

  - .bss section: resb 16384 (16KB)
  - stack_bottom → low address, stack_top → high address (stack grows down)
  - _uefi_start: lea rsp, [rel stack_top] (RIP-relative, position-independent)
  - long_mode (MB2): mov rsp, stack_top (absolute)
  - Physical = virtual (identity-mapped, load at 0x200000)

  Guard page: ABSENT. vmm_protect_kernel_sections (mm/vmm.c:239) explicitly defers it:
  // Guard page deferred for stability
  The 4KB page immediately below stack_bottom is mapped Present|Write as part of the 512MB identity slab. A stack
  overflow corrupts silently — no #PF.

  User stack

  vmm_alloc_user_stack allocates pages pages from USER_STACK_TOP - pages*PAGE_SIZE upward. The page at USER_STACK_TOP -
  (pages+1)*PAGE_SIZE is never mapped → guard page exists by omission. This is fragile: any concurrent mapping that
  touches that address removes the guard.

  IST stacks (TSS)

  tss_init (arch/x86/tss.c): ist1, ist2, ist3 — each a single PMM-allocated frame (4KB). Direct identity-mapped pointer.
   No guard pages. 4KB is tight for nested exception chains.

  ---
  F) GDT

  Boot GDT (boot/boot.asm:281-288)

  sel 0x00: null
  sel 0x08: 64-bit kernel code   (0x00209A0000000000)
  sel 0x10: kernel data          (0x0000920000000000)
  3 entries, no user mode descriptors, no TSS. Loaded by both _start and _uefi_start.

  Kernel GDT (arch/x86/tss.c)

  tss_init() builds a new GDT in static buffers and calls gdt_flush:
  sel 0x00: null
  sel 0x08: kernel code   (access 0x9A — Present|S|Code|Read, 64-bit)
  sel 0x10: kernel data   (access 0x92 — Present|S|Data|Write)
  sel 0x18: user data     (access 0xF2 — Present|DPL3|S|Data|Write)
  sel 0x20: user code     (access 0xFA — Present|DPL3|S|Code|Read, 64-bit)
  sel 0x28: TSS           (16-byte system descriptor)

  After tss_init() runs, the boot GDT is replaced. Kernel code/data selectors are compatible (0x08, 0x10 preserved) so
  execution continues uninterrupted.

  Timing risk: tss_init must be called BEFORE idt_init (or before any exception can fire). If a #PF occurs during
  vmm_init before tss_init, the CPU uses the 3-entry boot GDT with no TSS and no RSP0. The ring transition stack is
  undefined → #DF → triple fault. The init order in kernel/main.c must be audited.

  ---
  G) Risks When Activating Kernel-Owned CR3

  Risk 1: Identity casts break if identity mapping is not preserved

  All VMM page table walkers (get_pt_space, get_or_create_table, vmm_extend_physmap) cast physical addresses directly.
  The new kernel-owned tables MUST maintain the 0–512MB identity mapping, or every subsequent page table operation
  crashes. Identity mapping is non-negotiable for M1.

  Risk 2: UEFI table frames recycled by PMM

  pmm_init_uefi may mark the AllocatePages frames (where UEFI's PML4/PDPT/PDT live) as available RAM. If vmm_init then
  reads the inherited CR3 and those frames have been reallocated for other use, the entire page table tree is garbage.
  Kernel-owned tables must be built BEFORE PMM marks those frames free, or the UEFI memory map entries for those frames
  must be explicitly excluded.

  Risk 3: secos_boot_info overwritten by PMM

  static struct secos_boot_info bootinfo in uefi/boot.c lives in EFI_LOADER_DATA. If PMM marks that physical page as
  free (which it may, depending on how EFI memory types are classified), the first PMM allocation could overwrite
  bootinfo. kernel_main receives the pointer info = &bootinfo and reads it throughout init. bootinfo must be copied to a
   PMM-allocated buffer before PMM init runs, or its physical page explicitly marked used.

  Risk 4: Atomic CR3 switch requirement

  The new tables must be fully built and coherent BEFORE mov cr3, rax. The new PML4 must map:
  - The currently executing code (kernel text at 0x200000) ✓
  - The kernel stack (in .bss, identity) ✓
  - The PMM bitmap (at _kernel_end, identity) ✓
  - The physmap range if physmap_initialized is already true ✓
  - Any other kernel data the next instruction accesses ✓

  Risk 5: MB2 .bss tables aliased by new mapping

  Not a real risk IF the new tables also identity-map 0–512MB (which they must per Risk 1). The old .bss tables remain
  mapped and accessible; the new CR3 simply points elsewhere.

  Risk 6: vmm_space_destroy leaks compound

  Every process destruction already leaks all page table frames (only kfree(space)). After M1.1 introduces PMM-allocated
   kernel page table frames, the same bug leaks new frames too. Must be fixed in M1.5 before any process lifetime
  testing.

  Risk 7: COMMON symbols outside W^X range

  linker.ld places *(COMMON) BEFORE _bss_start:
  .bss : ALIGN(4K) {
      *(COMMON)          ← outside [_bss_start, _bss_end]
      _bss_start = .;
      *(.bss*)
      _bss_end = .;
  }
  vmm_protect_kernel_sections only applies W^X between _bss_start and _bss_end. COMMON symbols remain
  writable+executable after W^X enforcement. Not a boot blocker but a correctness hole.

  ---
  M1 Detailed Technical Plan

  M1.1 — Kernel-Owned Page Tables

  Goal: Build a kernel-owned PML4/PDPT/PDT allocated from PMM, load new CR3, while preserving all existing mappings.

  Sequencing constraint:
  1. pmm_init_uefi()
  2. copy secos_boot_info → PMM-allocated buffer
  3. mark bootloader table frames as USED in PMM
  4. build kernel PML4/PDPT/PDT via pmm_alloc_frame()
     - identity map 0–512MB (2MB pages)
     - map physmap range 0xFFFF888000000000 (if physmap already initialized)
     - map kernel sections with W^X flags
  5. switch CR3 to new PML4

  New functions needed:
  - vmm_build_kernel_tables() → builds and returns new pml4_phys
  - vmm_switch_cr3(uint64_t pml4_phys) → validates completeness, executes CR3 load
  - pmm_reserve_bootloader_tables() → marks UEFI/MB2 table frames as used

  Files: mm/vmm.c, mm/pmm.c, kernel/main.c (sequencing)

  Acceptance: kernel executes through full init sequence after CR3 switch; PMM allocations succeed; no #PF.

  ---
  M1.2 — Physmap Walk Transition

  Goal: Update all four identity-cast sites to use phys_to_virt when physmap_initialized is true.

  Changes:
  1. get_or_create_table (mm/vmm.c:96):
  return physmap_initialized ? (uint64_t*)phys_to_virt(phys) : (uint64_t*)phys;
  2. get_pt_space (mm/vmm.c:104): same guard for PML4 dereference
  3. vmm_extend_physmap (mm/vmm.c:40): internal PDPT/PDT walks use phys_to_virt
  4. vmm_init_physmap (mm/vmm.c:376): initial PML4 walk still uses identity (physmap not yet initialized at entry); only
   internal NEW allocations use physmap-aware path after the flag is set

  Constraint: Identity mapping stays alive through M1.2. Removal is deferred until every call site is confirmed
  physmap-aware (out of M1 scope).

  Files: mm/vmm.c

  Acceptance: allocate and walk page tables after vmm_init_physmap() — verify via debugcon that no identity cast is
  exercised for new allocations.

  ---
  M1.3 — Guard Pages

  Goal: Activate kernel stack guard page; make user stack guard page explicit; fix linker.ld COMMON placement.

  Kernel stack guard:
  - The 2MB page covering the kernel stack must be split into 4KB pages
  - Requires allocating a new PT (4KB frame from PMM) to replace the 2MB entry covering stack_bottom - PAGE_SIZE
  - Mark that one 4KB entry as not-present (P=0)
  - Must happen after M1.1 (PMM available) and after M1.2 (can allocate PT frame and walk it)

  User stack guard (make explicit):
  - After vmm_alloc_user_stack_in_space, explicitly map a not-present entry at stack_bottom - PAGE_SIZE
  - Removes reliance on "guard by omission"

  IST stack expansion:
  - Expand ist1, ist2, ist3 from 4KB to 8KB (2 PMM frames each)
  - Add 4KB not-present guard page below each IST stack
  - Update tss_init IST base addresses

  linker.ld COMMON fix:
  - Move *(COMMON) to AFTER _bss_start = .; — one-line change, no binary behavior change, closes W^X hole for global
  variables

  Files: mm/vmm.c, arch/x86/tss.c, linker.ld

  ---
  M1.4 — GDT Kernel-Owned (Timing Verification)

  Goal: Confirm tss_init() is called before any exception can fire. This is primarily a verification task.

  Action: Read kernel/main.c and map the full init sequence:
  - Is tss_init() called before vmm_init()?
  - Is idt_init() called after tss_init()?
  - Is there any code path between kernel_main entry and tss_init() that could trigger a fault (e.g., null pointer
  deref, bad memory access)?

  If out of order: Move tss_init() to be the first kernel init call (before PMM, VMM, heap). It has no dependencies — it
   uses static buffers and allocates IST stacks from PMM (which is the only dependency: must happen after PMM init or
  IST stacks must be allocated differently).

  Alternative: Pre-allocate IST stack frames in the bootloader (secos_boot_info) to allow tss_init before pmm_init.

  Files: kernel/main.c (likely a one-line reorder); possibly arch/x86/tss.c if IST allocation needs refactoring.

  This is the lowest-risk M1 subtask and should be verified first.

  ---
  M1.5 — Fix vmm_space_destroy

  Goal: Walk and free all page table frames when an address space is destroyed.

  Current bug (mm/vmm.c:358-362):
  void vmm_space_destroy(vmm_space_t* space) {
      kfree(space);  // leaks PML4/PDPT/PDT/PT frames
  }

  Fix: Recursive walk over user-space PML4 entries (entries 0..255 for identity-range user space):
  for each PML4[i] in user range:
    → walk PDPT entries:
      → walk PDT entries (skip if 2MB PS):
        → walk PT entries: pmm_free_frame(mapped phys) for user pages
        → pmm_free_frame(PT frame)
      → pmm_free_frame(PDT frame)
    → pmm_free_frame(PDPT frame)
  → pmm_free_frame(PML4 frame)
  → kfree(space)

  Constraint: Must NOT walk kernel-shared entries (PML4[256+] which map kernel and physmap). These are shared across all
   address spaces and must only be modified when the kernel shuts down.

  Files: mm/vmm.c

  ---
  M1 Dependency Graph

  M1.4  Verify/fix tss_init timing          ← no deps, do first
    │
    ▼
  M1.1  Kernel-owned page tables            ← requires PMM init sequence + M1.4
    │
    ├──► M1.2  Physmap walk update          ← requires M1.1 (new tables active)
    │
    ├──► M1.3  Guard pages                  ← requires M1.1 + M1.2 (physmap for PT alloc)
    │
    └──► M1.5  vmm_space_destroy fix        ← requires M1.1 (PMM active); independent of M1.2/M1.3

  M1.4 can be done immediately (read one file, possibly reorder two lines).
  M1.1 is the critical-path item — all others depend on it.
  M1.5 can proceed in parallel with M1.2/M1.3 once M1.1 is complete.

  ---
  Analisi completata — nessuna modifica al codice effettuata. M1 non ancora iniziato.