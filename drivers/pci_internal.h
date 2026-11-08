/*
 * SecOS Kernel - PCI internals shared by drivers/pci.c and drivers/pci_registry.c
 * (M53a). Not for drivers: they use pci.h.
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>

/* CF8/CFC dword read under pci_cfg_lock, whatever mechanism is active. */
uint32_t pci_cf8_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
/* ECAM dword read through a candidate window (the boot self-check). */
uint32_t pci_ecam_probe_read32(uint64_t virt_bus0, uint8_t bus, uint8_t slot,
                               uint8_t func, uint16_t off);
/* Route config accesses for segment 0 buses [start_bus, end_bus] through ECAM. */
void     pci_ecam_enable(uint64_t virt_bus0, uint8_t start_bus, uint8_t end_bus);
