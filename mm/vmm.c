#include "vmm.h"
#include "pmm.h"
#include "terminal.h"
#include "panic.h"
#include "heap.h" // kmalloc/kfree

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

// Helpers per sezione
extern uint8_t _text_start, _text_end;
extern uint8_t _rodata_start, _rodata_end;
extern uint8_t _data_start, _data_end;
extern uint8_t _bss_start, _bss_end;
extern uint8_t stack_bottom, stack_top; // da boot.asm

static inline uint64_t align_down(uint64_t v) { return v & ~0xFFFULL; }
static inline uint64_t align_up_4k(uint64_t v) { return (v + 0xFFFULL) & ~0xFFFULL; }

// Recupera PDT entry per indirizzo virtuale identita' (<16MB) e crea pagetable se huge
static uint64_t* ensure_pt_for_identity(uint64_t virt_base_2mb) {
    uint64_t pml4_phys = kernel_space.pml4_phys & ADDRESS_MASK;
    uint64_t* pml4 = (uint64_t*)pml4_phys;
    int pml4_i = (virt_base_2mb >> 39) & 0x1FF;
    uint64_t* pdpt = (uint64_t*)(pml4[pml4_i] & ADDRESS_MASK);
    if (!(pml4[pml4_i] & VMM_FLAG_PRESENT)) return NULL; // dovrebbe esistere
    int pdpt_i = (virt_base_2mb >> 30) & 0x1FF;
    uint64_t* pdt = (uint64_t*)(pdpt[pdpt_i] & ADDRESS_MASK);
    if (!(pdpt[pdpt_i] & VMM_FLAG_PRESENT)) return NULL;
    int pdt_i = (virt_base_2mb >> 21) & 0x1FF;
    uint64_t entry = pdt[pdt_i];
    if (entry & VMM_FLAG_PS) {
        void* frame = pmm_alloc_frame(); if (!frame) { terminal_writestring("[ERR] alloc PT fail\n"); return NULL; }
        zero_frame((uint64_t)frame);
        uint64_t phys_base = (entry & ADDRESS_MASK);
        uint64_t* pt = (uint64_t*)(((uint64_t)frame) & ADDRESS_MASK);
        for (int i=0;i<512;i++) {
            uint64_t phys = phys_base + (i * PAGE_SIZE);
            // Default permissivo: RW + executable; applicheremo restrizioni dopo
            pt[i] = phys | VMM_FLAG_PRESENT | VMM_FLAG_RW; // niente NX qui per evitare fault in fase conversione
        }
        pdt[pdt_i] = ((uint64_t)frame & ADDRESS_MASK) | VMM_FLAG_PRESENT | VMM_FLAG_RW; // clear PS
        return pt;
    }
    return (uint64_t*)(entry & ADDRESS_MASK);
}

static void set_page_flags(uint64_t virt, uint64_t flags_mask_clear, uint64_t flags_set) {
    // Assicurati che il 2MB chunk sia splittato
    ensure_pt_for_identity(virt & ~((2ULL*1024*1024)-1));
    uint64_t* pt = get_pt(virt, 0, 0);
    if (!pt) return;
    int pt_i = (virt >> 12) & 0x1FF;
    if (!(pt[pt_i] & VMM_FLAG_PRESENT)) return;
    uint64_t entry = pt[pt_i];
    entry &= ~flags_mask_clear;
    entry |= flags_set;
    pt[pt_i] = entry;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

void vmm_protect_kernel_sections(void) {
    terminal_writestring("[SEC] Protezione sezioni kernel (W^X)...\n");
    uint64_t text_start = (uint64_t)&_text_start;
    uint64_t text_end   = (uint64_t)&_text_end;
    uint64_t ro_start   = (uint64_t)&_rodata_start;
    uint64_t ro_end     = (uint64_t)&_rodata_end;
    uint64_t data_start = (uint64_t)&_data_start;
    uint64_t data_end   = (uint64_t)&_data_end;
    uint64_t bss_start  = (uint64_t)&_bss_start;
    uint64_t bss_end    = (uint64_t)&_bss_end;
    uint64_t stack_btm  = (uint64_t)&stack_bottom;
    uint64_t stack_tp   = (uint64_t)&stack_top;

    // Calcola range globale da proteggere (min start massimo top stack)
    uint64_t min_addr = text_start;
    if (ro_start < min_addr) min_addr = ro_start;
    if (data_start < min_addr) min_addr = data_start;
    if (bss_start < min_addr) min_addr = bss_start;
    if (stack_btm < min_addr) min_addr = stack_btm;
    uint64_t max_addr = stack_tp; // include stack

    uint64_t cur = min_addr & ~((2ULL*1024*1024)-1);
    uint64_t end = (max_addr + (2ULL*1024*1024 -1)) & ~((2ULL*1024*1024)-1);
    for (; cur < end; cur += (2ULL*1024*1024)) {
        ensure_pt_for_identity(cur);
    }

    // Text: RX (no RW, no NX)
    for (uint64_t v = align_down(text_start); v < align_up_4k(text_end); v += PAGE_SIZE) {
        set_page_flags(v, VMM_FLAG_RW | VMM_FLAG_NOEXEC, 0); // clear RW & NX
    }
    // Rodata: R, NX
    for (uint64_t v = align_down(ro_start); v < align_up_4k(ro_end); v += PAGE_SIZE) {
        set_page_flags(v, VMM_FLAG_RW, VMM_FLAG_NOEXEC); // remove RW, set NX
    }
    // Data + BSS: RW, NX
    for (uint64_t v = align_down(data_start); v < align_up_4k(data_end); v += PAGE_SIZE) {
        set_page_flags(v, VMM_FLAG_NOEXEC, VMM_FLAG_NOEXEC | VMM_FLAG_RW); // enforce NX & RW
    }
    for (uint64_t v = align_down(bss_start); v < align_up_4k(bss_end); v += PAGE_SIZE) {
        set_page_flags(v, VMM_FLAG_NOEXEC, VMM_FLAG_NOEXEC | VMM_FLAG_RW);
    }
    // Stack: RW, NX (guard page rinviata per stabilità)
    for (uint64_t v = align_down(stack_btm); v < align_up_4k(stack_tp); v += PAGE_SIZE) {
        set_page_flags(v, VMM_FLAG_NOEXEC, VMM_FLAG_NOEXEC | VMM_FLAG_RW);
    }

    terminal_writestring("[SEC] W^X applicato: text RX, rodata R/NX, data+bss RW/NX, stack RW/NX con guard page\n");
}

// ---- User space helpers ----
static int map_user_page(uint64_t virt, int rw, int exec) {
    if (virt & 0xFFF) return -1;
    void* frame = pmm_alloc_frame(); if (!frame) return -2;
    zero_frame((uint64_t)frame);
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (rw) flags |= VMM_FLAG_RW;
    if (!exec) flags |= VMM_FLAG_NOEXEC;
    int res = vmm_map(virt, (uint64_t)frame, flags);
    if (res != 0) return res;
    if (!rw) { // ensure RW cleared for code page
        uint64_t* pt = get_pt(virt, 0, 0);
        int pt_i = (virt >> 12) & 0x1FF;
        if (pt && (pt[pt_i] & VMM_FLAG_PRESENT)) pt[pt_i] &= ~VMM_FLAG_RW;
    }
    return 0;
}

int vmm_alloc_user_page(uint64_t virt) { return map_user_page(virt, 1, 0); }
int vmm_map_user_code(uint64_t virt) { return map_user_page(virt, 0, 1); }
int vmm_map_user_data(uint64_t virt) { return map_user_page(virt, 1, 0); }

uint64_t vmm_alloc_user_stack(int pages) {
    if (pages <= 0) pages = 4;
    uint64_t top = USER_STACK_TOP;
    for (int i=0;i<pages;i++) {
        uint64_t page_addr = top - (i+1)*PAGE_SIZE;
        if (vmm_alloc_user_page(page_addr) != 0) {
            terminal_writestring("[USER] alloc stack page fail\n");
            break;
        }
    }
    return top;
}

vmm_space_t* vmm_space_create_user(void) {
    void* pml4_new = pmm_alloc_frame(); if (!pml4_new) return NULL;
    zero_frame((uint64_t)pml4_new);
    uint64_t* old_pml4 = (uint64_t*)(kernel_space.pml4_phys & ADDRESS_MASK);
    uint64_t* new_pml4 = (uint64_t*)((uint64_t)pml4_new & ADDRESS_MASK);
    for (int i=0;i<PT_ENTRIES;i++) {
        uint64_t e = old_pml4[i];
        if (e & VMM_FLAG_PRESENT) new_pml4[i] = e & ~VMM_FLAG_USER; // share kernel mappings
    }
    vmm_space_t* space = (vmm_space_t*)kmalloc(sizeof(vmm_space_t));
    if (!space) return NULL;
    space->pml4_phys = (uint64_t)pml4_new & ADDRESS_MASK;
    terminal_writestring("[USER] new space CR3=");
    char hx[]="0123456789ABCDEF"; for(int i=60;i>=0;i-=4) terminal_putchar(hx[(space->pml4_phys>>i)&0xF]);
    terminal_writestring("\n");
    return space;
}

int vmm_space_destroy(vmm_space_t* space) {
    if (!space) return -1;
    kfree(space);
    return 0;
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
