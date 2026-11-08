/*
 * SecOS Kernel - Minimal PCI configuration-space access
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M53a] Config space goes through one of two mechanisms:
 *  - CF8/CFC (legacy port pair). The address write and the data access are two
 *    port operations, so every pair runs under pci_cfg_lock (it used to have no
 *    lock: two CPUs could interleave and read another function's register).
 *  - ECAM (memory-mapped, from the ACPI MCFG table, segment 0 only). One MMIO
 *    access per operation, so no lock is needed. The registry
 *    (drivers/pci_registry.c) switches it on only after a boot self-check
 *    compared it with CF8/CFC for every function; it also reaches extended
 *    config space (0x100-0xFFF).
 * The device table and pci_find* live in drivers/pci_registry.c.
 */
#include "pci.h"
#include "pci_internal.h"
#include "io.h"
#include "vmm.h"        /* vmm_map_mmio (BAR + MSI-X table maps) */
#include "debugcon.h"   /* [MSIX] markers */
#include "spinlock.h"

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

/* [M53a] Leaf lock: nothing is taken inside it; IRQ-safe. */
static spinlock_t pci_cfg_lock = SPINLOCK_INIT_CLASS(LK_S64_LEAF, "pci_cfg");

/* ECAM window for segment 0 (set by pci_ecam_enable after the self-check).
 * g_ecam_virt is the virtual address bus 0 would have; only buses in
 * [g_ecam_start, g_ecam_end] are mapped and used. */
static uint64_t g_ecam_virt;
static uint8_t  g_ecam_start, g_ecam_end;
static volatile int g_ecam_on;

/* Build the CONFIG_ADDRESS dword: enable bit + bus/slot/func + aligned offset. */
static inline uint32_t cfg_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (uint32_t)(((uint32_t)1u << 31) |
                      ((uint32_t)bus  << 16) |
                      ((uint32_t)(slot & 0x1F) << 11) |
                      ((uint32_t)(func & 0x07) << 8) |
                      ((uint32_t)off & 0xFC));
}

static inline volatile uint32_t* ecam_at(uint64_t virt_bus0, uint8_t bus, uint8_t slot,
                                         uint8_t func, uint16_t off) {
    return (volatile uint32_t*)(uintptr_t)(virt_bus0 + ((uint64_t)bus << 20) +
                                           ((uint64_t)(slot & 0x1F) << 15) +
                                           ((uint64_t)(func & 0x07) << 12) +
                                           (uint64_t)(off & 0xFFC));
}

/* ECAM dword of (bus, slot, func, off), or NULL when ECAM does not cover it. */
static inline volatile uint32_t* ecam_ptr(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off) {
    if (!g_ecam_on || bus < g_ecam_start || bus > g_ecam_end || off > 0xFFF) return 0;
    return ecam_at(g_ecam_virt, bus, slot, func, off);
}

uint32_t pci_cf8_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint64_t fl = spin_lock_irqsave(&pci_cfg_lock);
    io_outl(PCI_CONFIG_ADDRESS, cfg_addr(bus, slot, func, off));
    uint32_t v = io_inl(PCI_CONFIG_DATA);
    spin_unlock_irqrestore(&pci_cfg_lock, fl);
    return v;
}

uint32_t pci_ecam_probe_read32(uint64_t virt_bus0, uint8_t bus, uint8_t slot,
                               uint8_t func, uint16_t off) {
    return *ecam_at(virt_bus0, bus, slot, func, off);
}

void pci_ecam_enable(uint64_t virt_bus0, uint8_t start_bus, uint8_t end_bus) {
    g_ecam_virt = virt_bus0;
    g_ecam_start = start_bus;
    g_ecam_end = end_bus;
    __asm__ volatile("" ::: "memory");
    g_ecam_on = 1;
}

int pci_ecam_active(void) { return g_ecam_on; }

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    volatile uint32_t* p = ecam_ptr(bus, slot, func, off);
    if (p) return *p;
    return pci_cf8_read32(bus, slot, func, off);
}

uint32_t pci_config_read32_ext(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off) {
    if (off > 0xFFF) return 0xFFFFFFFFu;
    volatile uint32_t* p = ecam_ptr(bus, slot, func, off);
    if (p) return *p;
    if (off < 0x100) return pci_cf8_read32(bus, slot, func, (uint8_t)off);
    return 0xFFFFFFFFu;
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
    volatile uint32_t* p = ecam_ptr(bus, slot, func, off);
    if (p) { *p = val; return; }
    uint64_t fl = spin_lock_irqsave(&pci_cfg_lock);
    io_outl(PCI_CONFIG_ADDRESS, cfg_addr(bus, slot, func, off));
    io_outl(PCI_CONFIG_DATA, val);
    spin_unlock_irqrestore(&pci_cfg_lock, fl);
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t val) {
    /* Read-modify-write the containing dword to preserve the other half (the
     * semantics both paths always had). On CF8 the whole RMW is one lock hold,
     * so no other config access slips in between. */
    uint32_t shift = (uint32_t)(off & 2) * 8;
    volatile uint32_t* p = ecam_ptr(bus, slot, func, off);
    if (p) {
        uint32_t cur = *p;
        cur &= ~(0xFFFFu << shift);
        cur |= ((uint32_t)val << shift);
        *p = cur;
        return;
    }
    uint64_t fl = spin_lock_irqsave(&pci_cfg_lock);
    io_outl(PCI_CONFIG_ADDRESS, cfg_addr(bus, slot, func, off));
    uint32_t cur = io_inl(PCI_CONFIG_DATA);
    cur &= ~(0xFFFFu << shift);
    cur |= ((uint32_t)val << shift);
    io_outl(PCI_CONFIG_ADDRESS, cfg_addr(bus, slot, func, off));
    io_outl(PCI_CONFIG_DATA, cur);
    spin_unlock_irqrestore(&pci_cfg_lock, fl);
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

/* [M49] Size memory BAR[idx] (standard all-ones probe, memory decode off while
 * probing so the device never sees the transient address). Returns the size in
 * bytes, 0 for an I/O or unimplemented BAR. */
uint64_t pci_bar_size(const pci_device_t* d, int idx) {
    if (idx < 0 || idx > 5) return 0;
    /* [M53a] The registry sized every BAR once at boot (same probe, interrupts
     * off, before the APs ran): serve that unless the BAR moved since. */
    const pci_dev_t* r = pci_dev_of(d);
    if (r && idx < r->nbars && (r->bar_flags[idx] & PCI_BAR_PRESENT) &&
        !(r->bar_flags[idx] & (PCI_BAR_IO | PCI_BAR_UPPER)) &&
        r->bar_base[idx] == pci_bar_mem64(d, idx))
        return r->bar_size[idx];
    uint8_t off = (uint8_t)(PCI_CFG_BAR0 + idx * 4);
    uint32_t lo = pci_config_read32(d->bus, d->slot, d->func, off);
    if (lo & 0x1) return 0;
    int is64 = (((lo >> 1) & 0x3) == 0x2) && idx < 5;
    uint32_t hi = is64 ? pci_config_read32(d->bus, d->slot, d->func, (uint8_t)(off + 4)) : 0;
    uint16_t cmd = pci_config_read16(d->bus, d->slot, d->func, PCI_CFG_COMMAND);
    pci_config_write16(d->bus, d->slot, d->func, PCI_CFG_COMMAND, (uint16_t)(cmd & ~0x2u));
    pci_config_write32(d->bus, d->slot, d->func, off, 0xFFFFFFFFu);
    uint32_t mlo = pci_config_read32(d->bus, d->slot, d->func, off);
    uint32_t mhi = 0xFFFFFFFFu;
    if (is64) {
        pci_config_write32(d->bus, d->slot, d->func, (uint8_t)(off + 4), 0xFFFFFFFFu);
        mhi = pci_config_read32(d->bus, d->slot, d->func, (uint8_t)(off + 4));
        pci_config_write32(d->bus, d->slot, d->func, (uint8_t)(off + 4), hi);
    }
    pci_config_write32(d->bus, d->slot, d->func, off, lo);
    pci_config_write16(d->bus, d->slot, d->func, PCI_CFG_COMMAND, cmd);
    uint64_t mask = ((uint64_t)mhi << 32) | (mlo & 0xFFFFFFF0u);
    if ((mlo & 0xFFFFFFF0u) == 0) return 0;
    return ~mask + 1;
}

/* [M49] Map memory BAR[idx] uncached into the physmap (the physmap maps RAM
 * only). At least `min_size` bytes are mapped (a register window the driver
 * knows it needs even if sizing reads back odd), at most 256 MiB. Returns the
 * virtual address of the BAR, 0 on failure. */
uint64_t pci_bar_map(const pci_device_t* d, int idx, uint64_t min_size) {
    uint64_t base = pci_bar_mem64(d, idx);
    if (!base) return 0;
    uint64_t sz = pci_bar_size(d, idx);
    if (sz < min_size) sz = min_size;
    if (sz > (256ULL << 20)) sz = 256ULL << 20;
    return vmm_map_mmio(base, sz, VMM_CACHE_UC);
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
    volatile uint32_t* entry0 = (volatile uint32_t*)vmm_map_mmio(tbl_phys, 0x1000, VMM_CACHE_UC);
    if (!entry0) return -1;

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
