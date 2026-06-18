/*
 * vmxnet3.h — VMware vmxnet3 paravirtual NIC driver.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * VMware vmxnet3 NIC — IMPLEMENTED BUT NOT YET TESTED (QEMU has no vmxnet3;
 * validate on VMware).
 */
#pragma once

/* Probe the VMware vmxnet3 NIC (PCI 0x15AD:0x07B0), bring it up, and register it
 * with the network core. Returns 0 on success, -1 if no device / setup failure. */
int vmxnet3_init(void);
