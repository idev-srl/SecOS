#include "vmm.h"
#include "pmm.h"
#include "terminal.h"

// Strutture base page table
#define PAGE_SIZE 4096ULL
#define PT_ENTRIES 512

// Maschere
#define ADDRESS_MASK 0x000FFFFFFFFFF000ULL

static vmm_space_t kernel_space;
static int physmap_initialized = 0;

// Helper per zero frame
static void zero_frame(uint64_t phys) {
    uint8_t* p;
    if (physmap_initialized) {
        p = (uint8_t*)phys_to_virt(phys);
    } else {
        // Fallback identity: funziona finché i frame iniziali sono mappati
        p = (uint8_t*)phys;
    }
    for (int i = 0; i < PAGE_SIZE; i++) p[i] = 0;
}

static inline uint64_t read_cr3(void) {
    uint64_t val; __asm__ volatile("mov %%cr3, %0" : "=r"(val)); return val;
}
static inline void write_cr3(uint64_t val) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(val));
}

// Cammina o crea livello
static uint64_t* get_or_create_table(uint64_t* table, int index, uint64_t flags) {
    uint64_t entry = table[index];
    if (!(entry & VMM_FLAG_PRESENT)) {
        void* frame = pmm_alloc_frame();
        if (!frame) return NULL;
        zero_frame((uint64_t)frame);
        uint64_t phys = (uint64_t)frame & ADDRESS_MASK;
        table[index] = phys | (flags & (VMM_FLAG_RW|VMM_FLAG_USER|VMM_FLAG_PWT|VMM_FLAG_PCD)) | VMM_FLAG_PRESENT;
        return (uint64_t*)phys; // Identity assumption
    }
    return (uint64_t*)(entry & ADDRESS_MASK);
}

// Ottieni puntatore livello finale (PT) per virt
static uint64_t* get_pt(uint64_t virt, int create, uint64_t flags) {
    uint64_t pml4_phys = kernel_space.pml4_phys & ADDRESS_MASK;
    uint64_t* pml4 = (uint64_t*)pml4_phys; // Identity assumption

    int pml4_i = (virt >> 39) & 0x1FF;
    int pdpt_i = (virt >> 30) & 0x1FF;
    int pdt_i  = (virt >> 21) & 0x1FF;
    int pt_i   = (virt >> 12) & 0x1FF;

    uint64_t* pdpt = (uint64_t*)(pml4[pml4_i] & ADDRESS_MASK);
    if (!pdpt) {
        if (!create) return NULL;
        pdpt = get_or_create_table(pml4, pml4_i, flags);
        if (!pdpt) return NULL;
    }
    uint64_t* pdt = (uint64_t*)(pdpt[pdpt_i] & ADDRESS_MASK);
    if (!(pdpt[pdpt_i] & VMM_FLAG_PRESENT)) {
        if (!create) return NULL;
        pdt = get_or_create_table(pdpt, pdpt_i, flags);
        if (!pdt) return NULL;
    }
    uint64_t* pt = (uint64_t*)(pdt[pdt_i] & ADDRESS_MASK);
    if (!(pdt[pdt_i] & VMM_FLAG_PRESENT)) {
        if (!create) return NULL;
        pt = get_or_create_table(pdt, pdt_i, flags);
        if (!pt) return NULL;
    }
    (void)pt_i; // usato da vmm_map
    return pt;
}

void vmm_init(void) {
    kernel_space.pml4_phys = read_cr3();
    terminal_writestring("[OK] VMM init (CR3= ");
    uint64_t cr3 = kernel_space.pml4_phys; // Already physical
    char hex_chars[] = "0123456789ABCDEF"; char buf[17]; buf[16]='\0';
    for (int i=15;i>=0;i--){ buf[i]=hex_chars[cr3 & 0xF]; cr3 >>=4; }
    terminal_writestring("0x"); terminal_writestring(buf); terminal_writestring(")\n");
}

// Mappa tutta la memoria fisica disponibile usando pagine 2MB (huge) per ridurre numero di tabelle.
void vmm_init_physmap(void) {
    if (physmap_initialized) return;
    uint64_t total = pmm_get_total_memory();
    if (total == 0) {
        terminal_writestring("[WARN] physmap: memoria totale = 0\n");
        return;
    }
    // Arrotonda a multipli di 2MB
    const uint64_t HUGE_SIZE = 2ULL * 1024 * 1024;
    uint64_t limit = (total + HUGE_SIZE - 1) & ~(HUGE_SIZE - 1);

    uint64_t pml4_phys = kernel_space.pml4_phys & ADDRESS_MASK;
    uint64_t* pml4 = (uint64_t*)pml4_phys; // identity assumption

    // Calcola indici PML4 / PDPT / PDT per range physmap
    // Usando VMM_PHYSMAP_BASE: estrai pml4 index
    int pml4_i = (VMM_PHYSMAP_BASE >> 39) & 0x1FF;
    int pdpt_i_start = (VMM_PHYSMAP_BASE >> 30) & 0x1FF;

    // Assicura PDPT presente
    uint64_t* pdpt = (uint64_t*)(pml4[pml4_i] & ADDRESS_MASK);
    if (!(pml4[pml4_i] & VMM_FLAG_PRESENT)) {
        void* frame = pmm_alloc_frame(); if (!frame) { terminal_writestring("[ERR] physmap: PDPT alloc fail\n"); return; }
        zero_frame((uint64_t)frame);
        pml4[pml4_i] = ((uint64_t)frame & ADDRESS_MASK) | VMM_FLAG_PRESENT | VMM_FLAG_RW;
        pdpt = (uint64_t*)(((uint64_t)frame) & ADDRESS_MASK);
    }

    uint64_t phys_cursor = 0;
    while (phys_cursor < limit) {
        int pdpt_i = pdpt_i_start + ((phys_cursor >> 30) & 0x1FF); // Semplice, non supera 512 per 128MB
        if (pdpt_i >= 512) { terminal_writestring("[WARN] physmap: supera PDPT range\n"); break; }
        uint64_t* pdt = (uint64_t*)(pdpt[pdpt_i] & ADDRESS_MASK);
        if (!(pdpt[pdpt_i] & VMM_FLAG_PRESENT)) {
            void* frame = pmm_alloc_frame(); if (!frame) { terminal_writestring("[ERR] physmap: PDT alloc fail\n"); break; }
            zero_frame((uint64_t)frame);
            pdpt[pdpt_i] = ((uint64_t)frame & ADDRESS_MASK) | VMM_FLAG_PRESENT | VMM_FLAG_RW;
            pdt = (uint64_t*)(((uint64_t)frame) & ADDRESS_MASK);
        }
        // Riempie PDT con huge pages
        for (int pdt_i = 0; pdt_i < 512 && phys_cursor < limit; pdt_i++) {
            // Calcola virtuale corrispondente
            uint64_t virt = VMM_PHYSMAP_BASE + phys_cursor;
            int virt_pdt_i = (virt >> 21) & 0x1FF;
            if (virt_pdt_i != pdt_i) continue; // Allinea con indice locale
            if (!(pdt[pdt_i] & VMM_FLAG_PRESENT)) {
                // Marcare NX su physmap (non eseguibile) e RW
                pdt[pdt_i] = (phys_cursor & ADDRESS_MASK) | VMM_FLAG_PRESENT | VMM_FLAG_RW | VMM_FLAG_PS | VMM_FLAG_NOEXEC;
            }
            phys_cursor += HUGE_SIZE;
        }
    }
    physmap_initialized = 1;
    terminal_writestring("[OK] Physmap inizializzata fino a ");
    char hex[17]; hex[16]='\0'; uint64_t v=limit; char hc[]="0123456789ABCDEF"; for(int i=15;i>=0;i--){ hex[i]=hc[v & 0xF]; v >>=4; }
    terminal_writestring("0x"); terminal_writestring(hex); terminal_writestring(" (fisico)\n");
}

int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    if (virt & 0xFFF || phys & 0xFFF) return -1; // non allineati
    uint64_t* pt = get_pt(virt, 1, flags);
    if (!pt) return -2;
    int pt_i = (virt >> 12) & 0x1FF;
    if (pt[pt_i] & VMM_FLAG_PRESENT) return -3; // già mappato
    pt[pt_i] = (phys & ADDRESS_MASK) | (flags & ~VMM_FLAG_PS) | VMM_FLAG_PRESENT;
    return 0;
}

int vmm_unmap(uint64_t virt) {
    if (virt & 0xFFF) return -1;
    uint64_t* pt = get_pt(virt, 0, 0);
    if (!pt) return -2;
    int pt_i = (virt >> 12) & 0x1FF;
    if (!(pt[pt_i] & VMM_FLAG_PRESENT)) return -3;
    pt[pt_i] = 0;
    // Invalidate TLB
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    return 0;
}

uint64_t vmm_translate(uint64_t virt) {
    uint64_t* pt = get_pt(virt, 0, 0);
    if (!pt) return 0;
    int pt_i = (virt >> 12) & 0x1FF;
    if (!(pt[pt_i] & VMM_FLAG_PRESENT)) return 0;
    return (pt[pt_i] & ADDRESS_MASK) | (virt & 0xFFF);
}

int vmm_alloc_page(uint64_t virt, uint64_t flags) {
    void* frame = pmm_alloc_frame();
    if (!frame) return -1;
    zero_frame((uint64_t)frame);
    return vmm_map(virt, (uint64_t)frame, flags | VMM_FLAG_RW);
}

vmm_space_t* vmm_get_kernel_space(void) { return &kernel_space; }

vmm_space_t* vmm_clone_space(vmm_space_t* src) {
    (void)src; return NULL; // stub
}

int vmm_switch_space(vmm_space_t* space) {
    if (!space) return -1;
    write_cr3(space->pml4_phys & ADDRESS_MASK);
    return 0;
}

void vmm_dump_entry(uint64_t virt) {
    uint64_t phys = vmm_translate(virt);
    terminal_writestring("[VMM] Virt ");
    // naive hex
    char hex[17]; hex[16]='\0'; uint64_t v=virt; char hc[]="0123456789ABCDEF";
    for(int i=15;i>=0;i--){ hex[i]=hc[v & 0xF]; v >>=4; }
    terminal_writestring("0x"); terminal_writestring(hex);
    if (!phys) { terminal_writestring(" -> unmapped\n"); return; }
    v=phys; for(int i=15;i>=0;i--){ hex[i]=hc[v & 0xF]; v >>=4; }
    terminal_writestring(" -> phys 0x"); terminal_writestring(hex); terminal_writestring("\n");
}

// --- Region allocator stub ---
typedef struct vmm_region { uint64_t start; uint64_t size; uint64_t flags; } vmm_region_t;
static vmm_region_t regions[32];
static int region_count = 0;

int vmm_region_add(uint64_t start, uint64_t size, uint64_t flags) {
    if (region_count >= 32) return -1;
    regions[region_count++] = (vmm_region_t){start,size,flags};
    return 0;
}

const vmm_region_t* vmm_region_find(uint64_t addr) {
    for (int i=0;i<region_count;i++) {
        if (addr >= regions[i].start && addr < regions[i].start + regions[i].size) return &regions[i];
    }
    return NULL;
}

// Page Fault handler (chiamato da exception_handler per INT 14)
void vmm_handle_page_fault(uint64_t fault_addr, uint64_t error_code) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    terminal_writestring("[PAGEFAULT] indirizzo: ");
    char hex[17]; hex[16]='\0'; uint64_t v=fault_addr; char hc[]="0123456789ABCDEF"; for(int i=15;i>=0;i--){ hex[i]=hc[v & 0xF]; v >>=4; }
    terminal_writestring("0x"); terminal_writestring(hex);
    terminal_writestring(" EC="); v=error_code; for(int i=15;i>=0;i--){ hex[i]=hc[v & 0xF]; v >>=4; } terminal_writestring("0x"); terminal_writestring(hex);
    terminal_writestring(" ");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    // Decode error code bits
    // bit0 P, bit1 W/R, bit2 U/S, bit3 RSVD, bit4 I/D
    terminal_writestring("(" );
    if (error_code & 1) terminal_writestring("present "); else terminal_writestring("not-present ");
    if (error_code & 2) terminal_writestring("write "); else terminal_writestring("read ");
    if (error_code & 4) terminal_writestring("user "); else terminal_writestring("kernel ");
    if (error_code & 8) terminal_writestring("rsvd ");
    if (error_code & 16) terminal_writestring("instr ");
    terminal_writestring(")\n");

    const vmm_region_t* r = vmm_region_find(fault_addr);
    if (r && !(error_code & 1)) {
        // Demand-zero page se allineata a pagina
        uint64_t page = fault_addr & ~0xFFFULL;
        if (vmm_alloc_page(page, r->flags) == 0) {
            terminal_writestring("[PAGEFAULT] Demand alloc page -> OK\n");
            return; // handler continua esecuzione
        }
        terminal_writestring("[PAGEFAULT] Demand alloc fallita\n");
    }
    terminal_writestring("[PANIC] Page fault non gestito\n");
    while(1){ __asm__ volatile("hlt"); }
}
