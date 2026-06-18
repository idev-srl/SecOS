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
#define VMM_FLAG_NOEXEC    (1ULL<<63) // NX bit (requires EFER.NXE enabled)

// Address space structure (currently only physical PML4 pointer)
typedef struct vmm_space {
    uint64_t pml4_phys;  // Physical frame of PML4
} vmm_space_t;

// Virtual base of physmap (chosen in unused high area)
#define VMM_PHYSMAP_BASE 0xFFFF888000000000ULL

// [M12] Higher-half kernel base: the kernel image is linked at this VMA and the
// low identity (0-512MB) maps to it via PML4[511]. A kernel-image symbol's
// physical address is its VMA minus this base.
#define KERNEL_VMA_BASE 0xFFFFFFFF80000000ULL

// Initialize physmap for all physical memory (rounded up to 2MB boundary)
void vmm_init_physmap(void);
void vmm_extend_physmap(uint64_t phys_end); // extend physmap if needed (2MB granularity)

// Helper conversions
static inline uint64_t phys_to_virt(uint64_t phys) { return VMM_PHYSMAP_BASE + phys; }
static inline uint64_t virt_to_phys(uint64_t virt) { return (virt >= VMM_PHYSMAP_BASE) ? (virt - VMM_PHYSMAP_BASE) : 0; }

// [M12] Translate any kernel-reachable virtual address to physical:
//   - kernel image (>= KERNEL_VMA_BASE)  -> v - KERNEL_VMA_BASE
//   - physmap      (>= VMM_PHYSMAP_BASE) -> v - VMM_PHYSMAP_BASE
//   - otherwise (low identity)           -> v
// Used where a static/kernel pointer must be handed to hardware as a physical
// address (e.g. virtio DMA descriptors).
static inline uint64_t kvirt_to_phys(uint64_t v) {
    if (v >= KERNEL_VMA_BASE)  return v - KERNEL_VMA_BASE;
    if (v >= VMM_PHYSMAP_BASE) return v - VMM_PHYSMAP_BASE;
    return v;
}

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

// ---- M5: per-process guarded kernel stacks ----
// Each slot gets: [guard_lo(1 page)] [usable(16 pages)] [guard_hi(1 page)]
// Region grows downward from top of the first 2MB PT block.
// Slot index is bounded [0..M5_MAX_KSTACK_SLOTS-1], derived from proc_table index.
#define M5_KSTACK_PAGES        16
#define M5_KSTACK_SLOT_PAGES   (M5_KSTACK_PAGES + 2)   // 18 pages per slot
#define M5_KSTACK_SLOT_SIZE    (M5_KSTACK_SLOT_PAGES * 0x1000ULL)
#define M5_KSTACK_REGION_TOP   (M2_STACK_REGION_BASE + 0x200000ULL) // top of first 2MB block
#define M5_MAX_KSTACK_SLOTS    32  // must match MAX_PROCESSES

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
// Hardening: remove USER bit from shared kernel entries
void vmm_harden_user_space(vmm_space_t* space);

// Variants operating on a specific space (not the current kernel space)
int vmm_alloc_user_page_in_space(vmm_space_t* space, uint64_t virt);
int vmm_map_user_code_in_space(vmm_space_t* space, uint64_t virt);
int vmm_map_user_data_in_space(vmm_space_t* space, uint64_t virt);
uint64_t vmm_alloc_user_stack_in_space(vmm_space_t* space, int pages);

#endif // VMM_H
