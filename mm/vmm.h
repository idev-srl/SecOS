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

// Initialize physmap for all physical memory (rounded up to 2MB boundary)
void vmm_init_physmap(void);
void vmm_extend_physmap(uint64_t phys_end); // extend physmap if needed (2MB granularity)

// Helper conversions
static inline uint64_t phys_to_virt(uint64_t phys) { return VMM_PHYSMAP_BASE + phys; }
static inline uint64_t virt_to_phys(uint64_t virt) { return (virt >= VMM_PHYSMAP_BASE) ? (virt - VMM_PHYSMAP_BASE) : 0; }

// Initialize VMM on current space (uses existing CR3)
void vmm_init(void);

// Map a 4K page (create tables if needed)
int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
int vmm_map_in_space(vmm_space_t* space, uint64_t virt, uint64_t phys, uint64_t flags);

// Unmap a 4K page
int vmm_unmap(uint64_t virt);
int vmm_unmap_in_space(vmm_space_t* space, uint64_t virt);

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

// ---- User space (phase 1b) ----
#define USER_CODE_BASE  0x0000000100000000ULL
#define USER_DATA_BASE  0x0000000200000000ULL
#define USER_STACK_TOP  0x00000003FFF00000ULL

int vmm_alloc_user_page(uint64_t virt);       // RW/NX
int vmm_map_user_code(uint64_t virt);         // RX
int vmm_map_user_data(uint64_t virt);         // RW/NX
uint64_t vmm_alloc_user_stack(int pages);     // allocate stack pages
vmm_space_t* vmm_space_create_user(void);     // create new address space
int vmm_space_destroy(vmm_space_t* space);    // destroy address space
// Hardening: remove USER bit from shared kernel entries
void vmm_harden_user_space(vmm_space_t* space);

// Variants operating on a specific space (not the current kernel space)
int vmm_alloc_user_page_in_space(vmm_space_t* space, uint64_t virt);
int vmm_map_user_code_in_space(vmm_space_t* space, uint64_t virt);
int vmm_map_user_data_in_space(vmm_space_t* space, uint64_t virt);
uint64_t vmm_alloc_user_stack_in_space(vmm_space_t* space, int pages);

#endif // VMM_H
