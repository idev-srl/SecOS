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

/* Enable I/O space + bus mastering for a located device. */
void pci_enable_io_and_busmaster(const pci_device_t* d);

/* Read BAR0 and return the I/O port base (low bits masked off).
 * Only valid when BAR0 is an I/O BAR (bit0 == 1). */
uint16_t pci_bar0_io_base(const pci_device_t* d);
