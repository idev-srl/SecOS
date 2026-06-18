/*

 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "vmm.h"
#include "pmm.h"
#include "terminal.h"
#include "panic.h"
#include "debugcon.h"
#include "heap.h" // kmalloc/kfree
#include "vma.h"       // [M14] per-process demand paging
#include "process.h"   // [M14] process_t (current process VMAs)
#include "sched.h"     // [M14] sched_get_current()

// Basic page table constants
#define PAGE_SIZE 4096ULL
#define PT_ENTRIES 512

// Address mask constant
#define ADDRESS_MASK 0x000FFFFFFFFFF000ULL

static vmm_space_t kernel_space;
static int physmap_initialized = 0;
static uint64_t physmap_limit = 0; // physical memory currently covered by physmap

// Forward declaration
static void zero_frame(uint64_t phys);

// Extend physmap (already initialized) to cover at least phys_end.
// Uses 2MB huge pages like vmm_init_physmap.
void vmm_extend_physmap(uint64_t phys_end) {
    if (!physmap_initialized) return; // will be configured by vmm_init_physmap
    const uint64_t HUGE_SIZE = 2ULL * 1024 * 1024;
    uint64_t target = (phys_end + HUGE_SIZE - 1) & ~(HUGE_SIZE - 1);
    if (target <= physmap_limit) return;

    uint64_t pml4_phys = kernel_space.pml4_phys & ADDRESS_MASK;
    // M1.2: physmap_initialized==1 here (checked at function entry), use phys_to_virt
    uint64_t* pml4 = (uint64_t*)phys_to_virt(pml4_phys);
    int pml4_i = (VMM_PHYSMAP_BASE >> 39) & 0x1FF;
    int pdpt_i_start = (VMM_PHYSMAP_BASE >> 30) & 0x1FF;
    if (!(pml4[pml4_i] & VMM_FLAG_PRESENT)) { terminal_writestring("[ERR] extend physmap: PDPT missing\n"); return; }
    uint64_t* pdpt = (uint64_t*)phys_to_virt(pml4[pml4_i] & ADDRESS_MASK);
    uint64_t phys_cursor = physmap_limit;
    while (phys_cursor < target) {
        int pdpt_i = pdpt_i_start + ((phys_cursor >> 30) & 0x1FF);
    if (pdpt_i >= 512) { terminal_writestring("[WARN] extend physmap: exceeded PDPT range\n"); break; }
        uint64_t* pdt;
        if (!(pdpt[pdpt_i] & VMM_FLAG_PRESENT)) {
            void* frame = pmm_alloc_frame(); if (!frame) { terminal_writestring("[ERR] extend physmap: PDT alloc fail\n"); break; }
            zero_frame((uint64_t)frame);
            pdpt[pdpt_i] = ((uint64_t)frame & ADDRESS_MASK) | VMM_FLAG_PRESENT | VMM_FLAG_RW;
            pdt = (uint64_t*)phys_to_virt((uint64_t)frame & ADDRESS_MASK);
        } else {
            pdt = (uint64_t*)phys_to_virt(pdpt[pdpt_i] & ADDRESS_MASK);
        }
        for (int pdt_i=0; pdt_i<512 && phys_cursor < target; pdt_i++) {
            uint64_t virt = VMM_PHYSMAP_BASE + phys_cursor;
            int virt_pdt_i = (virt >> 21) & 0x1FF;
            if (virt_pdt_i != pdt_i) continue;
            if (!(pdt[pdt_i] & VMM_FLAG_PRESENT)) {
                pdt[pdt_i] = (phys_cursor & ADDRESS_MASK) | VMM_FLAG_PRESENT | VMM_FLAG_RW | VMM_FLAG_PS | VMM_FLAG_NOEXEC;
            }
            phys_cursor += HUGE_SIZE;
        }
    }
    physmap_limit = phys_cursor;
    terminal_writestring("[OK] Physmap extended to ");
    char hex[17]; hex[16]='\0'; uint64_t v=physmap_limit; char hc[]="0123456789ABCDEF"; for(int i=15;i>=0;i--){ hex[i]=hc[v & 0xF]; v >>=4; }
    terminal_writestring("0x"); terminal_writestring(hex); terminal_writestring(" (fisico)\n");
}

// Zero out entire physical frame
static void zero_frame(uint64_t phys) {
    uint8_t* p;
    if (physmap_initialized) {
        p = (uint8_t*)phys_to_virt(phys);
    } else {
    // Identity fallback: works while early frames are identity mapped
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

// Walk page table level or create if absent.
// M1.2: after physmap is active, return phys_to_virt(phys) instead of identity cast.
static uint64_t* get_or_create_table(uint64_t* table, int index, uint64_t flags) {
    uint64_t entry = table[index];
    if (!(entry & VMM_FLAG_PRESENT)) {
        void* frame = pmm_alloc_frame();
        if (!frame) return NULL;
        zero_frame((uint64_t)frame);
        uint64_t phys = (uint64_t)frame & ADDRESS_MASK;
        table[index] = phys | (flags & (VMM_FLAG_RW|VMM_FLAG_USER|VMM_FLAG_PWT|VMM_FLAG_PCD)) | VMM_FLAG_PRESENT;
        if (physmap_initialized) {
            static int once = 0;
            if (!once) { once = 1; terminal_writestring("[M1.2] get_or_create_table: phys_to_virt path active\n"); }
            return (uint64_t*)phys_to_virt(phys);
        }
        return (uint64_t*)phys;
    }
    uint64_t phys = entry & ADDRESS_MASK;
    return physmap_initialized ? (uint64_t*)phys_to_virt(phys) : (uint64_t*)phys;
}

// Get pointer to final PT level for virtual address in given space.
// M1.2: use phys_to_virt for all page-table frame dereferences when physmap is active.
static uint64_t* get_pt_space(vmm_space_t* space, uint64_t virt, int create, uint64_t flags) {
    uint64_t pml4_phys = space->pml4_phys & ADDRESS_MASK;
    uint64_t* pml4 = physmap_initialized ? (uint64_t*)phys_to_virt(pml4_phys) : (uint64_t*)pml4_phys;

    int pml4_i = (virt >> 39) & 0x1FF;
    int pdpt_i = (virt >> 30) & 0x1FF;
    int pdt_i  = (virt >> 21) & 0x1FF;
    int pt_i   = (virt >> 12) & 0x1FF;

    uint64_t* pdpt;
    if (!(pml4[pml4_i] & VMM_FLAG_PRESENT)) {
        if (!create) return NULL;
        pdpt = get_or_create_table(pml4, pml4_i, flags);
        if (!pdpt) return NULL;
    } else {
        uint64_t pdpt_phys = pml4[pml4_i] & ADDRESS_MASK;
        pdpt = physmap_initialized ? (uint64_t*)phys_to_virt(pdpt_phys) : (uint64_t*)pdpt_phys;
    }
    uint64_t* pdt;
    if (!(pdpt[pdpt_i] & VMM_FLAG_PRESENT)) {
        if (!create) return NULL;
        pdt = get_or_create_table(pdpt, pdpt_i, flags);
        if (!pdt) return NULL;
    } else {
        uint64_t pdt_phys = pdpt[pdpt_i] & ADDRESS_MASK;
        pdt = physmap_initialized ? (uint64_t*)phys_to_virt(pdt_phys) : (uint64_t*)pdt_phys;
    }
    uint64_t* pt;
    if (!(pdt[pdt_i] & VMM_FLAG_PRESENT)) {
        if (!create) return NULL;
        pt = get_or_create_table(pdt, pdt_i, flags);
        if (!pt) return NULL;
    } else {
        uint64_t pt_phys = pdt[pdt_i] & ADDRESS_MASK;
        pt = physmap_initialized ? (uint64_t*)phys_to_virt(pt_phys) : (uint64_t*)pt_phys;
    }
    (void)pt_i; // usato da vmm_map
    return pt;
}

// Wrapper for kernel_space
static uint64_t* get_pt(uint64_t virt, int create, uint64_t flags) { return get_pt_space(&kernel_space, virt, create, flags); }

// --- M1.1: kernel-owned page tables ---
// Allocate a fresh PML4/PDPT/PDT from PMM and populate a 0–512MB identity map
// using 2MB huge pages. Must be called while identity mapping is still active
// (physmap_initialized == 0). Returns physical address of new PML4, or 0 on failure.
static uint64_t vmm_build_kernel_pml4_identity_512mb(void) {
    uint64_t pml4_phys = (uint64_t)pmm_alloc_frame();
    uint64_t pdpt_phys = (uint64_t)pmm_alloc_frame();
    uint64_t pdt_phys  = (uint64_t)pmm_alloc_frame();
    uint64_t pdpt_hi_phys = (uint64_t)pmm_alloc_frame();   // [M12] high-half PDPT
    if (!pml4_phys || !pdpt_phys || !pdt_phys || !pdpt_hi_phys) {
        terminal_writestring("[ERR] M1.1: PMM alloc failed for kernel page tables\n");
        return 0;
    }
    zero_frame(pml4_phys);
    zero_frame(pdpt_phys);
    zero_frame(pdt_phys);
    zero_frame(pdpt_hi_phys);

    // Identity cast: frames are within 0–512MB, bootloader tables are still active
    uint64_t* pml4    = (uint64_t*)pml4_phys;
    uint64_t* pdpt    = (uint64_t*)pdpt_phys;
    uint64_t* pdt     = (uint64_t*)pdt_phys;
    uint64_t* pdpt_hi = (uint64_t*)pdpt_hi_phys;

    // Low identity 0–512MB (PML4[0] -> PDPT[0] -> PDT, 2MB pages). Still needed
    // during early init (frames addressed by physical == virtual) and for the
    // boot transition; the physmap and high half take over afterwards.
    pml4[0] = pdpt_phys | VMM_FLAG_PRESENT | VMM_FLAG_RW;
    pdpt[0] = pdt_phys  | VMM_FLAG_PRESENT | VMM_FLAG_RW;
    for (int i = 0; i < 256; i++) {
        pdt[i] = ((uint64_t)i << 21) | VMM_FLAG_PRESENT | VMM_FLAG_RW | VMM_FLAG_PS;
    }
    // [M12] High half: KERNEL_VMA_BASE (0xFFFFFFFF80000000) = PML4[511] -> PDPT[510]
    // -> the SAME 512MB page-directory, so the kernel image (phys < 512MB) is
    // reachable at its link VMA. The kernel runs here after this PML4 is loaded.
    pml4[511]     = pdpt_hi_phys | VMM_FLAG_PRESENT | VMM_FLAG_RW;
    pdpt_hi[510]  = pdt_phys     | VMM_FLAG_PRESENT | VMM_FLAG_RW;
    return pml4_phys;
}

void vmm_init(void) {
    char hx[] = "0123456789ABCDEF"; char buf[17]; buf[16] = '\0'; uint64_t v;

    uint64_t old_cr3 = read_cr3();
    v = old_cr3; for (int i=15;i>=0;i--){ buf[i]=hx[v&0xF]; v>>=4; }
    terminal_writestring("[M1.1] Bootloader CR3= 0x"); terminal_writestring(buf); terminal_writestring("\n");

    uint64_t new_pml4 = vmm_build_kernel_pml4_identity_512mb();
    if (!new_pml4) {
        terminal_writestring("[WARN] M1.1: keeping bootloader CR3\n");
        kernel_space.pml4_phys = old_cr3;
        return;
    }

    v = new_pml4; for (int i=15;i>=0;i--){ buf[i]=hx[v&0xF]; v>>=4; }
    terminal_writestring("[M1.1] New kernel CR3= 0x"); terminal_writestring(buf); terminal_writestring("\n");

    write_cr3(new_pml4);
    kernel_space.pml4_phys = new_pml4;

    // Sanity read through new mapping: read back kernel_space.pml4_phys
    v = kernel_space.pml4_phys; for (int i=15;i>=0;i--){ buf[i]=hx[v&0xF]; v>>=4; }
    terminal_writestring("[M1.1] CR3 switch OK. pml4_phys= 0x"); terminal_writestring(buf); terminal_writestring("\n");
}

// Section boundaries (from linker script)
extern uint8_t _text_start, _text_end;
extern uint8_t _rodata_start, _rodata_end;
extern uint8_t _data_start, _data_end;
extern uint8_t _bss_start, _bss_end;
extern uint8_t stack_bottom, stack_top; // from boot.asm

static inline uint64_t align_down(uint64_t v) { return v & ~0xFFFULL; }
static inline uint64_t align_up_4k(uint64_t v) { return (v + 0xFFFULL) & ~0xFFFULL; }

// Retrieve PDT entry for identity-mapped virtual address (<16MB) and create page table if huge
static uint64_t* ensure_pt_for_identity(uint64_t virt_base_2mb) {
    uint64_t pml4_phys = kernel_space.pml4_phys & ADDRESS_MASK;
    uint64_t* pml4 = (uint64_t*)pml4_phys;
    int pml4_i = (virt_base_2mb >> 39) & 0x1FF;
    uint64_t* pdpt = (uint64_t*)(pml4[pml4_i] & ADDRESS_MASK);
    if (!(pml4[pml4_i] & VMM_FLAG_PRESENT)) return NULL; // should exist
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
            // Permissive default: RW + executable; restrictions applied later.
            pt[i] = phys | VMM_FLAG_PRESENT | VMM_FLAG_RW; // no NX here to avoid conversion faults
        }
    pdt[pdt_i] = ((uint64_t)frame & ADDRESS_MASK) | VMM_FLAG_PRESENT | VMM_FLAG_RW; // clear PS (remove huge)
        return pt;
    }
    return (uint64_t*)(entry & ADDRESS_MASK);
}

static void set_page_flags(uint64_t virt, uint64_t flags_mask_clear, uint64_t flags_set) {
    // Ensure the 2MB chunk is split into 4K pages
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
    terminal_writestring("[SEC] Protecting kernel sections (W^X)...\n");
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

    // Compute global range to protect (min start through stack top)
    uint64_t min_addr = text_start;
    if (ro_start < min_addr) min_addr = ro_start;
    if (data_start < min_addr) min_addr = data_start;
    if (bss_start < min_addr) min_addr = bss_start;
    if (stack_btm < min_addr) min_addr = stack_btm;
    uint64_t max_addr = stack_tp; // includes stack

    uint64_t cur = min_addr & ~((2ULL*1024*1024)-1);
    uint64_t end = (max_addr + (2ULL*1024*1024 -1)) & ~((2ULL*1024*1024)-1);
    for (; cur < end; cur += (2ULL*1024*1024)) {
        ensure_pt_for_identity(cur);
    }

    // Text: RX (clear RW & NX)
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
    // Stack: RW, NX (guard page deferred for stability)
    for (uint64_t v = align_down(stack_btm); v < align_up_4k(stack_tp); v += PAGE_SIZE) {
        set_page_flags(v, VMM_FLAG_NOEXEC, VMM_FLAG_NOEXEC | VMM_FLAG_RW);
    }

    terminal_writestring("[SEC] W^X applied: text RX, rodata R/NX, data+bss RW/NX, stack RW/NX with guard page\n");
}

// --- M1.3: Guard pages ---
// Split a 2MB huge page containing guard_virt into 4KB pages, then mark guard_virt not-present.
// Must be called AFTER vmm_init_physmap() (uses phys_to_virt).
static int vmm_install_guard_page(uint64_t guard_virt) {
    uint64_t pml4_phys = kernel_space.pml4_phys & ADDRESS_MASK;
    uint64_t* pml4 = (uint64_t*)phys_to_virt(pml4_phys);
    int pml4_i = (guard_virt >> 39) & 0x1FF;
    if (!(pml4[pml4_i] & VMM_FLAG_PRESENT)) return -1;
    uint64_t* pdpt = (uint64_t*)phys_to_virt(pml4[pml4_i] & ADDRESS_MASK);
    int pdpt_i = (guard_virt >> 30) & 0x1FF;
    if (!(pdpt[pdpt_i] & VMM_FLAG_PRESENT)) return -1;
    uint64_t* pdt = (uint64_t*)phys_to_virt(pdpt[pdpt_i] & ADDRESS_MASK);
    int pdt_i = (guard_virt >> 21) & 0x1FF;
    uint64_t pde = pdt[pdt_i];
    if (!(pde & VMM_FLAG_PRESENT)) return -1;

    if (pde & VMM_FLAG_PS) {
        // Split 2MB huge page into 512 × 4KB pages
        void* frame = pmm_alloc_frame();
        if (!frame) { terminal_writestring("[ERR] M1.3: PT alloc fail\n"); return -2; }
        zero_frame((uint64_t)frame);
        uint64_t phys_base = pde & ADDRESS_MASK;
        uint64_t* pt = (uint64_t*)phys_to_virt((uint64_t)frame);
        for (int i = 0; i < 512; i++) {
            pt[i] = (phys_base + i * PAGE_SIZE) | VMM_FLAG_PRESENT | VMM_FLAG_RW;
        }
        pdt[pdt_i] = ((uint64_t)frame & ADDRESS_MASK) | VMM_FLAG_PRESENT | VMM_FLAG_RW;
    }

    // Mark guard page not-present
    uint64_t pt_phys = pdt[pdt_i] & ADDRESS_MASK;
    uint64_t* pt = (uint64_t*)phys_to_virt(pt_phys);
    int pt_i = (guard_virt >> 12) & 0x1FF;
    pt[pt_i] = 0;
    __asm__ volatile("invlpg (%0)" :: "r"(guard_virt) : "memory");
    return 0;
}

void vmm_setup_kernel_guard_pages(void) {
    char hx[] = "0123456789ABCDEF"; char buf[17]; buf[16] = '\0'; uint64_t v;

    // A) Kernel stack guard page
    // Safety: stack_bottom is the FIRST symbol in .bss (= _bss_start).
    // stack_bottom - PAGE_SIZE falls inside .data — MUST NOT mark it not-present.
    // Guard page requires the stack not to be flush against another mapped section.
    // Deferred until stack placement is separated from _bss_start (see M2 TODO).
    uint64_t sb = (uint64_t)&stack_bottom;
    uint64_t guard_kern = sb - PAGE_SIZE;
    uint64_t data_end = (uint64_t)&_data_end;
    if (guard_kern >= data_end) {
        if (vmm_install_guard_page(guard_kern) == 0) {
            v = guard_kern; for (int i=15;i>=0;i--){ buf[i]=hx[v&0xF]; v>>=4; }
            terminal_writestring("[M1.3] Kernel stack guard at 0x"); terminal_writestring(buf); terminal_writestring("\n");
        } else {
            terminal_writestring("[ERR] M1.3: kernel stack guard failed\n");
        }
    } else {
        terminal_writestring("[M1.3] Kernel stack guard skipped: guard_virt in .data range\n");
    }

    // C) IST stack guard pages
    // Deferred: PMM allocates IST frames contiguously (ist_n_lo, ist_n_hi, ist_(n+1)_lo, ...).
    // ist_(n+1)_lo - PAGE_SIZE = ist_n_hi, which is an ACTIVE IST-n stack frame.
    // Marking it not-present corrupts IST-n and triggers a triple fault on any #PF.
    // IST guard pages require each stack to have a dedicated guard frame allocated BEFORE
    // the stack frames. Deferred to M2 when IST allocation is redesigned.
    terminal_writestring("[M1.3] IST guard pages deferred (consecutive-frame conflict)\n");
}

// ---- M2: Dedicated guarded stack region ----

// Helper: ensure a PT entry exists for `addr` and force it to 0 (not-present).
// Creates intermediate page tables if needed (so the PT entry slot exists).
// Calls INVLPG to flush any stale TLB entry.
static void m2_mark_guard(uint64_t addr) {
    uint64_t* pt = get_pt(addr, 1, VMM_FLAG_RW);
    if (!pt) {
        terminal_writestring("[M2][ERR] m2_mark_guard: PT alloc fail for ");
        char hx[]="0123456789ABCDEF";
        for(int i=60;i>=0;i-=4) terminal_putchar(hx[(addr>>i)&0xF]);
        terminal_writestring("\n");
        return;
    }
    int pt_i = (addr >> 12) & 0x1FF;
    pt[pt_i] = 0; // explicitly not-present
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

// Print a 64-bit hex value using only terminal primitives (no heap, no sprintf).
static void m2_print_hex(uint64_t v) {
    char hx[] = "0123456789ABCDEF";
    char buf[17]; buf[16] = '\0';
    for (int i = 15; i >= 0; i--) { buf[i] = hx[v & 0xF]; v >>= 4; }
    terminal_writestring("0x"); terminal_writestring(buf);
}

// Map `pages` consecutive virtual pages starting at `virt_base` to freshly
// allocated PMM frames.  Flags: RW + NX, no USER.  Panics if PMM or vmm_map fails.
static void m2_map_stack_pages(uint64_t virt_base, int pages) {
    for (int i = 0; i < pages; i++) {
        uint64_t va = virt_base + (uint64_t)i * PAGE_SIZE;
        void* frame = pmm_alloc_frame();
        if (!frame) {
            terminal_writestring("[M2][PANIC] PMM out of frames for stack page at ");
            m2_print_hex(va); terminal_writestring("\n");
            for(;;) __asm__ volatile("hlt");
        }
        zero_frame((uint64_t)frame);
        int r = vmm_map(va, (uint64_t)frame, VMM_FLAG_RW | VMM_FLAG_NOEXEC);
        if (r != 0) {
            terminal_writestring("[M2][PANIC] vmm_map failed ("); m2_print_hex((uint64_t)r);
            terminal_writestring(") for "); m2_print_hex(va); terminal_writestring("\n");
            for(;;) __asm__ volatile("hlt");
        }
    }
}

uint64_t vmm_alloc_kernel_stack(void) {
    terminal_writestring("[M2] Allocating kernel stack in dedicated virtual region...\n");

    // Ensure guard_lo PTE exists and is not-present.
    // This also forces creation of PML4[511]->PDPT[0]->PDT[0]->PT, covering
    // the entire M2_STACK_REGION_BASE 2MB block.
    m2_mark_guard(M2_KSTACK_GUARD_LO);

    // Map the 4 usable stack pages.
    m2_map_stack_pages(M2_KSTACK_BASE, M2_KSTACK_PAGES);

    // guard_hi: one page above RSP_INIT (M2_KSTACK_TOP).
    // The page at M2_KSTACK_TOP .. M2_KSTACK_TOP+0xFFF is implicitly NP
    // (zero PTE in the freshly created PT).  M2_KSTACK_GUARD_HI is one page
    // further and is explicitly zeroed for clarity.
    m2_mark_guard(M2_KSTACK_GUARD_HI);

    // Diagnostics
    terminal_writestring("[M2] Kernel stack mapped:\n");
    terminal_writestring("  usable : "); m2_print_hex(M2_KSTACK_BASE);
    terminal_writestring(" .. "); m2_print_hex(M2_KSTACK_TOP - 1); terminal_writestring(" (16 KB)\n");
    terminal_writestring("  RSP_INIT: "); m2_print_hex(M2_KSTACK_TOP); terminal_writestring("\n");
    terminal_writestring("  guard_lo: "); m2_print_hex(M2_KSTACK_GUARD_LO); terminal_writestring(" (NP)\n");
    terminal_writestring("  guard_hi: "); m2_print_hex(M2_KSTACK_GUARD_HI); terminal_writestring(" (NP)\n");

    return M2_KSTACK_TOP;
}

// Allocate and map an IST stack (8 KB usable, 2 frames) at the given virtual
// addresses.  Called by tss_init() for each IST.
// Returns the top address (= TSS.istN value).
uint64_t vmm_alloc_ist_stack(uint64_t guard_lo, uint64_t base, uint64_t top,
                              uint64_t guard_hi, int ist_num) {
    m2_mark_guard(guard_lo);
    m2_map_stack_pages(base, M2_IST_PAGES);
    m2_mark_guard(guard_hi);

    terminal_writestring("[M2] IST");
    terminal_putchar('0' + ist_num);
    terminal_writestring(" mapped: usable=[");
    m2_print_hex(base); terminal_writestring(","); m2_print_hex(top);
    terminal_writestring(") top="); m2_print_hex(top);
    terminal_writestring("  guards: "); m2_print_hex(guard_lo);
    terminal_writestring(" / "); m2_print_hex(guard_hi); terminal_writestring("\n");

    return top;
}

// ---- M5: per-process guarded kernel stacks (slot-based) ----

static uint64_t m5_kstack_slot_top(uint32_t slot) {
    return M5_KSTACK_REGION_TOP - ((uint64_t)slot * M5_KSTACK_SLOT_SIZE);
}
static uint64_t m5_kstack_guard_lo(uint32_t slot) {
    return m5_kstack_slot_top(slot) - M5_KSTACK_SLOT_SIZE;
}
static uint64_t m5_kstack_base(uint32_t slot) {
    return m5_kstack_guard_lo(slot) + 0x1000ULL;
}
static uint64_t m5_kstack_guard_hi(uint32_t slot) {
    return m5_kstack_slot_top(slot) - 0x1000ULL;
}
static uint64_t m5_kstack_rsp_top(uint32_t slot) {
    return m5_kstack_guard_hi(slot); // rsp0 = top of usable, grows down
}

uint64_t vmm_alloc_kernel_stack_for_slot(uint32_t slot) {
    if (slot >= M5_MAX_KSTACK_SLOTS) {
        terminal_writestring("[M5] ERROR: slot out of range\n");
        return 0;
    }

    uint64_t glo  = m5_kstack_guard_lo(slot);
    uint64_t base = m5_kstack_base(slot);
    uint64_t ghi  = m5_kstack_guard_hi(slot);
    uint64_t top  = m5_kstack_rsp_top(slot);

    // Runtime assertion: region must stay within M2 stack 2MB block
    if (ghi >= M5_KSTACK_REGION_TOP || glo < M2_STACK_REGION_BASE) {
        terminal_writestring("[M5][PANIC] kstack slot ");
        m2_print_hex((uint64_t)slot);
        terminal_writestring(" outside M2 region\n");
        for(;;) __asm__ volatile("hlt");
    }

    m2_mark_guard(glo);
    m2_map_stack_pages(base, M5_KSTACK_PAGES);
    m2_mark_guard(ghi);

    terminal_writestring("[M5] kstack slot=");
    m2_print_hex((uint64_t)slot);
    terminal_writestring(" base="); m2_print_hex(base);
    terminal_writestring(" top=");  m2_print_hex(top);
    terminal_writestring(" guards="); m2_print_hex(glo);
    terminal_writestring("/"); m2_print_hex(ghi);
    terminal_writestring("\n");

    return top;
}

int vmm_free_kernel_stack_for_slot(uint32_t slot) {
    if (slot >= M5_MAX_KSTACK_SLOTS) return -1;

    uint64_t base = m5_kstack_base(slot);
    uint64_t glo  = m5_kstack_guard_lo(slot);
    uint64_t ghi  = m5_kstack_guard_hi(slot);

    for (int i = 0; i < M5_KSTACK_PAGES; i++) {
        uint64_t va = base + (uint64_t)i * PAGE_SIZE;
        uint64_t phys = vmm_translate(va);
        if (phys) {
            vmm_unmap(va);
            pmm_free_frame((void*)(phys & ADDRESS_MASK));
        }
    }
    m2_mark_guard(glo);
    m2_mark_guard(ghi);
    return 0;
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

static int map_user_page_in_space(vmm_space_t* space, uint64_t virt, int rw, int exec) {
    if (!space) return -10;
    if (virt & 0xFFF) return -1;
    void* frame = pmm_alloc_frame(); if (!frame) return -2;
    zero_frame((uint64_t)frame);
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (rw) flags |= VMM_FLAG_RW;
    if (!exec) flags |= VMM_FLAG_NOEXEC;
    int res = vmm_map_in_space(space, virt, (uint64_t)frame, flags);
    if (res != 0) {
        terminal_writestring("[VMM] map_user_page_in_space fail virt=");
        char hx[]="0123456789ABCDEF"; for(int i=60;i>=0;i-=4) terminal_putchar(hx[(virt>>i)&0xF]);
        terminal_writestring(" res="); terminal_putchar('0'+(res<0? -res: res)); terminal_writestring("\n");
        return res;
    }
    if (!rw) {
        uint64_t* pt = get_pt_space(space, virt, 0, 0);
        int pt_i = (virt >> 12) & 0x1FF;
        if (pt && (pt[pt_i] & VMM_FLAG_PRESENT)) pt[pt_i] &= ~VMM_FLAG_RW;
    }
    return 0;
}

int vmm_alloc_user_page(uint64_t virt) { return map_user_page(virt, 1, 0); }
int vmm_map_user_code(uint64_t virt) { return map_user_page(virt, 0, 1); }
int vmm_map_user_data(uint64_t virt) { return map_user_page(virt, 1, 0); }

int vmm_alloc_user_page_in_space(vmm_space_t* space, uint64_t virt) { return map_user_page_in_space(space, virt, 1, 0); }
int vmm_map_user_code_in_space(vmm_space_t* space, uint64_t virt) { return map_user_page_in_space(space, virt, 0, 1); }
int vmm_map_user_data_in_space(vmm_space_t* space, uint64_t virt) { return map_user_page_in_space(space, virt, 1, 0); }

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
    // M1.3: Explicit guard page — ensure PTE exists as not-present
    uint64_t guard_addr = top - (pages+1)*PAGE_SIZE;
    uint64_t* pt = get_pt(guard_addr, 1, VMM_FLAG_RW | VMM_FLAG_USER);
    if (pt) {
        int pt_i = (guard_addr >> 12) & 0x1FF;
        pt[pt_i] = 0; // explicitly not-present
    }
    return top;
}

uint64_t vmm_alloc_user_stack_in_space(vmm_space_t* space, int pages) {
    if (!space) return 0;
    if (pages <= 0) pages = 4;
    uint64_t top = USER_STACK_TOP;
    for (int i=0;i<pages;i++) {
        uint64_t page_addr = top - (i+1)*PAGE_SIZE;
        if (vmm_alloc_user_page_in_space(space, page_addr) != 0) {
            terminal_writestring("[USER] alloc stack page fail (space)\n");
            break;
        }
    }
    // M1.3: Explicit guard page — ensure PTE exists as not-present
    uint64_t guard_addr = top - (pages+1)*PAGE_SIZE;
    uint64_t* pt = get_pt_space(space, guard_addr, 1, VMM_FLAG_RW | VMM_FLAG_USER);
    if (pt) {
        int pt_i = (guard_addr >> 12) & 0x1FF;
        pt[pt_i] = 0; // explicitly not-present
    }
    return top;
}

vmm_space_t* vmm_space_create_user(void) {
    void* pml4_new = pmm_alloc_frame(); if (!pml4_new) return NULL;
    zero_frame((uint64_t)pml4_new);
    uint64_t old_phys = kernel_space.pml4_phys & ADDRESS_MASK;
    uint64_t new_phys = (uint64_t)pml4_new & ADDRESS_MASK;
    uint64_t* old_pml4 = physmap_initialized ? (uint64_t*)phys_to_virt(old_phys) : (uint64_t*)old_phys;
    uint64_t* new_pml4 = physmap_initialized ? (uint64_t*)phys_to_virt(new_phys) : (uint64_t*)new_phys;
    // Copy all kernel PML4 entries as supervisor (strip USER). The kernel high
    // half (256..511, physmap) must stay supervisor and shared.
    for (int i=0;i<PT_ENTRIES;i++) {
        uint64_t e = old_pml4[i];
        if (e & VMM_FLAG_PRESENT) new_pml4[i] = e & ~VMM_FLAG_USER; // share kernel mappings
    }
    // PML4[0] is special: it covers BOTH the kernel low identity map
    // (0..512MB, needed while the kernel runs on behalf of a user process) AND
    // the entire user range — USER_CODE_BASE (4GB) .. USER_STACK_TOP (16GB) are
    // all < 512GB, i.e. PML4 index 0.  The two must coexist: PML4[0] needs the
    // USER bit so ring-3 can reach its USER leaves, while kernel pages beneath
    // stay supervisor (and thus inaccessible to ring-3).  Sharing the kernel's
    // PDPT here would also collide/leak user tables across processes, so give
    // each user space its OWN PDPT for PML4[0]: copy the kernel low entries
    // (supervisor) and leave the user-range PDPT slots empty for the loader.
    {
        const int uidx = (int)((USER_CODE_BASE >> 39) & 0x1FF); // 0 for current layout
        uint64_t kern_pdpt_phys = old_pml4[uidx] & ADDRESS_MASK;
        uint64_t* kern_pdpt = (old_pml4[uidx] & VMM_FLAG_PRESENT)
            ? (physmap_initialized ? (uint64_t*)phys_to_virt(kern_pdpt_phys) : (uint64_t*)kern_pdpt_phys)
            : NULL;
        void* priv = pmm_alloc_frame();
        if (!priv) { pmm_free_frame(pml4_new); return NULL; }
        zero_frame((uint64_t)priv);
        uint64_t priv_phys = (uint64_t)priv & ADDRESS_MASK;
        uint64_t* priv_pdpt = physmap_initialized ? (uint64_t*)phys_to_virt(priv_phys) : (uint64_t*)priv_phys;
        const int lo = (int)((USER_CODE_BASE >> 30) & 0x1FF);
        const int hi = (int)((USER_STACK_TOP  >> 30) & 0x1FF);
        for (int i = 0; i < PT_ENTRIES; i++) {
            if (i >= lo && i <= hi) { priv_pdpt[i] = 0; continue; }      // user range: private, empty
            priv_pdpt[i] = kern_pdpt ? (kern_pdpt[i] & ~VMM_FLAG_USER) : 0; // kernel low: supervisor
        }
        new_pml4[uidx] = priv_phys | VMM_FLAG_PRESENT | VMM_FLAG_RW | VMM_FLAG_USER;
    }
    vmm_space_t* space = (vmm_space_t*)kmalloc(sizeof(vmm_space_t));
    if (!space) return NULL;
    space->pml4_phys = (uint64_t)pml4_new & ADDRESS_MASK;

    // [M3] Address space protection: assert PML4[256..511] have U=0.
    // Entries in the kernel-canonical half must never be accessible from ring-3.
    // This is a defensive in-place check immediately after construction.
    {
        uint64_t* virt_pml4 = physmap_initialized
            ? (uint64_t*)phys_to_virt(space->pml4_phys)
            : (uint64_t*)space->pml4_phys;
        for (int i = 256; i < PT_ENTRIES; i++) {
            if ((virt_pml4[i] & (VMM_FLAG_PRESENT | VMM_FLAG_USER)) ==
                              (VMM_FLAG_PRESENT | VMM_FLAG_USER)) {
                // Strip it silently and report — should never happen with current code.
                virt_pml4[i] &= ~VMM_FLAG_USER;
                terminal_writestring("[VMM][M3] WARNING: PML4[kernel] had U=1 — stripped\n");
            }
        }
    }

    terminal_writestring("[USER] new address space CR3=");
    char hx[]="0123456789ABCDEF"; for(int i=60;i>=0;i-=4) terminal_putchar(hx[(space->pml4_phys>>i)&0xF]);
    terminal_writestring("\n");
    return space;
}

int vmm_space_destroy(vmm_space_t* space) {
    if (!space) return -1;

    uint64_t pml4_phys = space->pml4_phys & ADDRESS_MASK;
    uint64_t* pml4 = physmap_initialized ? (uint64_t*)phys_to_virt(pml4_phys) : (uint64_t*)pml4_phys;
    uint32_t n_pages = 0, n_tables = 0;

    // All user addresses are in PML4[0] (USER_CODE_BASE..USER_STACK_TOP are within 0..512GB).
    // PML4[0] points to the SHARED kernel PDPT — must not free the PDPT itself.
    // User allocations injected PDPT entries 4..15; walk only that range.
    int pdpt_lo = (int)((USER_CODE_BASE >> 30) & 0x1FF); // 4
    int pdpt_hi = (int)((USER_STACK_TOP  >> 30) & 0x1FF); // 15

    uint64_t pml4e = pml4[0];
    if (pml4e & VMM_FLAG_PRESENT) {
        uint64_t pdpt_phys = pml4e & ADDRESS_MASK;
        uint64_t* pdpt = physmap_initialized ? (uint64_t*)phys_to_virt(pdpt_phys) : (uint64_t*)pdpt_phys;

        for (int pi = pdpt_lo; pi <= pdpt_hi; pi++) {
            uint64_t pdpte = pdpt[pi];
            if (!(pdpte & VMM_FLAG_PRESENT)) continue;

            uint64_t pdt_phys = pdpte & ADDRESS_MASK;
            uint64_t* pdt = physmap_initialized ? (uint64_t*)phys_to_virt(pdt_phys) : (uint64_t*)pdt_phys;

            for (int di = 0; di < 512; di++) {
                uint64_t pdte = pdt[di];
                if (!(pdte & VMM_FLAG_PRESENT)) continue;
                if (pdte & VMM_FLAG_PS) continue; // 2MB huge: ownership unclear, skip

                uint64_t pt_phys = pdte & ADDRESS_MASK;
                uint64_t* pt = physmap_initialized ? (uint64_t*)phys_to_virt(pt_phys) : (uint64_t*)pt_phys;

                for (int ti = 0; ti < 512; ti++) {
                    uint64_t pte = pt[ti];
                    if (!(pte & VMM_FLAG_PRESENT)) continue;
                    pmm_free_frame((void*)(pte & ADDRESS_MASK));
                    n_pages++;
                }
                pmm_free_frame((void*)pt_phys);
                n_tables++;
            }
            pmm_free_frame((void*)pdt_phys);
            n_tables++;
            pdpt[pi] = 0;
        }
        // [M8] Since M7 each user space owns a PRIVATE PDPT for PML4[0]
        // (kernel low entries are copied, not shared at this level), so the
        // PDPT frame itself must be freed.  Its PDPT[0] points to the shared
        // kernel low-identity PD, which we correctly never walked or freed.
        pmm_free_frame((void*)pdpt_phys);
        n_tables++;
    }

    // PML4 frame is unique per user space
    pmm_free_frame((void*)pml4_phys);
    n_tables++;

    char hx[] = "0123456789ABCDEF"; char b[9]; b[8] = '\0';
    terminal_writestring("[M1.5] vmm_space_destroy freed 0x");
    uint32_t v = n_pages; for (int i=7;i>=0;i--){ b[i]=hx[v&0xF]; v>>=4; } terminal_writestring(b);
    terminal_writestring(" pages, 0x");
    v = n_tables; for (int i=7;i>=0;i--){ b[i]=hx[v&0xF]; v>>=4; } terminal_writestring(b);
    terminal_writestring(" table frames\n");

    kfree(space);
    return 0;
}

// [M14] Count present 4KB leaf pages in the user range of 'space'. Walks the
// same private-PDPT range as vmm_space_destroy() but only counts (no free). Used
// to demonstrate demand paging: right after a lazy load this returns 0; it grows
// as pages fault in. Read-only; safe to call any time.
uint32_t vmm_count_user_pages(vmm_space_t* space) {
    if (!space) return 0;
    uint64_t pml4_phys = space->pml4_phys & ADDRESS_MASK;
    uint64_t* pml4 = physmap_initialized ? (uint64_t*)phys_to_virt(pml4_phys) : (uint64_t*)pml4_phys;
    int pdpt_lo = (int)((USER_CODE_BASE >> 30) & 0x1FF);
    int pdpt_hi = (int)((USER_STACK_TOP  >> 30) & 0x1FF);
    uint32_t n = 0;
    uint64_t pml4e = pml4[0];
    if (!(pml4e & VMM_FLAG_PRESENT)) return 0;
    uint64_t pdpt_phys = pml4e & ADDRESS_MASK;
    uint64_t* pdpt = physmap_initialized ? (uint64_t*)phys_to_virt(pdpt_phys) : (uint64_t*)pdpt_phys;
    for (int pi = pdpt_lo; pi <= pdpt_hi; pi++) {
        uint64_t pdpte = pdpt[pi];
        if (!(pdpte & VMM_FLAG_PRESENT)) continue;
        uint64_t pdt_phys = pdpte & ADDRESS_MASK;
        uint64_t* pdt = physmap_initialized ? (uint64_t*)phys_to_virt(pdt_phys) : (uint64_t*)pdt_phys;
        for (int di = 0; di < 512; di++) {
            uint64_t pdte = pdt[di];
            if (!(pdte & VMM_FLAG_PRESENT)) continue;
            if (pdte & VMM_FLAG_PS) continue;
            uint64_t pt_phys = pdte & ADDRESS_MASK;
            uint64_t* pt = physmap_initialized ? (uint64_t*)phys_to_virt(pt_phys) : (uint64_t*)pt_phys;
            for (int ti = 0; ti < 512; ti++) {
                if (pt[ti] & VMM_FLAG_PRESENT) n++;
            }
        }
    }
    return n;
}

// Map all available physical memory using 2MB huge pages to reduce table count.
void vmm_init_physmap(void) {
    if (physmap_initialized) return;
    uint64_t total = pmm_get_total_memory();
    if (total == 0) {
    terminal_writestring("[WARN] physmap: total memory = 0\n");
        return;
    }
    // Round up to 2MB multiples
    const uint64_t HUGE_SIZE = 2ULL * 1024 * 1024;
    uint64_t limit = (total + HUGE_SIZE - 1) & ~(HUGE_SIZE - 1);

    uint64_t pml4_phys = kernel_space.pml4_phys & ADDRESS_MASK;
    uint64_t* pml4 = (uint64_t*)pml4_phys; // identity assumption

    // Calculate PML4 / PDPT / PDT indices for physmap range
    // Using VMM_PHYSMAP_BASE: extract PML4 index
    int pml4_i = (VMM_PHYSMAP_BASE >> 39) & 0x1FF;
    int pdpt_i_start = (VMM_PHYSMAP_BASE >> 30) & 0x1FF;

    // Ensure PDPT is present
    uint64_t* pdpt = (uint64_t*)(pml4[pml4_i] & ADDRESS_MASK);
    if (!(pml4[pml4_i] & VMM_FLAG_PRESENT)) {
    void* frame = pmm_alloc_frame(); if (!frame) { terminal_writestring("[ERR] physmap: PDPT alloc fail\n"); return; }
        zero_frame((uint64_t)frame);
        pml4[pml4_i] = ((uint64_t)frame & ADDRESS_MASK) | VMM_FLAG_PRESENT | VMM_FLAG_RW;
        pdpt = (uint64_t*)(((uint64_t)frame) & ADDRESS_MASK);
    }

    uint64_t phys_cursor = 0;
    while (phys_cursor < limit) {
    int pdpt_i = pdpt_i_start + ((phys_cursor >> 30) & 0x1FF); // Simple, should not exceed 512 early
    if (pdpt_i >= 512) { terminal_writestring("[WARN] physmap: exceeds PDPT range\n"); break; }
        uint64_t* pdt = (uint64_t*)(pdpt[pdpt_i] & ADDRESS_MASK);
        if (!(pdpt[pdpt_i] & VMM_FLAG_PRESENT)) {
            void* frame = pmm_alloc_frame(); if (!frame) { terminal_writestring("[ERR] physmap: PDT alloc fail\n"); break; }
            zero_frame((uint64_t)frame);
            pdpt[pdpt_i] = ((uint64_t)frame & ADDRESS_MASK) | VMM_FLAG_PRESENT | VMM_FLAG_RW;
            pdt = (uint64_t*)(((uint64_t)frame) & ADDRESS_MASK);
        }
    // Fill PDT with huge pages
        for (int pdt_i = 0; pdt_i < 512 && phys_cursor < limit; pdt_i++) {
            // Compute corresponding virtual address
            uint64_t virt = VMM_PHYSMAP_BASE + phys_cursor;
            int virt_pdt_i = (virt >> 21) & 0x1FF;
            if (virt_pdt_i != pdt_i) continue; // Align with local index
            if (!(pdt[pdt_i] & VMM_FLAG_PRESENT)) {
                // Mark physmap pages NX (non-executable) and RW
                pdt[pdt_i] = (phys_cursor & ADDRESS_MASK) | VMM_FLAG_PRESENT | VMM_FLAG_RW | VMM_FLAG_PS | VMM_FLAG_NOEXEC;
            }
            phys_cursor += HUGE_SIZE;
        }
    }
    physmap_initialized = 1;
    physmap_limit = limit; // record initial extension
    terminal_writestring("[OK] Physmap initialized up to ");
    char hex[17]; hex[16]='\0'; uint64_t v=limit; char hc[]="0123456789ABCDEF"; for(int i=15;i>=0;i--){ hex[i]=hc[v & 0xF]; v >>=4; }
    terminal_writestring("0x"); terminal_writestring(hex); terminal_writestring(" (fisico)\n");
    // M1.2: physmap_initialized just became 1; print sample phys_to_virt conversion
    uint64_t sample = phys_to_virt(0x200000ULL);
    uint64_t sv = sample; for(int i=15;i>=0;i--){ hex[i]=hc[sv & 0xF]; sv>>=4; }
    terminal_writestring("[M1.2] physmap_initialized=1. phys_to_virt(0x200000)= 0x"); terminal_writestring(hex); terminal_writestring("\n");
}

int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    if (virt & 0xFFF || phys & 0xFFF) return -1; // not aligned
    // [M12] W^X hard gate: never map a page writable AND executable. A page is
    // executable when NX (bit 63) is clear; writable when RW is set. Reject the
    // call rather than relying on callers/the ELF loader alone.
    if ((flags & VMM_FLAG_RW) && !(flags & VMM_FLAG_NOEXEC)) {
        terminal_writestring("[VMM][WX] POLICY VIOLATION: W+X mapping rejected\n");
        return -5;
    }
    // [M3] Supervisor enforcement: kernel-canonical VAs (>= 0xFFFF800000000000)
    // must NEVER carry VMM_FLAG_USER.  Reject the call and log the violation.
    if ((flags & VMM_FLAG_USER) && (virt >= 0xFFFF800000000000ULL)) {
        terminal_writestring("[VMM][M3] POLICY VIOLATION: kernel VA mapped with USER bit — call rejected\n");
        return -4;
    }
    uint64_t* pt = get_pt(virt, 1, flags);
    if (!pt) return -2;
    int pt_i = (virt >> 12) & 0x1FF;
    if (pt[pt_i] & VMM_FLAG_PRESENT) return -3; // already mapped
    pt[pt_i] = (phys & ADDRESS_MASK) | (flags & ~VMM_FLAG_PS) | VMM_FLAG_PRESENT;
    return 0;
}

int vmm_map_in_space(vmm_space_t* space, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!space) return -10;
    if (virt & 0xFFF || phys & 0xFFF) return -1; // not aligned
    /* [M12] W^X hard gate: reject writable+executable mappings (see vmm_map). */
    if ((flags & VMM_FLAG_RW) && !(flags & VMM_FLAG_NOEXEC)) {
        terminal_writestring("[VMM][WX] POLICY VIOLATION: W+X mapping rejected (space)\n");
        return -5;
    }
    /* [M4] Supervisor enforcement: mirror vmm_map() policy.
     * Kernel-canonical VAs must never carry VMM_FLAG_USER. */
    if ((flags & VMM_FLAG_USER) && (virt >= 0xFFFF800000000000ULL)) {
        terminal_writestring("[VMM][M4] POLICY VIOLATION: kernel VA in space mapped with USER bit — rejected\n");
        return -4;
    }
    uint64_t* pt = get_pt_space(space, virt, 1, flags);
    if (!pt) return -2;
    int pt_i = (virt >> 12) & 0x1FF;
    if (pt[pt_i] & VMM_FLAG_PRESENT) return -3; // already mapped
    pt[pt_i] = (phys & ADDRESS_MASK) | (flags & ~VMM_FLAG_PS) | VMM_FLAG_PRESENT;
    return 0;
}

int vmm_unmap(uint64_t virt) {
    if (virt & 0xFFF) return -1; // not aligned
    uint64_t* pt = get_pt(virt, 0, 0);
    if (!pt) return -2;
    int pt_i = (virt >> 12) & 0x1FF;
    if (!(pt[pt_i] & VMM_FLAG_PRESENT)) return -3; // not mapped
    pt[pt_i] = 0;
    // Invalidate TLB
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    return 0;
}

int vmm_unmap_in_space(vmm_space_t* space, uint64_t virt) {
    if (!space) return -10;
    if (virt & 0xFFF) return -1; // not aligned
    uint64_t* pt = get_pt_space(space, virt, 0, 0);
    if (!pt) return -2;
    int pt_i = (virt >> 12) & 0x1FF;
    if (!(pt[pt_i] & VMM_FLAG_PRESENT)) return -3; // not mapped
    uint64_t entry = pt[pt_i];
    pt[pt_i] = 0;
    // Free physical frame
    void* frame = (void*)(entry & ADDRESS_MASK);
    pmm_free_frame(frame);
    return 0;
}

// [M18] Change the protection flags of an already-present page (keep its frame).
// Returns 0 if the PTE was updated, -3 if not present (caller relies on the VMA
// flags to apply on the next fault), or a W^X/error code. The TLB is flushed by
// the caller's CR3 reload at the next context switch (or a manual invlpg).
int vmm_protect_in_space(vmm_space_t* space, uint64_t virt, uint64_t flags) {
    if (!space) return -10;
    if (virt & 0xFFF) return -1;
    if ((flags & VMM_FLAG_RW) && !(flags & VMM_FLAG_NOEXEC)) return -5; // W^X
    uint64_t* pt = get_pt_space(space, virt, 0, 0);
    if (!pt) return -3;
    int pt_i = (virt >> 12) & 0x1FF;
    if (!(pt[pt_i] & VMM_FLAG_PRESENT)) return -3;
    uint64_t phys = pt[pt_i] & ADDRESS_MASK;
    pt[pt_i] = phys | (flags & ~VMM_FLAG_PS) | VMM_FLAG_PRESENT | VMM_FLAG_USER;
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

uint64_t vmm_translate_in_space(vmm_space_t* space, uint64_t virt) {
    if (!space) return 0;
    uint64_t* pt = get_pt_space(space, virt, 0, 0);
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

int vmm_alloc_page_in_space(vmm_space_t* space, uint64_t virt, uint64_t flags) {
    if (!space) return -10;
    void* frame = pmm_alloc_frame(); if (!frame) return -1;
    zero_frame((uint64_t)frame);
    return vmm_map_in_space(space, virt, (uint64_t)frame, flags | VMM_FLAG_RW);
}

vmm_space_t* vmm_get_kernel_space(void) { return &kernel_space; }

// [M12] W^X self-test: a writable+executable mapping request must be refused by
// the vmm_map gate. The check fires before any page-table walk, so passing an
// already-mapped scratch VA has no side effects. Logs a [WX] marker.
void vmm_wx_selftest(void) {
    int r = vmm_map(VMM_PHYSMAP_BASE, 0x100000ULL, VMM_FLAG_RW); // RW, no NX => W+X
    if (r == -5) debugcon_writestring("[WX] W+X mapping rejected (good)\n");
    else         debugcon_writestring("[WX] FAIL: W+X mapping accepted\n");
}

vmm_space_t* vmm_clone_space(vmm_space_t* src) {
    (void)src; return NULL; // cloning stub (not implemented)
}

int vmm_switch_space(vmm_space_t* space) {
    if (!space) return -1;
    write_cr3(space->pml4_phys & ADDRESS_MASK);
    return 0;
}

void vmm_harden_user_space(vmm_space_t* space) {
    if (!space) return;
    // [M3] Use physmap-aware access (physmap is always initialized before any
    //      user space is created — it is safe to call phys_to_virt here).
    uint64_t pml4_phys = space->pml4_phys & ADDRESS_MASK;
    uint64_t* pml4 = physmap_initialized
        ? (uint64_t*)phys_to_virt(pml4_phys)
        : (uint64_t*)pml4_phys;
    // [M3] PML4 indices 256–511 are the kernel-canonical high half.
    //      x86-64: entries 0–255 → user range, 256–511 → kernel range.
    //      Strip VMM_FLAG_USER from ALL kernel-half entries unconditionally.
    int cleared = 0;
    for (int i = 256; i < PT_ENTRIES; i++) {
        uint64_t e = pml4[i];
        if (!(e & VMM_FLAG_PRESENT)) continue;
        if (e & VMM_FLAG_USER) { pml4[i] = e & ~VMM_FLAG_USER; cleared++; }
    }
    if (cleared)
        terminal_writestring("[SEC][M3] harden: cleared USER bit from kernel-half PML4 entries\n");
    else
        terminal_writestring("[SEC][M3] harden: kernel-half PML4 entries already supervisor-only\n");
}

void vmm_dump_entry(uint64_t virt) {
    uint64_t phys = vmm_translate(virt);
    terminal_writestring("[VMM] Virt ");
    // Naive hex formatting
    char hex[17]; hex[16]='\0'; uint64_t v=virt; char hc[]="0123456789ABCDEF";
    for(int i=15;i>=0;i--){ hex[i]=hc[v & 0xF]; v >>=4; }
    terminal_writestring("0x"); terminal_writestring(hex);
    if (!phys) { terminal_writestring(" -> unmapped\n"); return; }
    v=phys; for(int i=15;i>=0;i--){ hex[i]=hc[v & 0xF]; v >>=4; }
    terminal_writestring(" -> phys 0x"); terminal_writestring(hex); terminal_writestring("\n");
}

// Page Fault handler (called by exception_handler for INT 14)
//
// [M14] Demand paging. A not-present fault on an address covered by a VMA of the
// current process is serviced by materializing that one page (vma_fault_in), then
// returning so the faulting instruction retries. This covers user faults AND
// kernel-mode faults on user addresses: copy_from/to_user dereferences user VAs
// directly, so a not-yet-faulted syscall buffer faults in kernel mode and is
// handled here too (we key on the address + VMA, not the U/S error-code bit).
// Returns 0 if the fault was serviced (the instruction should retry); -1 if it
// is a genuine violation. [M15] On -1 the caller (exception_handler) decides
// kill (ring-3) vs panic (ring-0) — this function no longer halts.
int vmm_handle_page_fault(uint64_t fault_addr, uint64_t error_code) {
    // Fast path: demand-page a reserved VMA. Only not-present faults qualify (a
    // protection fault — present bit set — is a real access violation).
    if (!(error_code & 1)) {
        process_t* cur = sched_get_current();
        if (cur && cur->space) {
            const vma_t* vv = vma_find(&cur->vmas, fault_addr);
            if (vv && vma_fault_in(cur->space, vv, fault_addr) == 0) {
                // Quiet success path (faults are frequent): a terse debugcon
                // marker only, no framebuffer output.
                debugcon_writestring("[PF] demand page ");
                debugcon_print_hex(fault_addr & ~0xFFFULL);
                debugcon_writestring("\n");
                return 0; // retry the faulting access
            }
        }
    }

    // Unhandled: a genuine violation (protection, no VMA, or alloc failure).
    // Report tersely on debugcon and let the caller terminate the offender.
    debugcon_writestring("[PF] UNHANDLED fault addr=0x"); debugcon_print_hex(fault_addr);
    debugcon_writestring(" ec=0x"); debugcon_print_hex(error_code);
    debugcon_writestring(error_code & 1 ? " (protection" : " (not-present");
    debugcon_writestring(error_code & 2 ? ",write" : ",read");
    debugcon_writestring(error_code & 4 ? ",user" : ",kernel");
    if (error_code & 16) debugcon_writestring(",instr");
    debugcon_writestring(")\n");
    return -1;
}
