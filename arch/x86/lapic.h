/*
 * lapic.h — Minimal Local APIC support for MSI/MSI-X interrupt delivery.
 *
 * MSI-X interrupt support — IMPLEMENTED BUT NOT YET TESTED / not enabled by
 * default (the kernel is polled). This kernel normally uses the legacy 8259
 * PIC + PIT for interrupts and never touches the LAPIC. Message-signalled
 * interrupts (MSI/MSI-X), however, can ONLY be delivered through the LAPIC, so
 * the gated interrupt-driven driver paths (built with XHCI_USE_IRQ /
 * NVME_USE_IRQ) call lapic_enable() to bring it up minimally and lapic_eoi()
 * to acknowledge. In the default build nothing here is invoked.
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef LAPIC_H
#define LAPIC_H
#include <stdint.h>

/* Minimally enable the Local APIC: map its MMIO page through the physmap, set
 * the spurious-interrupt-vector register APIC-enable bit. Idempotent. Returns 0
 * on success, -1 if the CPU reports no LAPIC. NOT YET TESTED on this kernel. */
int lapic_enable(void);

/* Signal End-Of-Interrupt to the LAPIC. Must be called from an MSI/MSI-X ISR
 * (NOT the PIC EOI). No-op if the LAPIC was never enabled. */
void lapic_eoi(void);

/* True once lapic_enable() has succeeded. */
int lapic_is_enabled(void);

#endif /* LAPIC_H */
