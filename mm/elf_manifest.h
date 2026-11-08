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
// [M24] CAP_NET: a single, general network capability. The signed manifest is
// the trust root — a process may use the socket syscalls only if this flag is
// set (covered by the Ed25519 signature, so it can't grant itself the cap).
#define MANIFEST_FLAG_CAP_NET               (1u<<4) // socket syscalls allowed

// [M35] Generalized least-privilege capabilities (signature-rooted). The trust
// model is ambient-by-default: a signed process gets full access UNLESS it opts
// into confinement by setting CAP_ENFORCE, in which case the kernel restricts it
// to exactly the capabilities declared below. A program cannot widen its own set
// (the flags are covered by the Ed25519 signature). Backward compatible: existing
// programs have CAP_ENFORCE clear and keep ambient access.
#define MANIFEST_FLAG_CAP_ENFORCE           (1u<<5) // enforce the capability bits below
#define MANIFEST_FLAG_CAP_FS_READ           (1u<<6) // open/read/stat/getdents/readlink
#define MANIFEST_FLAG_CAP_FS_WRITE          (1u<<7) // create/write/unlink/mkdir/chmod/...
#define MANIFEST_FLAG_CAP_PROC              (1u<<8) // spawn/fork/wait
#define MANIFEST_FLAG_CAP_IPC               (1u<<9) // msg_send/recv, pipe
#define MANIFEST_FLAG_CAP_SIGNAL            (1u<<10)// kill/signal other processes
#define MANIFEST_FLAG_CAP_TIME              (1u<<11)// getticks/sleep
#define MANIFEST_FLAG_CAP_POWER             (1u<<12)// poweroff/reboot/halt (SYS_REBOOT)
// [M40] GUI privileges. Unlike the bits above they are EXPLICIT: required even
// for an ambient (non-CAP_ENFORCE) program, like CAP_NET. Reading input devices
// lets a process snoop every keystroke and the framebuffer shows everything on
// screen, so only a program signed with these bits (the compositor) gets them.
#define MANIFEST_FLAG_CAP_INPUT             (1u<<13)// open /dev/input/eventN (evdev)
#define MANIFEST_FLAG_CAP_DISPLAY           (1u<<14)// open /dev/fb0, KDSETMODE KD_GRAPHICS
// CAP_NET (bit 4) is the network capability and folds into the same model.
// [M43] Set-id aware: a set-user-id / set-group-id bit on this program's file is
// honoured only when its signed manifest carries this flag (and the file is on
// the root filesystem). su, passwd and sudo have it; nothing else does, so a
// chmod u+s on any other signed binary has no effect. Not a capability.
#define MANIFEST_FLAG_SETID                 (1u<<15)
// [M46] Session gateway: a confined program whose job is to start authenticated
// sessions (Dropbear). It stays confined itself, but it does NOT get the
// automatic no_new_privs bit that every other confined program gets, so the
// session it starts runs under the session program's own manifest. Signed (an
// unsigned program cannot claim it) and has no effect once no_new_privs is
// already set (inherited or voluntary). Not a capability.
#define MANIFEST_FLAG_SESSION_GATEWAY       (1u<<16)
#define MANIFEST_CAP_MASK \
    (MANIFEST_FLAG_CAP_NET | MANIFEST_FLAG_CAP_FS_READ | MANIFEST_FLAG_CAP_FS_WRITE | \
     MANIFEST_FLAG_CAP_PROC | MANIFEST_FLAG_CAP_IPC | MANIFEST_FLAG_CAP_SIGNAL | \
     MANIFEST_FLAG_CAP_TIME | MANIFEST_FLAG_CAP_POWER | MANIFEST_FLAG_CAP_INPUT | \
     MANIFEST_FLAG_CAP_DISPLAY)

// Manifest versions
#define MANIFEST_VERSION_BASE    1u   // version/flags/max_mem/entry_hint (24-byte desc)
#define MANIFEST_VERSION_DRIVER  2u   // adds proc_type/dev_id/dev_caps (40-byte desc)
#define MANIFEST_VERSION_UDRV    3u   // [M57] adds the ring-3 PCI driver fields (96-byte desc)
#define MANIFEST_UDRV_MATCH_MAX  4u

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
    // --- v3 (MANIFEST_VERSION_UDRV, [M57] Driver Space v2): descsz >= 96 ---
    // Which PCI functions the driver may claim and what it may do with them.
    // Covered by the signature like every other field. Absent -> zero (no
    // match: a v2 DRIVER can claim nothing).
    uint32_t udrv_flags;  // reserved for the kernel proxies (M58: DRV_NET ...)
    uint16_t bar_mask;    // memory BARs it may map (bit n = BAR n)
    uint8_t  irq_max;     // MSI/MSI-X vectors it may set up (<= 32)
    uint8_t  nmatch;      // valid entries in match[] (<= 4)
    uint64_t dma_max;     // DMA bytes it may hold (0 = no DMA)
    uint32_t cfg_write_mask; // UDRV_CFGW_* (user/include/secos/udrv.h)
    uint32_t reserved2;
    struct {
        uint16_t vendor, device;   // 0xFFFF = any
        uint8_t  class_code, subclass; // 0xFF = any
        uint16_t reserved;
    } match[MANIFEST_UDRV_MATCH_MAX];
} elf_manifest_raw_t;
#define MANIFEST_RAW_V2_SIZE 40u
typedef char elf_manifest_raw_v3_is_96_bytes[(sizeof(elf_manifest_raw_t) == 96) ? 1 : -1];

// Struttura interna usata dal kernel (espande se servono campi derivati)
typedef struct elf_manifest {
    uint32_t version;
    uint32_t flags;
    uint64_t max_mem;
    uint64_t entry_hint;
    uint32_t proc_type;   // [M11] derivato dal manifest (default USER se v1)
    uint32_t dev_id;      // [M11] device richiesto (valido se DRIVER)
    uint32_t dev_caps;    // [M11] capability richieste (DEV_CAP_* mask)
    // [M57] v3 ring-3 PCI driver fields (zero unless version >= 3, DRIVER).
    uint32_t udrv_flags;
    uint16_t bar_mask;
    uint8_t  irq_max;
    uint8_t  nmatch;
    uint64_t dma_max;
    uint32_t cfg_write_mask;
    struct {
        uint16_t vendor, device;
        uint8_t  class_code, subclass;
    } match[MANIFEST_UDRV_MATCH_MAX];
} elf_manifest_t;

int elf_manifest_parse(const void* elf_buf, size_t size, elf_manifest_t* out);
int elf_manifest_validate(const elf_manifest_t* mf, uint64_t real_entry);

#endif // ELF_MANIFEST_H
