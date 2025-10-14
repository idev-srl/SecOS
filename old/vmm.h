#ifndef VMM_H
#define VMM_H

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

// Conversioni helper
static inline uint64_t phys_to_virt(uint64_t phys) { return VMM_PHYSMAP_BASE + phys; }
static inline uint64_t virt_to_phys(uint64_t virt) { return (virt >= VMM_PHYSMAP_BASE) ? (virt - VMM_PHYSMAP_BASE) : 0; }

// Inizializza il VMM sullo spazio corrente (usa CR3 esistente)
void vmm_init(void);

// Mappa una pagina 4K (crea tabelle se necessario)
int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);

// Smappa una pagina
int vmm_unmap(uint64_t virt);

// Traduci indirizzo virtuale -> fisico (ritorna 0 se non presente)
uint64_t vmm_translate(uint64_t virt);

// Alloca e mappa nuova pagina fisica (zeroed) in virt
int vmm_alloc_page(uint64_t virt, uint64_t flags);

// Ottieni spazio kernel corrente
vmm_space_t* vmm_get_kernel_space(void);

// Clona spazio (stub)
vmm_space_t* vmm_clone_space(vmm_space_t* src);

// Switch indirizzi (carica CR3)
int vmm_switch_space(vmm_space_t* space);

// Dump di una pagina (debug)
void vmm_dump_entry(uint64_t virt);

#endif // VMM_H
