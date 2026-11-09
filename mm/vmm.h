#ifndef VMM_H
#define VMM_H
/*

 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdbool.h>

// Page flags (x86-64 standard)
#define VMM_FLAG_PRESENT   (1ULL<<0)
#define VMM_FLAG_RW        (1ULL<<1)
#define VMM_FLAG_USER      (1ULL<<2)
#define VMM_FLAG_PWT       (1ULL<<3)
#define VMM_FLAG_PCD       (1ULL<<4)
#define VMM_FLAG_ACCESSED  (1ULL<<5)
#define VMM_FLAG_DIRTY     (1ULL<<6)
#define VMM_FLAG_PS        (1ULL<<7)  // Page size (used only in non-PT levels)
#define VMM_FLAG_GLOBAL    (1ULL<<8)
#define VMM_FLAG_COW       (1ULL<<9)  // [M19] software bit: copy-on-write page (CPU-ignored)
// Software bit for PROT_NONE. In a VMA's flags it means "reserved, no access":
// the #PF handler never demand-faults such a page. In a leaf PTE it marks a
// PARKED page: PRESENT=0 (so every user AND kernel access faults) but the PTE
// still holds the frame, so mprotect() back to an accessible protection
// restores the page with its contents. Walkers that free/share/count user
// frames must treat a parked leaf as owning its frame (vmm_pte_has_frame).
#define VMM_FLAG_PROTNONE  (1ULL<<10)
// [M40] Software bit: the leaf maps DEVICE memory (e.g. the framebuffer) that the
// PMM does not own. Walkers that free/share/COW user frames must leave such a
// frame alone: unmap/destroy just drop the PTE, fork copies it verbatim (the
// mapping stays shared), mprotect keeps the bit and never adds COW.
#define VMM_FLAG_DEVICE    (1ULL<<11)
// [M57] Software bit (CPU-ignored, outside ADDRESS_MASK): a ring-3 driver's
// device mapping (BAR, DMA buffer). vmm_fork_space leaves it out of the child;
// always set together with VMM_FLAG_DEVICE.
#define VMM_FLAG_DONTFORK  (1ULL<<52)
#define VMM_FLAG_NOEXEC    (1ULL<<63) // NX bit (requires EFER.NXE enabled)

// Address space structure.
// [M48] active_cpus: bit n set while CPU n has this space's CR3 loaded
// (maintained by vmm_switch_space). A TLB shootdown for a user space only
// interrupts the CPUs in this set; the kernel space ignores it (shared by all).
#define VMM_CPU_MASK_WORDS 4   // 256 CPUs = SMP_MAX_CPUS (arch/x86/percpu.h)
typedef struct vmm_space {
    uint64_t pml4_phys;  // Physical frame of PML4
    volatile uint64_t active_cpus[VMM_CPU_MASK_WORDS];
} vmm_space_t;

// Virtual base of the physmap (Linux's 4-level direct-map base). [M49] The
// window is 64 TiB (128 PML4 slots). The base must stay 1 GiB aligned; every
// user of it goes through this one definition so it can be randomized later.
#define VMM_PHYSMAP_BASE 0xFFFF888000000000ULL
#define VMM_PHYSMAP_SIZE (64ULL << 40)

// [M12] Higher-half kernel LINK base: the kernel image is linked at this VMA.
// [M45] KASLR: it RUNS at kernel_vma_base() = KERNEL_VMA_BASE + g_kaslr_slide
// (a random 2 MiB multiple, boot/kaslr.h), where the low 0-512MB is mapped via
// PML4[511]. A kernel-image symbol's physical address is its runtime VMA minus
// kernel_vma_base().
#define KERNEL_VMA_BASE 0xFFFFFFFF80000000ULL
extern uint64_t g_kaslr_slide;   // kernel/kaslr.c
static inline uint64_t kernel_vma_base(void) { return KERNEL_VMA_BASE + g_kaslr_slide; }
// [M45] A kernel-image address as it is in kernel.bin (runtime VA minus the
// slide), for logs: it resolves with nm/addr2line and does not reveal the slide.
// Addresses outside the high-half window are returned unchanged.
static inline uint64_t kaddr_link(uint64_t a) {
    uint64_t base = kernel_vma_base();
    return (a >= base && a - base < (512ULL << 20)) ? a - g_kaslr_slide : a;
}

// Build the physmap from the PMM's RAM ranges (WB, 1G/2M/4K pages, NX).
void vmm_init_physmap(void);
// [M49] Cache types for physmap mappings.
#define VMM_CACHE_WB 0
#define VMM_CACHE_WC 1
#define VMM_CACHE_UC 2
// [M49] Map device memory [phys, phys+size) into the physmap with `cache`
// (retagging pages already mapped). Returns phys_to_virt(phys), 0 on failure.
uint64_t vmm_map_mmio(uint64_t phys, uint64_t size, int cache);
// [M49] Map firmware memory (ACPI tables, ...) write-back where it is not
// mapped yet. Returns phys_to_virt(phys), 0 on failure.
uint64_t vmm_map_phys(uint64_t phys, uint64_t size);
// Deprecated: map the 2 MiB block below phys_end uncached if unmapped. Use
// vmm_map_mmio.
void vmm_extend_physmap(uint64_t phys_end);
void vmm_pat_init(void);                          // [FB-WC] program PAT slot 1 = Write-Combining
void vmm_set_physmap_wc(uint64_t phys, uint64_t size); // [FB-WC] retag physmap range as WC
// [M49] Physmap geometry / cost, and a leaf audit (supervisor + NX everywhere).
typedef struct {
    uint64_t ram_end, span, slots, tables;
    uint64_t leaves_4k, leaves_2m, leaves_1g;
    int gbpages, maxphy, la57;
} vmm_physmap_stats_t;
void vmm_physmap_get_stats(vmm_physmap_stats_t* st);
void vmm_physmap_audit(void);

// Helper conversions
static inline uint64_t phys_to_virt(uint64_t phys) { return VMM_PHYSMAP_BASE + phys; }
static inline uint64_t virt_to_phys(uint64_t virt) { return (virt >= VMM_PHYSMAP_BASE) ? (virt - VMM_PHYSMAP_BASE) : 0; }

// [M12] Translate any kernel-reachable virtual address to physical:
//   - kernel image (>= KERNEL_VMA_BASE)  -> v - kernel_vma_base()
//   - physmap      (>= VMM_PHYSMAP_BASE) -> v - VMM_PHYSMAP_BASE
//   - otherwise                          -> page-table walk ([M44])
// Used where a static/kernel pointer must be handed to hardware as a physical
// address (e.g. virtio DMA descriptors).
// [M44] Anything else (the M2/M5 kernel-stack region, ...) is resolved by a
// page-table walk; there is no identity fallback any more (the low identity map
// is retired after early boot), so a low pointer yields 0, never "itself".
uint64_t vmm_kvirt_to_phys_slow(uint64_t v);
static inline uint64_t kvirt_to_phys(uint64_t v) {
    if (v >= KERNEL_VMA_BASE)  return v - kernel_vma_base();
    if (v >= VMM_PHYSMAP_BASE && v < VMM_PHYSMAP_BASE + VMM_PHYSMAP_SIZE) return v - VMM_PHYSMAP_BASE;
    return vmm_kvirt_to_phys_slow(v);
}

// [M44] 1 once vmm_init_physmap() ran (phys_to_virt is usable).
int vmm_physmap_ready(void);
// [M44] Unhook the boot low identity map from the kernel page tables.
void vmm_drop_low_identity(void);
// [M44] CR3 (phys, < 4 GB) for the SMP AP trampoline: kernel high half + boot
// identity map. 0 if unavailable.
uint64_t vmm_ap_trampoline_cr3(void);
// [M44] Present lower-half entries outside the user window (must be 0).
int vmm_space_low_kernel_entries(vmm_space_t* space);

// Initialize VMM on current space (uses existing CR3)
void vmm_init(void);

// Map a 4K page (create tables if needed)
int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
int vmm_map_in_space(vmm_space_t* space, uint64_t virt, uint64_t phys, uint64_t flags);
// [M12] W^X self-test (logs [WX] marker; a W+X request must be refused).
void vmm_wx_selftest(void);

// Unmap a 4K page
int vmm_unmap(uint64_t virt);
int vmm_unmap_in_space(vmm_space_t* space, uint64_t virt);
int vmm_protect_in_space(vmm_space_t* space, uint64_t virt, uint64_t flags); // [M18] change page prot
vmm_space_t* vmm_fork_space(vmm_space_t* parent); // [M19] COW duplicate of an address space
int vmm_cow_fault(vmm_space_t* space, uint64_t addr); // [M19] resolve a COW write fault
// A user leaf PTE that owns a frame: present, or parked by PROT_NONE.
static inline int vmm_pte_has_frame(uint64_t pte) {
    return (pte & (VMM_FLAG_PRESENT | VMM_FLAG_PROTNONE)) != 0;
}

// Translate virtual -> physical (return 0 if not mapped)
uint64_t vmm_translate(uint64_t virt);
uint64_t vmm_translate_in_space(vmm_space_t* space, uint64_t virt);

// Allocate and map new physical page (zeroed) at virtual address
int vmm_alloc_page(uint64_t virt, uint64_t flags);
int vmm_alloc_page_in_space(vmm_space_t* space, uint64_t virt, uint64_t flags);

// Get current kernel space
vmm_space_t* vmm_get_kernel_space(void);

// Clone space (stub)
vmm_space_t* vmm_clone_space(vmm_space_t* src);

// Switch address space (load CR3)
int vmm_switch_space(vmm_space_t* space);

// Dump page mapping (debug)
void vmm_dump_entry(uint64_t virt);

// Protect kernel sections applying W^X: text RX, rodata R (NX), data/bss RW (NX), stack RW (NX), guard page
void vmm_protect_kernel_sections(void);

// [M39] Boot-time W^X auditor: walks the high-half page tables, logs the count of
// pages that are both writable and executable (0 = full W^X proven).
void vmm_audit_wx(void);

// M1.3: Install guard pages for kernel stack and IST stacks.
// DEPRECATED (M2): superseded by vmm_alloc_kernel_stack() + vmm_alloc_ist_stack().
// Kept for reference; NOT called from kernel_main in M2.
void vmm_setup_kernel_guard_pages(void);

// ---- M2: dedicated guarded stack region ----
// Virtual base of the M2 stack region (PML4[511], PDPT[0], PDT[0] → single PT)
#define M2_STACK_REGION_BASE    0xFFFFFF8000000000ULL

// Kernel main stack layout (16 KB usable, 4 frames)
//   [GUARD_LO][page0..page3 usable][implicit-NP][GUARD_HI]
//   RSP_INIT = M2_KSTACK_TOP  (end of last usable page, NOT a guard page)
#define M2_KSTACK_GUARD_LO  (M2_STACK_REGION_BASE + 0x0000ULL)  // PTE = 0 (NP)
#define M2_KSTACK_BASE      (M2_STACK_REGION_BASE + 0x1000ULL)  // first usable page
#define M2_KSTACK_TOP       (M2_STACK_REGION_BASE + 0x5000ULL)  // RSP_INIT
#define M2_KSTACK_GUARD_HI  (M2_STACK_REGION_BASE + 0x6000ULL)  // PTE = 0 (NP)
#define M2_KSTACK_PAGES     4

// IST1 (Double Fault, IST index 1) — 8 KB usable, 2 frames
#define M2_IST1_GUARD_LO    (M2_STACK_REGION_BASE + 0x0F000ULL) // PTE = 0 (NP)
#define M2_IST1_BASE        (M2_STACK_REGION_BASE + 0x10000ULL) // first usable page
#define M2_IST1_TOP         (M2_STACK_REGION_BASE + 0x12000ULL) // TSS.ist1
#define M2_IST1_GUARD_HI    (M2_STACK_REGION_BASE + 0x13000ULL) // PTE = 0 (NP)

// IST2 (Page Fault, IST index 2) — 8 KB usable, 2 frames
#define M2_IST2_GUARD_LO    (M2_STACK_REGION_BASE + 0x14000ULL) // PTE = 0 (NP)
#define M2_IST2_BASE        (M2_STACK_REGION_BASE + 0x15000ULL)
#define M2_IST2_TOP         (M2_STACK_REGION_BASE + 0x17000ULL) // TSS.ist2
#define M2_IST2_GUARD_HI    (M2_STACK_REGION_BASE + 0x18000ULL) // PTE = 0 (NP)

// IST3 (General Protection Fault, IST index 3) — 8 KB usable, 2 frames
#define M2_IST3_GUARD_LO    (M2_STACK_REGION_BASE + 0x19000ULL) // PTE = 0 (NP)
#define M2_IST3_BASE        (M2_STACK_REGION_BASE + 0x1A000ULL)
#define M2_IST3_TOP         (M2_STACK_REGION_BASE + 0x1C000ULL) // TSS.ist3
#define M2_IST3_GUARD_HI    (M2_STACK_REGION_BASE + 0x1D000ULL) // PTE = 0 (NP)

#define M2_IST_PAGES        2   // pages per IST usable region (8 KB)

// End of the fixed M2 stacks (boot kernel stack + IST1..3 and their guards).
#define M2_FIXED_STACKS_END    (M2_IST3_GUARD_HI + 0x1000ULL)

// ---- M5: per-process guarded kernel stacks ----
// Each slot gets: [guard_lo(1 page)] [usable(16 pages)] [guard_hi(1 page)]
// Region grows downward from the top of the second 2MB PT block. (It used to
// start at the top of the FIRST block, which only fits 26 slots above the fixed
// M2 stacks: slot 26 landed on IST3, its vmm_map failed and the kernel halted
// once 27 processes coexisted — see the static assert in vmm.c.)
// Slot index is bounded [0..M5_MAX_KSTACK_SLOTS-1], derived from proc_table index.
#define M5_KSTACK_PAGES        16
#define M5_KSTACK_SLOT_PAGES   (M5_KSTACK_PAGES + 2)   // 18 pages per slot
#define M5_KSTACK_SLOT_SIZE    (M5_KSTACK_SLOT_PAGES * 0x1000ULL)
// [U9] 1024 slots (72 KiB of VA each, frames only while in use) for threads:
// the region now ends 80 MiB above the M2 base (PML4[511] PDPT[0], far below
// the kernel image); its page tables are created on first use.
#define M5_KSTACK_REGION_TOP   (M2_STACK_REGION_BASE + 0x5000000ULL)
#define M5_MAX_KSTACK_SLOTS    1024  // = MAX_PROCESSES (kernel/process.c): one per task

// Allocate guarded per-process kernel stack for a bounded slot index.
// Returns rsp0 (top of usable area, just below guard_hi). 0 on failure.
uint64_t vmm_alloc_kernel_stack_for_slot(uint32_t slot);

// Free per-process kernel stack (unmap usable pages, free PMM frames).
int vmm_free_kernel_stack_for_slot(uint32_t slot);

// Allocate and map the kernel main stack in the M2 region.
// Must be called after vmm_init_physmap() (uses phys_to_virt for PT walks).
// Returns M2_KSTACK_TOP — the RSP_INIT value for the new stack.
uint64_t vmm_alloc_kernel_stack(void);

// Allocate and map a single IST stack (called by tss_init for each IST).
// guard_lo / base / top / guard_hi must be page-aligned and non-overlapping.
// Returns top (= value to store in TSS.istN).
uint64_t vmm_alloc_ist_stack(uint64_t guard_lo, uint64_t base, uint64_t top,
                              uint64_t guard_hi, int ist_num);

// ---- User space (phase 1b) ----
#define USER_CODE_BASE  0x0000000100000000ULL  //  4 GB: ELF code
#define USER_DATA_BASE  0x0000000200000000ULL  //  8 GB: ELF data/bss
#define USER_HEAP_BASE  0x0000000280000000ULL  // 10 GB: brk/sbrk heap (grows up)  [M18]
#define VMM_USER_TOP    0x0000800000000000ULL  // end of the user canonical half (exclusive)
#define USER_MMAP_BASE  0x0000000300000000ULL  // 12 GB: mmap arena (grows up)     [M18]
#define USER_MMAP_END   0x00000003F0000000ULL  // below the stack guard band       [M18]
#define USER_STACK_TOP  0x00000003FFF00000ULL  // 16 GB: stack top (grows down)

int vmm_alloc_user_page(uint64_t virt);       // RW/NX
int vmm_map_user_code(uint64_t virt);         // RX
int vmm_map_user_data(uint64_t virt);         // RW/NX
uint64_t vmm_alloc_user_stack(int pages);     // allocate stack pages
vmm_space_t* vmm_space_create_user(void);     // create new address space
int vmm_space_destroy(vmm_space_t* space);    // destroy address space
uint32_t vmm_count_user_pages(vmm_space_t* space); // [M14] present user leaf pages (demand-paging metric)
int vmm_handle_page_fault(uint64_t fault_addr, uint64_t error_code); // [M15] 0=handled, -1=unhandled
int vmm_try_handle_page_fault(uint64_t fault_addr, uint64_t error_code); // same, but silent on -1
void vmm_report_page_fault(uint64_t fault_addr, uint64_t error_code);     // debugcon [PF] UNHANDLED line
// Hardening: remove USER bit from shared kernel entries
void vmm_harden_user_space(vmm_space_t* space);

// Variants operating on a specific space (not the current kernel space)
int vmm_alloc_user_page_in_space(vmm_space_t* space, uint64_t virt);
int vmm_map_user_code_in_space(vmm_space_t* space, uint64_t virt);
int vmm_map_user_data_in_space(vmm_space_t* space, uint64_t virt);
uint64_t vmm_alloc_user_stack_in_space(vmm_space_t* space, int pages);

#endif // VMM_H
