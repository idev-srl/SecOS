/*
 * SecOS UEFI ELF Loader (Strategy B External Bootloader)
 * Loads an external ELF64 kernel file ("kernel.elf") from the root of the FAT volume
 * via UEFI Simple File System + File Protocol. Performs minimal ELF validation and
 * maps loadable segments into memory (identity assumptions). Permissions/W^X will be
 * finalized later when page tables are established.
 *
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "efi.h"
#include <stdint.h>

#define ELF_MAGIC 0x464C457FU /* '\x7FELF' little-endian */

typedef struct {
    uint32_t e_magic;     // 0x7F 'E' 'L' 'F'
    uint8_t  e_class;     // 2 = 64-bit
    uint8_t  e_data;      // 1 = little endian
    uint8_t  e_version;   // 1
    uint8_t  e_osabi;     // ignored
    uint8_t  e_abiversion;// ignored
    uint8_t  e_pad[7];
    uint16_t e_type;      // 2 = ET_EXEC, 3 = ET_DYN
    uint16_t e_machine;   // 0x3E = x86-64
    uint32_t e_version2;  // 1
    uint64_t e_entry;     // entry virtual address
    uint64_t e_phoff;     // program header table offset
    uint64_t e_shoff;     // section header table offset
    uint32_t e_flags;     // unused
    uint16_t e_ehsize;    // ELF header size
    uint16_t e_phentsize; // size of one program header
    uint16_t e_phnum;     // number of program headers
    uint16_t e_shentsize; // section header size
    uint16_t e_shnum;     // number of section headers
    uint16_t e_shstrndx;  // section name string table index
} elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;  // PF_X=1 PF_W=2 PF_R=4 (bitmask)
    uint64_t p_offset; // file offset
    uint64_t p_vaddr;  // virtual addr
    uint64_t p_paddr;  // physical addr (ignored / same as vaddr for identity assumption)
    uint64_t p_filesz; // size in file
    uint64_t p_memsz;  // size in memory
    uint64_t p_align;  // alignment
} elf64_phdr_t;

#define PT_LOAD 1

/* ELF section header (for symbol table lookup) */
typedef struct {
    uint32_t sh_name;       // offset into section name string table
    uint32_t sh_type;       // SHT_SYMTAB=2, SHT_STRTAB=3
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;     // file offset
    uint64_t sh_size;
    uint32_t sh_link;       // for SHT_SYMTAB: index of associated SHT_STRTAB
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} elf64_shdr_t;

/* ELF symbol table entry */
typedef struct {
    uint32_t st_name;       // offset into string table
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;      // symbol virtual address
    uint64_t st_size;
} elf64_sym_t;

#define SHT_SYMTAB 2

static int strneq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static void memcpy8(uint8_t* dst, const uint8_t* src, uint64_t n) { while(n--) *dst++ = *src++; }
static void memset8(uint8_t* dst, uint8_t v, uint64_t n) { while(n--) *dst++ = v; }

// Limiti massimi segmenti caricabili (ragionevole per kernel)
#define SECOS_MAX_SEGMENTS 16
secos_loaded_segment_t g_loaded_segments[SECOS_MAX_SEGMENTS];
uint16_t g_loaded_segment_count = 0;
void* g_kernel_entry = NULL;

// Reads entire file into a newly allocated pool buffer.
static EFI_STATUS read_file_into_buffer(EFI_SYSTEM_TABLE* st, const CHAR16* name, uint8_t** out_buf, uint64_t* out_size) {
    EFI_STATUS status;
    // Locate Simple File System protocol
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* sfs = NULL;
    status = st->BootServices->LocateProtocol(&EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, NULL, (void**)&sfs);
    if (status != EFI_SUCCESS || !sfs) return EFI_ERR(2);
    EFI_FILE_PROTOCOL* root = NULL;
    status = sfs->OpenVolume(sfs, &root);
    if (status != EFI_SUCCESS || !root) return EFI_ERR(3);
    EFI_FILE_PROTOCOL* file = NULL;
    status = root->Open(root, &file, name, EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS || !file) { if(root) root->Close(root); return EFI_ERR(4); }
    // Read loop: start with an estimate. We attempt doubling strategy.
    uint64_t alloc_size = 1024*1024; // 1MB initial guess
    uint8_t* buffer = NULL;
    status = st->BootServices->AllocatePool(EFI_LOADER_DATA, alloc_size, (void**)&buffer);
    if (status != EFI_SUCCESS) { file->Close(file); root->Close(root); return EFI_ERR(5); }
    uint64_t read_size = alloc_size;
    status = file->Read(file, &read_size, buffer);
    if (status != EFI_SUCCESS) { file->Close(file); root->Close(root); return EFI_ERR(6); }
    // Heuristic: if read_size == alloc_size we may need more; skip for simplicity now.
    *out_buf = buffer; *out_size = read_size;
    file->Close(file); root->Close(root);
    return EFI_SUCCESS;
}

EFI_STATUS elf_load_kernel(EFI_SYSTEM_TABLE* SystemTable, void** entry_out) {
    if (!SystemTable || !entry_out) return EFI_ERR(1);
    uint8_t* file_buf = NULL; uint64_t file_len = 0;
    EFI_STATUS status = read_file_into_buffer(SystemTable, (const CHAR16*)L"kernel.elf", &file_buf, &file_len);
    if (status != EFI_SUCCESS) return status;
    if (file_len < sizeof(elf64_ehdr_t)) return EFI_ERR(7);
    elf64_ehdr_t* eh = (elf64_ehdr_t*)file_buf;
    if (eh->e_magic != ELF_MAGIC || eh->e_class != 2 || eh->e_machine != 0x3E) return EFI_ERR(8);
    if (eh->e_phentsize != sizeof(elf64_phdr_t)) return EFI_ERR(9);
    if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(elf64_phdr_t) > file_len) return EFI_ERR(10);
    elf64_phdr_t* ph = (elf64_phdr_t*)(file_buf + eh->e_phoff);
    // Validazione avanzata: controlla overlapping e alignment
    // Primo: colleziona segmenti PT_LOAD in array temporaneo
    elf64_phdr_t* load_list[SECOS_MAX_SEGMENTS];
    uint16_t load_count = 0;
    for (uint16_t i=0;i<eh->e_phnum;i++) {
        if (ph[i].p_type == PT_LOAD) {
            if (load_count >= SECOS_MAX_SEGMENTS) return EFI_ERR(14);
            // p_align deve essere potenza di due o 0/1
            if (ph[i].p_align && (ph[i].p_align & (ph[i].p_align - 1)) != 0) return EFI_ERR(15);
            // p_filesz <= p_memsz
            if (ph[i].p_filesz > ph[i].p_memsz) return EFI_ERR(16);
            // Limite range identity (512MB) provvisorio
            if (ph[i].p_vaddr + ph[i].p_memsz > (512ULL * 1024 * 1024)) return EFI_ERR(17);
            load_list[load_count++] = &ph[i];
        }
    }
    // Overlap check O(n^2) su load_count (massimo 16 quindi ok)
    for (uint16_t a=0;a<load_count;a++) {
        for (uint16_t b=a+1;b<load_count;b++) {
            uint64_t a_start = load_list[a]->p_vaddr;
            uint64_t a_end   = a_start + load_list[a]->p_memsz;
            uint64_t b_start = load_list[b]->p_vaddr;
            uint64_t b_end   = b_start + load_list[b]->p_memsz;
            if (!(a_end <= b_start || b_end <= a_start)) {
                return EFI_ERR(18); // overlap
            }
        }
    }
    // Caricamento segmenti
    for (uint16_t i=0;i<eh->e_phnum;i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        // Simple identity mapping assumption: allocate memory for segment at its virtual address later.
        // Here we only copy into a temporary buffer; page table remap will occur in later boot phase.
        uint8_t* seg_dst = (uint8_t*)ph[i].p_vaddr; // NOTE: unsafe until we map; placeholder
        // For now allocate pool memory instead of using actual target address.
        uint8_t* pool_seg = NULL;
        status = SystemTable->BootServices->AllocatePool(EFI_LOADER_DATA, ph[i].p_memsz, (void**)&pool_seg);
        if (status != EFI_SUCCESS) return EFI_ERR(11);
        memset8(pool_seg, 0, ph[i].p_memsz);
        if (ph[i].p_offset + ph[i].p_filesz <= file_len) {
            memcpy8(pool_seg, file_buf + ph[i].p_offset, ph[i].p_filesz);
        } else return EFI_ERR(12);
        // Later: store mapping information to actually place at p_vaddr.
        if (g_loaded_segment_count < SECOS_MAX_SEGMENTS) {
            secos_loaded_segment_t* s = &g_loaded_segments[g_loaded_segment_count++];
            s->vaddr = ph[i].p_vaddr;
            s->memsz = ph[i].p_memsz;
            s->filesz = ph[i].p_filesz;
            s->flags = ph[i].p_flags;
            s->data = pool_seg;
        } else {
            return EFI_ERR(13); // troppi segmenti
        }
        (void)seg_dst; // placeholder
    }
    /* Symbol table lookup: prefer _uefi_start over e_entry if present.
     * _uefi_start is a BITS 64 entry that handles being called from UEFI long mode. */
    uint64_t uefi_entry = 0;
    if (eh->e_shoff && eh->e_shnum && eh->e_shentsize == sizeof(elf64_shdr_t) &&
        eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(elf64_shdr_t) <= file_len) {
        elf64_shdr_t* sh = (elf64_shdr_t*)(file_buf + eh->e_shoff);
        for (uint16_t s = 0; s < eh->e_shnum && !uefi_entry; s++) {
            if (sh[s].sh_type != SHT_SYMTAB) continue;
            uint32_t strtab_idx = sh[s].sh_link;
            if (strtab_idx >= eh->e_shnum) continue;
            elf64_shdr_t* strtab_sh = &sh[strtab_idx];
            if (strtab_sh->sh_offset + strtab_sh->sh_size > file_len) continue;
            const char* strtab = (const char*)(file_buf + strtab_sh->sh_offset);
            if (sh[s].sh_offset + sh[s].sh_size > file_len) continue;
            uint64_t nsyms = sh[s].sh_size / sizeof(elf64_sym_t);
            elf64_sym_t* syms = (elf64_sym_t*)(file_buf + sh[s].sh_offset);
            for (uint64_t si = 0; si < nsyms; si++) {
                if (syms[si].st_name + 12 > strtab_sh->sh_size) continue;
                if (strneq(strtab + syms[si].st_name, "_uefi_start"))
                    uefi_entry = syms[si].st_value;
            }
        }
    }
    *entry_out = (void*)(uefi_entry ? uefi_entry : eh->e_entry);
    g_kernel_entry = *entry_out;
    return EFI_SUCCESS;
}
