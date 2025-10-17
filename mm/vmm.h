#ifndef VMM_H
#define VMM_H
/*

 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdbool.h>

// Flags pagina (x86-64 standard)
#define VMM_FLAG_PRESENT   (1ULL<<0)
#define VMM_FLAG_RW        (1ULL<<1)
#define VMM_FLAG_USER      (1ULL<<2)
#define VMM_FLAG_PWT       (1ULL<<3)
#define VMM_FLAG_PCD       (1ULL<<4)
#define VMM_FLAG_ACCESSED  (1ULL<<5)
#define VMM_FLAG_DIRTY     (1ULL<<6)
#define VMM_FLAG_PS        (1ULL<<7)  // Page size (usato solo nei livelli non PT)
#define VMM_FLAG_GLOBAL    (1ULL<<8)
#define VMM_FLAG_NOEXEC    (1ULL<<63) // Bit NX (richiede EFER.NXE attivo)

// Struttura address space (per ora solo puntatore a PML4 fisico)
typedef struct vmm_space {
    uint64_t pml4_phys;  // Frame fisico della PML4
} vmm_space_t;

// Base virtuale della physmap (scelta in area alta non usata)
#define VMM_PHYSMAP_BASE 0xFFFF888000000000ULL

// Inizializza physmap per tutta la memoria fisica (fino a total_memory arrotondato a 2MB)
void vmm_init_physmap(void);
void vmm_extend_physmap(uint64_t phys_end); // estende physmap se serve (2MB granularity)

// Conversioni helper
static inline uint64_t phys_to_virt(uint64_t phys) { return VMM_PHYSMAP_BASE + phys; }
static inline uint64_t virt_to_phys(uint64_t virt) { return (virt >= VMM_PHYSMAP_BASE) ? (virt - VMM_PHYSMAP_BASE) : 0; }

// Inizializza il VMM sullo spazio corrente (usa CR3 esistente)
void vmm_init(void);

// Mappa una pagina 4K (crea tabelle se necessario)
int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
int vmm_map_in_space(vmm_space_t* space, uint64_t virt, uint64_t phys, uint64_t flags);

// Smappa una pagina
int vmm_unmap(uint64_t virt);
int vmm_unmap_in_space(vmm_space_t* space, uint64_t virt);

// Traduci indirizzo virtuale -> fisico (ritorna 0 se non presente)
uint64_t vmm_translate(uint64_t virt);
uint64_t vmm_translate_in_space(vmm_space_t* space, uint64_t virt);

// Alloca e mappa nuova pagina fisica (zeroed) in virt
int vmm_alloc_page(uint64_t virt, uint64_t flags);
int vmm_alloc_page_in_space(vmm_space_t* space, uint64_t virt, uint64_t flags);

// Ottieni spazio kernel corrente
vmm_space_t* vmm_get_kernel_space(void);

// Clona spazio (stub)
vmm_space_t* vmm_clone_space(vmm_space_t* src);

// Switch indirizzi (carica CR3)
int vmm_switch_space(vmm_space_t* space);

// Dump di una pagina (debug)
void vmm_dump_entry(uint64_t virt);

// Protegge sezioni kernel applicando W^X: text RX, rodata R (NX), data/bss RW (NX), stack NX, guard page
void vmm_protect_kernel_sections(void);

// ---- User space (fase 1b) ----
#define USER_CODE_BASE  0x0000000100000000ULL
#define USER_DATA_BASE  0x0000000200000000ULL
#define USER_STACK_TOP  0x00000003FFF00000ULL

int vmm_alloc_user_page(uint64_t virt);       // RW/NX
int vmm_map_user_code(uint64_t virt);         // RX
int vmm_map_user_data(uint64_t virt);         // RW/NX
uint64_t vmm_alloc_user_stack(int pages);     // allocate stack pages
vmm_space_t* vmm_space_create_user(void);     // new address space
int vmm_space_destroy(vmm_space_t* space);    // destroy space
// Hardening: rimuove eventuali bit USER dalle entry condivise kernel
void vmm_harden_user_space(vmm_space_t* space);

// Varianti che operano su uno spazio specifico (non sul kernel corrente)
int vmm_alloc_user_page_in_space(vmm_space_t* space, uint64_t virt);
int vmm_map_user_code_in_space(vmm_space_t* space, uint64_t virt);
int vmm_map_user_data_in_space(vmm_space_t* space, uint64_t virt);
uint64_t vmm_alloc_user_stack_in_space(vmm_space_t* space, int pages);

#endif // VMM_H
