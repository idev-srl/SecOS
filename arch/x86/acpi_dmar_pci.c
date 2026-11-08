/*
 * SecOS Kernel - PCI config access for DMAR scope resolution (M53b)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * The one place where acpi_dmar.c touches PCI: device-scope paths are
 * resolved by reading vendor / header type / class and bridge bus numbers.
 * [M53 integration] The reads go through the M53a registry and its config
 * path: a function the boot enumeration did not record is absent (unless the
 * registry overflowed, then the live read decides), and a recorded one is
 * read with pci_config_read32_ext() (ECAM when the self-check passed, else
 * CF8/CFC under pci_cfg_lock). acpi_dmar_init() runs after pci_init().
 * Segment 0 only, like the registry. Kept out of acpi_dmar.c so the host fuzz
 * harness can supply its own topology.
 */
#include "acpi_dmar.h"
#include "pci.h"

uint32_t acpi_dmar_cfg_read32(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn,
                              uint16_t off){
    if(seg != 0 || dev > 31 || fn > 7 || off > 0xFFC || (off & 3)) return 0xFFFFFFFFu;
    if(!pci_dev_lookup(seg, bus, dev, fn) && pci_registry_complete()) return 0xFFFFFFFFu;
    return pci_config_read32_ext(bus, dev, fn, off);
}
