#include "elf.h"
#include "vmm.h"
#include "terminal.h"
#include "panic.h"
#include "pmm.h"
#include "heap.h"
/*

 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
// Simple ELF64 loader (PT_LOAD segments only). Assumptions:
// - File entirely in memory (buffer)
// - space->pml4_phys already created and CR3 NOT switched (we map through functions that use kernel_space for now)
// - p_vaddr user segments fall within planned USER_CODE_BASE / USER_DATA_BASE / USER_STACK_TOP range
// - We don't handle relocation, requires static PIE or predefined addresses
// Limitations: Doesn't verify complex overlaps, no dynamic (PT_INTERP), no TLS.

static int check_magic(const Elf64_Ehdr* eh) {
    uint32_t m = ((uint32_t)eh->e_ident[0] << 24) | ((uint32_t)eh->e_ident[1] << 16) | ((uint32_t)eh->e_ident[2] << 8) | ((uint32_t)eh->e_ident[3]);
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') return ELF_ERR_MAGIC;
    return 0;
}

int elf_load_image(const void* buffer, size_t size, vmm_space_t* space, uint64_t* entry_out, uint64_t** pages_out, uint32_t* page_count_out) {
    if (!buffer || size < sizeof(Elf64_Ehdr)) return ELF_ERR_FMT;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)buffer;
    if (check_magic(eh) != 0) { terminal_writestring("[ELF] Magic err\n"); return ELF_ERR_MAGIC; }
    if (eh->e_phoff == 0 || eh->e_phnum == 0) { terminal_writestring("[ELF] No PH\n"); return ELF_ERR_FMT; }
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) { terminal_writestring("[ELF] PH size mismatch\n"); return ELF_ERR_FMT; }
    if (eh->e_entry == 0) { terminal_writestring("[ELF] Entry 0\n"); }

    // Entry point
    if (entry_out) *entry_out = eh->e_entry;

    // Stato memoria fisica prima del mapping
    extern uint64_t pmm_get_free_memory(void);
    uint64_t free_before = pmm_get_free_memory();
    terminal_writestring("[ELF] free mem prima mapping=");
    char hx_fb[]="0123456789ABCDEF"; for(int b=60;b>=0;b-=4) terminal_putchar(hx_fb[(free_before>>b)&0xF]); terminal_writestring("\n");
    // Itera program headers
    const uint8_t* base = (const uint8_t*)buffer;
    // Pass 1: conteggio pagine totali PT_LOAD
    uint32_t total_pages = 0;
    if (pages_out && page_count_out) {
        for (int i=0;i<eh->e_phnum;i++) {
            const Elf64_Phdr* ph = (const Elf64_Phdr*)(base + eh->e_phoff + i * sizeof(Elf64_Phdr));
            if (ph->p_type != PT_LOAD) continue;
            uint64_t memsz = ph->p_memsz; if (memsz < ph->p_filesz) memsz = ph->p_filesz;
            if (memsz == 0) continue;
            uint64_t start = ph->p_vaddr & ~0xFFFULL;
            uint64_t end = (ph->p_vaddr + memsz + 0xFFFULL) & ~0xFFFULL;
            total_pages += (uint32_t)((end - start) >> 12);
        }
    }
    uint64_t* pages_arr = NULL;
    uint32_t pages_idx = 0;
    if (pages_out && page_count_out && total_pages) {
        pages_arr = (uint64_t*)kmalloc(sizeof(uint64_t)*total_pages);
        if (!pages_arr) { terminal_writestring("[ELF] alloc pages_arr fail\n"); total_pages = 0; }
    }
    for (int i=0;i<eh->e_phnum;i++) {
        const Elf64_Phdr* ph = (const Elf64_Phdr*)(base + eh->e_phoff + i * sizeof(Elf64_Phdr));
        if ((const uint8_t*)ph + sizeof(Elf64_Phdr) > base + size) return ELF_ERR_RANGE;
        if (ph->p_type != PT_LOAD) continue;
        // Validazioni flags: proibire W|X contemporanei
        if ((ph->p_flags & PF_X) && (ph->p_flags & PF_W)) { terminal_writestring("[ELF] segment W|X rifiutato\n"); return ELF_ERR_FLAG; }
        // Range file
        if (ph->p_offset + ph->p_filesz > size) { terminal_writestring("[ELF] segment range oltre file\n"); return ELF_ERR_RANGE; }
        // Range virtuale consentito: code segment (exec) deve stare >= USER_CODE_BASE e < USER_DATA_BASE
        // data segment (non exec) deve stare >= USER_DATA_BASE e < USER_STACK_TOP - qualche margine
        if (ph->p_flags & PF_X) {
            if (ph->p_vaddr < USER_CODE_BASE || ph->p_vaddr >= USER_DATA_BASE) { terminal_writestring("[ELF] code fuori range\n"); return ELF_ERR_RANGE; }
        } else {
            if (ph->p_vaddr < USER_DATA_BASE || ph->p_vaddr >= USER_STACK_TOP - 0x100000) { terminal_writestring("[ELF] data fuori range\n"); return ELF_ERR_RANGE; }
        }
        // Determina mapping
        uint64_t vaddr = ph->p_vaddr; // assumiamo già nell'intervallo user
        uint64_t memsz = ph->p_memsz;
        uint64_t filesz = ph->p_filesz;
        if (memsz < filesz) memsz = filesz; // sanità
    // Check p_align (richiediamo 0x1000 o 0)
    if (ph->p_align != 0 && ph->p_align != 0x1000ULL) { terminal_writestring("[ELF] p_align non supportato\n"); return ELF_ERR_FMT; }
        if (memsz == 0) continue;
        // Allineamento pagine
        uint64_t start = vaddr & ~0xFFFULL;
        uint64_t end = (vaddr + memsz + 0xFFFULL) & ~0xFFFULL;
        int exec = (ph->p_flags & PF_X) ? 1 : 0;
        int rw   = (ph->p_flags & PF_W) ? 1 : 0;
        // Mappa pagine
        for (uint64_t va = start; va < end; va += 0x1000ULL) {
            int r = 0;
            if (exec && !rw) r = vmm_map_user_code_in_space(space, va);
            else r = vmm_alloc_user_page_in_space(space, va); // RW NX
            if (r != 0) { terminal_writestring("[ELF] map fallita (r)"); char hx2[]="0123456789ABCDEF"; for(int b=4;b>=0;b-=4) terminal_putchar(hx2[(r>>b)&0xF]); terminal_writestring(" virt="); for(int b=60;b>=0;b-=4) terminal_putchar(hx2[(va>>b)&0xF]); terminal_writestring("\n"); return ELF_ERR_MAP; }
            // Logging dettagli pagina
            terminal_writestring("[ELF] page ");
            char hx[]="0123456789ABCDEF"; for(int b=60;b>=0;b-=4) terminal_putchar(hx[(va>>b)&0xF]);
            terminal_writestring(" -> "); terminal_putchar(exec? 'X':'-'); terminal_putchar(rw? 'W':'R'); terminal_writestring("\n");
            if (pages_arr && pages_idx < total_pages) pages_arr[pages_idx++] = va;
        }
        // Copia contenuto file nelle pagine (solo filesz)
        const uint8_t* src = base + ph->p_offset;
        for (uint64_t off = 0; off < filesz; off++) {
            uint64_t va = vaddr + off;
            uint64_t phys = vmm_translate_in_space(space, va);
            if (!phys) { terminal_writestring("[ELF] translate fail space\n"); return ELF_ERR_MAP; }
            uint8_t* dst = (uint8_t*)phys_to_virt(phys);
            dst[va & 0xFFFULL] = src[off];
        }
        // Zero tail se memsz > filesz
        for (uint64_t off = filesz; off < memsz; off++) {
            uint64_t va = vaddr + off;
            uint64_t phys = vmm_translate_in_space(space, va);
            if (!phys) continue;
            uint8_t* dst = (uint8_t*)phys_to_virt(phys);
            dst[va & 0xFFFULL] = 0;
        }
        terminal_writestring("[ELF] Segmento caricato: vaddr=");
        char hx[]="0123456789ABCDEF"; for(int b=60;b>=0;b-=4) terminal_putchar(hx[(vaddr>>b)&0xF]);
        terminal_writestring(" size=");
        for(int b=60;b>=0;b-=4) terminal_putchar(hx[(memsz>>b)&0xF]);
        terminal_writestring(" flags=");
        terminal_putchar(exec?'X':'-'); terminal_putchar(rw?'W':'R'); terminal_writestring("\n");
    }
    terminal_writestring("[ELF] Caricamento completato\n");
    if (pages_out && page_count_out) {
        *pages_out = pages_arr;
        *page_count_out = pages_idx;
    }
    return ELF_OK;
}
