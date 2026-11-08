/*
 * vtd.h — Intel VT-d DMA remapping (Phase 5, M54). Internal to
 * arch/x86/vtd.c (units, registers, invalidation, faults, contexts) and
 * arch/x86/vtd_pt.c (second-level page tables). Everything else uses the
 * vendor-neutral kernel/iommu.h.
 *
 * Legacy (non-scalable) mode only: root table -> context tables -> second-
 * level page tables (3 levels for a 39-bit AGAW, 4 for 48-bit). No ATS, no
 * first-level / nested translation. Interrupt remapping (M56) lives in
 * arch/x86/vtd_ir.c on top of the unit accessors at the end of this file.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VTD_H
#define VTD_H
#include <stdint.h>
#include "iommu.h"
#include "acpi_dmar.h"

/* ---------------------------------------------------------- registers */
#define VTD_REG_VER       0x000
#define VTD_REG_CAP       0x008
#define VTD_REG_ECAP      0x010
#define VTD_REG_GCMD      0x018
#define VTD_REG_GSTS      0x01C
#define VTD_REG_RTADDR    0x020
#define VTD_REG_CCMD      0x028
#define VTD_REG_FSTS      0x034
#define VTD_REG_FECTL     0x038
#define VTD_REG_FEDATA    0x03C
#define VTD_REG_FEADDR    0x040
#define VTD_REG_FEUADDR   0x044
#define VTD_REG_PMEN      0x064
#define VTD_REG_IQH       0x080
#define VTD_REG_IQT       0x088
#define VTD_REG_IQA       0x090
#define VTD_REG_ICS       0x09C
#define VTD_REG_IRTA      0x0B8     /* M56: interrupt-remapping table address */

/* CAP */
#define VTD_CAP_ND(c)       ((uint32_t)((c) & 7))
#define VTD_CAP_RWBF        (1ull << 4)
#define VTD_CAP_PLMR        (1ull << 5)
#define VTD_CAP_PHMR        (1ull << 6)
#define VTD_CAP_CM          (1ull << 7)
#define VTD_CAP_SAGAW(c)    ((uint32_t)(((c) >> 8) & 0x1F))
#define VTD_CAP_MGAW(c)     ((uint32_t)(((c) >> 16) & 0x3F) + 1)
#define VTD_CAP_FRO(c)      ((uint32_t)(((c) >> 24) & 0x3FF) * 16)
#define VTD_CAP_SLLPS(c)    ((uint32_t)(((c) >> 34) & 0xF))
#define VTD_CAP_PSI         (1ull << 39)
#define VTD_CAP_NFR(c)      ((uint32_t)(((c) >> 40) & 0xFF) + 1)
#define VTD_CAP_MAMV(c)     ((uint32_t)(((c) >> 48) & 0x3F))
#define VTD_CAP_DWD         (1ull << 54)
#define VTD_CAP_DRD         (1ull << 55)
/* ECAP */
#define VTD_ECAP_C          (1ull << 0)
#define VTD_ECAP_QI         (1ull << 1)
#define VTD_ECAP_DT         (1ull << 2)
#define VTD_ECAP_IR         (1ull << 3)
#define VTD_ECAP_EIM        (1ull << 4)
#define VTD_ECAP_PT         (1ull << 6)
#define VTD_ECAP_SC         (1ull << 7)
#define VTD_ECAP_IRO(e)     ((uint32_t)(((e) >> 8) & 0x3FF) * 16)
#define VTD_ECAP_SMTS       (1ull << 43)

/* GCMD / GSTS */
#define VTD_GCMD_TE         (1u << 31)
#define VTD_GCMD_SRTP       (1u << 30)
#define VTD_GCMD_WBF        (1u << 27)
#define VTD_GCMD_QIE        (1u << 26)
#define VTD_GCMD_IRE        (1u << 25)
#define VTD_GCMD_SIRTP      (1u << 24)   /* M56: latch IRTA                  */
#define VTD_GCMD_CFI        (1u << 23)   /* M56: compatibility format allowed */
#define VTD_GSTS_TES        (1u << 31)
#define VTD_GSTS_RTPS       (1u << 30)
#define VTD_GSTS_WBFS       (1u << 27)
#define VTD_GSTS_QIES       (1u << 26)
#define VTD_GSTS_IRES       (1u << 25)
#define VTD_GSTS_IRTPS      (1u << 24)
#define VTD_GSTS_CFIS       (1u << 23)
/* GSTS bits that are persistent commands (kept when writing GCMD). */
#define VTD_GSTS_PERSIST    0x96FFFFFFu

/* FSTS / FECTL */
#define VTD_FSTS_PFO        (1u << 0)
#define VTD_FSTS_PPF        (1u << 1)
#define VTD_FSTS_IQE        (1u << 4)
#define VTD_FSTS_ICE        (1u << 5)
#define VTD_FSTS_ITE        (1u << 6)
#define VTD_FSTS_FRI(s)     (((s) >> 8) & 0xFF)
#define VTD_FECTL_IM        (1u << 31)
#define VTD_FECTL_IP        (1u << 30)

/* PMEN */
#define VTD_PMEN_EPM        (1u << 31)
#define VTD_PMEN_PRS        (1u << 0)

/* Register-based invalidation. */
#define VTD_CCMD_ICC        (1ull << 63)
#define VTD_CCMD_GLOBAL     (1ull << 61)
#define VTD_CCMD_DOMAIN     (2ull << 61)
#define VTD_CCMD_DEVICE     (3ull << 61)
#define VTD_IOTLB_IVT       (1ull << 63)
#define VTD_IOTLB_GLOBAL    (1ull << 60)
#define VTD_IOTLB_DSI       (2ull << 60)
#define VTD_IOTLB_PSI       (3ull << 60)
#define VTD_IOTLB_DR        (1ull << 49)
#define VTD_IOTLB_DW        (1ull << 48)

/* Queued-invalidation descriptors (128-bit). */
#define VTD_QI_CC           0x1     /* context-cache invalidate  */
#define VTD_QI_IOTLB        0x2
#define VTD_QI_WAIT         0x5
#define VTD_QI_IEC          0x4     /* M56: interrupt-entry-cache invalidate */
#define VTD_QI_IEC_INDEX    (1ull << 4)   /* index-selective (else global)   */
#define VTD_QI_GRAN_GLOBAL  (1ull << 4)
#define VTD_QI_GRAN_DOMAIN  (2ull << 4)
#define VTD_QI_GRAN_DEVPAGE (3ull << 4)   /* CC: device; IOTLB: page */
#define VTD_QI_IOTLB_DR     (1ull << 7)
#define VTD_QI_IOTLB_DW     (1ull << 6)
#define VTD_QI_WAIT_SW      (1ull << 5)
#define VTD_QI_WAIT_FN      (1ull << 6)

/* Second-level entries. */
#define VTD_SL_R            (1ull << 0)
#define VTD_SL_W            (1ull << 1)
#define VTD_SL_PS           (1ull << 7)
#define VTD_SL_ADDR         0x000FFFFFFFFFF000ull

/* Fault reasons (VT-d spec 7.1.3, legacy mode). */
#define VTD_FR_ROOT_NP      0x01
#define VTD_FR_CTX_NP       0x02
#define VTD_FR_CTX_INV      0x03
#define VTD_FR_BEYOND_AGAW  0x04
#define VTD_FR_WRITE        0x05
#define VTD_FR_READ         0x06
#define VTD_FR_PT_ACCESS    0x07

/* ----------------------------------------------------- per-domain state */
typedef struct vtd_dom {
    uint16_t did;                 /* hardware domain id (never 0)          */
    uint8_t  levels;              /* 3 (39-bit) or 4 (48-bit)              */
    uint8_t  pt;                  /* context TT=10b, no page tables        */
    uint64_t root;                /* top second-level table (phys), or 0   */
    uint64_t tables;              /* table pages owned                     */
    volatile uint32_t unit_refs[DMAR_MAX_DRHD];  /* contexts per unit       */
} vtd_dom_t;

/* ---------------------------------------- shared with vtd_pt.c (tables) */
/* Allocation limit for table pages (2^haw: the IOMMU cannot address frames
 * above the host address width) and whether table writes need clflush
 * (some unit has ECAP.C == 0). */
extern uint64_t g_vtd_table_limit;
extern int      g_vtd_clflush;

uint64_t vtd_table_alloc(void);           /* zeroed frame below the limit, or 0 */
void     vtd_table_free(uint64_t phys);
void     vtd_flush_cache(const void* p, uint64_t len);

/* Second-level tables. `huge` = SLLPS-style bits: 1 = 2 MiB, 2 = 1 GiB pages
 * allowed. `idem` lets an existing identical mapping stand (identity build,
 * RMRRs). Return 0 or IOMMU_E*. */
int      vtd_pt_init(vtd_dom_t* h, int levels);
int      vtd_pt_map(vtd_dom_t* h, uint64_t iova, uint64_t phys, uint64_t len,
                    int prot, uint32_t huge, int idem);
/* Clears leaves in [iova, iova+len); *cleared gets the 4 KiB page count. A
 * superpage only partly inside the range refuses the unmap (IOMMU_EINVAL)
 * before anything changes. */
int      vtd_pt_unmap(vtd_dom_t* h, uint64_t iova, uint64_t len, uint64_t* cleared);
uint64_t vtd_pt_translate(vtd_dom_t* h, uint64_t iova, int* prot);
void     vtd_pt_free(vtd_dom_t* h);

void vtd_probe(void);   /* kernel/iommu.c calls this from iommu_init */

/* ------------------------- unit access for arch/x86/vtd_ir.c (M56) */
/* Interrupt-remapping fault reasons (0x20..0x27). */
#define VTD_FR_IR_FIRST     0x20
#define VTD_FR_IR_LAST      0x2F

int      vtd_unit_count(void);                 /* slots, used or not        */
int      vtd_unit_used(int i);                 /* probed, kept, translating */
uint64_t vtd_unit_ecap(int i);
int      vtd_unit_has_qi(int i);               /* queued invalidation in use */
uint32_t vtd_unit_index(int i);                /* DRHD index (logs)          */
uint32_t vtd_unit_gsts(int i);
/* Under the unit lock: write IRTA, latch it (SIRTP), global IEC + wait. */
int      vtd_unit_set_irta(int i, uint64_t irta);
/* Under the unit lock: set/clear a persistent GCMD bit, wait for GSTS. */
int      vtd_unit_gcmd(int i, uint32_t bit, int on);
/* Under the unit lock: queue one IEC descriptor (global when count == 0,
 * else index-selective for [index, index+count), count a power of two
 * aligned to it) and wait for it. */
int      vtd_unit_iec(int i, uint32_t index, uint32_t count);
/* The unit (slot index) that remaps this function, or -1. */
int      vtd_unit_for_dev(const pci_dev_t* dev);
/* The unit whose DMAR scope names the IOAPIC / HPET source id, or -1. */
int      vtd_unit_for_sid(uint16_t seg, uint16_t sid);
/* The requester id a function's messages carry: *svt = 1 (exact SID) or
 * 2 (bus range: *sid = start bus << 8 | end bus, for a PCIe-to-PCI bridge
 * alias). */
void     vtd_dev_requester(const pci_dev_t* dev, uint16_t* sid, uint8_t* svt);

/* arch/x86/vtd_ir.c (M56). */
int      vtd_ir_init(void);                    /* after vtd_enable           */
int      vtd_ir_compose(const pci_dev_t* dev, int vec, uint32_t cpu,
                        uint64_t* addr, uint32_t* data);
int      vtd_ir_enabled(void);
int      vtd_ir_covers(const pci_dev_t* dev);
void     vtd_ir_free(int vec);
int      vtd_ir_set_cpu(int vec, uint32_t cpu);
void     vtd_ir_generate(void (*emit)(void* ctx, const char* line), void* ctx);
const char* vtd_ir_fault_reason(uint8_t r);

#endif /* VTD_H */
