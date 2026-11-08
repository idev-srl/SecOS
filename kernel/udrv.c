/*
 * udrv.c — Driver Space v2: PCI functions driven from ring 3 (Phase 5, M57).
 *
 * See kernel/udrv.h (policy, locking) and user/include/secos/udrv.h (ABI).
 * Everything a driver hands in is untrusted: struct sizes are exact, offsets
 * and widths are range-checked, counts are bounded, and the device itself is
 * untrusted too (its MSI-X geometry is checked against its BAR, its DMA goes
 * through its own IOMMU domain).
 *
 * Teardown order of a claim (plan D10), release_locked():
 *   1. bus mastering + memory decode off, INTx disabled (no new DMA);
 *   2. every vector masked, MSI/MSI-X off, vectors freed;
 *   3. FLR / PM reset when the function has one (header restored after);
 *   4. detach to the blocked domain (returns after the context-cache + IOTLB
 *      invalidation completed);
 *   5. (no kernel proxy yet: M58);
 *   6. the driver's BAR and DMA mappings unmapped (TLB shootdown);
 *   7. only now the IOMMU tables and the DMA frames are freed.
 *
 * SPDX-License-Identifier: MIT
 */
#include "udrv.h"
#include "../user/include/secos/udrv.h"
#include "process.h"
#include "sched.h"
#include "signal.h"
#include "klock.h"
#include "spinlock.h"
#include "lock_order.h"
#include "iommu.h"
#include "pci.h"
#include "pci_msi.h"
#include "irq.h"
#include "vmm.h"
#include "vma.h"
#include "pmm.h"
#include "heap.h"
#include "debugcon.h"
#include "timer.h"
#include "tsc.h"
#include "cap.h"
#include "vfs.h"
#include "elf_manifest.h"
#include "../mm/user_copy.h"
#include <stddef.h>

/* errno values (Linux numbering, as the rest of the kernel). */
#define U_EPERM      1
#define U_EIO        5
#define U_EBADF      9
#define U_EAGAIN    11
#define U_ENOMEM    12
#define U_EACCES    13
#define U_EFAULT    14
#define U_EBUSY     16
#define U_ENODEV    19
#define U_EINVAL    22
#define U_EMFILE    24
#define U_ENOSPC    28
#define U_EOPNOTSUPP 95
#define U_EINTR      4
#define WOULDBLOCK   (-2)

#define UDRV_MAX_DEV     8
#define UDRV_MAX_VEC     32
#define UDRV_MAX_DMA     32
#define UDRV_MAX_MAP     16
#define UDRV_MAX_FD      16
#define UDRV_DMA_MAX_PAGES 1024          /* 4 MiB per buffer */
#define UDRV_QUAR_N      3               /* DMA-fault kills ...                  */
#define UDRV_QUAR_WINDOW_MS 60000ull     /* ... within this window: quarantine   */

/* PCI bits used here. */
#define CMD_IO          0x0001
#define CMD_MEM         0x0002
#define CMD_BM          0x0004
#define CMD_INTX_DIS    0x0400
#define MSI_MC_ENABLE   0x0001

/* [M56] Interrupt remapping. The real implementation (arch/x86/vtd_ir.c)
 * overrides this weak stub when it is linked in. */
__attribute__((weak)) int iommu_ir_enabled(void) { return 0; }

/* Installs a kernel object as an fd of c (kernel/syscall.c). */
extern int ksys_fd_install(process_t* c, void* ino, uint32_t flags, int cloexec);

/* ------------------------------------------------------------------ state */
struct udrv_dev;
typedef struct udrv_vec { struct udrv_dev* d; uint8_t idx; int vec; } udrv_vec_t;
typedef struct { uint64_t uva, iova, phys, len; uint8_t used; } udrv_dma_t;
typedef struct { uint64_t uva, len; } udrv_map_t;

typedef struct udrv_dev {
    const pci_dev_t* pd;
    pci_device_t loc;
    uint16_t bdf;
    spinlock_t ev;                  /* LK_S25_UDRV_EV                          */
    /* static, from udrv_init */
    uint8_t  cap_bytes[32];         /* config bytes 0..255 inside a capability */
    uint32_t hdr[16];               /* config header saved at boot (reset)     */
    uint16_t msix_n;                /* MSI-X entries (0 = none)                */
    uint8_t  msix_tbir, msix_pbir;
    uint32_t msix_toff, msix_poff;
    uint8_t  msi_cap, msi_pvm, msi_max;
    uint8_t  pm_reset;              /* PM cap without No_Soft_Reset            */
    /* claim (g_udrv_lock; the fields marked ev also under ev) */
    volatile uint8_t claimed;       /* ev */
    uint32_t gen;                   /* ev: bumped per claim                    */
    process_t* owner;               /* ev */
    uint32_t owner_pid;
    uint8_t  faulted;               /* ev: a DMA fault killed this claim       */
    uint8_t  quarantined;           /* ev */
    uint8_t  dma_ok;
    iommu_domain_t* dom;
    uint64_t dma_mask;
    uint32_t bar_mask, cfgw, irq_max;
    uint64_t dma_max, dma_used;
    uint64_t bar_uva[6];
    int      irq_mode, nvec;        /* ev */
    udrv_vec_t vec[UDRV_MAX_VEC];
    uint64_t irq_count, last_read;  /* ev */
    uint32_t pending, masked, evflags; /* ev */
    uint64_t stray;                 /* ev: calls with nothing to deliver      */
    udrv_dma_t dma[UDRV_MAX_DMA];
    udrv_map_t map[UDRV_MAX_MAP];
    uint32_t nmap;
    /* statistics */
    uint32_t claims, kills;
    uint64_t faults;
    uint64_t kill_ms[UDRV_QUAR_N];
    uint32_t kill_head;
} udrv_dev_t;

typedef struct udrv_fdo {
    vfs_inode_t ino;                /* substituted into the fd                */
    int      used, refs;
    udrv_dev_t* d;
    uint32_t gen;                   /* the claim this fd belongs to           */
} udrv_fdo_t;

static klock_t    g_udrv_lock = KLOCK_INIT(LK_K45_UDRV, "udrv");
static spinlock_t g_fd_lock = SPINLOCK_INIT_CLASS(LK_S24_UDRV_FD, "udrv-fd");
static udrv_dev_t g_dev[UDRV_MAX_DEV];
static int        g_ndev;
static udrv_fdo_t g_fdo[UDRV_MAX_FD];
static int        g_unsafe, g_noir, g_tainted;
static uint64_t   g_refused_dma, g_refused_claim;

/* ---------------------------------------------------------------- helpers */
static uint64_t now_ms(void) { return ktime_ns() / 1000000ull; }

static void delay_ms(uint32_t ms) {
    uint64_t end = ktime_ns() + (uint64_t)ms * 1000000ull;
    while (ktime_ns() < end) cpu_relax_poll();
}

static void bzero8(void* p, uint64_t n) { uint8_t* b = (uint8_t*)p; for (uint64_t i = 0; i < n; i++) b[i] = 0; }

static void ln_dev(iommu_line_t* l, const udrv_dev_t* d) { iommu_ln_bdf(l, d->bdf); }

static void log_line(iommu_line_t* l) { debugcon_writestring(iommu_ln_end(l)); }

static uint32_t cfg_rd32(const udrv_dev_t* d, uint16_t off) {
    if (off >= 0x100) return pci_config_read32_ext(d->loc.bus, d->loc.slot, d->loc.func, off);
    return pci_config_read32(d->loc.bus, d->loc.slot, d->loc.func, (uint8_t)off);
}
static void cfg_wr32(const udrv_dev_t* d, uint8_t off, uint32_t v) {
    pci_config_write32(d->loc.bus, d->loc.slot, d->loc.func, off, v);
}
static uint16_t cmd_read(const udrv_dev_t* d) { return (uint16_t)cfg_rd32(d, 0x04); }
/* Write the command register only: the status half is written as zero, so no
 * write-1-to-clear status bit is ever cleared as a side effect. */
static void cmd_write(const udrv_dev_t* d, uint16_t cmd) { cfg_wr32(d, 0x04, cmd); }

static void msi_enable_bit(const udrv_dev_t* d, int on) {
    if (!d->msi_cap) return;
    uint32_t v = cfg_rd32(d, d->msi_cap);           /* id, next, control */
    uint16_t mc = (uint16_t)(v >> 16);
    mc = on ? (uint16_t)(mc | MSI_MC_ENABLE) : (uint16_t)(mc & ~MSI_MC_ENABLE);
    cfg_wr32(d, d->msi_cap, (v & 0xFFFFu) | ((uint32_t)mc << 16));
}

static udrv_dev_t* dev_by_bdf(uint16_t bdf) {
    for (int i = 0; i < g_ndev; i++) if (g_dev[i].bdf == bdf) return &g_dev[i];
    return NULL;
}

/* -------------------------------------------------------- vector masking */
/* ev held. MSI-X: the entry's mask bit (lock-free MMIO). MSI with per-vector
 * masking: the mask register. MSI without it: the enable bit (INTx Disable is
 * set, so the function cannot fall back to INTx while it is off). */
static void vec_mask_locked(udrv_dev_t* d, int i) {
    uint32_t bit = 1u << i;
    if (d->irq_mode == UDRV_IRQ_MSIX) pci_msix_mask(&d->loc, i);
    else if (d->irq_mode == UDRV_IRQ_MSI) {
        if (d->msi_pvm) pci_msi_mask(&d->loc, i);
        else if (!d->masked) msi_enable_bit(d, 0);
    }
    d->masked |= bit;
}
static void vec_unmask_locked(udrv_dev_t* d, int i) {
    uint32_t bit = 1u << i;
    if (!(d->masked & bit)) return;
    d->masked &= ~bit;
    if (d->irq_mode == UDRV_IRQ_MSIX) pci_msix_unmask(&d->loc, i);
    else if (d->irq_mode == UDRV_IRQ_MSI) {
        if (d->msi_pvm) pci_msi_unmask(&d->loc, i);
        else if (!d->masked) msi_enable_bit(d, 1);
    }
}

/* Wake the owner if it sleeps in read()/poll() on us. ev held. */
static void owner_wake_locked(udrv_dev_t* d) {
    process_t* p = d->owner;
    if (!p) return;
    sched_wake_note();                       /* a racing block re-runs at once */
    if (p->state == PROC_BLOCKED && (p->wait_udrv == (void*)d || p->ki.poll_wait)) {
        p->wait_udrv = NULL;
        sched_wake_task(p, PROC_BLOCKED);
    }
}

/* The device's MSI/MSI-X handler (hardware IRQ context, IF=0). Masks the
 * source until the driver acknowledges it: a slow or hostile driver cannot
 * make it storm a CPU. Tolerates stray calls (a message latched before the
 * vector was freed). */
static void udrv_irq(uint8_t vec, void* ctx) {
    udrv_vec_t* v = (udrv_vec_t*)ctx;
    udrv_dev_t* d = v ? v->d : NULL;
    if (!d) return;
    uint64_t fl = spin_lock_irqsave(&d->ev);
    if (!d->claimed || v->vec != (int)vec || (int)v->idx >= d->nvec) { d->stray++; spin_unlock_irqrestore(&d->ev, fl); return; }
    uint32_t bit = 1u << v->idx;
    if (d->masked & bit) d->stray++;         /* latched before the mask took */
    vec_mask_locked(d, v->idx);
    d->irq_count++;
    d->pending |= bit;
    owner_wake_locked(d);
    spin_unlock_irqrestore(&d->ev, fl);
}

/* IOMMU fault hook (IRQ context, no IOMMU lock held). Never detaches here
 * (iommu_detach spins on invalidations): it stops the device's bus mastering,
 * marks the claim and SIGKILLs the driver; teardown runs in its exit path. */
static void udrv_fault_hook(const iommu_fault_t* f) {
    if (!f || f->seg != 0) return;
    udrv_dev_t* d = dev_by_bdf(f->bdf);
    if (!d) return;
    uint64_t fl = spin_lock_irqsave(&d->ev);
    d->faults++;
    if (d->claimed && !d->faulted && d->owner) {
        d->faulted = 1;
        cmd_write(d, (uint16_t)((cmd_read(d) & ~CMD_BM) | CMD_INTX_DIS));
        d->kills++;
        uint64_t t = now_ms();
        d->kill_ms[d->kill_head % UDRV_QUAR_N] = t;
        d->kill_head++;
        iommu_line_t l; l.n = 0;
        iommu_ln_s(&l, "[UDRV] dma fault bdf="); ln_dev(&l, d);
        iommu_ln_s(&l, " iova="); iommu_ln_hex(&l, f->iova);
        iommu_ln_s(&l, f->is_write ? " W" : " R");
        iommu_ln_s(&l, " -> kill pid="); iommu_ln_dec(&l, d->owner_pid);
        log_line(&l);
        if (d->kill_head >= UDRV_QUAR_N) {
            uint64_t oldest = d->kill_ms[d->kill_head % UDRV_QUAR_N];
            if (t - oldest <= UDRV_QUAR_WINDOW_MS) {
                d->quarantined = 1;
                l.n = 0;
                iommu_ln_s(&l, "[UDRV] "); ln_dev(&l, d);
                iommu_ln_s(&l, " quarantined: "); iommu_ln_dec(&l, UDRV_QUAR_N);
                iommu_ln_s(&l, " DMA-fault kills in "); iommu_ln_dec(&l, (t - oldest) / 1000);
                iommu_ln_s(&l, " s (device off until reboot)");
                log_line(&l);
            }
        }
        d->evflags |= UDRV_EV_STOP;
        signal_post(d->owner, SIGKILL);
        owner_wake_locked(d);
    }
    spin_unlock_irqrestore(&d->ev, fl);
}

/* ------------------------------------------------------------- fd objects */
static int g_fd_read_stub(vfs_inode_t* i, size_t o, void* b, size_t l) { (void)i; (void)o; (void)b; (void)l; return -1; }
static int g_fd_write_stub(vfs_inode_t* i, size_t o, const void* b, size_t l) { (void)i; (void)o; (void)b; (void)l; return -1; }
static vfs_fs_ops_t g_fd_ops = { .flags = VFS_FS_NOCACHE, .read = g_fd_read_stub, .write = g_fd_write_stub };

static udrv_fdo_t* fdo_of(const void* ino) {
    const uint8_t* p = (const uint8_t*)ino;
    const uint8_t* lo = (const uint8_t*)&g_fdo[0];
    const uint8_t* hi = (const uint8_t*)&g_fdo[UDRV_MAX_FD];
    if (!p || p < lo || p >= hi) return NULL;
    udrv_fdo_t* f = &g_fdo[(p - lo) / sizeof(udrv_fdo_t)];
    return (ino == &f->ino && f->used) ? f : NULL;
}
int udrv_is_fd(const void* ino) { return fdo_of(ino) != NULL; }

static udrv_fdo_t* fdo_alloc(udrv_dev_t* d, uint32_t gen) {
    uint64_t fl = spin_lock_irqsave(&g_fd_lock);
    udrv_fdo_t* f = NULL;
    for (int i = 0; i < UDRV_MAX_FD; i++) if (!g_fdo[i].used) { f = &g_fdo[i]; break; }
    if (f) {
        bzero8(&f->ino, sizeof(f->ino));
        const char* nm = "anon_inode:[udrv]";
        for (int k = 0; nm[k] && k < 255; k++) f->ino.path[k] = nm[k];
        f->ino.type = VFS_NODE_FILE;
        f->ino.mode = 0100600;
        f->ino.nlink = 1;
        f->ino.ino = 0x55440000u + (uint64_t)(f - g_fdo);
        f->ino.fs_data = f;
        f->ino.ops = &g_fd_ops;
        f->used = 1; f->refs = 1; f->d = d; f->gen = gen;
    }
    spin_unlock_irqrestore(&g_fd_lock, fl);
    return f;
}

void udrv_fd_ref(void* ino) {
    uint64_t fl = spin_lock_irqsave(&g_fd_lock);
    udrv_fdo_t* f = fdo_of(ino);
    if (f) f->refs++;
    spin_unlock_irqrestore(&g_fd_lock, fl);
}

void udrv_fd_unref(void* ino) {
    uint64_t fl = spin_lock_irqsave(&g_fd_lock);
    udrv_fdo_t* f = fdo_of(ino);
    if (f && --f->refs <= 0) { f->used = 0; f->refs = 0; f->d = NULL; }
    spin_unlock_irqrestore(&g_fd_lock, fl);
}

/* The claim behind an fd of c, if c owns it (NULL + *err otherwise). */
static udrv_dev_t* fd_claim(process_t* c, uint64_t fd, uint32_t* gen, int* err) {
    *err = -U_EBADF;
    if (fd >= 32 || !c->fds[fd].used || !c->fds[fd].inode) return NULL;
    udrv_fdo_t* f = fdo_of(c->fds[fd].inode);
    if (!f || !f->d) return NULL;
    udrv_dev_t* d = f->d;
    if (!d->claimed || d->gen != f->gen) { *err = -U_ENODEV; return NULL; }
    if (d->owner != c) { *err = -U_EPERM; return NULL; }
    *gen = f->gen;
    return d;
}

int udrv_fd_read(process_t* c, void* ino, void* ubuf, int len, int nonblock) {
    udrv_fdo_t* f = fdo_of(ino);
    if (!c || !f || !f->d) return -U_EBADF;
    if (len < (int)sizeof(struct udrv_events)) return -U_EINVAL;
    udrv_dev_t* d = f->d;
    c->wait_udrv = NULL;
    struct udrv_events ev;
    uint64_t fl = spin_lock_irqsave(&d->ev);
    if (!d->claimed || d->gen != f->gen) { spin_unlock_irqrestore(&d->ev, fl); return -U_ENODEV; }
    if (d->owner != c) { spin_unlock_irqrestore(&d->ev, fl); return -U_EPERM; }
    if (d->irq_count != d->last_read || d->evflags) {
        ev.irq_count = d->irq_count;
        ev.vec_pending_mask = d->pending;
        ev.flags = d->evflags;
        d->last_read = d->irq_count;
        d->evflags = 0;
        spin_unlock_irqrestore(&d->ev, fl);
        if (copy_to_user(ubuf, &ev, sizeof(ev)) != 0) return -U_EFAULT;
        return (int)sizeof(ev);
    }
    if (nonblock) { spin_unlock_irqrestore(&d->ev, fl); return -U_EAGAIN; }
    if (signal_pending(c)) { spin_unlock_irqrestore(&d->ev, fl); return -U_EINTR; }
    /* Block: the IRQ handler wakes us; the deadline is only a backstop. */
    c->wait_udrv = d;
    c->sleep_until = timer_get_ticks() + 100;
    spin_unlock_irqrestore(&d->ev, fl);
    return WOULDBLOCK;
}

int udrv_fd_write(process_t* c, void* ino, const void* ubuf, int len) {
    udrv_fdo_t* f = fdo_of(ino);
    if (!c || !f || !f->d) return -U_EBADF;
    if (len < 4) return -U_EINVAL;
    struct udrv_ack a; a.vec_mask = 0; a.flags = 0;
    if (copy_from_user(&a, ubuf, len >= (int)sizeof(a) ? sizeof(a) : 4) != 0) return -U_EFAULT;
    udrv_dev_t* d = f->d;
    uint64_t fl = spin_lock_irqsave(&d->ev);
    if (!d->claimed || d->gen != f->gen) { spin_unlock_irqrestore(&d->ev, fl); return -U_ENODEV; }
    if (d->owner != c) { spin_unlock_irqrestore(&d->ev, fl); return -U_EPERM; }
    if (d->faulted) { spin_unlock_irqrestore(&d->ev, fl); return -U_EIO; }
    uint32_t valid = d->nvec >= 32 ? 0xFFFFFFFFu : ((1u << d->nvec) - 1u);
    uint32_t m = a.vec_mask & valid;
    d->pending &= ~m;
    for (int i = 0; i < d->nvec; i++) if (m & (1u << i)) vec_unmask_locked(d, i);
    spin_unlock_irqrestore(&d->ev, fl);
    return len;
}

int udrv_fd_poll(void* ino) {
    udrv_fdo_t* f = fdo_of(ino);
    if (!f || !f->d) return 0x20;                     /* POLLNVAL */
    udrv_dev_t* d = f->d;
    uint64_t fl = spin_lock_irqsave(&d->ev);
    int r;
    if (!d->claimed || d->gen != f->gen) r = 0x010 | 0x008;           /* HUP|ERR */
    else r = 0x004 | ((d->irq_count != d->last_read || d->evflags) ? (0x001 | 0x040) : 0);
    spin_unlock_irqrestore(&d->ev, fl);
    return r;
}

/* ----------------------------------------------------- user mappings */
/* Map [phys, phys+len) at a fresh VA of c as one pinned PHYS region. mm_lock
 * of c is taken here; returns the VA or 0. */
static uint64_t map_region(process_t* c, uint64_t va, uint64_t phys, uint64_t len, uint8_t vflags) {
    uint64_t vf = VMM_FLAG_USER | VMM_FLAG_RW | VMM_FLAG_NOEXEC;
    uint64_t fl = mm_lock(c);
    if (vma_add_phys(&c->vmas, va, va + len, vf, phys) != 0) { mm_unlock(c, fl); return 0; }
    vma_t* v = vma_find_mut(&c->vmas, va);
    if (!v) { mm_unlock(c, fl); return 0; }
    v->vflags = vflags | VMA_VF_DONTFORK | VMA_VF_PINNED;
    for (uint64_t a = va; a < va + len; a += 0x1000) {
        int r = vma_fault_in(c->space, v, a);
        if (r != 0 && r != -3) {                     /* undo what we mapped */
            for (uint64_t b = va; b < a; b += 0x1000) vmm_unmap_in_space(c->space, b);
            vma_remove(&c->vmas, v);
            mm_unlock(c, fl);
            return 0;
        }
    }
    mm_unlock(c, fl);
    return va;
}

/* Reserve [base, base+len) of c's mmap arena (plus a guard page). 0 = none. */
static uint64_t va_reserve(process_t* c, uint64_t len) {
    uint64_t fl = mm_lock(c);
    uint64_t base = c->mmap_next;
    if (base >= USER_MMAP_END || len > USER_MMAP_END - base - 0x1000 ||
        vma_overlaps(&c->vmas, base, base + len)) { mm_unlock(c, fl); return 0; }
    c->mmap_next = base + len + 0x1000;
    mm_unlock(c, fl);
    return base;
}

/* Unmap [va, va+len) of p (pages + the pinned VMAs inside it). */
static void unmap_region(process_t* p, uint64_t va, uint64_t len) {
    if (!p || !p->space || !len) return;
    uint64_t fl = mm_lock(p);
    for (uint64_t a = va; a < va + len; a += 0x1000) vmm_unmap_in_space(p->space, a);
    for (uint32_t i = 0; i < p->vmas.count; i++) {
        vma_t* v = &p->vmas.v[i];
        if (v->type == VMA_TYPE_PHYS && (v->vflags & VMA_VF_PINNED) && v->start >= va && v->end <= va + len)
            vma_remove(&p->vmas, v);
    }
    mm_unlock(p, fl);
}

/* ------------------------------------------------------- IRQ teardown */
static void irq_teardown_locked(udrv_dev_t* d) {
    if (!d->nvec) return;
    uint64_t fl = spin_lock_irqsave(&d->ev);
    for (int i = 0; i < d->nvec; i++) vec_mask_locked(d, i);
    int mode = d->irq_mode;
    spin_unlock_irqrestore(&d->ev, fl);
    if (mode == UDRV_IRQ_MSIX) pci_msix_disable(&d->loc);
    else if (mode == UDRV_IRQ_MSI) pci_msi_disable(&d->loc);
    for (int i = 0; i < d->nvec; i++) if (d->vec[i].vec >= 0) irq_vec_free(d->vec[i].vec);
    fl = spin_lock_irqsave(&d->ev);
    for (int i = 0; i < UDRV_MAX_VEC; i++) d->vec[i].vec = -1;
    d->nvec = 0; d->irq_mode = UDRV_IRQ_NONE; d->pending = 0; d->masked = 0;
    spin_unlock_irqrestore(&d->ev, fl);
}

/* ------------------------------------------------------------- reset */
static void restore_header(udrv_dev_t* d) {
    for (uint8_t off = 0x10; off < 0x28; off += 4) cfg_wr32(d, off, d->hdr[off / 4]);
    cfg_wr32(d, 0x0C, d->hdr[0x0C / 4] & 0x0000FFFFu);   /* cache line, latency */
    cfg_wr32(d, 0x3C, d->hdr[0x3C / 4] & 0x0000FFFFu);   /* INTx line           */
}

/* FLR, else the PM D3hot -> D0 cycle. 0, or -EOPNOTSUPP. The header (BARs) is
 * restored afterwards; the command register is left with BM off. */
static int do_reset(udrv_dev_t* d, const char** how) {
    const pci_dev_t* pd = d->pd;
    if ((pd->flr & PCI_FLR_PCIE) && pd->cap_pcie) {
        uint8_t dc = (uint8_t)(pd->cap_pcie + 8);         /* DevCtl | DevSta */
        for (int i = 0; i < 100 && (cfg_rd32(d, dc) & (1u << 21)); i++) delay_ms(1);  /* transactions pending */
        uint32_t v = cfg_rd32(d, dc);
        cfg_wr32(d, dc, (v & 0x0000FFFFu) | 0x8000u);      /* Initiate FLR; DevSta RW1C bits written 0 */
        delay_ms(100);
        *how = "flr";
    } else if ((pd->flr & PCI_FLR_AF) && pd->cap_af) {
        uint32_t v = cfg_rd32(d, (uint8_t)(pd->cap_af + 4)); /* ctrl @ +4, status @ +5 */
        cfg_wr32(d, (uint8_t)(pd->cap_af + 4), (v & 0xFFFF0000u) | 0x1u);
        delay_ms(100);
        *how = "af-flr";
    } else if (d->pm_reset) {
        uint8_t pm = (uint8_t)(pd->cap_pm + 4);
        uint32_t v = cfg_rd32(d, pm);
        cfg_wr32(d, pm, (v & ~0x3u) | 0x3u);               /* D3hot */
        delay_ms(10);
        v = cfg_rd32(d, pm);
        cfg_wr32(d, pm, v & ~0x3u);                        /* D0 */
        delay_ms(10);
        *how = "pm";
    } else {
        return -U_EOPNOTSUPP;
    }
    restore_header(d);
    cmd_write(d, CMD_INTX_DIS);
    return 0;
}

/* ------------------------------------------------------------ release */
static void release_locked(udrv_dev_t* d, const char* why) {
    if (!d->claimed) return;
    /* 0. Unpublish: the IRQ handler and the fault hook ignore it from now. */
    uint64_t fl = spin_lock_irqsave(&d->ev);
    process_t* owner = d->owner;
    uint32_t pid = d->owner_pid;
    d->claimed = 0;
    d->owner = NULL;
    if (owner && owner->wait_udrv == (void*)d) owner->wait_udrv = NULL;
    spin_unlock_irqrestore(&d->ev, fl);
    /* 1. No new DMA, no decode. */
    cmd_write(d, CMD_INTX_DIS);
    /* 2. Vectors. */
    irq_teardown_locked(d);
    /* 3. Reset when the function has one. */
    const char* how = "none";
    int rr = do_reset(d, &how);
    /* 4. Blocked domain: returns after the invalidation completed. */
    if (d->dom) iommu_detach(d->pd);
    /* 6. The driver's mappings. */
    for (uint32_t i = 0; i < d->nmap; i++) unmap_region(owner, d->map[i].uva, d->map[i].len);
    for (int i = 0; i < UDRV_MAX_DMA; i++) if (d->dma[i].used) unmap_region(owner, d->dma[i].uva, d->dma[i].len);
    /* 7. Only now: IOMMU tables, then the frames. */
    if (d->dom) { iommu_domain_free(d->dom); d->dom = NULL; }
    uint64_t freed = 0;
    for (int i = 0; i < UDRV_MAX_DMA; i++) {
        if (!d->dma[i].used) continue;
        pmm_free_contiguous((void*)d->dma[i].phys, (size_t)(d->dma[i].len / 4096));
        freed += d->dma[i].len;
        d->dma[i].used = 0;
    }
    d->nmap = 0; d->dma_used = 0;
    for (int i = 0; i < 6; i++) d->bar_uva[i] = 0;
    iommu_line_t l; l.n = 0;
    iommu_ln_s(&l, "[UDRV] release pid="); iommu_ln_dec(&l, pid);
    iommu_ln_s(&l, " bdf="); ln_dev(&l, d);
    iommu_ln_s(&l, " ("); iommu_ln_s(&l, why); iommu_ln_s(&l, ") reset=");
    iommu_ln_s(&l, rr == 0 ? how : "none");
    iommu_ln_s(&l, " dma_freed="); iommu_ln_dec(&l, freed);
    if (d->quarantined) iommu_ln_s(&l, " QUARANTINED");
    log_line(&l);
}

void udrv_release_all(process_t* p) {
    if (!p) return;
    int any = 0;
    for (int i = 0; i < g_ndev; i++) if (g_dev[i].claimed && g_dev[i].owner_pid == p->pid && g_dev[i].owner == p) any = 1;
    if (!any) return;
    klock_lock(&g_udrv_lock);
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i].claimed && g_dev[i].owner == p)
            release_locked(&g_dev[i], p->state == PROC_ZOMBIE ? "reap" : "exit");
    klock_unlock(&g_udrv_lock);
}

/* ----------------------------------------------------------------- ops */
static void refuse_log(process_t* c, const udrv_dev_t* d, const char* what) {
    iommu_line_t l; l.n = 0;
    iommu_ln_s(&l, "[UDRV] claim pid="); iommu_ln_dec(&l, c ? c->pid : 0);
    iommu_ln_s(&l, " bdf=");
    if (d) ln_dev(&l, d); else iommu_ln_s(&l, "?");
    iommu_ln_s(&l, " refused("); iommu_ln_s(&l, what); iommu_ln_s(&l, ")");
    log_line(&l);
    g_refused_claim++;
    if (c) cap_audit(c->pid, CAP_AUDIT_DRIVER, SYS_UDRV, "udrv-claim", 0);
}

static void op_log(process_t* c, const udrv_dev_t* d, const char* op, const char* what) {
    iommu_line_t l; l.n = 0;
    iommu_ln_s(&l, "[UDRV] "); iommu_ln_s(&l, op);
    iommu_ln_s(&l, " pid="); iommu_ln_dec(&l, c->pid);
    iommu_ln_s(&l, " bdf="); ln_dev(&l, d);
    iommu_ln_s(&l, " "); iommu_ln_s(&l, what);
    log_line(&l);
}

static int mf_match(const elf_manifest_t* mf, const pci_dev_t* pd) {
    for (uint32_t i = 0; i < mf->nmatch && i < MANIFEST_UDRV_MATCH_MAX; i++) {
        if (mf->match[i].vendor != 0xFFFF && mf->match[i].vendor != pd->vendor_id) continue;
        if (mf->match[i].device != 0xFFFF && mf->match[i].device != pd->device_id) continue;
        if (mf->match[i].class_code != 0xFF && mf->match[i].class_code != pd->class_code) continue;
        if (mf->match[i].subclass != 0xFF && mf->match[i].subclass != pd->subclass) continue;
        if (mf->match[i].vendor == 0xFFFF && mf->match[i].device == 0xFFFF &&
            mf->match[i].class_code == 0xFF && mf->match[i].subclass == 0xFF) continue;  /* no wildcard-all */
        return 1;
    }
    return 0;
}

static long op_claim(process_t* c, const struct udrv_claim* a) {
    udrv_dev_t* d = NULL;
    if (a->flags & ~UDRV_CLAIM_BY_ID) return -U_EINVAL;
    for (int i = 0; i < g_ndev && !d; i++) {
        const pci_dev_t* pd = g_dev[i].pd;
        if (a->flags & UDRV_CLAIM_BY_ID) {
            if (pd->vendor_id == a->vendor && pd->device_id == a->device && !g_dev[i].claimed) d = &g_dev[i];
        } else if (pd->bus == a->bus && pd->slot == a->dev && pd->func == a->fn) d = &g_dev[i];
    }
    if (c->proc_type != PROC_TYPE_DRIVER) { refuse_log(c, d, "not-a-driver"); return -U_EPERM; }
    if (c->cred.euid != 0) { refuse_log(c, d, "not-root"); return -U_EPERM; }
    const elf_manifest_t* mf = (const elf_manifest_t*)c->manifest;
    if (!mf || mf->version < MANIFEST_VERSION_UDRV || mf->nmatch == 0) { refuse_log(c, d, "no-v3-manifest"); return -U_EPERM; }
    if (!d) {
        /* By id, every matching function may already be claimed. */
        if (a->flags & UDRV_CLAIM_BY_ID)
            for (int i = 0; i < g_ndev; i++)
                if (g_dev[i].pd->vendor_id == a->vendor && g_dev[i].pd->device_id == a->device) { refuse_log(c, &g_dev[i], "busy"); return -U_EBUSY; }
        refuse_log(c, NULL, "not-assigned");
        return -U_ENODEV;
    }
    if (!mf_match(mf, d->pd)) { refuse_log(c, d, "manifest-mismatch"); return -U_EACCES; }
    if (d->quarantined) { refuse_log(c, d, "quarantined"); return -U_EIO; }
    if (d->claimed) { refuse_log(c, d, "busy"); return -U_EBUSY; }

    /* DMA rights: an IOMMU that translates this function plus interrupt
     * remapping (a DMA-capable device can forge any MSI), or an override. */
    int covered = iommu_present() && iommu_dev_covered(d->pd);
    int ir = iommu_ir_enabled();
    d->dma_ok = covered ? (ir || g_noir || g_unsafe) : g_unsafe;
    d->dma_mask = a->dma_mask ? a->dma_mask : 0xFFFFFFFFull;
    d->dom = NULL;
    if (covered) {
        d->dom = iommu_domain_new(IDOM_TRANSLATED);
        if (!d->dom) { refuse_log(c, d, "no-domain"); return -U_ENOMEM; }
        iommu_domain_set_dma_mask(d->dom, d->dma_mask);
        if (iommu_attach(d->dom, d->pd) != 0) {
            iommu_domain_free(d->dom); d->dom = NULL;
            refuse_log(c, d, "attach-failed");
            return -U_EIO;
        }
    }
    uint32_t gen = d->gen + 1;
    udrv_fdo_t* f = fdo_alloc(d, gen);
    if (!f) {
        if (d->dom) { iommu_detach(d->pd); iommu_domain_free(d->dom); d->dom = NULL; }
        refuse_log(c, d, "no-fd");
        return -U_EMFILE;
    }
    d->bar_mask = mf->bar_mask; d->cfgw = mf->cfg_write_mask;
    d->irq_max = mf->irq_max > UDRV_MAX_VEC ? UDRV_MAX_VEC : mf->irq_max;
    d->dma_max = mf->dma_max; d->dma_used = 0; d->nmap = 0;
    for (int i = 0; i < 6; i++) d->bar_uva[i] = 0;
    for (int i = 0; i < UDRV_MAX_DMA; i++) d->dma[i].used = 0;
    for (int i = 0; i < UDRV_MAX_VEC; i++) { d->vec[i].d = d; d->vec[i].idx = (uint8_t)i; d->vec[i].vec = -1; }
    /* Decode on, bus mastering off until the driver asks (and may). */
    cmd_write(d, CMD_MEM | CMD_INTX_DIS);
    uint64_t fl = spin_lock_irqsave(&d->ev);
    d->gen = gen; d->owner = c; d->owner_pid = c->pid;
    d->faulted = 0; d->irq_mode = UDRV_IRQ_NONE; d->nvec = 0;
    d->irq_count = 0; d->last_read = 0; d->pending = 0; d->masked = 0; d->evflags = 0;
    d->claimed = 1;
    spin_unlock_irqrestore(&d->ev, fl);
    int fd = ksys_fd_install(c, &f->ino, 2 /*O_RDWR*/, 0);
    if (fd < 0) {
        udrv_fd_unref(&f->ino);
        release_locked(d, "no-fd");
        g_refused_claim++;
        return -U_EMFILE;
    }
    d->claims++;
    iommu_line_t l; l.n = 0;
    iommu_ln_s(&l, "[UDRV] claim pid="); iommu_ln_dec(&l, c->pid);
    iommu_ln_s(&l, " bdf="); ln_dev(&l, d);
    iommu_ln_s(&l, " ok fd="); iommu_ln_dec(&l, (uint64_t)fd);
    iommu_ln_s(&l, d->dom ? " iommu=translated" : " iommu=none");
    iommu_ln_s(&l, d->dma_ok ? " dma=yes" : " dma=no");
    if (d->dma_ok && !d->dom) iommu_ln_s(&l, " TAINTED(unsafe)");
    else if (d->dma_ok && !ir) iommu_ln_s(&l, g_unsafe ? " ir=no(unsafe)" : " ir=no(udrv.noir)");
    log_line(&l);
    cap_audit(c->pid, CAP_AUDIT_DRIVER, SYS_UDRV, "udrv-claim", 1);
    return fd;
}

static long op_info(udrv_dev_t* d, struct udrv_info* o) {
    const pci_dev_t* pd = d->pd;
    bzero8(o, sizeof(*o));
    o->vendor = pd->vendor_id; o->device = pd->device_id;
    o->bus = pd->bus; o->dev = pd->slot; o->fn = pd->func;
    o->class_code = pd->class_code; o->subclass = pd->subclass; o->prog_if = pd->prog_if;
    o->revision = pd->revision; o->nbars = pd->nbars;
    for (int i = 0; i < 6; i++) { o->bar_flags[i] = pd->bar_flags[i]; o->bar_size[i] = pd->bar_size[i]; }
    o->msix_count = d->msix_n; o->msi_max = d->msi_max; o->msi_pvm = d->msi_pvm;
    o->iommu = d->dom != NULL; o->ir = (uint8_t)(iommu_ir_enabled() != 0);
    o->dma_ok = d->dma_ok; o->unsafe = (uint8_t)g_unsafe;
    o->flr = (pd->flr & (PCI_FLR_PCIE | PCI_FLR_AF)) ? 1 : 0; o->pm_reset = d->pm_reset;
    o->irq_mode = (uint32_t)d->irq_mode;
    o->dma_used = d->dma_used; o->dma_max = d->dma_max;
    o->msix_table_bar = d->msix_tbir; o->msix_table_off = d->msix_toff;
    o->msix_pba_bar = d->msix_pbir; o->msix_pba_off = d->msix_poff;
    o->irq_count = d->irq_count;
    o->bar_mask = d->bar_mask; o->cfg_write_mask = d->cfgw;
    return 0;
}

/* Is page `pg` (BAR offset, page aligned) of BAR `bar` an MSI-X table / PBA page? */
static int msix_hole(const udrv_dev_t* d, int bar, uint64_t pg) {
    if (!d->msix_n) return 0;
    uint64_t tl = (uint64_t)d->msix_n * 16, pl = (((uint64_t)d->msix_n + 63) / 64) * 8;
    if (d->msix_tbir == bar) {
        uint64_t lo = d->msix_toff & ~0xFFFull, hi = (d->msix_toff + tl + 0xFFF) & ~0xFFFull;
        if (pg >= lo && pg < hi) return 1;
    }
    if (d->msix_pbir == bar) {
        uint64_t lo = d->msix_poff & ~0xFFFull, hi = (d->msix_poff + pl + 0xFFF) & ~0xFFFull;
        if (pg >= lo && pg < hi) return 1;
    }
    return 0;
}

static long op_map_bar(process_t* c, udrv_dev_t* d, struct udrv_map_bar* a) {
    const pci_dev_t* pd = d->pd;
    if (a->flags) return -U_EINVAL;
    if (a->bar >= 6 || a->bar >= pd->nbars) return -U_EINVAL;
    int b = (int)a->bar;
    if (!(d->bar_mask & (1u << b))) { op_log(c, d, "map_bar", "refused(manifest bar_mask)"); return -U_EPERM; }
    uint8_t fl = pd->bar_flags[b];
    if (!(fl & PCI_BAR_PRESENT) || (fl & PCI_BAR_UPPER)) return -U_EINVAL;
    if (fl & PCI_BAR_IO) { op_log(c, d, "map_bar", "refused(io-bar)"); return -U_EPERM; }
    uint64_t base = pd->bar_base[b], size = pd->bar_size[b];
    if (size < 0x1000 || (base & 0xFFF) || !base) { op_log(c, d, "map_bar", "refused(bar<4K)"); return -U_EINVAL; }
    if (size > (256ull << 20)) return -U_EINVAL;
    if (d->bar_uva[b]) return -U_EBUSY;
    if (d->nmap + 3 > UDRV_MAX_MAP) return -U_ENOSPC;
    uint64_t len = (size + 0xFFF) & ~0xFFFull;
    uint64_t va = va_reserve(c, len);
    if (!va) return -U_ENOMEM;
    uint32_t holes = 0, first = d->nmap;
    for (uint64_t off = 0; off < len; ) {
        if (msix_hole(d, b, off)) { off += 0x1000; holes++; continue; }
        uint64_t run = off;
        while (run < len && !msix_hole(d, b, run)) run += 0x1000;
        if (!map_region(c, va + off, base + off, run - off, VMA_VF_UC)) {
            for (uint32_t i = first; i < d->nmap; i++) unmap_region(c, d->map[i].uva, d->map[i].len);
            d->nmap = first;
            return -U_ENOMEM;
        }
        d->map[d->nmap].uva = va + off; d->map[d->nmap].len = run - off; d->nmap++;
        off = run;
    }
    d->bar_uva[b] = va;
    a->uva = va; a->len = len;
    iommu_line_t l; l.n = 0;
    iommu_ln_s(&l, "[UDRV] map_bar pid="); iommu_ln_dec(&l, c->pid);
    iommu_ln_s(&l, " bdf="); ln_dev(&l, d);
    iommu_ln_s(&l, " bar="); iommu_ln_dec(&l, (uint64_t)b);
    iommu_ln_s(&l, " len="); iommu_ln_hex(&l, len);
    iommu_ln_s(&l, " msix_hole_pages="); iommu_ln_dec(&l, holes);
    log_line(&l);
    return 0;
}

static long op_cfg_read(udrv_dev_t* d, struct udrv_cfg* a) {
    if ((a->width != 1 && a->width != 2 && a->width != 4) || a->off > 0xFFF || (a->off & (a->width - 1)))
        return -U_EINVAL;
    uint32_t v = cfg_rd32(d, (uint16_t)(a->off & ~3u));
    v >>= (a->off & 3) * 8;
    if (a->width == 1) v &= 0xFF; else if (a->width == 2) v &= 0xFFFF;
    a->val = v;
    return 0;
}

static int in_cap(const udrv_dev_t* d, uint32_t off, uint32_t width) {
    for (uint32_t b = off; b < off + width; b++)
        if (b >= 0x100 || (d->cap_bytes[b >> 3] & (1u << (b & 7)))) return 1;
    return 0;
}

static long op_cfg_write(process_t* c, udrv_dev_t* d, const struct udrv_cfg* a) {
    uint32_t off = a->off, w = a->width;
    if ((w != 1 && w != 2 && w != 4) || off > 0xFFF || (off & (w - 1))) return -U_EINVAL;
    uint32_t val = a->val;
    if (w == 1) val &= 0xFF; else if (w == 2) val &= 0xFFFF;
    if (off >= 0x04 && off < 0x08) {
        if (!(d->cfgw & UDRV_CFGW_COMMAND)) { op_log(c, d, "cfg_write", "refused(command, manifest)"); return -U_EPERM; }
        uint32_t dw = val << ((off & 3) * 8);
        if (off >= 0x06) return 0;                  /* status only: masked, nothing written */
        uint16_t want = (uint16_t)dw;                 /* command bytes of the write */
        uint16_t bmask = (w == 1 && off == 0x05) ? 0xFF00 : (w == 1) ? 0x00FF : 0xFFFF;
        uint16_t cur = cmd_read(d);
        uint16_t neu = (uint16_t)((cur & ~bmask) | (want & bmask));
        if (neu & CMD_IO) { op_log(c, d, "cfg_write", "refused(io-space)"); return -U_EPERM; }
        if ((neu & CMD_BM) && !d->dma_ok) {
            g_refused_dma++;
            op_log(c, d, "cfg_write", d->dom ? "refused(busmaster: no interrupt remapping)" : "refused(busmaster: no IOMMU)");
            return -U_EPERM;
        }
        if (d->faulted) return -U_EIO;
        /* Only MEM, BM and INTx Disable come from the driver; INTx stays off
         * (no INTx support before M61). */
        uint16_t out = (uint16_t)((cur & ~(CMD_MEM | CMD_BM)) | (neu & (CMD_MEM | CMD_BM)) | CMD_INTX_DIS);
        cmd_write(d, out);
        return 0;
    }
    if (off >= 0x40 && off + w <= 0x100) {
        if (!(d->cfgw & UDRV_CFGW_DEVSPEC)) { op_log(c, d, "cfg_write", "refused(devspec, manifest)"); return -U_EPERM; }
        if (in_cap(d, off, w)) { op_log(c, d, "cfg_write", "refused(capability)"); return -U_EPERM; }
        uint32_t dw = cfg_rd32(d, (uint16_t)(off & ~3u));
        uint32_t sh = (off & 3) * 8;
        uint32_t m = (w == 4) ? 0xFFFFFFFFu : (((1u << (w * 8)) - 1u) << sh);
        dw = (dw & ~m) | ((val << sh) & m);
        cfg_wr32(d, (uint8_t)(off & ~3u), dw);
        return 0;
    }
    op_log(c, d, "cfg_write", off >= 0x10 && off < 0x28 ? "refused(bar)" : "refused(offset)");
    return -U_EPERM;
}

/* DMA backing store. [M55] switches this to kernel/dma.h (dma_alloc_coherent
 * on the claim's domain); until then: zeroed contiguous PMM frames. */
static uint64_t dma_backend_alloc(udrv_dev_t* d, uint64_t npages) {
    void* p;
    if (d->dom) p = pmm_alloc_contiguous((size_t)npages);
    else p = pmm_alloc_contiguous_below((size_t)npages, d->dma_mask + 1 ? d->dma_mask + 1 : ~0ull);
    if (!p) return 0;
    uint64_t phys = (uint64_t)p;
    bzero8((void*)phys_to_virt(phys), npages * 4096);
    return phys;
}

static long op_dma_alloc(process_t* c, udrv_dev_t* d, struct udrv_dma* a) {
    if (!d->dma_ok) {
        g_refused_dma++;
        op_log(c, d, "dma_alloc", d->dom ? "refused(no interrupt remapping)" : "refused(no IOMMU)");
        return -U_ENODEV;
    }
    if (d->faulted) return -U_EIO;
    if (a->flags || a->dir < UDRV_DMA_TO_DEV || a->dir > UDRV_DMA_BIDIR || !a->size) return -U_EINVAL;
    if (a->size > (uint64_t)UDRV_DMA_MAX_PAGES * 4096) return -U_EINVAL;
    uint64_t len = (a->size + 0xFFF) & ~0xFFFull;
    if (d->dma_used + len > d->dma_max) { op_log(c, d, "dma_alloc", "refused(manifest dma_max)"); return -U_ENOMEM; }
    if (c->mem_limit && vma_total_bytes(&c->vmas) + len > c->mem_limit) return -U_ENOMEM;
    int slot = -1;
    for (int i = 0; i < UDRV_MAX_DMA; i++) if (!d->dma[i].used) { slot = i; break; }
    if (slot < 0) return -U_ENOSPC;
    uint64_t phys = dma_backend_alloc(d, len / 4096);
    if (!phys) return -U_ENOMEM;
    uint64_t iova;
    if (d->dom) {
        iova = iommu_iova_alloc(d->dom, len);
        int prot = (a->dir & UDRV_DMA_TO_DEV ? IOMMU_READ : 0) | (a->dir & UDRV_DMA_FROM_DEV ? IOMMU_WRITE : 0);
        if (!iova || iommu_map(d->dom, iova, phys, len, prot) != 0) {
            pmm_free_contiguous((void*)phys, (size_t)(len / 4096));
            return -U_ENOMEM;
        }
    } else {
        iova = phys;                               /* udrv.unsafe: no isolation */
        if (!g_tainted) {
            g_tainted = 1;
            debugcon_writestring("[UDRV] WARNING: TAINTED: ring-3 DMA without IOMMU (udrv.unsafe)\n");
        }
    }
    uint64_t va = va_reserve(c, len);
    if (!va || !map_region(c, va, phys, len, VMA_VF_WB)) {
        if (d->dom) iommu_unmap(d->dom, iova, len);
        pmm_free_contiguous((void*)phys, (size_t)(len / 4096));
        return -U_ENOMEM;
    }
    d->dma[slot].uva = va; d->dma[slot].iova = iova; d->dma[slot].phys = phys;
    d->dma[slot].len = len; d->dma[slot].used = 1;
    d->dma_used += len;
    a->uva = va; a->iova = iova;
    return 0;
}

static long op_dma_free(process_t* c, udrv_dev_t* d, const struct udrv_dma* a) {
    int slot = -1;
    for (int i = 0; i < UDRV_MAX_DMA; i++) if (d->dma[i].used && d->dma[i].uva == a->uva) { slot = i; break; }
    if (slot < 0) return -U_EINVAL;
    udrv_dma_t* b = &d->dma[slot];
    /* Device side first (returns after the IOTLB flush), then the user side,
     * then the frames. */
    if (d->dom && iommu_unmap(d->dom, b->iova, b->len) != 0) {
        op_log(c, d, "dma_free", "iommu_unmap FAILED: buffer leaked, not reused");
        b->used = 0;
        return -U_EIO;
    }
    unmap_region(c, b->uva, b->len);
    pmm_free_contiguous((void*)b->phys, (size_t)(b->len / 4096));
    d->dma_used -= b->len;
    b->used = 0;
    return 0;
}

static long op_irq_setup(process_t* c, udrv_dev_t* d, struct udrv_irq* a) {
    if (d->nvec) return -U_EBUSY;
    if (!d->dma_ok) {
        /* An MSI is a memory write by the device: it needs bus mastering. */
        op_log(c, d, "irq_setup", d->dom ? "refused(no interrupt remapping)" : "refused(no IOMMU: MSI needs bus mastering)");
        return -U_EPERM;
    }
    uint32_t n = a->nvec;
    if (n < 1 || n > d->irq_max || n > UDRV_MAX_VEC) { op_log(c, d, "irq_setup", "refused(manifest irq_max)"); return -U_EINVAL; }
    int cpu = irq_pick_cpu(IRQ_CPU_ANY);
    if (cpu < 0) return -U_EIO;
    if (d->msix_n) {
        if (n > d->msix_n) return -U_EINVAL;
        if (pci_msix_enable(&d->loc, (int)n) != 0) return -U_EIO;
        for (uint32_t i = 0; i < n; i++) {
            int v = irq_vec_alloc((uint32_t)cpu, udrv_irq, &d->vec[i], "udrv");
            if (v < 0 || pci_msix_set(&d->loc, (int)i, (uint8_t)v, (uint32_t)cpu) != 0) {
                if (v >= 0) irq_vec_free(v);
                pci_msix_disable(&d->loc);
                for (uint32_t k = 0; k < i; k++) { irq_vec_free(d->vec[k].vec); d->vec[k].vec = -1; }
                return -U_ENOSPC;
            }
            d->vec[i].vec = v;
        }
        uint64_t fl = spin_lock_irqsave(&d->ev);
        d->irq_mode = UDRV_IRQ_MSIX; d->nvec = (int)n;
        d->masked = (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);   /* pci_msix_enable masked all */
        spin_unlock_irqrestore(&d->ev, fl);
        a->mode = UDRV_IRQ_MSIX;
    } else if (d->msi_cap) {
        if (n > d->msi_max || (n & (n - 1))) return -U_EINVAL;
        int base = (n == 1) ? irq_vec_alloc((uint32_t)cpu, udrv_irq, &d->vec[0], "udrv")
                            : -1;
        if (n > 1) {
            /* A block shares one ctx: the handler tells vectors apart by vec. */
            return -U_EINVAL;                       /* multi-message MSI: not needed yet */
        }
        if (base < 0) return -U_ENOSPC;
        d->vec[0].vec = base;
        uint64_t fl = spin_lock_irqsave(&d->ev);
        d->irq_mode = UDRV_IRQ_MSI; d->nvec = 1; d->masked = 0;
        spin_unlock_irqrestore(&d->ev, fl);
        if (pci_msi_enable(&d->loc, 1, (uint8_t)base, (uint32_t)cpu) != 0) {
            fl = spin_lock_irqsave(&d->ev);
            d->irq_mode = UDRV_IRQ_NONE; d->nvec = 0;
            spin_unlock_irqrestore(&d->ev, fl);
            irq_vec_free(base); d->vec[0].vec = -1;
            return -U_EIO;
        }
        fl = spin_lock_irqsave(&d->ev);
        vec_mask_locked(d, 0);                      /* starts masked, like MSI-X */
        spin_unlock_irqrestore(&d->ev, fl);
        a->mode = UDRV_IRQ_MSI;
    } else {
        return -U_EOPNOTSUPP;                       /* INTx: M61 */
    }
    iommu_line_t l; l.n = 0;
    iommu_ln_s(&l, "[UDRV] irq_setup pid="); iommu_ln_dec(&l, c->pid);
    iommu_ln_s(&l, " bdf="); ln_dev(&l, d);
    iommu_ln_s(&l, a->mode == UDRV_IRQ_MSIX ? " msix" : " msi");
    iommu_ln_s(&l, " nvec="); iommu_ln_dec(&l, n);
    iommu_ln_s(&l, " vec0="); iommu_ln_hex(&l, (uint64_t)d->vec[0].vec);
    log_line(&l);
    return 0;
}

static long op_reset(process_t* c, udrv_dev_t* d) {
    const char* how = "none";
    if (!(d->pd->flr & (PCI_FLR_PCIE | PCI_FLR_AF)) && !d->pm_reset) return -U_EOPNOTSUPP;
    irq_teardown_locked(d);
    cmd_write(d, CMD_INTX_DIS);
    int r = do_reset(d, &how);
    cmd_write(d, CMD_MEM | CMD_INTX_DIS);
    if (r == 0) op_log(c, d, "reset", how);
    return r;
}

/* -------------------------------------------------------------- syscall */
long udrv_syscall(uint64_t op, uint64_t handle, uint64_t arg, uint64_t size) {
    process_t* c = sched_get_current();
    if (!c) return -U_EINVAL;
    union {
        struct udrv_claim cl; struct udrv_info in; struct udrv_map_bar mb;
        struct udrv_cfg cf; struct udrv_dma dm; struct udrv_irq iq;
    } k;
    uint64_t want;
    int out = 0;
    switch (op) {
    case UDRV_OP_CLAIM:     want = sizeof(k.cl); break;
    case UDRV_OP_INFO:      want = sizeof(k.in); out = 1; break;
    case UDRV_OP_MAP_BAR:   want = sizeof(k.mb); out = 1; break;
    case UDRV_OP_CFG_READ:  want = sizeof(k.cf); out = 1; break;
    case UDRV_OP_CFG_WRITE: want = sizeof(k.cf); break;
    case UDRV_OP_DMA_ALLOC: want = sizeof(k.dm); out = 1; break;
    case UDRV_OP_DMA_FREE:  want = sizeof(k.dm); break;
    case UDRV_OP_IRQ_SETUP: want = sizeof(k.iq); out = 1; break;
    case UDRV_OP_RESET:
    case UDRV_OP_RELEASE:   want = 0; break;
    default: return -U_EINVAL;
    }
    if (size != want) return -U_EINVAL;
    if (want && op != UDRV_OP_INFO && copy_from_user(&k, (const void*)arg, want) != 0) return -U_EFAULT;
    if (want && op == UDRV_OP_INFO && !user_range_valid((const void*)arg, want)) return -U_EFAULT;
    if (g_ndev == 0 && op == UDRV_OP_CLAIM) {
        refuse_log(c, NULL, "not-assigned");
        return -U_ENODEV;
    }
    long r;
    klock_lock(&g_udrv_lock);
    if (op == UDRV_OP_CLAIM) {
        r = op_claim(c, &k.cl);
    } else {
        int err; uint32_t gen;
        udrv_dev_t* d = fd_claim(c, handle, &gen, &err);
        if (!d) { r = err; goto done; }
        if (d->faulted && op != UDRV_OP_INFO && op != UDRV_OP_RELEASE) { r = -U_EIO; goto done; }
        switch (op) {
        case UDRV_OP_INFO:      r = op_info(d, &k.in); break;
        case UDRV_OP_MAP_BAR:   r = op_map_bar(c, d, &k.mb); break;
        case UDRV_OP_CFG_READ:  r = op_cfg_read(d, &k.cf); break;
        case UDRV_OP_CFG_WRITE: r = op_cfg_write(c, d, &k.cf); break;
        case UDRV_OP_DMA_ALLOC: r = op_dma_alloc(c, d, &k.dm); break;
        case UDRV_OP_DMA_FREE:  r = op_dma_free(c, d, &k.dm); break;
        case UDRV_OP_IRQ_SETUP: r = op_irq_setup(c, d, &k.iq); break;
        case UDRV_OP_RESET:     r = op_reset(c, d); break;
        case UDRV_OP_RELEASE:   release_locked(d, "release"); r = 0; break;
        default:                r = -U_EINVAL; break;
        }
    }
done:
    klock_unlock(&g_udrv_lock);
    if (r >= 0 && out && copy_to_user((void*)arg, &k, want) != 0) r = -U_EFAULT;
    return r;
}

/* ------------------------------------------------------------- boot */
static int hexv(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}
/* Parse a hex number of exactly `n` digits at s; -1 on error. */
static int hexn(const char* s, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) { int h = hexv(s[i]); if (h < 0) return -1; v = v * 16 + h; }
    return v;
}

/* Bytes 0x40..0xFF covered by a capability structure. The chain is untrusted:
 * bounded walk; a capability of unknown length covers up to the next
 * capability (or 0x100). */
static void build_cap_bytes(udrv_dev_t* d) {
    for (int i = 0; i < 32; i++) d->cap_bytes[i] = 0;
    uint8_t offs[48]; uint8_t ids[48]; int n = 0;
    uint16_t st = (uint16_t)(cfg_rd32(d, 0x04) >> 16);
    if (st & PCI_STATUS_CAP_LIST) {
        uint8_t p = (uint8_t)(cfg_rd32(d, 0x34) & 0xFC);
        while (p >= 0x40 && n < 48) {
            int dup = 0; for (int k = 0; k < n; k++) if (offs[k] == p) dup = 1;
            if (dup) break;
            uint32_t h = cfg_rd32(d, p);
            offs[n] = p; ids[n] = (uint8_t)h; n++;
            p = (uint8_t)((h >> 8) & 0xFC);
        }
    }
    for (int i = 0; i < n; i++) {
        uint32_t lo = offs[i], len = 0;
        uint32_t next = 0x100;
        for (int k = 0; k < n; k++) if (offs[k] > lo && offs[k] < next) next = offs[k];
        uint32_t h = cfg_rd32(d, (uint16_t)lo);
        switch (ids[i]) {
        case PCI_CAP_ID_PM:   len = 8; break;
        case PCI_CAP_ID_MSIX: len = 12; break;
        case PCI_CAP_ID_PCIE: len = 0x3C; break;
        case PCI_CAP_ID_AF:   len = 6; break;
        case PCI_CAP_ID_MSI: {
            uint16_t mc = (uint16_t)(h >> 16);
            len = 10 + ((mc & 0x80) ? 4 : 0) + ((mc & 0x100) ? 10 : 0);
            break;
        }
        case 0x09: len = (h >> 16) & 0xFF; if (len < 3) len = next - lo; break;  /* vendor specific */
        default:   len = next - lo; break;                 /* unknown: up to the next one */
        }
        for (uint32_t b = lo; b < lo + len && b < 0x100; b++) d->cap_bytes[b >> 3] |= (uint8_t)(1u << (b & 7));
    }
    /* The bytes 0x00..0x3F are the header: never device-specific. */
    for (int b = 0; b < 0x40; b++) d->cap_bytes[b >> 3] |= (uint8_t)(1u << (b & 7));
}

static void read_irq_geometry(udrv_dev_t* d) {
    const pci_dev_t* pd = d->pd;
    d->msix_n = 0; d->msi_cap = 0; d->msi_pvm = 0; d->msi_max = 0;
    if (pd->cap_msix) {
        uint32_t h = cfg_rd32(d, pd->cap_msix);
        uint32_t t = cfg_rd32(d, (uint8_t)(pd->cap_msix + 4));
        uint32_t b = cfg_rd32(d, (uint8_t)(pd->cap_msix + 8));
        uint16_t n = (uint16_t)(((h >> 16) & 0x7FF) + 1);
        uint8_t tb = (uint8_t)(t & 7), pb = (uint8_t)(b & 7);
        uint32_t to = t & ~7u, po = b & ~7u;
        /* Untrusted geometry: both structures must sit inside a memory BAR. */
        int ok = tb < 6 && pb < 6 && (pd->bar_flags[tb] & PCI_BAR_PRESENT) && !(pd->bar_flags[tb] & PCI_BAR_IO) &&
                 (pd->bar_flags[pb] & PCI_BAR_PRESENT) && !(pd->bar_flags[pb] & PCI_BAR_IO) &&
                 (uint64_t)to + (uint64_t)n * 16 <= pd->bar_size[tb] &&
                 (uint64_t)po + (((uint64_t)n + 63) / 64) * 8 <= pd->bar_size[pb];
        if (ok) { d->msix_n = n; d->msix_tbir = tb; d->msix_pbir = pb; d->msix_toff = to; d->msix_poff = po; }
        else debugcon_writestring("[UDRV] MSI-X geometry outside its BAR: MSI-X not offered\n");
    }
    if (pd->cap_msi) {
        uint16_t mc = (uint16_t)(cfg_rd32(d, pd->cap_msi) >> 16);
        d->msi_cap = pd->cap_msi;
        d->msi_pvm = (mc & 0x100) ? 1 : 0;
        uint32_t mmc = (mc >> 1) & 7; if (mmc > 5) mmc = 5;
        d->msi_max = (uint8_t)(1u << mmc);
    }
    d->pm_reset = 0;
    if (pd->cap_pm) {
        uint32_t pmcsr = cfg_rd32(d, (uint8_t)(pd->cap_pm + 4));
        if (!(pmcsr & 0x8)) d->pm_reset = 1;       /* No_Soft_Reset clear */
    }
}

/* Turn on the ACS isolation controls the upstream ports advertise (M53a only
 * read them): an assigned device's group is trusted only with them on. */
static void enable_acs_path(const pci_dev_t* pd) {
    int idx = pd->parent, hops = 0;
    while (idx >= 0 && hops++ < 32) {
        const pci_dev_t* b = pci_dev_at(idx);
        if (!b) break;
        if (b->ecap_acs) {
            uint32_t v = pci_config_read32_ext(b->bus, b->slot, b->func, (uint16_t)(b->ecap_acs + 4));
            uint16_t ctrl = (uint16_t)(v >> 16) | (uint16_t)(b->acs_cap & PCI_ACS_ISOLATE);
            pci_config_write32_ext(b->bus, b->slot, b->func, (uint16_t)(b->ecap_acs + 4),
                                   (v & 0xFFFFu) | ((uint32_t)ctrl << 16));
        }
        idx = b->parent;
    }
}

static void assign(const pci_dev_t* pd) {
    iommu_line_t l; l.n = 0;
    uint16_t bdf = (uint16_t)((pd->bus << 8) | (pd->slot << 3) | pd->func);
    if (dev_by_bdf(bdf)) return;
    iommu_ln_s(&l, "[UDRV] udrv= "); iommu_ln_bdf(&l, bdf); iommu_ln_s(&l, " ");
    iommu_ln_hexw(&l, pd->vendor_id, 4); iommu_ln_s(&l, ":"); iommu_ln_hexw(&l, pd->device_id, 4);
    if (pd->class_code == 0x01) { iommu_ln_s(&l, " refused: storage is never assignable"); log_line(&l); return; }
    if (pd->header_type != 0) { iommu_ln_s(&l, " refused: not an endpoint"); log_line(&l); return; }
    if (!pd->assignable) {
        iommu_ln_s(&l, " refused: not assignable ("); iommu_ln_s(&l, pci_assign_reason(pd->why));
        iommu_ln_s(&l, ")"); log_line(&l); return;
    }
    if (g_ndev >= UDRV_MAX_DEV) { iommu_ln_s(&l, " refused: too many"); log_line(&l); return; }
    udrv_dev_t* d = &g_dev[g_ndev];
    bzero8(d, sizeof(*d));
    d->pd = pd; d->bdf = bdf;
    d->loc.bus = pd->bus; d->loc.slot = pd->slot; d->loc.func = pd->func;
    d->loc.vendor_id = pd->vendor_id; d->loc.device_id = pd->device_id;
    spin_init_class_dynamic(&d->ev, LK_S25_UDRV_EV, "udrv-ev");
    for (int i = 0; i < UDRV_MAX_VEC; i++) { d->vec[i].d = d; d->vec[i].idx = (uint8_t)i; d->vec[i].vec = -1; }
    for (int i = 0; i < 16; i++) d->hdr[i] = cfg_rd32(d, (uint16_t)(i * 4));
    pci_registry_reserve(pd);                 /* in-kernel probes skip it */
    cmd_write(d, CMD_INTX_DIS);               /* no decode, no DMA until claimed */
    build_cap_bytes(d);
    read_irq_geometry(d);
    enable_acs_path(pd);
    int covered = iommu_present() && iommu_dev_covered(pd);
    if (covered) iommu_detach(pd);            /* blocked until a driver claims it */
    g_ndev++;
    iommu_ln_s(&l, covered ? " assigned to ring 3 (iommu: blocked)" : " assigned to ring 3 (no iommu: MMIO only)");
    if (d->msix_n) { iommu_ln_s(&l, " msix="); iommu_ln_dec(&l, d->msix_n); }
    if (d->msi_cap) { iommu_ln_s(&l, " msi="); iommu_ln_dec(&l, d->msi_max); iommu_ln_s(&l, d->msi_pvm ? "+pvm" : ""); }
    iommu_ln_s(&l, (pd->flr & (PCI_FLR_PCIE | PCI_FLR_AF)) ? " flr" : d->pm_reset ? " pm-reset" : " no-reset");
    log_line(&l);
}

void udrv_init(void) {
    char buf[200];
    int n = iommu_cmdline_value("udrv", buf, sizeof(buf));
    g_unsafe = iommu_cmdline_has("udrv.unsafe");
    g_noir = iommu_cmdline_has("udrv.noir");
    for (int i = 0; i < UDRV_MAX_DEV; i++) g_dev[i].bdf = 0xFFFF;
    if (n <= 0) {
        if (g_unsafe || g_noir) debugcon_writestring("[UDRV] udrv.unsafe/udrv.noir ignored: no udrv= devices\n");
        g_unsafe = g_noir = 0;
        return;
    }
    if (!pci_registry_complete()) {
        debugcon_writestring("[UDRV] udrv= ignored: PCI registry incomplete\n");
        return;
    }
    for (int i = 0; i < n; ) {
        int j = i; while (j < n && buf[j] != ',') j++;
        const char* t = &buf[i];
        int tl = j - i;
        int matched = 0;
        if (tl == 7 && t[2] == ':' && t[5] == '.') {           /* bb:dd.f */
            int b = hexn(t, 2), s = hexn(t + 3, 2), f = hexn(t + 6, 1);
            if (b >= 0 && s >= 0 && s < 32 && f >= 0 && f < 8) {
                const pci_dev_t* pd = pci_dev_lookup(0, (uint8_t)b, (uint8_t)s, (uint8_t)f);
                if (pd) { assign(pd); matched = 1; }
            }
        } else if (tl == 9 && t[4] == ':') {                    /* vvvv:dddd */
            int v = hexn(t, 4), dv = hexn(t + 5, 4);
            if (v >= 0 && dv >= 0)
                for (int k = 0; k < pci_dev_count(); k++) {
                    const pci_dev_t* pd = pci_dev_at(k);
                    if (pd && pd->vendor_id == v && pd->device_id == dv) { assign(pd); matched = 1; }
                }
        }
        if (!matched) {
            iommu_line_t l; l.n = 0;
            iommu_ln_s(&l, "[UDRV] udrv= entry ignored (no such function or bad syntax): ");
            for (int k = 0; k < tl && k < 16; k++) { char ch[2] = { t[k], 0 }; iommu_ln_s(&l, ch); }
            log_line(&l);
        }
        i = j + 1;
    }
    if (g_ndev) iommu_set_fault_hook(udrv_fault_hook);
    iommu_line_t l; l.n = 0;
    iommu_ln_s(&l, "[UDRV] "); iommu_ln_dec(&l, (uint64_t)g_ndev);
    iommu_ln_s(&l, " device(s) for ring-3 drivers; iommu=");
    iommu_ln_s(&l, iommu_present() ? "on" : "off");
    iommu_ln_s(&l, " ir="); iommu_ln_s(&l, iommu_ir_enabled() ? "on" : "off");
    if (g_noir) iommu_ln_s(&l, " udrv.noir");
    if (g_unsafe) iommu_ln_s(&l, " udrv.unsafe");
    log_line(&l);
    if (g_unsafe) {
        debugcon_writestring("[UDRV] *** WARNING: udrv.unsafe: ring-3 drivers may DMA WITHOUT an IOMMU or interrupt remapping. ***\n");
        debugcon_writestring("[UDRV] *** A buggy or hostile driver can overwrite any memory. Development only.            ***\n");
    }
}

/* ------------------------------------------------------------- /proc/udrv */
void udrv_generate(void (*emit)(void* ctx, const char* line), void* ctx) {
    iommu_line_t l; l.n = 0;
    iommu_ln_s(&l, "udrv devices "); iommu_ln_dec(&l, (uint64_t)g_ndev);
    iommu_ln_s(&l, " iommu="); iommu_ln_s(&l, iommu_present() ? "on" : "off");
    iommu_ln_s(&l, " ir="); iommu_ln_s(&l, iommu_ir_enabled() ? "on" : "off");
    iommu_ln_s(&l, " noir="); iommu_ln_dec(&l, (uint64_t)g_noir);
    iommu_ln_s(&l, " unsafe="); iommu_ln_dec(&l, (uint64_t)g_unsafe);
    iommu_ln_s(&l, " refused_claims="); iommu_ln_dec(&l, g_refused_claim);
    iommu_ln_s(&l, " refused_dma="); iommu_ln_dec(&l, g_refused_dma);
    emit(ctx, iommu_ln_end(&l));
    if (g_unsafe || g_tainted) {
        l.n = 0;
        iommu_ln_s(&l, g_tainted ? "TAINTED: ring-3 DMA without IOMMU" : "WARNING: udrv.unsafe set (ring-3 DMA without IOMMU allowed)");
        emit(ctx, iommu_ln_end(&l));
    }
    for (int i = 0; i < g_ndev; i++) {
        udrv_dev_t* d = &g_dev[i];
        uint64_t fl = spin_lock_irqsave(&d->ev);
        int claimed = d->claimed, q = d->quarantined, mode = d->irq_mode, nv = d->nvec;
        uint32_t pid = d->owner_pid, pend = d->pending, msk = d->masked;
        uint64_t irqs = d->irq_count, stray = d->stray, faults = d->faults;
        spin_unlock_irqrestore(&d->ev, fl);
        l.n = 0;
        iommu_ln_s(&l, "dev "); ln_dev(&l, d); iommu_ln_s(&l, " ");
        iommu_ln_hexw(&l, d->pd->vendor_id, 4); iommu_ln_s(&l, ":"); iommu_ln_hexw(&l, d->pd->device_id, 4);
        iommu_ln_s(&l, q ? " quarantined" : claimed ? " claimed" : " free");
        if (claimed) { iommu_ln_s(&l, " pid="); iommu_ln_dec(&l, pid); }
        iommu_ln_s(&l, " claims="); iommu_ln_dec(&l, d->claims);
        iommu_ln_s(&l, " faults="); iommu_ln_dec(&l, faults);
        iommu_ln_s(&l, " kills="); iommu_ln_dec(&l, d->kills);
        iommu_ln_s(&l, " irqs="); iommu_ln_dec(&l, irqs);
        iommu_ln_s(&l, " stray="); iommu_ln_dec(&l, stray);
        iommu_ln_s(&l, " mode="); iommu_ln_s(&l, mode == UDRV_IRQ_MSIX ? "msix" : mode == UDRV_IRQ_MSI ? "msi" : "none");
        iommu_ln_s(&l, " nvec="); iommu_ln_dec(&l, (uint64_t)nv);
        iommu_ln_s(&l, " pending="); iommu_ln_hex(&l, pend);
        iommu_ln_s(&l, " masked="); iommu_ln_hex(&l, msk);
        iommu_ln_s(&l, " dma="); iommu_ln_dec(&l, claimed ? d->dma_used : 0);
        emit(ctx, iommu_ln_end(&l));
    }
}
