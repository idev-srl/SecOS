/*
 * pci_msi.h — MSI / MSI-X programming (M53c; moved out of drivers/pci.c).
 *
 * The vector comes from the dynamic allocator (arch/x86/irq.h) and the target
 * is a CPU index; msi_compose() turns (vector, CPU) into the message
 * address/data pair. It is the one place that knows the message format, so
 * M56 (interrupt remapping) replaces only it with the remappable format.
 *
 * MSI-X: pci_msix_enable() maps the table (bounds-checked against the BAR),
 * masks every entry and turns MSI-X on. pci_msix_set() programs one entry
 * and keeps its mask state; pci_msix_unmask() lets it fire. mask/unmask are
 * a single MMIO read-modify-write of the entry's vector-control word, safe
 * from interrupt context (no lock).
 *
 * MSI: pci_msi_enable() programs `nvec` messages (a power of two the device
 * supports; `vec` must be a block aligned to nvec, see irq_vec_alloc_block)
 * and turns MSI on. pci_msi_mask/unmask need per-vector masking (PVM) and
 * return -1 without it.
 *
 * Device registers are untrusted: table offsets and sizes are checked against
 * the BAR before anything is mapped. All calls return 0 on success, -1 on
 * failure, and leave the device as it was when they fail before enabling.
 *
 * Locking: g_msi_lock (LK_S58_MSI, IRQ-safe) around the per-device records and
 * the control-word read-modify-writes; MSI-X mask/unmask take no lock.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PCI_MSI_H
#define PCI_MSI_H
#include <stdint.h>
#include "pci.h"

/* The LAPIC message-address window (fixed delivery, physical destination). */
#define MSI_ADDR_BASE       0xFEE00000u

/* Compose the message for `vec` delivered to CPU index `cpu` (IRQ_CPU_ANY =
 * the allocator's choice). Without interrupt remapping the destination field
 * is 8 bits, so a CPU whose APIC ID is above 254 cannot be targeted: -1.
 * `d` identifies the requester (unused until M56). */
int msi_compose(const pci_device_t* d, uint8_t vec, uint32_t cpu,
                uint64_t* addr, uint32_t* data);

/* ---- MSI-X ---- */
int pci_msix_table_size(const pci_device_t* d);      /* entries, or -1 */
int pci_msix_enable(const pci_device_t* d, int nvec);
int pci_msix_set(const pci_device_t* d, int entry, uint8_t vec, uint32_t cpu);
int pci_msix_mask(const pci_device_t* d, int entry);
int pci_msix_unmask(const pci_device_t* d, int entry);
int pci_msix_disable(const pci_device_t* d);

/* ---- MSI ---- */
int pci_msi_max_vectors(const pci_device_t* d);      /* 1..32, or -1 */
int pci_msi_has_pvm(const pci_device_t* d);          /* 1, 0, or -1 */
int pci_msi_enable(const pci_device_t* d, int nvec, uint8_t vec, uint32_t cpu);
int pci_msi_mask(const pci_device_t* d, int idx);
int pci_msi_unmask(const pci_device_t* d, int idx);
int pci_msi_disable(const pci_device_t* d);

/* Common driver path: one interrupt on entry/message 0 targeted at `cpu`.
 * Tries MSI-X (enable, set entry 0, unmask), then single-message MSI.
 * Returns 1 for MSI-X, 2 for MSI, -1 if the device has neither (or the
 * target cannot be addressed). */
int pci_irq_setup_single(const pci_device_t* d, uint8_t vec, uint32_t cpu);

#endif /* PCI_MSI_H */
