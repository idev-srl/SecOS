/*
 * SecOS UEFI Bootloader Entry (Strategy B External Loader)
 * Responsible for initial environment setup: console greeting, GOP query, memory map probe
 * and delegating to ELF loader. Further phases will add page table setup and ExitBootServices.
 *
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "efi.h"
#include "bootinfo.h"
// Nota: page table setup minimale (solo identità + segmenti kernel) prima di ExitBootServices.

// Attiva le nostre tabelle di pagine: carica CR3, assicura PAE in CR4, NXE in EFER.
static void activate_page_tables(uint8_t* pml4) {
    if (!pml4) return;
    // CR4: abilita PAE (bit 5)
    unsigned long cr4;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1UL << 5); // PAE
    __asm__ __volatile__("mov %0, %%cr4" :: "r"(cr4));

    // Carica nuovo CR3 (PML4 phys address). Assumiamo identity mapping fisica per pml4.
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(pml4));

    // EFER: abilita NXE (bit 11) se non presente. LME è già attivo in ambiente UEFI.
    unsigned long eax, edx;
    __asm__ __volatile__("mov $0xC0000080, %%ecx; rdmsr" : "=a"(eax), "=d"(edx) : : "ecx");
    eax |= (1UL << 11); // NXE
    __asm__ __volatile__("mov $0xC0000080, %%ecx; wrmsr" :: "a"(eax), "d"(edx): "ecx");

    // CR0: assicura paging (bit 31) e protezione (bit 0) già attivi. Reinforza PG.
    unsigned long cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1UL << 31); // PG
    __asm__ __volatile__("mov %0, %%cr0" :: "r"(cr0));
}

// Forward: ELF kernel loader (implemented in elf_load.c)
extern EFI_STATUS elf_load_kernel(EFI_SYSTEM_TABLE* SystemTable, void** entry_out);

static void puts16(EFI_SYSTEM_TABLE* st, const CHAR16* s) {
    st->ConOut->OutputString(st->ConOut, s);
}

// Converts a 64-bit value to "0x" + 16 hex digits + null into out[19].
static void hex64_to_str(uint64_t v, CHAR16 out[19]) {
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int shift = (15 - i) * 4;
        uint8_t ny = (uint8_t)((v >> shift) & 0xF);
        out[2 + i] = (ny < 10) ? ('0' + ny) : ('A' + ny - 10);
    }
    out[18] = 0;
}

// Prints: "<where>: <0x...>\r\n"
static void print_status(EFI_SYSTEM_TABLE* st, const CHAR16* where, EFI_STATUS s) {
    CHAR16 buf[19];
    hex64_to_str(s, buf);
    puts16(st, where);
    puts16(st, (const CHAR16*)L": ");
    puts16(st, buf);
    puts16(st, (const CHAR16*)L"\r\n");
}

// Basic hex print helper (used for non-status values)
static void print_hex64(EFI_SYSTEM_TABLE* st, uint64_t v) {
    CHAR16 buf[19];
    hex64_to_str(v, buf);
    puts16(st, buf);
}

// Executes expr; if != EFI_SUCCESS prints "<where>: <status>" and returns.
// Requires `SystemTable` in scope.
#define CHECK_OK(where, expr) do { \
    EFI_STATUS __s = (expr); \
    if (__s != EFI_SUCCESS) { \
        print_status(SystemTable, WIDE(where), __s); \
        return __s; \
    } \
} while(0)

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    puts16(SystemTable, WIDE("[BOOT] entered efi_main\r\n"));

    // Locate GOP
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    CHECK_OK("Locate GOP", SystemTable->BootServices->LocateProtocol(&EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, NULL, (void**)&gop));
    if (gop && gop->Mode && gop->Mode->Info) {
        puts16(SystemTable, WIDE("GOP found: ")); print_hex64(SystemTable, gop->Mode->FrameBufferBase); puts16(SystemTable, WIDE("\r\n"));
    } else {
        puts16(SystemTable, WIDE("GOP not found\r\n"));
    }

    // Fase 1: Ottieni mappa di memoria completa
    uint64_t map_size = 0, map_key=0, desc_size=0; uint32_t desc_ver=0;
    {
        // First call: expected to return EFI_BUFFER_TOO_SMALL, filling map_size.
        EFI_STATUS __s = SystemTable->BootServices->GetMemoryMap(&map_size, NULL, &map_key, &desc_size, &desc_ver);
        if (__s != EFI_SUCCESS && map_size == 0) {
            print_status(SystemTable, WIDE("GetMemMap size"), __s);
            return __s;
        }
    }
    // Aggiungi margine per nuove allocazioni temporanee prima di ExitBootServices
    map_size += 4096 * 8;
    uint64_t alloc_buf_size = map_size;  /* salva dimensione allocata (map_size viene sovrascritto da GetMemoryMap) */
    EFI_MEMORY_DESCRIPTOR* mem_map = NULL;
    CHECK_OK("AllocPool memmap", SystemTable->BootServices->AllocatePool(EFI_LOADER_DATA, map_size, (void**)&mem_map));
    CHECK_OK("GetMemoryMap", SystemTable->BootServices->GetMemoryMap(&map_size, mem_map, &map_key, &desc_size, &desc_ver));
    puts16(SystemTable, WIDE("[OK] Mappa memoria acquisita\r\n"));

    // Carica kernel ELF
    void* kernel_entry = NULL;
    CHECK_OK("ELF load", elf_load_kernel(SystemTable, &kernel_entry));
    puts16(SystemTable, WIDE("[OK] Kernel ELF caricato, entry= "));
    print_hex64(SystemTable, (uint64_t)kernel_entry);
    puts16(SystemTable, WIDE("\r\n"));

    // Fase 2: costruzione tabelle di pagine (PML4, PDPT, PDT, PT) minimale
    // Strategia: identity map area bassa (<=512MB) + mappa segmenti kernel alle loro vaddr se rientrano.
    // Per semplicità: usiamo pagine da 2MB (PS) come nel percorso BIOS iniziale.
    uint64_t pml4_phys = 0, pdpt_phys = 0, pdt_phys = 0;
    uint8_t* pml4 = NULL; uint8_t* pdpt = NULL; uint8_t* pdt = NULL;
    CHECK_OK("AllocPages PML4", SystemTable->BootServices->AllocatePages(AllocateAnyPages, EFI_LOADER_DATA, 1, &pml4_phys));
    CHECK_OK("AllocPages PDPT", SystemTable->BootServices->AllocatePages(AllocateAnyPages, EFI_LOADER_DATA, 1, &pdpt_phys));
    CHECK_OK("AllocPages PDT",  SystemTable->BootServices->AllocatePages(AllocateAnyPages, EFI_LOADER_DATA, 1, &pdt_phys));
    // Se arriviamo qui, le allocazioni sono andate a buon fine, quindi possiamo procedere
    pml4 = (uint8_t*)pml4_phys; pdpt = (uint8_t*)pdpt_phys; pdt = (uint8_t*)pdt_phys;
    for(int i=0;i<4096;i++){ pml4[i]=0; pdpt[i]=0; pdt[i]=0; }
    // Link PML4->PDPT, PDPT->PDT
    ((uint64_t*)pml4)[0] = (uint64_t)pdpt | 0x3;
    ((uint64_t*)pdpt)[0] = (uint64_t)pdt | 0x3;
    // Identity map prime 512MB con entry 2MB
    for(int e=0;e<256;e++) {
        uint64_t phys = (uint64_t)e << 21; // e*2MB
        ((uint64_t*)pdt)[e] = phys | 0x83; // Present|Write|PS
    }

    // Fase 3: Rimappare segmenti ELF (placeholder: già copiati in pool; mapping reale post-ExitBootServices non ancora implementato).
    // Per implementazione completa servirebbe allocare memoria fisica alle vaddr e copiare i dati fuori da pool temporaneo.
    if (g_loaded_segment_count > 0) {
        puts16(SystemTable, WIDE("[INFO] Copia segmenti ELF nelle vaddr target (assunzione identity)\r\n"));
        for (uint16_t si = 0; si < g_loaded_segment_count; ++si) {
            secos_loaded_segment_t* seg = &g_loaded_segments[si];
            uint64_t vaddr = seg->vaddr;
            uint64_t filesz = seg->filesz;
            uint64_t memsz = seg->memsz;
            uint8_t* src = seg->data;
            uint8_t* dst = (uint8_t*)vaddr;
            // Verifica che rientri nei 512MB identity (semplice controllo)
            if (vaddr + memsz > (512ULL * 1024 * 1024)) {
                puts16(SystemTable, WIDE("[WARN] Segmento oltre area identity mappata, salto\r\n"));
                continue;
            }
            // Copia parte file
            for (uint64_t k = 0; k < filesz; ++k) dst[k] = src[k];
            // Zero padding BSS
            for (uint64_t k = filesz; k < memsz; ++k) dst[k] = 0;
            puts16(SystemTable, WIDE("[OK] Segmento copiato vaddr= "));
            print_hex64(SystemTable, vaddr);
            puts16(SystemTable, WIDE(" size= "));
            print_hex64(SystemTable, memsz);
            puts16(SystemTable, WIDE(" flags= "));
            print_hex64(SystemTable, seg->flags);
            puts16(SystemTable, WIDE("\r\n"));
        }
        puts16(SystemTable, WIDE("[INFO] Copia segmenti completata (W^X da applicare con pagine 4KB)\r\n"));
        // TODO: Convertire flags ELF in permessi pagine (rimappando con 4KB granularità): RW per data, RX per codice.
    }

    // Salva valori GOP prima di ExitBootServices (gop->Mode è boot-services memory, freed dopo EBS).
    uint64_t saved_fb_addr = 0, saved_fb_w = 0, saved_fb_h = 0, saved_fb_pitch = 0;
    if (gop && gop->Mode && gop->Mode->Info) {
        saved_fb_addr  = gop->Mode->FrameBufferBase;
        saved_fb_w     = gop->Mode->Info->HorizontalResolution;
        saved_fb_h     = gop->Mode->Info->VerticalResolution;
        saved_fb_pitch = gop->Mode->Info->PixelsPerScanLine * 4;
    }

    // Fase 4: ExitBootServices — retry loop (UEFI spec §7.4.6)
    // Any puts16 after GetMemoryMap allocates console buffers and stales the map_key.
    // Solution: print BEFORE the loop, then only GetMemoryMap + EBS inside it, no console.
    uint64_t final_map_size = alloc_buf_size; uint64_t final_map_key=0; uint64_t final_desc_size=0; uint32_t final_desc_ver=0;
    puts16(SystemTable, WIDE("[BOOT] Calling ExitBootServices\r\n"));
    EFI_STATUS ebs = EFI_INVALID_PARAMETER;
    for (int _att = 0; _att < 8 && ebs == EFI_INVALID_PARAMETER; _att++) {
        final_map_size = alloc_buf_size; // reset to full buffer capacity each attempt
        EFI_STATUS gm = SystemTable->BootServices->GetMemoryMap(
            &final_map_size, mem_map, &final_map_key, &final_desc_size, &final_desc_ver);
        if (gm != EFI_SUCCESS) { ebs = gm; break; }
        ebs = SystemTable->BootServices->ExitBootServices(ImageHandle, final_map_key);
        // EFI_INVALID_PARAMETER → map_key stale, loop retries; any other code exits loop
    }
    // Console still available iff ebs != EFI_SUCCESS (boot services not yet exited)
    if (ebs != EFI_SUCCESS) { print_status(SystemTable, WIDE("ExitBootServices"), ebs); return ebs; }
    /* === No UEFI Boot Services or Console calls past this point === */

    // Fase 5: Costruisci struttura handoff
    static struct secos_boot_info bootinfo;
    bootinfo.flags = 0;
    if (saved_fb_addr) {
        bootinfo.fb_addr   = saved_fb_addr;
        bootinfo.fb_width  = (uint32_t)saved_fb_w;
        bootinfo.fb_height = (uint32_t)saved_fb_h;
        bootinfo.fb_pitch  = (uint32_t)saved_fb_pitch;
        bootinfo.fb_bpp    = 32;
        bootinfo.flags    |= 1ULL; // bit0 GOP
    } else {
        bootinfo.fb_addr=bootinfo.fb_width=bootinfo.fb_height=bootinfo.fb_pitch=bootinfo.fb_bpp=0;
    }
    bootinfo.mem_descs        = mem_map;
    bootinfo.mem_desc_count   = final_map_size / desc_size;
    bootinfo.mem_desc_size    = desc_size;
    bootinfo.mem_desc_version = desc_ver;
    bootinfo.flags           |= (1ULL<<1); // bit1 memory map valida

    // Fase 6: Salta al kernel
    if (!kernel_entry) {
        /* No console available post-EBS — just halt */
        for(;;){ __asm__ __volatile__("hlt"); }
    }
    /* Attiva page tables custom prima del handoff.
     * A questo punto bootinfo è già costruita (gop->Mode letto sopra),
     * quindi non servono più accessi a strutture dati UEFI.
     * Il codice del bootloader e il bootinfo statico sono entrambi
     * sotto 512MB (memoria convenzionale), quindi l'identity map copre
     * istruzioni correnti, stack e il puntatore bootinfo. */
    activate_page_tables(pml4);
    // Firma attesa: void kernel_main(uint32_t multiboot_magic, uint64_t multiboot_info)
    void (*kentry)(uint32_t, uint64_t) = (void(*)(uint32_t, uint64_t))kernel_entry;
    kentry(0, (uint64_t)&bootinfo);
    // Se mai ritorna, fermiamo
    for(;;){ __asm__ __volatile__("hlt"); }
    return EFI_SUCCESS; // non raggiunto
}

// Rimosso stub load_kernel: si usa elf_load_kernel esterno
