/*
 * SecOS Kernel - Minimal PCI configuration-space access
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "pci.h"
#include "io.h"
#include "vmm.h"        /* phys_to_virt / vmm_extend_physmap (MSI-X table map) */
#include "debugcon.h"   /* [MSIX] markers */

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

/* Build the CONFIG_ADDRESS dword: enable bit + bus/slot/func + aligned offset. */
static inline uint32_t cfg_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (uint32_t)(((uint32_t)1u << 31) |
                      ((uint32_t)bus  << 16) |
                      ((uint32_t)(slot & 0x1F) << 11) |
                      ((uint32_t)(func & 0x07) << 8) |
                      ((uint32_t)off & 0xFC));
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    io_outl(PCI_CONFIG_ADDRESS, cfg_addr(bus, slot, func, off));
    return io_inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_config_read32(bus, slot, func, off);
    return (uint16_t)((v >> ((off & 2) * 8)) & 0xFFFF);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_config_read32(bus, slot, func, off);
    return (uint8_t)((v >> ((off & 3) * 8)) & 0xFF);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val) {
    io_outl(PCI_CONFIG_ADDRESS, cfg_addr(bus, slot, func, off));
    io_outl(PCI_CONFIG_DATA, val);
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t val) {
    /* Read-modify-write the containing dword to preserve the other half. */
    uint32_t cur = pci_config_read32(bus, slot, func, off);
    uint32_t shift = (off & 2) * 8;
    cur &= ~(0xFFFFu << shift);
    cur |= ((uint32_t)val << shift);
    pci_config_write32(bus, slot, func, off, cur);
}

int pci_find(uint16_t want_vendor, uint16_t want_device, pci_device_t* out) {
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint16_t vendor = pci_config_read16((uint8_t)bus, slot, 0, PCI_CFG_VENDOR_ID);
            if (vendor == 0xFFFF) continue; /* no device in this slot */
            uint8_t header = pci_config_read8((uint8_t)bus, slot, 0, 0x0E);
            uint8_t nfunc  = (header & 0x80) ? 8 : 1; /* multi-function? */
            for (uint8_t func = 0; func < nfunc; func++) {
                uint16_t v = pci_config_read16((uint8_t)bus, slot, func, PCI_CFG_VENDOR_ID);
                if (v == 0xFFFF) continue;
                uint16_t d = pci_config_read16((uint8_t)bus, slot, func, PCI_CFG_DEVICE_ID);
                if (v == want_vendor && (want_device == 0 || d == want_device)) {
                    if (out) {
                        out->bus = (uint8_t)bus; out->slot = slot; out->func = func;
                        out->vendor_id = v; out->device_id = d;
                    }
                    return 0;
                }
            }
        }
    }
    return -1;
}

int pci_find_class(uint8_t cls, uint8_t subcls, uint8_t progif, pci_device_t* out) {
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint16_t vendor = pci_config_read16((uint8_t)bus, slot, 0, PCI_CFG_VENDOR_ID);
            if (vendor == 0xFFFF) continue;
            uint8_t header = pci_config_read8((uint8_t)bus, slot, 0, 0x0E);
            uint8_t nfunc  = (header & 0x80) ? 8 : 1;
            for (uint8_t func = 0; func < nfunc; func++) {
                uint16_t v = pci_config_read16((uint8_t)bus, slot, func, PCI_CFG_VENDOR_ID);
                if (v == 0xFFFF) continue;
                uint8_t pif = pci_config_read8((uint8_t)bus, slot, func, 0x09);
                uint8_t sub = pci_config_read8((uint8_t)bus, slot, func, 0x0A);
                uint8_t bc  = pci_config_read8((uint8_t)bus, slot, func, 0x0B);
                if (bc == cls && sub == subcls && (progif == 0xFF || pif == progif)) {
                    if (out) {
                        out->bus = (uint8_t)bus; out->slot = slot; out->func = func;
                        out->vendor_id = v;
                        out->device_id = pci_config_read16((uint8_t)bus, slot, func, PCI_CFG_DEVICE_ID);
                    }
                    return 0;
                }
            }
        }
    }
    return -1;
}

void pci_enable_io_and_busmaster(const pci_device_t* d) {
    uint16_t cmd = pci_config_read16(d->bus, d->slot, d->func, PCI_CFG_COMMAND);
    cmd |= (PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER);
    pci_config_write16(d->bus, d->slot, d->func, PCI_CFG_COMMAND, cmd);
}

void pci_enable_mem_and_busmaster(const pci_device_t* d) {
    uint16_t cmd = pci_config_read16(d->bus, d->slot, d->func, PCI_CFG_COMMAND);
    cmd |= (PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);
    pci_config_write16(d->bus, d->slot, d->func, PCI_CFG_COMMAND, cmd);
}

uint16_t pci_bar0_io_base(const pci_device_t* d) {
    uint32_t bar = pci_config_read32(d->bus, d->slot, d->func, PCI_CFG_BAR0);
    if (!(bar & 0x1)) return 0;          /* not an I/O BAR */
    return (uint16_t)(bar & 0xFFFCu);
}

uint32_t pci_bar_mem(const pci_device_t* d, int idx) {
    if (idx < 0 || idx > 5) return 0;
    uint32_t bar = pci_config_read32(d->bus, d->slot, d->func, (uint8_t)(PCI_CFG_BAR0 + idx * 4));
    if (bar & 0x1) return 0;             /* I/O BAR, not memory */
    return bar & 0xFFFFFFF0u;            /* mask type/prefetch bits */
}

uint64_t pci_bar_mem64(const pci_device_t* d, int idx) {
    if (idx < 0 || idx > 5) return 0;
    uint32_t lo = pci_config_read32(d->bus, d->slot, d->func, (uint8_t)(PCI_CFG_BAR0 + idx * 4));
    if (lo & 0x1) return 0;                       /* I/O BAR, not memory */
    uint64_t base = (uint64_t)(lo & 0xFFFFFFF0u); /* mask type/prefetch bits */
    if (((lo >> 1) & 0x3) == 0x2 && idx < 5) {    /* type 10b => 64-bit BAR */
        uint32_t hi = pci_config_read32(d->bus, d->slot, d->func, (uint8_t)(PCI_CFG_BAR0 + (idx + 1) * 4));
        base |= (uint64_t)hi << 32;
    }
    return base;
}

/* ============================================================================
 * MSI / MSI-X interrupt support.
 *
 * MSI-X interrupt support — IMPLEMENTED BUT NOT YET TESTED / not enabled by
 * default (the kernel is polled). Programming the device-side capability is
 * correct per the PCI spec, but a delivered interrupt also requires the LAPIC
 * (arch/x86/lapic.c) and validation on real hardware / QEMU with the *_USE_IRQ
 * build flags. None of the routines below run in the default build.
 * ========================================================================== */

uint8_t pci_find_capability(const pci_device_t* d, uint8_t cap_id) {
    uint16_t status = pci_config_read16(d->bus, d->slot, d->func, PCI_CFG_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) return 0;       /* no capability list */
    uint8_t ptr = pci_config_read8(d->bus, d->slot, d->func, PCI_CFG_CAP_PTR) & 0xFC;
    /* Bounded walk: a malformed/looping list must never hang the probe. */
    for (int guard = 0; ptr != 0 && guard < 48; guard++) {
        uint8_t id   = pci_config_read8(d->bus, d->slot, d->func, ptr);
        uint8_t next = pci_config_read8(d->bus, d->slot, d->func, (uint8_t)(ptr + 1)) & 0xFC;
        if (id == cap_id) return ptr;
        ptr = next;
    }
    return 0;
}

/* Build the MSI message-data value for a fixed-mode, edge-triggered delivery of
 * the given vector. Trigger-mode / level bits are 0 (edge). */
static inline uint32_t msi_message_data(uint8_t vector) {
    return (uint32_t)vector;   /* delivery mode 000 (fixed), edge, deassert */
}

int pci_enable_msix(const pci_device_t* d, uint8_t vector) {
    uint8_t cap = pci_find_capability(d, PCI_CAP_ID_MSIX);
    if (cap == 0) return -1;

    /* Message Control is the 16-bit word at cap+2. Table size is bits 10:0 of
     * MC (encoded as size-1). The Table Offset/BIR dword is at cap+4. */
    uint16_t mc = pci_config_read16(d->bus, d->slot, d->func, (uint8_t)(cap + 2));
    uint32_t tbl = pci_config_read32(d->bus, d->slot, d->func, (uint8_t)(cap + 4));
    uint8_t  bir = (uint8_t)(tbl & 0x7);
    uint32_t off = tbl & ~0x7u;

    /* Locate the BAR that holds the MSI-X table (can be 32- or 64-bit). */
    uint64_t bar = pci_bar_mem64(d, bir);
    if (bar == 0) {
        debugcon_writestring("[MSIX] table BAR not memory, cap=0x");
        debugcon_print_hex(cap); debugcon_writestring("\n");
        return -1;
    }

    uint64_t tbl_phys = bar + off;
    /* Map the table window through the physmap (one entry = 16 bytes; map a page
     * to be safe — the table can hold many entries). */
    vmm_extend_physmap(tbl_phys + 0x1000);
    volatile uint32_t* entry0 = (volatile uint32_t*)phys_to_virt(tbl_phys);

    /* MSI-X table entry layout (16 bytes): msg addr lo, msg addr hi, msg data,
     * vector control (bit0 = mask). Program entry 0. */
    entry0[0] = MSI_ADDR_BASE;        /* LAPIC, physical dest id 0 (BSP) */
    entry0[1] = 0;                    /* upper 32 bits of message address */
    entry0[2] = msi_message_data(vector);
    entry0[3] = 0;                    /* vector control: mask bit cleared = unmasked */
    io_mfence();

    /* Set MSI-X Enable (bit 15) and clear Function Mask (bit 14) in MC. */
    mc |= (1u << 15);
    mc &= ~(1u << 14);
    pci_config_write16(d->bus, d->slot, d->func, (uint8_t)(cap + 2), mc);

    debugcon_writestring("[MSIX] cap found at 0x"); debugcon_print_hex(cap);
    debugcon_writestring(" bir="); debugcon_print_hex(bir);
    debugcon_writestring(" tblphys=0x"); debugcon_print_hex(tbl_phys);
    debugcon_writestring(" vector=0x"); debugcon_print_hex(vector);
    debugcon_writestring(" (NOT TESTED, polled default)\n");
    return 0;
}

int pci_enable_msi(const pci_device_t* d, uint8_t vector) {
    uint8_t cap = pci_find_capability(d, PCI_CAP_ID_MSI);
    if (cap == 0) return -1;

    /* MSI capability layout: MC at cap+2; if MC bit7 (64-bit addr capable) is
     * set, message data lives at cap+0x0C, else at cap+0x08. Address dword at
     * cap+4 (low), cap+8 (high, only if 64-bit). */
    uint16_t mc = pci_config_read16(d->bus, d->slot, d->func, (uint8_t)(cap + 2));
    int addr64 = (mc & (1u << 7)) != 0;

    pci_config_write32(d->bus, d->slot, d->func, (uint8_t)(cap + 4), MSI_ADDR_BASE);
    if (addr64) {
        pci_config_write32(d->bus, d->slot, d->func, (uint8_t)(cap + 8), 0);
        pci_config_write16(d->bus, d->slot, d->func, (uint8_t)(cap + 0x0C),
                           (uint16_t)msi_message_data(vector));
    } else {
        pci_config_write16(d->bus, d->slot, d->func, (uint8_t)(cap + 8),
                           (uint16_t)msi_message_data(vector));
    }

    /* MC bits 6:4 = Multiple Message Enable; force 1 message. Bit0 = MSI Enable. */
    mc &= ~(0x7u << 4);
    mc |= (1u << 0);
    pci_config_write16(d->bus, d->slot, d->func, (uint8_t)(cap + 2), mc);

    debugcon_writestring("[MSIX] MSI cap found at 0x"); debugcon_print_hex(cap);
    debugcon_writestring(" addr64="); debugcon_print_hex((uint64_t)addr64);
    debugcon_writestring(" vector=0x"); debugcon_print_hex(vector);
    debugcon_writestring(" (NOT TESTED, polled default)\n");
    return 0;
}
