/*
 * irq.c — dynamic interrupt vectors (M53c): allocator, generic dispatch,
 * /proc/interrupts counters, boot self-test. See irq.h for the contract.
 *
 * SPDX-License-Identifier: MIT
 */
#include "irq.h"
#include "spinlock.h"
#include "percpu.h"
#include "lapic.h"
#include "tsc.h"
#include "klock.h"
#include "debugcon.h"
#include "iommu.h"
#include <stddef.h>

typedef struct irq_desc {
    /* Published state (read by irq_dispatch without a lock). */
    irq_handler_t     handler;      /* NULL = not bound                    */
    void*             ctx;
    volatile uint32_t active;       /* CPUs inside the handler right now   */
    /* Allocator state (g_irqvec_lock). */
    uint8_t           allocated;    /* reserved, possibly not yet bound    */
    uint32_t          cpu;          /* target CPU index                    */
    const char*       name;
    /* Counters. */
    volatile uint64_t count;        /* handler runs                        */
    volatile uint64_t off_cpu;      /* delivered to a CPU other than `cpu` */
} irq_desc_t;

static irq_desc_t g_irq_desc[IRQ_VEC_COUNT];
static spinlock_t g_irqvec_lock = SPINLOCK_INIT_CLASS(LK_S66_IRQVEC, "irqvec");
static volatile uint64_t g_irq_spurious;    /* in range, nothing bound      */

static inline int vec_ok(int vec) { return vec >= IRQ_VEC_FIRST && vec <= IRQ_VEC_LAST; }

int irq_pick_cpu(uint32_t cpu) {
    if (cpu == IRQ_CPU_ANY) return 0;                 /* the BSP, as before M53c */
    if (cpu >= smp_cpu_count() || !cpu_by_index(cpu)) return -1;
    return (int)cpu;
}

/* Reserve [base, base+count) under the lock, then publish the handler. */
static void bind_range(int first, uint32_t count, uint32_t cpu,
                       irq_handler_t handler, void* ctx, const char* name) {
    for (uint32_t i = 0; i < count; i++) {
        irq_desc_t* d = &g_irq_desc[first + i];
        d->allocated = 1;
        d->cpu = cpu;
        d->name = name ? name : "irq";
        d->count = 0;
        d->off_cpu = 0;
        d->ctx = ctx;
        __atomic_store_n(&d->handler, handler, __ATOMIC_RELEASE);
    }
}

int irq_vec_alloc_block(uint32_t cpu, uint32_t count, irq_handler_t handler,
                        void* ctx, const char* name) {
    if (!handler || count == 0 || count > 32 || (count & (count - 1))) return -1;
    int c = irq_pick_cpu(cpu);
    if (c < 0) return -1;
    int got = -1;
    uint64_t fl = spin_lock_irqsave(&g_irqvec_lock);
    /* First vector >= IRQ_VEC_FIRST whose number is a multiple of count. */
    int start = (IRQ_VEC_FIRST + (int)count - 1) & ~((int)count - 1);
    for (int v = start; v + (int)count - 1 <= IRQ_VEC_LAST; v += (int)count) {
        int idx = v - IRQ_VEC_FIRST, free = 1;
        for (uint32_t i = 0; i < count; i++)
            if (g_irq_desc[idx + i].allocated) { free = 0; break; }
        if (!free) continue;
        bind_range(idx, count, (uint32_t)c, handler, ctx, name);
        got = v;
        break;
    }
    spin_unlock_irqrestore(&g_irqvec_lock, fl);
    return got;
}

int irq_vec_alloc(uint32_t cpu, irq_handler_t handler, void* ctx, const char* name) {
    return irq_vec_alloc_block(cpu, 1, handler, ctx, name);
}

void irq_vec_free(int vec) {
    if (!vec_ok(vec)) return;
    irq_desc_t* d = &g_irq_desc[vec - IRQ_VEC_FIRST];
    uint64_t fl = spin_lock_irqsave(&g_irqvec_lock);
    int was = d->allocated && d->handler;
    if (was) __atomic_store_n(&d->handler, NULL, __ATOMIC_SEQ_CST);
    spin_unlock_irqrestore(&g_irqvec_lock, fl);
    if (!was) return;
    /* [M56] With interrupt remapping, clear the vector's remapping entry
     * (and wait for the entry-cache invalidation): a late message from the
     * old source now faults instead of reaching a later owner. */
    iommu_ir_vec_free(vec);
    /* Dekker pair with irq_dispatch: it increments `active` (seq_cst) before
     * loading the handler; we cleared the handler (seq_cst) before reading
     * `active`. So once active reads 0, no CPU can still use handler/ctx. The
     * wait keeps answering TLB shootdowns (a handler may be waiting on one). */
    while (__atomic_load_n(&d->active, __ATOMIC_SEQ_CST))
        cpu_relax_poll();
    fl = spin_lock_irqsave(&g_irqvec_lock);
    d->ctx = NULL;
    d->name = NULL;
    d->allocated = 0;
    spin_unlock_irqrestore(&g_irqvec_lock, fl);
}

void irq_vec_free_block(int base, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) irq_vec_free(base + (int)i);
}

int irq_vec_cpu(int vec) {
    if (!vec_ok(vec)) return -1;
    irq_desc_t* d = &g_irq_desc[vec - IRQ_VEC_FIRST];
    return d->allocated ? (int)d->cpu : -1;
}

int irq_vec_set_cpu(int vec, uint32_t cpu) {
    if (!vec_ok(vec)) return -1;
    int c = irq_pick_cpu(cpu);
    if (c < 0) return -1;
    irq_desc_t* d = &g_irq_desc[vec - IRQ_VEC_FIRST];
    uint64_t fl = spin_lock_irqsave(&g_irqvec_lock);
    int r = d->allocated ? 0 : -1;
    if (!r) d->cpu = (uint32_t)c;
    spin_unlock_irqrestore(&g_irqvec_lock, fl);
    /* [M56] A remapped source follows its vector: the remapping entry's
     * destination is rewritten (no-op without interrupt remapping). */
    if (!r) iommu_ir_vec_set_cpu(vec, (uint32_t)c);
    return r;
}

void irq_dispatch(uint64_t vec) {
    if (vec >= IRQ_VEC_FIRST && vec <= IRQ_VEC_LAST) {
        irq_desc_t* d = &g_irq_desc[vec - IRQ_VEC_FIRST];
        __atomic_add_fetch(&d->active, 1, __ATOMIC_SEQ_CST);
        irq_handler_t h = __atomic_load_n(&d->handler, __ATOMIC_ACQUIRE);
        if (h) {
            if (this_cpu()->index != d->cpu)
                __atomic_add_fetch(&d->off_cpu, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&d->count, 1, __ATOMIC_RELAXED);
            h((uint8_t)vec, d->ctx);
        } else {
            __atomic_add_fetch(&g_irq_spurious, 1, __ATOMIC_RELAXED);
        }
        __atomic_sub_fetch(&d->active, 1, __ATOMIC_SEQ_CST);
    } else {
        __atomic_add_fetch(&g_irq_spurious, 1, __ATOMIC_RELAXED);
    }
    /* Every vector in the range arrives through the LAPIC (MSI, IPI or an
     * IOAPIC line): EOI it whether or not anything was bound. */
    lapic_eoi();
}

uint64_t irq_spurious_count(void) { return g_irq_spurious; }

uint32_t irq_vec_free_count(void) {
    uint32_t n = 0;
    uint64_t fl = spin_lock_irqsave(&g_irqvec_lock);
    for (int i = 0; i < IRQ_VEC_COUNT; i++) if (!g_irq_desc[i].allocated) n++;
    spin_unlock_irqrestore(&g_irqvec_lock, fl);
    return n;
}

/* ---- /proc/interrupts ---------------------------------------------------- */
static int put_str(char* b, int n, int cap, const char* s) {
    while (s && *s && n < cap - 1) b[n++] = *s++;
    return n;
}
static int put_u64(char* b, int n, int cap, uint64_t v, int width) {
    char t[24]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 23);
    while (width-- > k && n < cap - 1) b[n++] = ' ';
    while (k && n < cap - 1) b[n++] = t[--k];
    return n;
}
static int put_hex2(char* b, int n, int cap, uint32_t v) {
    static const char hx[] = "0123456789abcdef";
    if (n < cap - 3) { b[n++] = hx[(v >> 4) & 15]; b[n++] = hx[v & 15]; }
    return n;
}

void irq_vec_generate(void (*emit)(void* ctx, const char* line), void* ectx) {
    char line[96];
    int n = put_str(line, 0, sizeof line, "vec  cpu      count    off_cpu name\n");
    line[n] = 0; emit(ectx, line);
    for (int i = 0; i < IRQ_VEC_COUNT; i++) {
        /* Snapshot under the lock; emit outside it (emit may take any lock). */
        uint64_t fl = spin_lock_irqsave(&g_irqvec_lock);
        irq_desc_t s = g_irq_desc[i];
        spin_unlock_irqrestore(&g_irqvec_lock, fl);
        if (!s.allocated) continue;
        n = put_str(line, 0, sizeof line, " ");
        n = put_hex2(line, n, sizeof line, (uint32_t)(IRQ_VEC_FIRST + i));
        n = put_str(line, n, sizeof line, ":");
        n = put_u64(line, n, sizeof line, s.cpu, 4);
        n = put_u64(line, n, sizeof line, s.count, 11);
        n = put_u64(line, n, sizeof line, s.off_cpu, 11);
        n = put_str(line, n, sizeof line, " ");
        n = put_str(line, n, sizeof line, s.handler ? s.name : "(unbinding)");
        n = put_str(line, n, sizeof line, "\n");
        line[n] = 0; emit(ectx, line);
    }
    n = put_str(line, 0, sizeof line, "spurious ");
    n = put_u64(line, n, sizeof line, g_irq_spurious, 0);
    n = put_str(line, n, sizeof line, " free ");
    n = put_u64(line, n, sizeof line, irq_vec_free_count(), 0);
    n = put_str(line, n, sizeof line, "\n");
    line[n] = 0; emit(ectx, line);
}

/* ---- boot self-test ------------------------------------------------------ */
static volatile uint32_t g_st_hits;
static volatile uint32_t g_st_cpu;
static volatile uint32_t g_st_badctx;
static uint32_t g_st_cookie;

static void st_handler(uint8_t vec, void* ctx) {
    (void)vec;
    if (ctx != &g_st_cookie) g_st_badctx++;
    g_st_cpu = this_cpu()->index;
    __atomic_add_fetch(&g_st_hits, 1, __ATOMIC_SEQ_CST);
}

static int st_wait_hits(uint32_t want) {
    uint64_t t0 = ktime_us();
    while (g_st_hits < want && ktime_us() - t0 < 500000ULL) cpu_wait_irq();
    return g_st_hits >= want;
}

static void st_fail(const char* what) {
    debugcon_writestring("[IRQ] vector selftest FAIL: ");
    debugcon_writestring(what);
    debugcon_writestring("\n");
}

void irq_vec_selftest(void) {
    if (!apic_mode_active() || !lapic_is_enabled()) {
        debugcon_writestring("[IRQ] vector selftest skipped (no LAPIC)\n");
        return;
    }
    uint32_t free0 = irq_vec_free_count();
    g_st_hits = 0; g_st_badctx = 0;

    /* 1. A self-IPI to an allocated vector runs its handler with its ctx. */
    int v = irq_vec_alloc(0, st_handler, &g_st_cookie, "selftest");
    if (v < 0) { st_fail("alloc"); return; }
    lapic_ipi(cpu_by_index(0)->lapic_id, (uint8_t)v);
    if (!st_wait_hits(1) || g_st_badctx || g_st_cpu != 0) {
        st_fail("self-IPI to an allocated vector"); irq_vec_free(v); return;
    }

    /* 2. Retarget to an AP (when one is online): the same vector runs there. */
    uint32_t ap = 0;
    for (uint32_t i = 1; i < smp_cpu_count(); i++) if (smp_cpu_online(i)) { ap = i; break; }
    if (ap) {
        irq_vec_set_cpu(v, ap);
        lapic_ipi(cpu_by_index(ap)->lapic_id, (uint8_t)v);
        if (!st_wait_hits(2) || g_st_cpu != ap) {
            st_fail("IPI to an AP-targeted vector"); irq_vec_free(v); return;
        }
    }

    /* 3. Negative: a freed vector never calls the old handler; it is counted
     *    as spurious and still EOIed (the LAPIC keeps delivering). */
    uint32_t hits = g_st_hits;
    uint64_t sp0 = g_irq_spurious;
    irq_vec_free(v);
    lapic_ipi(cpu_by_index(0)->lapic_id, (uint8_t)v);
    uint64_t t0 = ktime_us();
    while (g_irq_spurious == sp0 && ktime_us() - t0 < 500000ULL) cpu_wait_irq();
    if (g_st_hits != hits || g_irq_spurious != sp0 + 1) { st_fail("freed vector"); return; }

    /* 4. Blocks are aligned to their size; 5. a full table refuses. */
    int b = irq_vec_alloc_block(0, 8, st_handler, &g_st_cookie, "selftest-blk");
    if (b < 0 || (b & 7)) { st_fail("aligned block"); if (b >= 0) irq_vec_free_block(b, 8); return; }
    irq_vec_free_block(b, 8);
    if (irq_vec_alloc_block(0, 3, st_handler, NULL, "x") != -1 ||
        irq_vec_alloc(0, NULL, NULL, "x") != -1 ||
        irq_vec_alloc(SMP_MAX_CPUS, st_handler, NULL, "x") != -1) {
        st_fail("bad arguments accepted"); return;
    }
    int got[IRQ_VEC_COUNT + 1], n = 0;
    while (n <= IRQ_VEC_COUNT) {
        int x = irq_vec_alloc(0, st_handler, &g_st_cookie, "selftest-fill");
        if (x < 0) break;
        got[n++] = x;
    }
    int full_ok = ((uint32_t)n == free0);
    for (int i = 0; i < n; i++) irq_vec_free(got[i]);
    if (!full_ok) { st_fail("exhaustion"); return; }
    if (irq_vec_free_count() != free0) { st_fail("leak"); return; }

    debugcon_writestring("[IRQ] vectors 0x50-0xdf: ");
    debugcon_print_hex(free0);
    debugcon_writestring(ap ? " free, self-IPI + AP-IPI + freed-vector + block + full OK\n"
                            : " free, self-IPI + freed-vector + block + full OK (no AP)\n");
    debugcon_writestring("[IRQ] vector selftest PASS\n");
}
