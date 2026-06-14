/*

 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "mm/elf_manifest.h"
#include "terminal.h"
#include "mm/elf.h"

// Parser PT_NOTE per name "SECOS" e type SECOS_NOTE_TYPE
int elf_manifest_parse(const void* elf_buf, size_t size, elf_manifest_t* out) {
    if (!elf_buf || size < sizeof(Elf64_Ehdr) || !out) return MANIFEST_ERR_FMT;
    const uint8_t* base = (const uint8_t*)elf_buf;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)elf_buf;
    if (eh->e_phoff == 0 || eh->e_phnum == 0) return MANIFEST_ERR_NOT_FOUND;
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) return MANIFEST_ERR_FMT;
    // Scansiona PHDR in cerca di PT_NOTE
    const Elf64_Phdr* ph;
    const elf_manifest_raw_t* raw = NULL;
    uint32_t raw_descsz = 0;
    for (int i=0;i<eh->e_phnum;i++) {
        ph = (const Elf64_Phdr*)(base + eh->e_phoff + i*sizeof(Elf64_Phdr));
        if ((const uint8_t*)ph + sizeof(Elf64_Phdr) > base + size) return MANIFEST_ERR_RANGE;
        if (ph->p_type != PT_NOTE) continue;
        // La regione note contiene: namesz, descsz, type, name, desc
        if (ph->p_offset + ph->p_filesz > size) return MANIFEST_ERR_RANGE;
        const uint8_t* note = base + ph->p_offset;
        const uint8_t* end  = note + ph->p_filesz;
        while (note + 12 <= end) {
            uint32_t namesz = *(const uint32_t*)(note);
            uint32_t descsz = *(const uint32_t*)(note+4);
            uint32_t type   = *(const uint32_t*)(note+8);
            const char* name = (const char*)(note+12);
            const uint8_t* desc = (const uint8_t*)(note + 12 + ((namesz +3)&~3));
            const uint8_t* next = desc + ((descsz +3)&~3);
            if (next > end) break;
            if (namesz && descsz && type == SECOS_NOTE_TYPE) {
                // Verifica nome
                if (namesz >= sizeof(SECOS_NOTE_NAME) && name[0]=='S' && name[1]=='E' && name[2]=='C' && name[3]=='O' && name[4]=='S') {
                    // Il manifest base (v1) ha descsz==24; v2 (driver) ha descsz>=40.
                    // Accetta qualunque desc con almeno il blocco base.
                    if (descsz >= 24) {
                        raw = (const elf_manifest_raw_t*)desc;
                        raw_descsz = descsz;
                        break;
                    }
                }
            }
            note = next;
        }
        if (raw) break;
    }
    if (!raw) return MANIFEST_ERR_NOT_FOUND;
    out->version = raw->version;
    out->flags   = raw->flags;
    out->max_mem = raw->max_mem;
    out->entry_hint = raw->entry_hint;
    // [M11] Campi driver (v2): presenti solo se il desc è abbastanza grande E
    // la versione li dichiara. Altrimenti default USER, nessun grant.
    out->proc_type = MANIFEST_PROC_TYPE_USER;
    out->dev_id    = 0;
    out->dev_caps  = 0;
    if (raw->version >= MANIFEST_VERSION_DRIVER &&
        raw_descsz >= sizeof(elf_manifest_raw_t)) {
        out->proc_type = raw->proc_type;
        out->dev_id    = raw->dev_id;
        out->dev_caps  = raw->dev_caps;
    }
    terminal_writestring("[MANIFEST] parsed versione=");
    char hx[]="0123456789ABCDEF"; for(int i=4;i>=0;i-=4) terminal_putchar(hx[(out->version>>i)&0xF]);
    terminal_writestring(" flags="); for(int i=31;i>=0;i-=4) terminal_putchar(hx[(out->flags>>i)&0xF]);
    if (out->proc_type == MANIFEST_PROC_TYPE_DRIVER) {
        terminal_writestring(" type=DRIVER dev=");
        for(int i=4;i>=0;i-=4) terminal_putchar(hx[(out->dev_id>>i)&0xF]);
        terminal_writestring(" caps="); for(int i=7;i>=0;i-=4) terminal_putchar(hx[(out->dev_caps>>i)&0xF]);
    }
    terminal_writestring("\n");
    return MANIFEST_OK;
}

int elf_manifest_validate(const elf_manifest_t* mf, uint64_t real_entry) {
    if (!mf) return MANIFEST_ERR_FMT;
    // entry_hint check
    if (mf->entry_hint && mf->entry_hint != real_entry) {
        terminal_writestring("[MANIFEST] entry mismatch\n");
        return MANIFEST_ERR_UNSUPPORTED;
    }
    // Flags enforcement (alcuni già garantiti dal loader)
    if (mf->flags & MANIFEST_FLAG_REQUIRE_WX_BLOCK) {
        // Il loader già rifiuta W|X, nessuna azione.
    }
    if (mf->flags & MANIFEST_FLAG_REQUIRE_STACK_GUARD) {
        // Il nostro allocator crea guard page (già fatto)
    }
    // max_mem placeholder: non abbiamo ancora contatori per processo, skip
    return MANIFEST_OK;
}
