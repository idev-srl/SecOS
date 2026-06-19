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

/* ---- M28-2: APIC switchover (LAPIC timer + IOAPIC, retire the 8259/PIT) ----
 *
 * apic_switchover() promotes the system off the legacy 8259 PIC + PIT IRQ0 onto
 * the Local APIC timer (scheduler tick) and the IOAPIC (device IRQs). It needs a
 * parsed MADT (acpi_get()->found with at least one IOAPIC); if that is missing it
 * leaves the kernel on PIC/PIT and returns -1. On success the LAPIC timer fires
 * the existing vector 0x20 (isr_timer) at `hz` and the keyboard (ISA IRQ1) is
 * routed through the IOAPIC to vector 0x21 (isr_keyboard); the 8259 is masked.
 * Returns 0 if the APIC now owns interrupts, -1 if we stayed on PIC/PIT. */
int apic_switchover(uint32_t hz);

/* True once apic_switchover() has put the system in symmetric-I/O (APIC) mode. */
int apic_mode_active(void);

/* This CPU's Local APIC ID (bits 31:24 of the ID register). 0 if the LAPIC is
 * not yet mapped (pre-switchover) — safe default for the single-core path. */
uint32_t lapic_get_id(void);

/* ---- M29 SMP: inter-processor interrupts via the LAPIC ICR ----
 * Send an INIT IPI then two STARTUP (SIPI) IPIs to bring an AP out of reset; the
 * AP begins executing at physical (vector << 12). lapic_send_init / lapic_send_sipi
 * target a destination LAPIC ID. lapic_ipi(dest, vector) sends a plain fixed IPI
 * (used for TLB shootdown / reschedule). All no-op if the LAPIC is not mapped. */
void lapic_send_init(uint32_t dest_apic_id);
void lapic_send_sipi(uint32_t dest_apic_id, uint8_t vector);
void lapic_ipi(uint32_t dest_apic_id, uint8_t vector);

/* Start this CPU's LAPIC timer at `hz` on the scheduler-tick vector (used by APs,
 * which calibrate off the BSP's already-computed period). Requires lapic_enable(). */
void lapic_timer_start_this_cpu(uint32_t hz);

/* End-of-interrupt for a level-0 hardware IRQ (timer/keyboard). Dispatches to the
 * LAPIC in APIC mode, or the 8259 master otherwise. Called from the ISR stubs and
 * the scheduler's preempt path so the EOI source tracks the active mode. */
void irq_eoi(void);

#endif /* LAPIC_H */
