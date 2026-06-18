/*
 * igc.h — Intel igc (I225/I226 2.5GbE) NIC driver.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Intel igc (I225/I226 2.5GbE) NIC — IMPLEMENTED BUT NOT YET TESTED (not
 * emulated by QEMU; validate on hardware).
 *
 * Drives an Intel Foxville I225 / I226 family 2.5 GbE controller (PCI vendor
 * 0x8086, device in the I225/I226 set). Maps BAR0 (64-bit MMIO) through the
 * physmap, resets the MAC, brings the link up, reads the station MAC from
 * RAL0/RAH0, and sets up one advanced-descriptor RX ring and one advanced TX
 * ring with static page-aligned DMA buffers (physical addresses via
 * kvirt_to_phys). Registers a net_dev_t with transmit + poll (NAPI-style RX
 * drain). The igc register model is an evolution of e1000e (advanced
 * descriptors). The core (net_request_irq) picks MSI-X / INTx / poll; this
 * driver only provides dev->poll.
 */
#ifndef IGC_H
#define IGC_H

/* Probe for an Intel I225/I226 NIC, bring it up, and register it with the net
 * core. Returns 0 on success, -1 if no supported controller is present or
 * bring-up fails. */
int igc_init(void);

#endif /* IGC_H */
