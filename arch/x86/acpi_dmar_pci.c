/*
 * SecOS Kernel - PCI config access for DMAR scope resolution (M53b)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * The one place where acpi_dmar.c touches PCI: device-scope paths are
 * resolved by reading bridge secondary-bus numbers. Today this is a direct
 * legacy config read (segment 0 only, CF8/CFC through drivers/pci.c). When
 * the M53a device registry lands, this function switches to it (ECAM reaches
 * other segments, and the registry already knows every bridge), and
 * acpi_dmar.c stays unchanged. Kept out of acpi_dmar.c so the host fuzz
 * harness can supply its own topology.
 */
#include "acpi_dmar.h"
#include "pci.h"

uint32_t acpi_dmar_cfg_read32(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn,
                              uint16_t off){
    if(seg != 0 || dev > 31 || fn > 7 || off > 0xFC || (off & 3)) return 0xFFFFFFFFu;
    return pci_config_read32(bus, dev, fn, (uint8_t)off);
}
