/*
 * SecOS Kernel - Minimal PCI configuration-space access
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "pci.h"
#include "io.h"

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

void pci_enable_io_and_busmaster(const pci_device_t* d) {
    uint16_t cmd = pci_config_read16(d->bus, d->slot, d->func, PCI_CFG_COMMAND);
    cmd |= (PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER);
    pci_config_write16(d->bus, d->slot, d->func, PCI_CFG_COMMAND, cmd);
}

uint16_t pci_bar0_io_base(const pci_device_t* d) {
    uint32_t bar = pci_config_read32(d->bus, d->slot, d->func, PCI_CFG_BAR0);
    if (!(bar & 0x1)) return 0;          /* not an I/O BAR */
    return (uint16_t)(bar & 0xFFFCu);
}
