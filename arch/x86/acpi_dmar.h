/*
 * SecOS Kernel - ACPI DMAR (Intel VT-d) and IVRS (AMD-Vi) parsing (M53b)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Read-only discovery of the platform's DMA-remapping hardware. Nothing here
 * touches an IOMMU register: M54 (VT-d core) and M64 (AMD-Vi) consume the
 * parsed tables through acpi_dmar_get() / acpi_ivrs_get() and the lookup
 * helpers below.
 *
 * Everything in a firmware table is untrusted. The parsers take a byte buffer
 * and its length, walk it with bounded loops and checked arithmetic, and keep
 * going past a bad structure only when its length field is still usable
 * (else the walk stops). Every rejected or dropped entry is counted in
 * `invalid` / `dropped`; consumers must treat a unit with `valid == 0` as
 * absent. The parsed state is written once at boot (before the APs start)
 * and is read-only afterwards, so it needs no lock.
 *
 * The parsers are also built unchanged on the host by tools/acpi_fuzz/.
 */
#ifndef ACPI_DMAR_H
#define ACPI_DMAR_H
#include <stdint.h>

/* ---------------------------------------------------------------- limits */
#define DMAR_MAX_DRHD    16
#define DMAR_MAX_RMRR    32
#define DMAR_MAX_ATSR    8
#define DMAR_MAX_RHSA    16
#define DMAR_MAX_SATC    8
#define DMAR_MAX_SCOPES  128   /* pool shared by DRHD/RMRR/ATSR/SATC         */
#define DMAR_MAX_PATH    8     /* (dev,fn) hops in one device scope          */

#define IVRS_MAX_IVHD    16
#define IVRS_MAX_IVMD    32
#define IVRS_MAX_DEVS    256   /* device-entry pool shared by all IVHDs      */

/* --------------------------------------------------------- DMAR (VT-d) */
/* DMAR header flags (offset 37). */
#define DMAR_F_INTR_REMAP          (1u << 0)
#define DMAR_F_X2APIC_OPT_OUT      (1u << 1)
#define DMAR_F_DMA_CTRL_OPT_IN     (1u << 2)

/* DRHD flags. */
#define DRHD_F_INCLUDE_PCI_ALL     (1u << 0)
/* ATSR flags. */
#define ATSR_F_ALL_PORTS           (1u << 0)
/* SATC flags. */
#define SATC_F_ATC_REQUIRED        (1u << 0)

/* Device scope entry types (VT-d spec 8.3.1). */
#define DMAR_SCOPE_ENDPOINT   1
#define DMAR_SCOPE_BRIDGE     2   /* PCI-PCI bridge: covers its bus range     */
#define DMAR_SCOPE_IOAPIC     3   /* enum_id = I/O APIC id                    */
#define DMAR_SCOPE_HPET       4   /* enum_id = HPET number                    */
#define DMAR_SCOPE_NAMESPACE  5   /* enum_id = ACPI device number (ANDD)      */

/* A BDF packed as bus<<8 | dev<<3 | fn (the VT-d / PCI requester id). */
#define DMAR_BDF(b, d, f) ((uint16_t)(((b) << 8) | (((d) & 0x1F) << 3) | ((f) & 7)))

typedef struct {
    uint8_t  type;          /* DMAR_SCOPE_*                                   */
    uint8_t  enum_id;
    uint8_t  start_bus;
    uint8_t  path_len;      /* number of (dev,fn) hops, 1..DMAR_MAX_PATH      */
    uint8_t  path[DMAR_MAX_PATH][2];
    /* Resolution (bridge walk at boot). For ENDPOINT/BRIDGE `resolved` means
     * the function exists in config space now; for IOAPIC/HPET/NAMESPACE the
     * BDF is the source id the firmware assigned and the device need not
     * answer config cycles (`resolved` = every intermediate bridge existed). */
    uint8_t  resolved;
    uint8_t  is_display;    /* ENDPOINT whose class code is 03h (display)     */
    uint16_t bdf;           /* DMAR_BDF(...)                                  */
    uint8_t  sec_bus;       /* BRIDGE only: secondary..subordinate bus range  */
    uint8_t  sub_bus;
} dmar_scope_t;

typedef struct {
    uint8_t  valid;
    uint8_t  flags;         /* DRHD_F_*                                       */
    uint8_t  include_all;   /* INCLUDE_PCI_ALL (catch-all for its segment)    */
    uint8_t  gfx_only;      /* every scope is a display endpoint (iGPU unit)  */
    uint8_t  size_pages_log2; /* register set size = 2^N 4 KiB pages (VT-d 3+) */
    uint8_t  has_rhsa;
    uint8_t  scopes_incomplete; /* some scopes invalid or dropped: the unit's
                                 * coverage is not fully known               */
    uint16_t segment;
    uint64_t reg_base;      /* 4 KiB-aligned register base                    */
    uint32_t proximity;     /* RHSA proximity domain (if has_rhsa)            */
    uint16_t scope_first;   /* index into acpi_dmar_t.scopes                  */
    uint16_t scope_count;
} dmar_drhd_t;

typedef struct {
    uint8_t  valid;
    uint8_t  scopes_incomplete;
    uint16_t segment;
    uint64_t base;          /* 4 KiB-aligned                                  */
    uint64_t limit;         /* inclusive; limit + 1 is 4 KiB-aligned          */
    uint16_t scope_first;
    uint16_t scope_count;
} dmar_rmrr_t;

typedef struct {
    uint8_t  valid;
    uint8_t  flags;         /* ATSR_F_ALL_PORTS                               */
    uint16_t segment;
    uint16_t scope_first;   /* root ports (BRIDGE scopes) unless ALL_PORTS    */
    uint16_t scope_count;
} dmar_atsr_t;

typedef struct {
    uint64_t reg_base;
    uint32_t proximity;
} dmar_rhsa_t;

typedef struct {
    uint8_t  valid;
    uint8_t  flags;         /* SATC_F_ATC_REQUIRED                            */
    uint16_t segment;
    uint16_t scope_first;
    uint16_t scope_count;
} dmar_satc_t;

typedef struct {
    int      present;       /* a DMAR table was found and its header parsed   */
    uint8_t  checksum_bad;  /* table checksum wrong (parsed anyway, logged)   */
    uint8_t  revision;
    uint8_t  haw_bad;       /* haw outside 32..64 (recorded, logged)          */
    uint16_t haw;           /* host address width = header byte 36 + 1        */
    uint8_t  flags;         /* DMAR_F_*                                       */
    uint32_t length;        /* table length as parsed (bounded)               */
    uint32_t drhd_count, rmrr_count, atsr_count, rhsa_count, satc_count;
    uint32_t andd_count;    /* ACPI namespace device declarations (counted)   */
    uint32_t unknown_count; /* structures of unknown type (skipped)           */
    uint32_t scope_count;
    uint32_t invalid;       /* structures/scopes rejected as malformed        */
    uint32_t dropped;       /* valid entries beyond the static limits         */
    uint8_t  truncated;     /* the walk stopped early on a broken length      */
    dmar_drhd_t  drhd[DMAR_MAX_DRHD];
    dmar_rmrr_t  rmrr[DMAR_MAX_RMRR];
    dmar_atsr_t  atsr[DMAR_MAX_ATSR];
    dmar_rhsa_t  rhsa[DMAR_MAX_RHSA];
    dmar_satc_t  satc[DMAR_MAX_SATC];
    dmar_scope_t scopes[DMAR_MAX_SCOPES];
} acpi_dmar_t;

/* ------------------------------------------------------- IVRS (AMD-Vi) */
/* IVRS device entry kinds, normalized from the raw entry types. */
#define IVRS_DEV_ALL        1   /* type 01h: every device on the segment       */
#define IVRS_DEV_SELECT     2   /* type 02h: one device                        */
#define IVRS_DEV_RANGE      3   /* types 03h..04h: start..end inclusive        */
#define IVRS_DEV_ALIAS      4   /* 42h select / 43h..04h range, with alias id  */
#define IVRS_DEV_EXT        5   /* 46h select / 47h..04h range, extended data  */
#define IVRS_DEV_SPECIAL    6   /* 48h: IOAPIC (variety 1) / HPET (variety 2)  */
#define IVRS_DEV_ACPI_HID   7   /* F0h: ACPI HID device (counted, devid kept)  */

#define IVRS_SPECIAL_IOAPIC 1
#define IVRS_SPECIAL_HPET   2

typedef struct {
    uint8_t  kind;          /* IVRS_DEV_*                                     */
    uint8_t  dte;           /* DTE setting byte (INITPass/EIntPass/...)       */
    uint16_t start;         /* device id (BDF)                                */
    uint16_t end;           /* inclusive; == start for single-device kinds    */
    uint16_t alias;         /* ALIAS: requester id used instead               */
    uint8_t  handle;        /* SPECIAL: IOAPIC id / HPET number               */
    uint8_t  variety;       /* SPECIAL: IVRS_SPECIAL_*                        */
    uint32_t ext;           /* EXT: extended data dword                       */
} ivrs_dev_t;

typedef struct {
    uint8_t  valid;
    uint8_t  type;          /* 0x10, 0x11 or 0x40                             */
    uint8_t  flags;
    uint8_t  primary;       /* this block's type is the one an OS must use    */
    uint16_t devid;         /* the IOMMU's own PCI function                   */
    uint16_t cap_offset;    /* capability block offset in its config space    */
    uint16_t segment;
    uint16_t info;          /* IOMMU info (MSI number, UnitID)                */
    uint64_t base;          /* MMIO base (16 KiB-aligned)                     */
    uint32_t attr;          /* 10h: feature reporting; 11h/40h: attributes    */
    uint64_t efr;           /* 11h/40h: EFR register image                    */
    uint64_t efr2;          /* 11h/40h: EFR2 image                            */
    uint16_t dev_first;     /* index into acpi_ivrs_t.devs                    */
    uint16_t dev_count;
} ivrs_ivhd_t;

typedef struct {
    uint8_t  valid;
    uint8_t  type;          /* 0x20 all, 0x21 one device, 0x22 range          */
    uint8_t  flags;         /* unity / IR / IW / exclusion range bits         */
    uint16_t start;
    uint16_t end;
    uint64_t base;          /* 4 KiB-aligned                                  */
    uint64_t length;        /* > 0, base + length does not overflow           */
} ivrs_ivmd_t;

typedef struct {
    int      present;
    uint8_t  checksum_bad;
    uint8_t  revision;
    uint32_t ivinfo;
    uint32_t length;
    uint8_t  primary_type;  /* highest IVHD type present (0x10/0x11/0x40)     */
    uint32_t ivhd_count, ivmd_count, dev_count;
    uint32_t unknown_count;
    uint32_t invalid;
    uint32_t dropped;
    uint8_t  truncated;
    ivrs_ivhd_t ivhd[IVRS_MAX_IVHD];
    ivrs_ivmd_t ivmd[IVRS_MAX_IVMD];
    ivrs_dev_t  devs[IVRS_MAX_DEVS];
} acpi_ivrs_t;

/* ------------------------------------------------------------ parsers */
/* Parse a DMAR / IVRS table (`len` bytes at `t`, header included) into `out`
 * (zeroed first). Return 0 if a usable header was found, -1 otherwise.
 * acpi_dmar_parse() resolves device scopes through acpi_dmar_cfg_read32(). */
int acpi_dmar_parse(const uint8_t* t, uint32_t len, acpi_dmar_t* out);
int acpi_ivrs_parse(const uint8_t* t, uint32_t len, acpi_ivrs_t* out);

/* Log a parsed table (`[IOMMU] ...` lines on debugcon). */
void acpi_dmar_log(const acpi_dmar_t* d);
void acpi_ivrs_log(const acpi_ivrs_t* v);

/* PCI config read used for scope resolution (the one hook to switch to the
 * M53a device registry): returns 0xFFFFFFFF for an absent function or an
 * unreachable segment. Implemented in acpi_dmar_pci.c (host: the harness). */
uint32_t acpi_dmar_cfg_read32(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn,
                              uint16_t off);

/* --------------------------------------------------------- boot + API */
/* Find DMAR and IVRS through acpi.c, parse and log them. Logs `[IOMMU] none`
 * when the platform has neither. Call once, after acpi_init(). */
void acpi_dmar_init(void);

/* Parsed tables (never NULL; `present == 0` when the table is absent). */
const acpi_dmar_t* acpi_dmar_get(void);
const acpi_ivrs_t* acpi_ivrs_get(void);

/* The DRHD that translates requester `bdf` on `segment`: an explicit
 * endpoint scope, then a bridge scope whose bus range holds the bus, then the
 * segment's INCLUDE_PCI_ALL unit. NULL if no valid unit covers it. */
const dmar_drhd_t* acpi_dmar_drhd_for(uint16_t segment, uint16_t bdf);

/* Source id of an I/O APIC / HPET from the DRHD scopes (for interrupt
 * remapping, M56). Return 0 and set *sid, or -1 if not declared. */
int acpi_dmar_ioapic_sid(uint8_t ioapic_id, uint16_t* sid);
int acpi_dmar_hpet_sid(uint8_t hpet_id, uint16_t* sid);

/* 1 if `bdf` on `segment` is named by an RMRR scope (must keep its identity
 * map); `*first` (optional) gets the index of the first such RMRR. */
int acpi_dmar_rmrr_covers(uint16_t segment, uint16_t bdf, uint32_t* first);

/* The primary IVHD that lists device `devid` (ALL/SELECT/RANGE/ALIAS/EXT
 * entries; ALIAS returns the alias requester id in *alias if non-NULL, else
 * devid). NULL if none. */
const ivrs_ivhd_t* acpi_ivrs_ivhd_for(uint16_t segment, uint16_t devid,
                                      uint16_t* alias);

/* Device id of an IOAPIC/HPET from an IVHD special entry (M64 IR). */
int acpi_ivrs_special_devid(uint8_t variety, uint8_t handle, uint16_t* devid);

#endif /* ACPI_DMAR_H */
