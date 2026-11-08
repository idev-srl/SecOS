/*
 * SecOS Kernel - Minimal PCI configuration-space access
 * Legacy port-mapped config mechanism (CONFIG_ADDRESS 0xCF8 / CONFIG_DATA 0xCFC).
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>

/* A located PCI function. */
typedef struct pci_device {
    uint8_t  bus;
    uint8_t  slot;     /* device number 0..31 */
    uint8_t  func;     /* function number 0..7 */
    uint16_t vendor_id;
    uint16_t device_id;
} pci_device_t;

/* Common PCI config-space offsets. */
#define PCI_CFG_VENDOR_ID   0x00
#define PCI_CFG_DEVICE_ID   0x02
#define PCI_CFG_COMMAND     0x04
#define PCI_CFG_STATUS      0x06
#define PCI_CFG_SUBSYS_ID   0x2E
#define PCI_CFG_INT_LINE    0x3C
#define PCI_CFG_BAR0        0x10

/* PCI command register bits. */
#define PCI_CMD_IO_SPACE    0x0001
#define PCI_CMD_MEM_SPACE   0x0002
#define PCI_CMD_BUS_MASTER  0x0004

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint8_t  pci_config_read8 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val);
void     pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t val);

/* Find the first function matching a (vendor, device) pair on bus 0..255.
 * If want_device is 0, matches any device id for that vendor.
 * Returns 0 and fills *out on success, -1 if not found. */
int pci_find(uint16_t want_vendor, uint16_t want_device, pci_device_t* out);

/* [M21] Find the first function matching a class/subclass/prog-if (config bytes
 * 0x0B/0x0A/0x09). Pass progif == 0xFF to match any prog-if. (AHCI = 01/06/01.)
 * Returns 0 and fills *out on success, -1 if not found. */
int pci_find_class(uint8_t cls, uint8_t subcls, uint8_t progif, pci_device_t* out);

/* Enable I/O space + bus mastering for a located device. */
void pci_enable_io_and_busmaster(const pci_device_t* d);

/* [M21] Enable memory space + bus mastering (for MMIO devices like AHCI). */
void pci_enable_mem_and_busmaster(const pci_device_t* d);

/* Read BAR0 and return the I/O port base (low bits masked off).
 * Only valid when BAR0 is an I/O BAR (bit0 == 1). */
uint16_t pci_bar0_io_base(const pci_device_t* d);

/* [M21] Read BAR[idx] (idx 0..5) as a 32-bit memory BAR base (low 4 bits masked).
 * Returns 0 if it is an I/O BAR. */
uint32_t pci_bar_mem(const pci_device_t* d, int idx);

/* [M22] Read BAR[idx] as a full 64-bit memory BAR base. If the BAR type bits
 * (1:2) mark it 64-bit, the high half is read from BAR[idx+1]. Low 4 bits are
 * masked. Returns 0 if BAR[idx] is an I/O BAR. NVMe/XHCI use 64-bit MMIO BARs. */
uint64_t pci_bar_mem64(const pci_device_t* d, int idx);

/* [M49] Size of memory BAR[idx] in bytes (0 = I/O / unimplemented). */
uint64_t pci_bar_size(const pci_device_t* d, int idx);
/* [M49] Map memory BAR[idx] uncached into the physmap (>= min_size bytes).
 * Returns the BAR's virtual address (phys_to_virt of its base) or 0. */
uint64_t pci_bar_map(const pci_device_t* d, int idx, uint64_t min_size);

/* ----------------------------------------------------------------------------
 * [M53a] PCI device registry.
 *
 * pci_init() enumerates every function once at boot (BSP only, before the APs
 * start) into a read-only table; pci_find / pci_find_class search it (same API
 * and the same first-match order as the old bus 0..255 brute scan). Config
 * space goes through ECAM (ACPI MCFG, segment 0) when the boot self-check
 * against CF8/CFC passes, else through CF8/CFC under pci_cfg_lock.
 * ------------------------------------------------------------------------- */

#define PCI_MAX_DEVS        256

/* BAR flags (pci_dev_t.bar_flags[i]). */
#define PCI_BAR_PRESENT     0x01   /* implemented (size > 0)                   */
#define PCI_BAR_IO          0x02   /* I/O space                                */
#define PCI_BAR_MEM64       0x04   /* 64-bit memory BAR (uses the next slot)   */
#define PCI_BAR_PREFETCH    0x08
#define PCI_BAR_UPPER       0x10   /* upper half of the previous 64-bit BAR    */

/* PCIe device/port type (PCIe cap +2, bits 7:4); PCI_EXP_NONE = conventional. */
#define PCI_EXP_ENDPOINT        0x0
#define PCI_EXP_LEG_ENDPOINT    0x1
#define PCI_EXP_ROOT_PORT       0x4
#define PCI_EXP_UPSTREAM        0x5
#define PCI_EXP_DOWNSTREAM      0x6
#define PCI_EXP_PCIE_TO_PCI     0x7
#define PCI_EXP_PCI_TO_PCIE     0x8
#define PCI_EXP_RC_END          0x9
#define PCI_EXP_RC_EC           0xA
#define PCI_EXP_NONE            0xFF

/* Standard / extended capability ids recorded by the registry. */
#define PCI_CAP_ID_PM       0x01
#define PCI_CAP_ID_PCIE     0x10
#define PCI_CAP_ID_AF       0x13
#define PCI_ECAP_ID_AER     0x0001
#define PCI_ECAP_ID_ACS     0x000D
#define PCI_ECAP_ID_ATS     0x000F
#define PCI_ECAP_ID_SRIOV   0x0010

/* ACS capability bits (ACS cap +4, the capability register; control = +6). */
#define PCI_ACS_SV          0x0001  /* source validation        */
#define PCI_ACS_TB          0x0002  /* translation blocking     */
#define PCI_ACS_RR          0x0004  /* P2P request redirect     */
#define PCI_ACS_CR          0x0008  /* P2P completion redirect  */
#define PCI_ACS_UF          0x0010  /* upstream forwarding      */
#define PCI_ACS_ISOLATE     (PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF)

/* FLR support (pci_dev_t.flr). */
#define PCI_FLR_PCIE        0x01    /* PCIe DevCap bit 28          */
#define PCI_FLR_AF          0x02    /* Advanced Features cap (TP+FLR) */

/* Why a function is not assignable (pci_dev_t.why). */
#define PCI_ASSIGN_OK           0
#define PCI_ASSIGN_BRIDGE       1   /* a bridge / host bridge itself           */
#define PCI_ASSIGN_CONV_BRIDGE  2   /* behind a conventional (or PCIe-to-PCI)
                                       bridge: shares the bridge's requester ID */
#define PCI_ASSIGN_NO_ACS       3   /* an upstream port lacks ACS isolation    */
#define PCI_ASSIGN_MF_NO_ACS    4   /* multifunction sibling without ACS       */
#define PCI_ASSIGN_ORPHAN       5   /* bus not reached by any bridge (bad tree)*/
#define PCI_ASSIGN_IOMMU        6   /* the IOMMU itself (class 08/06, AMD-Vi) */

typedef struct pci_dev {
    uint16_t seg;
    uint8_t  bus, slot, func;
    uint8_t  header_type;      /* config 0x0E bits 6:0 (0 device, 1 bridge, 2 CardBus) */
    uint8_t  multifunc;        /* function 0 of the slot has bit 7 set */
    uint8_t  class_code, subclass, prog_if, revision;
    uint16_t vendor_id, device_id;
    uint16_t subsys_vendor, subsys_id;
    uint8_t  irq_line, irq_pin;
    uint8_t  nbars;            /* 6 (device), 2 (bridge), 1 (CardBus) */
    uint8_t  bar_flags[6];
    uint64_t bar_base[6];
    uint64_t bar_size[6];
    /* standard capabilities (config offsets, 0 = absent) */
    uint8_t  cap_pm, cap_msi, cap_msix, cap_pcie, cap_af;
    uint8_t  pcie_type;        /* PCI_EXP_*                              */
    /* extended capabilities (ECAM only; offsets 0x100.., 0 = absent)    */
    uint16_t ecap_aer, ecap_acs, ecap_ats, ecap_sriov;
    uint16_t acs_cap, acs_ctrl;
    uint8_t  flr;              /* PCI_FLR_*                              */
    /* bridges (header type 1/2) */
    uint8_t  sec_bus, sub_bus;
    int16_t  parent;           /* registry index of the upstream bridge, -1 = root bus */
    /* isolation (M57) */
    uint8_t  assignable;       /* 1 = may be given to a ring-3 driver alone */
    uint8_t  why;              /* PCI_ASSIGN_*                            */
    int16_t  group;            /* isolation group = registry index of its representative */
} pci_dev_t;

/* Enumerate (idempotent; pci_find* call it on first use). Call after
 * acpi_init() so ECAM is found. */
void pci_init(void);

int              pci_dev_count(void);
const pci_dev_t* pci_dev_at(int idx);
const pci_dev_t* pci_dev_lookup(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func);
const pci_dev_t* pci_dev_of(const pci_device_t* d);
/* The bridge whose secondary bus is `bus` (NULL = root bus or unknown). */
const pci_dev_t* pci_bridge_for_bus(uint16_t seg, uint8_t bus);
int              pci_dev_index(const pci_dev_t* d);

/* 1 if extended config space (ECAM) is in use. */
int      pci_ecam_active(void);
/* Extended config read (off 0..0xFFF, dword aligned). 0xFFFFFFFF without ECAM. */
uint32_t pci_config_read32_ext(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off);

/* Short reason name for pci_dev_t.why. */
const char* pci_assign_reason(uint8_t why);

/* ----------------------------------------------------------------------------
 * Capabilities. [M53c] MSI / MSI-X programming lives in drivers/pci_msi.h.
 * ------------------------------------------------------------------------- */

/* PCI status register bit 4 (0x10): capability list present at cfg 0x34. */
#define PCI_STATUS_CAP_LIST 0x10
#define PCI_CFG_CAP_PTR     0x34

/* PCI capability IDs we care about. */
#define PCI_CAP_ID_MSI      0x05
#define PCI_CAP_ID_MSIX     0x11

/* Walk the PCI capability list and return the config-space offset of the
 * capability with the given id, or 0 if absent. */
uint8_t pci_find_capability(const pci_device_t* d, uint8_t cap_id);
