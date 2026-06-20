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

// [UEFI-FIX] Install the kernel's high-half mapping WITHOUT discarding the
// firmware's identity map.
//
// The old code built a fresh PML4 that identity-mapped only 0-512 MB and loaded
// it into CR3. That assumes the loader image, its stack, the bootinfo struct, the
// UEFI memory map and the framebuffer all live below 512 MB. OVMF and VMware do
// place everything low, so they worked. Real firmware (e.g. the ASUS E406S) may
// place any of those ABOVE 512 MB, so the very next instruction fetch / data
// access after the CR3 load was unmapped → triple fault, before the kernel IDT
// exists (no [EXC], the machine just resets — exactly the reported symptom).
//
// Fix: COPY the live firmware PML4 (so we inherit its *complete* identity map,
// wherever RAM/MMIO is) and add only entry 511 → our high-half PDPT. Entry 511 is
// the top canonical slot the kernel lives in and firmware does not use it.
static void install_kernel_mapping(uint8_t* pml4_copy, uint8_t* pdpt_hi) {
    if (!pml4_copy || !pdpt_hi) return;
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    const uint64_t* cur = (const uint64_t*)(cr3 & 0x000FFFFFFFFFF000ULL); // firmware PML4 (identity-accessible)
    uint64_t* dst = (uint64_t*)pml4_copy;
    for (int i = 0; i < 512; i++) dst[i] = cur[i];            // inherit full firmware identity
    dst[511] = (uint64_t)pdpt_hi | 0x3;                       // + kernel high half (Present|Write)

    // EFER.NXE: the kernel sets the NX bit on data/stack pages. LME/PAE are
    // already on under UEFI long mode; only NXE may be missing.
    unsigned long eax, edx;
    __asm__ __volatile__("mov $0xC0000080, %%ecx; rdmsr" : "=a"(eax), "=d"(edx) : : "ecx");
    eax |= (1UL << 11); // NXE
    __asm__ __volatile__("mov $0xC0000080, %%ecx; wrmsr" :: "a"(eax), "d"(edx): "ecx");

    __asm__ __volatile__("mov %0, %%cr3" :: "r"((uint64_t)pml4_copy) : "memory");
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

// [UEFI-FIX] Blind-debug progress marker: paint a full-width colored stripe
// straight into the framebuffer. There is no serial on the target laptop, so on a
// reset the last visible color tells us how far the handoff got. Safe before AND
// after ExitBootServices because the final CR3 inherits the firmware identity map
// (the framebuffer is mapped wherever it physically lives). No-op without GOP.
static void fb_bar(uint64_t fb, uint32_t pitch_px, uint32_t width, uint32_t row, uint32_t color) {
    if (!fb || !pitch_px || !width) return;
    volatile uint32_t* p = (volatile uint32_t*)fb;
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < width; x++)
            p[(uint64_t)(row * 16 + y) * pitch_px + x] = color;
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
    uint64_t fb_base = 0; uint32_t fb_pitch_px = 0, fb_w = 0;
    if (gop && gop->Mode && gop->Mode->Info) {
        puts16(SystemTable, WIDE("GOP found: ")); print_hex64(SystemTable, gop->Mode->FrameBufferBase); puts16(SystemTable, WIDE("\r\n"));
        fb_base     = gop->Mode->FrameBufferBase;
        fb_pitch_px = gop->Mode->Info->PixelsPerScanLine;
        fb_w        = gop->Mode->Info->HorizontalResolution;
    } else {
        puts16(SystemTable, WIDE("GOP not found\r\n"));
    }
    // [UEFI-FIX] stage markers (top of screen): blue=entered+GOP.
    fb_bar(fb_base, fb_pitch_px, fb_w, 0, 0x000000FF);

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
    fb_bar(fb_base, fb_pitch_px, fb_w, 1, 0x0000FFFF); // stage2: cyan = ELF loaded

    // Fase 2: build ONLY the kernel's high-half backing; the low identity map is
    // inherited from the firmware at activate time (see install_kernel_mapping).
    // [M12] Higher-half: PML4[511] -> PDPThi[510] -> PDT (2MB pages) so the kernel,
    // linked at KERNEL_VMA (0xFFFFFFFF80000000) but loaded at its low LMA, is
    // reachable until it installs its own page tables. We allocate one page for a
    // COPY of the firmware PML4 (filled in install_kernel_mapping), the high PDPT
    // and the PDT.
    uint64_t pml4_phys = 0, pdt_phys = 0, pdpt_hi_phys = 0;
    uint8_t* pml4_copy = NULL; uint8_t* pdt = NULL; uint8_t* pdpt_hi = NULL;
    CHECK_OK("AllocPages PML4",   SystemTable->BootServices->AllocatePages(AllocateAnyPages, EFI_LOADER_DATA, 1, &pml4_phys));
    CHECK_OK("AllocPages PDT",    SystemTable->BootServices->AllocatePages(AllocateAnyPages, EFI_LOADER_DATA, 1, &pdt_phys));
    CHECK_OK("AllocPages PDPThi", SystemTable->BootServices->AllocatePages(AllocateAnyPages, EFI_LOADER_DATA, 1, &pdpt_hi_phys));
    pml4_copy = (uint8_t*)pml4_phys; pdt = (uint8_t*)pdt_phys; pdpt_hi = (uint8_t*)pdpt_hi_phys;
    for(int i=0;i<4096;i++){ pdt[i]=0; pdpt_hi[i]=0; } // pml4_copy is filled from the live PML4 at activate time
    // High-half backing: identity phys 0-512MB with 2MB pages, exposed at KERNEL_VMA.
    for(int e=0;e<256;e++) {
        uint64_t phys = (uint64_t)e << 21; // e*2MB
        ((uint64_t*)pdt)[e] = phys | 0x83; // Present|Write|PS
    }
    // PML4[511] -> PDPThi[510] -> PDT (high half KERNEL_VMA -> phys 0)
    ((uint64_t*)pdpt_hi)[510] = (uint64_t)pdt | 0x3;

    // Fase 3: Rimappare segmenti ELF (placeholder: già copiati in pool; mapping reale post-ExitBootServices non ancora implementato).
    // Per implementazione completa servirebbe allocare memoria fisica alle vaddr e copiare i dati fuori da pool temporaneo.
    if (g_loaded_segment_count > 0) {
        puts16(SystemTable, WIDE("[INFO] Copia segmenti ELF nelle vaddr target (assunzione identity)\r\n"));
        for (uint16_t si = 0; si < g_loaded_segment_count; ++si) {
            secos_loaded_segment_t* seg = &g_loaded_segments[si];
            uint64_t paddr = seg->paddr;     // [M12] place at the LMA (low identity)
            uint64_t filesz = seg->filesz;
            uint64_t memsz = seg->memsz;
            uint8_t* src = seg->data;
            uint8_t* dst = (uint8_t*)paddr;  // identity: phys == current VA (UEFI maps low)
            // Verifica che la collocazione fisica rientri nei 512MB identity.
            if (paddr + memsz > (512ULL * 1024 * 1024)) {
                puts16(SystemTable, WIDE("[WARN] Segmento oltre area identity mappata, salto\r\n"));
                continue;
            }
            // Copia parte file
            for (uint64_t k = 0; k < filesz; ++k) dst[k] = src[k];
            // Zero padding BSS
            for (uint64_t k = filesz; k < memsz; ++k) dst[k] = 0;
            puts16(SystemTable, WIDE("[OK] Segmento copiato paddr= "));
            print_hex64(SystemTable, paddr);
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

    // [M28] Find the ACPI RSDP in the EFI configuration table (prefer ACPI 2.0).
    // Plain memory reads — valid before ExitBootServices; the RSDP itself persists.
    uint64_t saved_rsdp = 0;
    {
        EFI_CONFIGURATION_TABLE* ct = SystemTable->ConfigurationTable;
        uint64_t n = SystemTable->NumberOfTableEntries;
        for (uint64_t i = 0; i < n; i++) {
            EFI_GUID* g = &ct[i].VendorGuid;
            int is20 = g->Data1==EFI_ACPI_20_TABLE_GUID.Data1 && g->Data2==EFI_ACPI_20_TABLE_GUID.Data2 && g->Data3==EFI_ACPI_20_TABLE_GUID.Data3;
            int is10 = g->Data1==EFI_ACPI_10_TABLE_GUID.Data1 && g->Data2==EFI_ACPI_10_TABLE_GUID.Data2 && g->Data3==EFI_ACPI_10_TABLE_GUID.Data3;
            if (is20) { saved_rsdp = (uint64_t)ct[i].VendorTable; break; }      // 2.0 wins
            if (is10 && !saved_rsdp) saved_rsdp = (uint64_t)ct[i].VendorTable;  // 1.0 fallback
        }
    }

    // Fase 4: ExitBootServices — retry loop (UEFI spec §7.4.6)
    // Any puts16 after GetMemoryMap allocates console buffers and stales the map_key.
    // Solution: print BEFORE the loop, then only GetMemoryMap + EBS inside it, no console.
    uint64_t final_map_size = alloc_buf_size; uint64_t final_map_key=0; uint64_t final_desc_size=0; uint32_t final_desc_ver=0;
    fb_bar(fb_base, fb_pitch_px, fb_w, 2, 0x00FFFF00); // stage3: yellow = page tables built, about to EBS
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
    fb_bar(fb_base, fb_pitch_px, fb_w, 3, 0x00FF00FF); // stage4: magenta = ExitBootServices done

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
    bootinfo.acpi_rsdp        = saved_rsdp;   // [M28] ACPI RSDP for the kernel
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
    /* Install our page tables before the handoff. bootinfo is already fully built
     * (GOP/memory map/ACPI read above), so no UEFI structures are touched past
     * this point. install_kernel_mapping inherits the firmware identity map, so
     * the loader's current code, stack and the bootinfo pointer stay mapped
     * REGARDLESS of where firmware placed them (the old 512MB-only map was the
     * real-hardware triple-fault). The kernel then copies bootinfo into its own
     * memory in phase1 before switching to its own CR3 (kernel/kernel.c). */
    install_kernel_mapping(pml4_copy, pdpt_hi);
    fb_bar(fb_base, fb_pitch_px, fb_w, 4, 0x0000FF00); // stage5: green = CR3 installed, jumping to kernel
    // Firma attesa: void kernel_main(uint32_t multiboot_magic, uint64_t multiboot_info)
    void (*kentry)(uint32_t, uint64_t) = (void(*)(uint32_t, uint64_t))kernel_entry;
    kentry(0, (uint64_t)&bootinfo);
    // Se mai ritorna, fermiamo
    for(;;){ __asm__ __volatile__("hlt"); }
    return EFI_SUCCESS; // non raggiunto
}

// Rimosso stub load_kernel: si usa elf_load_kernel esterno
