/*
 * SecOS Kernel - ACPI table discovery (Phase I, M28-1)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Read-only discovery of the platform topology from ACPI: the CPU list + Local
 * APIC IDs, the LAPIC base address, and the IOAPIC(s), parsed from the MADT.
 * This is the prerequisite for retiring the 8259 PIC (APIC/IOAPIC, M28-2) and
 * for bringing up the other cores (SMP, M29). It does NOT touch the live
 * interrupt path. RSDP is located by scanning the legacy BIOS area (works under
 * SeaBIOS/QEMU and most OVMF setups); a future UEFI hand-off can pass it directly.
 */
#ifndef ACPI_H
#define ACPI_H
#include <stdint.h>

#define ACPI_MAX_CPUS    64
#define ACPI_MAX_IOAPICS 4
#define ACPI_MAX_OVERRIDES 16

typedef struct {
    int      found;                       /* ACPI tables parsed OK              */
    uint64_t lapic_base;                  /* Local APIC MMIO physical address   */
    uint32_t cpu_count;                   /* enabled processors (LAPICs)        */
    uint8_t  lapic_ids[ACPI_MAX_CPUS];    /* per-CPU Local APIC IDs             */
    uint32_t bsp_lapic_id;                /* this (boot) CPU's LAPIC ID         */
    uint32_t ioapic_count;
    struct { uint8_t id; uint64_t base; uint32_t gsi_base; } ioapics[ACPI_MAX_IOAPICS];
    uint32_t override_count;
    struct { uint8_t src_irq; uint32_t gsi; uint16_t flags; } overrides[ACPI_MAX_OVERRIDES];
} acpi_topology_t;

/* Parse ACPI tables into the global topology. rsdp_hint is the RSDP physical
 * address from the UEFI loader (0 = unknown, scan the BIOS area). Returns 0 on
 * success, -1 if no usable ACPI/MADT was found (keep the legacy PIC/PIT path). */
int acpi_init(uint64_t rsdp_hint);

/* The discovered topology (valid after acpi_init() returns 0). */
const acpi_topology_t* acpi_get(void);

/* Map a legacy ISA IRQ to its Global System Interrupt, applying MADT interrupt
 * source overrides (identity by default). Used to program the IOAPIC. */
uint32_t acpi_irq_to_gsi(uint8_t irq, uint16_t* flags_out);

#endif /* ACPI_H */
