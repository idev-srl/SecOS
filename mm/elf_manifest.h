#ifndef ELF_MANIFEST_H
#define ELF_MANIFEST_H
/*

 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
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

// Manifest versions
#define MANIFEST_VERSION_BASE    1u   // version/flags/max_mem/entry_hint (24-byte desc)
#define MANIFEST_VERSION_DRIVER  2u   // adds proc_type/dev_id/dev_caps (40-byte desc)

// [M11] Process type carried by the (signed) manifest. The signature is the
// trust root for this claim: a binary can only run as a driver if the signed
// manifest declares PROC_TYPE_DRIVER. Absent (v1 manifest) defaults to USER.
#define MANIFEST_PROC_TYPE_USER    0u
#define MANIFEST_PROC_TYPE_DRIVER  1u

// Errori parser
#define MANIFEST_OK              0
#define MANIFEST_ERR_NOT_FOUND  -1
#define MANIFEST_ERR_FMT        -2
#define MANIFEST_ERR_RANGE      -3
#define MANIFEST_ERR_UNSUPPORTED -4

typedef struct elf_manifest_raw {
    uint32_t version;     // versione manifest (MANIFEST_VERSION_*)
    uint32_t flags;       // bitmask flags
    uint64_t max_mem;     // limite massimo memoria virtuale che il processo può mappare (placeholder)
    uint64_t entry_hint;  // entry point atteso, 0 = ignora
    // --- v2 (MANIFEST_VERSION_DRIVER): campi Driver Space, opzionali ---
    // Assenti nei manifest v1 (descsz==24); il parser li azzera in quel caso.
    uint32_t proc_type;   // MANIFEST_PROC_TYPE_USER / _DRIVER
    uint32_t dev_id;      // device che il driver può legare (significativo se DRIVER)
    uint32_t dev_caps;    // capability concesse (DEV_CAP_* mask) — sottoinsieme del device
    uint32_t reserved;    // padding/futuro (0)
} elf_manifest_raw_t;

// Struttura interna usata dal kernel (espande se servono campi derivati)
typedef struct elf_manifest {
    uint32_t version;
    uint32_t flags;
    uint64_t max_mem;
    uint64_t entry_hint;
    uint32_t proc_type;   // [M11] derivato dal manifest (default USER se v1)
    uint32_t dev_id;      // [M11] device richiesto (valido se DRIVER)
    uint32_t dev_caps;    // [M11] capability richieste (DEV_CAP_* mask)
} elf_manifest_t;

int elf_manifest_parse(const void* elf_buf, size_t size, elf_manifest_t* out);
int elf_manifest_validate(const elf_manifest_t* mf, uint64_t real_entry);

#endif // ELF_MANIFEST_H
