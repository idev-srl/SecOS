/*
 * iommu.h — vendor-neutral DMA remapping (Phase 5, M54; plan D1/D2).
 *
 * One interface over the platform's DMA-remapping hardware. Intel VT-d
 * (arch/x86/vtd*.c) implements it from M54; AMD-Vi (M64) plugs the same ops
 * in, so nothing above this header changes.
 *
 * A *domain* is one I/O address space. Kinds:
 *   IDOM_BLOCKED     no translation at all: every DMA faults. Detaching a
 *                    device puts it here (VT-d: context entry not present).
 *   IDOM_IDENTITY    IOVA == phys over all RAM (and the RMRRs), built with the
 *                    largest page size the hardware supports.
 *   IDOM_PT          passthrough: the hardware does not translate (VT-d
 *                    TT=10b); falls back to an identity domain on hardware
 *                    without ECAP.PT (or with the `iommu.pt=off` boot word).
 *   IDOM_TRANSLATED  starts empty; iommu_map()/iommu_unmap() fill it. Ring-3
 *                    drivers (M57) get one each, with IOVA != phys.
 *
 * Policy in M54 (boot word `iommu=on`, default off): every PCI function a
 * unit translates is attached to one shared IDOM_PT domain, so the in-kernel
 * drivers (which still hand physical addresses to their devices, until M55's
 * DMA API) keep working unchanged. Units the firmware describes badly (DMAR
 * checksum/haw wrong, a unit whose scopes were not all parsed, an iGPU-only
 * unit) are left untouched.
 *
 * Locking (kernel/lock_order.h):
 *   LK_S65_IOMMU_DEV   device -> domain table (attach/detach);
 *   LK_S67_IOMMU_DOM   one per domain, around its page-table edits and the
 *                      IOTLB flush that follows (heap S70 / PMM S75 inside);
 *   LK_S68_IOMMU_UNIT  one per hardware unit: registers, invalidation queue,
 *                      context tables, fault records. The fault interrupt
 *                      takes only this one.
 * All three are IRQ-safe spinlocks (irqsave), so iommu_map/unmap may be called
 * with tx_lock (N3), xhci_lock (S45) or any lower lock held, and from IRQ
 * context. Nothing in here sleeps.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef IOMMU_H
#define IOMMU_H
#include <stdint.h>
#include "pci.h"
#include "spinlock.h"

/* Domain kinds. */
#define IDOM_BLOCKED     0
#define IDOM_IDENTITY    1
#define IDOM_PT          2
#define IDOM_TRANSLATED  3

/* iommu_map() protection bits. */
#define IOMMU_READ       0x1
#define IOMMU_WRITE      0x2

/* Error returns (negative). */
#define IOMMU_EINVAL     (-22)
#define IOMMU_ENOMEM     (-12)
#define IOMMU_EBUSY      (-16)
#define IOMMU_EEXIST     (-17)
#define IOMMU_ERANGE     (-34)
#define IOMMU_ENODEV     (-19)
#define IOMMU_EIO        (-5)

typedef struct iommu_domain iommu_domain_t;

/* -------------------------------------------------------------- public API */
/* Boot: read `iommu=`, find and enable the units, attach every function to the
 * passthrough domain. Call once after pci_init(), acpi_dmar_init() and the
 * vector self-test, before any driver probes. Logs `[IOMMU] ...`. */
void iommu_init(void);

/* 1 if at least one DMA-remapping unit has translation enabled. */
int  iommu_present(void);
/* [M55] Replace the /proc/iommu state text (static string; only while on). */
void iommu_set_state(const char* st);

/* A new domain of `kind` (NULL on failure or with no IOMMU). */
iommu_domain_t* iommu_domain_new(int kind);
/* Free a domain with no device attached (IOMMU_EBUSY otherwise). Its page
 * tables are freed only after every unit finished flushing it. */
int  iommu_domain_free(iommu_domain_t* d);
int  iommu_domain_kind(const iommu_domain_t* d);
/* The shared passthrough domain of the M54 policy (NULL when off). */
iommu_domain_t* iommu_kernel_domain(void);

/* Largest IOVA a device in this domain may use (`mask`, e.g. 0x0FFFFFFF for
 * edu's 28-bit DMA). iommu_map() refuses anything above it; the default is
 * the hardware's input-address width. */
int      iommu_domain_set_dma_mask(iommu_domain_t* d, uint64_t mask);
/* A trivial bump IOVA allocator inside the mask (page granular, never
 * reused): 0 when the space is exhausted. A real allocator is M55's. */
uint64_t iommu_iova_alloc(iommu_domain_t* d, uint64_t len);

/* Map [iova, iova+len) -> [phys, ...) with `prot` (IOMMU_READ|IOMMU_WRITE).
 * Page aligned. Refuses (IOMMU_EEXIST) a range that is already mapped: an
 * existing translation is only ever changed by unmap + map. Only for
 * IDOM_TRANSLATED / IDOM_IDENTITY domains. The mapping is usable by the
 * device when this returns (caching-mode flushes done). */
int  iommu_map(iommu_domain_t* d, uint64_t iova, uint64_t phys, uint64_t len, int prot);
/* Unmap. Returns only after the IOTLB invalidation completed on every unit
 * the domain is attached to: a later DMA to the range faults, so the frames
 * may be freed or reused afterwards. */
int  iommu_unmap(iommu_domain_t* d, uint64_t iova, uint64_t len);
/* What `iova` maps to (phys, or (uint64_t)-1); *prot gets the bits. */
uint64_t iommu_translate(iommu_domain_t* d, uint64_t iova, int* prot);

/* Attach a function to `d` (moving it from its current domain: the old
 * context is invalidated before the new one is installed). RMRR regions
 * named for the device are identity-mapped into a translated domain. An
 * IDOM_BLOCKED domain is never attached (IOMMU_EINVAL): iommu_detach()
 * blocks a function. Functions behind a PCIe-to-PCI / PCI bridge share the
 * bridge's requester id, so they can only be in the same domain
 * (IOMMU_EBUSY otherwise). */
int  iommu_attach(iommu_domain_t* d, const pci_dev_t* dev);
/* Detach to the blocked domain; returns after the context-cache and IOTLB
 * invalidations completed (no DMA from the device is translated any more). */
void iommu_detach(const pci_dev_t* dev);
/* The domain a function is attached to (NULL = blocked / not translated). */
iommu_domain_t* iommu_dev_domain(const pci_dev_t* dev);
/* 1 if an enabled unit translates this function's DMA. */
int  iommu_dev_covered(const pci_dev_t* dev);

/* The MSI message for vector `vec` on CPU index `cpu` for `dev`: the
 * remappable format when interrupt remapping is on and covers `dev`, else
 * the plain compatibility format. */
int  iommu_irq_compose(const pci_dev_t* dev, int vec, uint32_t cpu,
                       uint64_t* addr, uint32_t* data);

/* ---------------------------------------------- interrupt remapping (M56)
 * Active only with `iommu=on` (and not `iommu.ir=off`) on hardware whose
 * every DMA-remapping unit has interrupt remapping + queued invalidation.
 * Then every MSI / MSI-X message (drivers/pci_msi.c msi_compose) and every
 * routed IOAPIC line (arch/x86/lapic.c) is in remappable format: one
 * interrupt-remapping table entry per vector, with source-id validation
 * (SVT=01b, the requester id; SVT=10b, a bus range, behind a PCIe-to-PCI
 * bridge; the DMAR scope id for an IOAPIC), compatibility-format interrupts
 * blocked (GSTS.CFIS clear), x2APIC destinations through EIM when the
 * hardware has it and the DMAR does not opt out. The IOMMU's own fault MSI
 * is never remapped. Freeing a vector (irq_vec_free) clears its entry: a
 * late message from its old source faults instead of being delivered. */

/* 1 when interrupt remapping is on (M57 requires it for ring-3 drivers
 * that do DMA). */
int  iommu_ir_enabled(void);
/* 1 when `dev`'s messages are remapped (IR on and its unit remaps). */
int  iommu_ir_covers(const pci_dev_t* dev);
/* irq_vec_free / irq_vec_set_cpu hooks (arch/x86/irq.c): no-ops when off. */
void iommu_ir_vec_free(int vec);
void iommu_ir_vec_set_cpu(int vec, uint32_t cpu);
/* For drivers/pci_msi.c msi_compose(): the remappable message for (dev,
 * vec, cpu). Returns 0 and fills addr/data, -1 on error, or 1 when IR does
 * not apply to this device (the caller then uses the compatibility
 * format). */
int  iommu_ir_msi_compose(const pci_device_t* dev, uint8_t vec, uint32_t cpu,
                          uint64_t* addr, uint32_t* data);

typedef struct {
    uint64_t total;          /* IR faults recorded (not in the DMA total)    */
    uint64_t logged, suppressed;
    uint16_t last_bdf, last_index;
    uint8_t  last_reason;
} iommu_ir_fault_stats_t;
void iommu_ir_fault_stats(iommu_ir_fault_stats_t* out);

/* ------------------------------------------------------------------ faults */
/* Fault reasons are vendor codes; the name helper is vendor aware. */
typedef struct {
    uint16_t seg, bdf;       /* requester (source id)                        */
    uint64_t iova;           /* faulting address (page)                      */
    uint8_t  reason;         /* vendor fault reason                          */
    uint8_t  is_write;       /* 1 write, 0 read                              */
    uint8_t  unit;
    uint8_t  is_ir;          /* M56: an interrupt-remapping fault (iova 0)   */
    uint16_t ir_index;       /* M56: the interrupt index it named            */
} iommu_fault_t;

/* Called for every recorded DMA fault and (M56) interrupt-remapping fault
 * (is_ir set), in interrupt context, with no IOMMU lock held (M57:
 * attribute the fault to a ring-3 binding and kill it). */
typedef void (*iommu_fault_hook_t)(const iommu_fault_t* f);
void iommu_set_fault_hook(iommu_fault_hook_t hook);

typedef struct {
    uint64_t total;          /* faults recorded                              */
    uint64_t irqs;           /* fault-event interrupts taken                 */
    uint64_t polled;         /* fault records found by iommu_fault_poll()    */
    uint64_t logged;         /* [IOMMU] DMA fault lines printed              */
    uint64_t suppressed;     /* lines dropped by the rate limit              */
    uint64_t overflow;       /* hardware fault-record overflows (PFO)        */
    iommu_fault_t last;
} iommu_fault_stats_t;
void iommu_fault_stats(iommu_fault_stats_t* out);
/* Drain the fault records without waiting for the interrupt (tests). */
void iommu_fault_poll(void);

/* /proc/iommu: `emit` is called once per line. */
void iommu_generate(void (*emit)(void* ctx, const char* line), void* ctx);

/* ------------------------------------------------- vendor back end (ops) */
/* The domain, as the vendor layer sees it. Fields above `hw` are owned by
 * kernel/iommu.c. */
struct iommu_domain {
    int        kind;
    uint32_t   id;           /* debug id (not the hardware domain id)        */
    spinlock_t lock;         /* LK_S67_IOMMU_DOM                             */
    uint64_t   dma_mask;     /* largest IOVA allowed                         */
    uint64_t   iova_next;    /* iommu_iova_alloc bump pointer                */
    volatile uint32_t ndev;  /* attached functions                           */
    uint64_t   mapped_pages; /* currently mapped 4 KiB pages (stats)         */
    void*      hw;           /* vendor state                                 */
};

typedef struct iommu_ops {
    const char* name;
    /* Domain lifetime: allocate the hardware state (page-table root, domain
     * id). dom_init for IDOM_PT builds the identity tables if needed. */
    int      (*dom_init)(iommu_domain_t* d);
    void     (*dom_fini)(iommu_domain_t* d);
    /* Page-table edits, called with d->lock held; unmap returns after the
     * IOTLB flush completed on every unit the domain is attached to. */
    int      (*map)(iommu_domain_t* d, uint64_t iova, uint64_t phys, uint64_t len, int prot);
    int      (*unmap)(iommu_domain_t* d, uint64_t iova, uint64_t len);
    uint64_t (*translate)(iommu_domain_t* d, uint64_t iova, int* prot);
    /* Context programming, called with the device lock (S65) held: install
     * `d` for the function (replacing any old context, flushed), or remove
     * it (d == NULL: blocked). */
    int      (*set_context)(iommu_domain_t* d, const pci_dev_t* dev);
    int      (*covers)(const pci_dev_t* dev);
    uint64_t (*max_iova)(void);
    int      (*irq_compose)(const pci_dev_t* dev, int vec, uint32_t cpu,
                            uint64_t* addr, uint32_t* data);
    const char* (*fault_reason)(uint8_t reason);
    void     (*fault_poll)(void);
    /* Turn translation on (after the boot attaches); 0 if any unit is on. */
    int      (*enable)(void);
    void     (*generate)(void (*emit)(void* ctx, const char* line), void* ctx);
    /* M56 interrupt remapping (all optional). ir_init runs after enable(). */
    int      (*ir_init)(void);
    int      (*ir_enabled)(void);
    int      (*ir_covers)(const pci_dev_t* dev);
    void     (*ir_free)(int vec);
    int      (*ir_set_cpu)(int vec, uint32_t cpu);
    void     (*ir_generate)(void (*emit)(void* ctx, const char* line), void* ctx);
} iommu_ops_t;

/* Called by the vendor layer. */
void iommu_register_ops(const iommu_ops_t* ops);
/* Report one fault record (fault IRQ or poll; no IOMMU lock held). */
void iommu_report_fault(const iommu_fault_t* f, int from_irq);
void iommu_note_fault_irq(void);
void iommu_note_overflow(void);

/* Small line builder shared by the IOMMU files (logs and /proc). */
typedef struct { char b[200]; uint32_t n; } iommu_line_t;
void iommu_ln_s(iommu_line_t* l, const char* s);
void iommu_ln_hex(iommu_line_t* l, uint64_t v);          /* 0x... */
void iommu_ln_dec(iommu_line_t* l, uint64_t v);
void iommu_ln_hexw(iommu_line_t* l, uint64_t v, int digits);
void iommu_ln_bdf(iommu_line_t* l, uint16_t bdf);       /* bb:dd.f */
const char* iommu_ln_end(iommu_line_t* l);              /* NUL-terminate, \n */

/* Boot-word helpers: `word` present, or `key=` followed by a value. */
int  iommu_cmdline_has(const char* word);
int  iommu_cmdline_value(const char* key, char* out, int cap);

#endif /* IOMMU_H */
