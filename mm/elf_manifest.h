#ifndef ELF_MANIFEST_H
#define ELF_MANIFEST_H
#include <stddef.h>
#include <stdint.h>

// Sezione .note.secos: formato ELF PT_NOTE
// name: "SECOS"\0
// desc: struct elf_manifest_raw
// type: 0x51534543 (ASCII 'QSEC') per identificare il tipo manifest

#define SECOS_NOTE_NAME "SECOS"
#define SECOS_NOTE_TYPE 0x51534543U // 'QSEC'

// Flags manifest
#define MANIFEST_FLAG_REQUIRE_WX_BLOCK      (1u<<0) // nessun segmento W|X
#define MANIFEST_FLAG_REQUIRE_STACK_GUARD   (1u<<1) // stack utente con guard page
#define MANIFEST_FLAG_REQUIRE_NX_DATA       (1u<<2) // data RW NX
#define MANIFEST_FLAG_REQUIRE_RX_CODE       (1u<<3) // code RX

// Errori parser
#define MANIFEST_OK              0
#define MANIFEST_ERR_NOT_FOUND  -1
#define MANIFEST_ERR_FMT        -2
#define MANIFEST_ERR_RANGE      -3
#define MANIFEST_ERR_UNSUPPORTED -4

typedef struct elf_manifest_raw {
    uint32_t version;     // versione manifest
    uint32_t flags;       // bitmask flags
    uint64_t max_mem;     // limite massimo memoria virtuale che il processo può mappare (placeholder)
    uint64_t entry_hint;  // entry point atteso, 0 = ignora
} elf_manifest_raw_t;

// Struttura interna usata dal kernel (espande se servono campi derivati)
typedef struct elf_manifest {
    uint32_t version;
    uint32_t flags;
    uint64_t max_mem;
    uint64_t entry_hint;
} elf_manifest_t;

int elf_manifest_parse(const void* elf_buf, size_t size, elf_manifest_t* out);
int elf_manifest_validate(const elf_manifest_t* mf, uint64_t real_entry);

#endif // ELF_MANIFEST_H
