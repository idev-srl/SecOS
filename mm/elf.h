#ifndef ELF_H
#define ELF_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "vmm.h"
struct process; // forward

// ELF64 header structures (solo campi necessari)
#define ELF_MAGIC 0x464C457FULL // '\x7FELF' little endian

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

// Program header types
#define PT_LOAD 1
#define PT_NOTE 4

// p_flags bits
#define PF_X 1
#define PF_W 2
#define PF_R 4

// Result codes
#define ELF_OK 0
#define ELF_ERR_MAGIC -1
#define ELF_ERR_FMT   -2
#define ELF_ERR_RANGE -3
#define ELF_ERR_FLAG  -4
#define ELF_ERR_MAP   -5

// Loader API (caricamento in uno spazio utente già creato)
// buffer: puntatore al file ELF in memoria kernel
// size: dimensione del buffer
// space: address space user dove mappare
// entry_out: ritorna entry point virtuale
int elf_load_image(const void* buffer, size_t size, vmm_space_t* space, uint64_t* entry_out, uint64_t** pages_out, uint32_t* page_count_out);
int elf_unload_process(struct process* p); // forward dichiarazione process

#endif // ELF_H
