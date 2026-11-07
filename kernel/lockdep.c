/*
 * lockdep.c — run-time lock validator (M50, -DSECOS_LOCKDEP). See lockdep.h.
 *
 * Per CPU: a stack of the spinlocks it holds (with the IF state at acquisition
 * and the interrupt frame they were taken in) and a stack of interrupt-frame
 * bases. Per task: the klock stack in klock_task_t (kernel/klock.c). Order is
 * checked within one interrupt frame: an IRQ handler starts a new frame, since
 * it only nests inside locks the interrupted code took with interrupts ON, and
 * those are covered by the IRQ-safety rule (a lock taken with IF on and also
 * from IRQ context can deadlock its own CPU). Recursion is checked across all
 * frames.
 *
 * Reports go to debugcon through the raw (untracked) output locks, then panic
 * unless the boot command line has `lockdep=warn` (each distinct report is
 * then printed once and the kernel goes on). While a CPU is reporting, its
 * hooks do the raw lock operations only, so the panic path cannot recurse.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lockdep.h"

#ifdef SECOS_LOCKDEP
#include "spinlock.h"
#include "klock.h"
#include "percpu.h"
#include "process.h"
#include "panic.h"
#include "debugcon.h"

extern uint64_t tsc_read(void);
extern uint64_t tsc_hz(void);
extern int kernel_cmdline_has(const char* word);
void lockstat_line(void (*emit)(void*, const char*), void* ctx, const char* kind,
                   const char* name, uint16_t level, uint64_t acq, uint64_t cont,
                   uint64_t max_cyc);

#define RF_IF (1ULL << 9)
#define LD_F_IRQ   0x01        /* taken from IRQ context                   */
#define LD_F_IFON  0x02        /* taken with IF on outside IRQ context     */

#define LD_STACK   16
#define LD_FRAMES  8
#define LD_STUCK_S 2           /* watchdog: a wait longer than this        */

struct ld_ent {
    spinlock_t* l;
    uint16_t level;
    uint8_t  if_on;            /* IF at acquisition                        */
    uint8_t  frame;            /* interrupt frame it was taken in          */
};
struct ld_cpu {
    uint32_t depth;
    struct ld_ent st[LD_STACK];
    uint32_t nframe;
    uint32_t fbase[LD_FRAMES];
    volatile uint32_t in_report;
};
static struct ld_cpu g_ld[SMP_MAX_CPUS];

static inline uint64_t ld_rflags(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; pop %0" : "=r"(f));
    return f;
}
static inline uint32_t ld_idx(void) {
    uint32_t i = this_cpu()->index;
    return i < SMP_MAX_CPUS ? i : 0;
}
static inline struct ld_cpu* ld_me(void) { return &g_ld[ld_idx()]; }

/* ---- reporting ------------------------------------------------------------ */
static int g_warn_mode = -1;           /* -1 = not read yet */
#define LD_SEEN_MAX 64
static const void* g_seen[LD_SEEN_MAX][2];
static volatile uint32_t g_nseen;
static volatile uint32_t g_nreports;

static void ld_name(const char* n) { debugcon_writestring(n ? n : "?"); }

static void ld_dump(struct ld_cpu* d) {
    debugcon_writestring("[LOCKDEP]   cpu ");
    debugcon_print_hex(ld_idx());
    debugcon_writestring(" holds:");
    if (!d->depth) debugcon_writestring(" no spinlock");
    for (uint32_t i = 0; i < d->depth && i < LD_STACK; i++) {
        debugcon_writestring(" ");
        ld_name(d->st[i].l->name);
        debugcon_writestring("(");
        debugcon_print_hex(d->st[i].level);
        debugcon_writestring(d->st[i].if_on ? ",if)" : ")");
    }
    debugcon_writestring("\n");
}

void lockdep_dump_cpu(void) { ld_dump(ld_me()); }

/* Report `what` about lock `a` (and `b`, the lock it conflicts with). */
static volatile int g_ld_expect;        /* boot selftest: count, do not panic */
static volatile uint32_t g_ld_caught;

static void ld_report2(const char* what, const char* a, const char* b, void* caller) {
    struct ld_cpu* d = ld_me();
    if (d->in_report) return;
    if (g_ld_expect) {
        g_ld_caught++;
        debugcon_writestring("[LDTEST]   caught: ");
        debugcon_writestring(what);
        debugcon_writestring(" (");
        ld_name(a);
        debugcon_writestring(")\n");
        return;
    }
    if (g_warn_mode < 0) g_warn_mode = kernel_cmdline_has("lockdep=warn");
    if (g_warn_mode) {
        uint32_t n = g_nseen;
        for (uint32_t i = 0; i < n && i < LD_SEEN_MAX; i++)
            if (g_seen[i][0] == what && g_seen[i][1] == (const void*)a) return;
        if (n < LD_SEEN_MAX) { g_seen[n][0] = what; g_seen[n][1] = a; g_nseen = n + 1; }
    }
    d->in_report = 1;
    __atomic_add_fetch(&g_nreports, 1, __ATOMIC_RELAXED);
    debugcon_writestring("[LOCKDEP] ");
    debugcon_writestring(what);
    debugcon_writestring(": ");
    ld_name(a);
    if (b) { debugcon_writestring(" vs "); ld_name(b); }
    debugcon_writestring(" caller=");
    debugcon_print_hex(kaddr_link((uint64_t)caller));
    debugcon_writestring("\n");
    klock_dump_held();
    debugcon_kflush();
    if (!g_warn_mode) kernel_panic("lockdep violation", __FILE__, __LINE__);
    d->in_report = 0;
}

void lockdep_report(const char* what, const char* name) {
    ld_report2(what, name, 0, __builtin_return_address(0));
}

/* ---- /proc/lockstat registry ---------------------------------------------- */
#define LD_REG_MAX 192
static spinlock_t* g_reg[LD_REG_MAX];
static volatile uint32_t g_nreg;

static void ld_register(spinlock_t* l) {        /* called by the holder */
    uint32_t i = __atomic_fetch_add(&g_nreg, 1, __ATOMIC_SEQ_CST);
    if (i >= LD_REG_MAX) { l->reg = 0xFF; return; }
    g_reg[i] = l;
    l->reg = (uint8_t)((i + 1) > 0xFE ? 0xFE : (i + 1));
}

void lockdep_stat_spin(void (*emit)(void*, const char*), void* ctx) {
    uint32_t n = g_nreg < LD_REG_MAX ? g_nreg : LD_REG_MAX;
    for (uint32_t i = 0; i < n; i++) {
        spinlock_t* l = g_reg[i];
        if (!l) continue;
        lockstat_line(emit, ctx, "spin", l->name, l->level, l->n_acq, l->n_contended,
                      l->max_wait_cyc);
    }
    char b[64]; int k = 0;
    const char* s = "lockdep reports ";
    while (*s) b[k++] = *s++;
    uint32_t v = g_nreports; char t[12]; int m = 0;
    do { t[m++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (m) b[k++] = t[--m];
    b[k++] = '\n'; b[k] = 0;
    emit(ctx, b);
}

/* ---- spinlocks ------------------------------------------------------------ */
static void ld_check_acquire(struct ld_cpu* d, spinlock_t* l, uint64_t rf, void* caller) {
    uint32_t base = d->nframe ? d->fbase[d->nframe - 1] : 0;
    uint32_t n = d->depth < LD_STACK ? d->depth : LD_STACK;
    for (uint32_t i = n; i-- > 0;) {
        struct ld_ent* e = &d->st[i];
        if (e->l == l) { ld_report2("recursive spinlock acquisition", l->name, 0, caller); return; }
        if (i >= base && e->level != LK_NONE && l->level != LK_NONE && e->level >= l->level) {
            ld_report2("lock order inversion: taking", l->name, e->l->name, caller);
            return;
        }
    }
    /* IRQ safety: a lock taken with IF on can be interrupted by a handler on
     * the same CPU that wants it too. */
    uint8_t f = d->nframe ? LD_F_IRQ : ((rf & RF_IF) ? LD_F_IFON : 0);
    if (f && !(l->ld_flags & f)) {
        uint8_t nf = (uint8_t)(l->ld_flags | f);
        if ((nf & (LD_F_IRQ | LD_F_IFON)) == (LD_F_IRQ | LD_F_IFON))
            ld_report2("spinlock taken with interrupts on and from IRQ context", l->name, 0, caller);
        __atomic_or_fetch(&l->ld_flags, f, __ATOMIC_RELAXED);
    }
}

static void ld_push(struct ld_cpu* d, spinlock_t* l, uint64_t rf) {
    if (d->depth >= LD_STACK) { ld_report2("spinlock stack overflow", l->name, 0, 0); return; }
    struct ld_ent* e = &d->st[d->depth++];
    e->l = l;
    e->level = l->level;
    e->if_on = (rf & RF_IF) ? 1 : 0;
    e->frame = (uint8_t)d->nframe;
}

static void ld_acquired(struct ld_cpu* d, spinlock_t* l, uint64_t rf) {
    l->owner_cpu = ld_idx() + 1;
    l->t_acq = tsc_read();
    l->n_acq++;
    if (!l->reg) ld_register(l);
    ld_push(d, l, rf);
}

static void ld_stuck(const char* kind, const char* name, const char* okind, uint64_t oid,
                     uint64_t held_cyc) {
    struct ld_cpu* d = ld_me();
    if (d->in_report) return;
    d->in_report = 1;
    uint64_t hz = tsc_hz();
    debugcon_writestring("[LOCKDEP] stuck on ");
    debugcon_writestring(kind);
    debugcon_writestring(" ");
    ld_name(name);
    debugcon_writestring(" owner=");
    debugcon_writestring(okind);
    debugcon_writestring(" ");
    debugcon_print_hex(oid);
    debugcon_writestring(" held_ms=");
    debugcon_print_hex(hz ? held_cyc / (hz / 1000 ? hz / 1000 : 1) : 0);
    debugcon_writestring(" mine:\n");
    klock_dump_held();
    debugcon_kflush();
    __atomic_add_fetch(&g_nreports, 1, __ATOMIC_RELAXED);
    d->in_report = 0;
}

void lockdep_spin_lock(spinlock_t* l) {
    uint64_t rf = ld_rflags();
    struct ld_cpu* d = ld_me();
    if (d->in_report) { spin_lock_raw(l); return; }
    void* caller = __builtin_return_address(0);
    ld_check_acquire(d, l, rf, caller);
    if (!spin_trylock_raw(l)) {
        uint64_t t0 = tsc_read(), hz = tsc_hz();
        uint32_t spins = 0;
        int reported = 0;
        for (;;) {
            while (l->locked) {
                cpu_relax_poll();
                if (!reported && hz && (++spins & 0x3FF) == 0 &&
                    tsc_read() - t0 > hz * LD_STUCK_S) {
                    reported = 1;
                    uint32_t oc = l->owner_cpu;
                    ld_stuck("spinlock", l->name, "cpu", oc ? oc - 1 : 0xFFFF,
                             tsc_read() - l->t_acq);
                }
            }
            if (spin_trylock_raw(l)) break;
        }
        uint64_t w = tsc_read() - t0;
        l->n_contended++;
        if (w > l->max_wait_cyc) l->max_wait_cyc = w;
    }
    ld_acquired(d, l, rf);
}

int lockdep_spin_trylock(spinlock_t* l) {
    uint64_t rf = ld_rflags();
    struct ld_cpu* d = ld_me();
    if (d->in_report) return spin_trylock_raw(l);
    /* A trylock cannot deadlock: no order check, but recursion is a bug. */
    uint32_t n = d->depth < LD_STACK ? d->depth : LD_STACK;
    for (uint32_t i = 0; i < n; i++)
        if (d->st[i].l == l) {
            ld_report2("trylock of a spinlock this CPU holds", l->name, 0,
                       __builtin_return_address(0));
            return 0;
        }
    if (!spin_trylock_raw(l)) return 0;
    ld_acquired(d, l, rf);
    return 1;
}

void lockdep_spin_unlock(spinlock_t* l) {
    struct ld_cpu* d = ld_me();
    uint32_t n = d->depth < LD_STACK ? d->depth : LD_STACK;
    uint32_t i = n;
    while (i-- > 0) if (d->st[i].l == l) break;
    if (i < n) {
        if (!d->st[i].if_on && (ld_rflags() & RF_IF) && !d->in_report)
            ld_report2("interrupts enabled while holding a spinlock taken with them off",
                       l->name, 0, __builtin_return_address(0));
        for (uint32_t k = i; k + 1 < n; k++) d->st[k] = d->st[k + 1];
        d->depth--;
    } else if (!d->in_report) {
        ld_report2("release of a spinlock this CPU does not hold", l->name, 0,
                   __builtin_return_address(0));
    }
    l->owner_cpu = 0;
    spin_unlock_raw(l);
}

int lockdep_spin_held(const spinlock_t* l) {
    struct ld_cpu* d = ld_me();
    uint32_t n = d->depth < LD_STACK ? d->depth : LD_STACK;
    for (uint32_t i = 0; i < n; i++) if (d->st[i].l == l) return 1;
    return 0;
}

uint32_t lockdep_cpu_spin_depth(void) { return ld_me()->depth; }

/* ---- interrupt frames ----------------------------------------------------- */
void lockdep_irq_frame_push(void) {
    struct ld_cpu* d = ld_me();
    if (d->nframe < LD_FRAMES) d->fbase[d->nframe] = d->depth;
    d->nframe++;
}

void lockdep_irq_frame_pop(void) {
    struct ld_cpu* d = ld_me();
    if (!d->nframe) return;
    uint32_t base = d->nframe <= LD_FRAMES ? d->fbase[d->nframe - 1] : d->depth;
    if (d->depth > base && !d->in_report)
        ld_report2("IRQ handler returned holding a spinlock",
                   d->st[d->depth - 1 < LD_STACK ? d->depth - 1 : 0].l->name, 0, 0);
    d->nframe--;
}

void lockdep_cpu_reset(void) {
    struct ld_cpu* d = ld_me();
    if (d->depth && !d->in_report) {
        ld_report2("spinlock held across switch_to", d->st[0].l->name, 0,
                   __builtin_return_address(0));
        d->depth = 0;
    }
    d->nframe = 0;
}

/* ---- klocks --------------------------------------------------------------- */
void lockdep_klock_before(klock_t* l, klock_task_t* t) {
    struct ld_cpu* d = ld_me();
    if (d->in_report) return;
    void* caller = __builtin_return_address(0);
    if (l->level > LK_KLOCK_MAX) ld_report2("klock with a spinlock level", l->name, 0, caller);
    if (d->depth) {
        ld_report2("klock taken while holding a spinlock", l->name,
                   d->st[d->depth - 1 < LD_STACK ? d->depth - 1 : 0].l->name, caller);
        return;
    }
    uint32_t n = t->depth < KLOCK_STACK ? t->depth : KLOCK_STACK;
    for (uint32_t i = 0; i < n; i++) {
        klock_t* h = t->held[i];
        if (h && h->level >= l->level) {
            ld_report2("klock order inversion: taking", l->name, h->name, caller);
            return;
        }
    }
}

void lockdep_klock_wait(klock_t* l, uint64_t t0, int* reported) {
    static const uint32_t mask = 0x3FF;
    static uint32_t spins;          /* sampling only: races are harmless */
    if (*reported || ((++spins) & mask)) return;
    uint64_t hz = tsc_hz();
    if (!hz || tsc_read() - t0 <= hz * LD_STUCK_S) return;
    *reported = 1;
    uint64_t id;
    const char* k = klock_owner_desc(l, &id);
    ld_stuck("klock", l->name, k, id, tsc_read() - l->t_acq);
}

/* ---- user copies ---------------------------------------------------------- */
void lockdep_check_copy_user(void* caller) {
    struct ld_cpu* d = ld_me();
    if (d->in_report) return;
    if (d->depth)
        ld_report2("spinlock held across a user copy",
                   d->st[d->depth - 1 < LD_STACK ? d->depth - 1 : 0].l->name, 0, caller);
    uint64_t f = ld_rflags();
    __asm__ volatile ("cli" ::: "memory");
    cpu_t* c = this_cpu();
    klock_task_t* t = c->current ? &c->current->klk : &c->klk;
    const char* bad = 0;
    uint32_t n = t->depth < KLOCK_STACK ? t->depth : KLOCK_STACK;
    for (uint32_t i = 0; i < n; i++)
        if (t->held[i] && t->held[i]->level != LK_K0_BKL) bad = t->held[i]->name;
    __asm__ volatile ("push %0; popfq" :: "r"(f) : "memory", "cc");
    if (bad) ld_report2("klock (other than the BKL) held across a user copy", bad, 0, caller);
}

/* ---- boot selftest (negative controls) --------------------------------------
 * Proves the validator sees what it must: each case commits one violation on
 * private test locks with reports counted instead of fatal, and must be
 * caught exactly once. Runs once at boot (IDT up, before the APs and the
 * first task). */
static spinlock_t g_t_outer = SPINLOCK_INIT_CLASS(LK_S30_PROC, "selftest-S30");
static spinlock_t g_t_inner = SPINLOCK_INIT_CLASS(LK_S70_HEAP, "selftest-S70");
static klock_t    g_tk_hi   = KLOCK_INIT(LK_K30_FS, "selftest-K30");
static klock_t    g_tk_lo   = KLOCK_INIT(LK_K20_VFS_NS, "selftest-K20");

static int ld_st_case(const char* name, uint32_t before) {
    int ok = (g_ld_caught == before + 1);
    debugcon_writestring(ok ? "[LDTEST] PASS " : "[LDTEST] FAIL ");
    debugcon_writestring(name);
    debugcon_writestring("\n");
    return ok;
}

void lockdep_selftest(void) {
    int pass = 0, n = 0;
    uint32_t b;
    uint64_t f;
    g_ld_expect = 1;

    /* 0: a correct nesting is silent */
    b = g_ld_caught;
    f = spin_lock_irqsave(&g_t_outer); spin_lock(&g_t_inner);
    spin_unlock(&g_t_inner); spin_unlock_irqrestore(&g_t_outer, f);
    klock_lock(&g_tk_lo); klock_lock(&g_tk_hi); klock_unlock(&g_tk_hi); klock_unlock(&g_tk_lo);
    n++; if (g_ld_caught == b) { pass++; debugcon_writestring("[LDTEST] PASS ordered nesting is silent\n"); }
    else debugcon_writestring("[LDTEST] FAIL ordered nesting is silent\n");

    /* 1: spinlock order inversion */
    b = g_ld_caught;
    f = spin_lock_irqsave(&g_t_inner); spin_lock(&g_t_outer);
    spin_unlock(&g_t_outer); spin_unlock_irqrestore(&g_t_inner, f);
    n++; pass += ld_st_case("spinlock order inversion", b);

    /* 2: trylock of a spinlock this CPU holds */
    b = g_ld_caught;
    f = spin_lock_irqsave(&g_t_outer);
    if (spin_trylock(&g_t_outer)) spin_unlock(&g_t_outer);
    spin_unlock_irqrestore(&g_t_outer, f);
    n++; pass += ld_st_case("spinlock recursion (trylock)", b);

    /* 3: a klock under a spinlock */
    b = g_ld_caught;
    f = spin_lock_irqsave(&g_t_outer);
    klock_lock(&g_tk_lo); klock_unlock(&g_tk_lo);
    spin_unlock_irqrestore(&g_t_outer, f);
    n++; pass += ld_st_case("klock taken under a spinlock", b);

    /* 4: klock order inversion */
    b = g_ld_caught;
    klock_lock(&g_tk_hi); klock_lock(&g_tk_lo); klock_unlock(&g_tk_lo); klock_unlock(&g_tk_hi);
    n++; pass += ld_st_case("klock order inversion", b);

    /* 5: a spinlock still held on the way back to ring 3 */
    b = g_ld_caught;
    f = spin_lock_irqsave(&g_t_outer);
    klock_check_user_return("selftest");
    spin_unlock_irqrestore(&g_t_outer, f);
    n++; pass += ld_st_case("spinlock held on return to ring 3", b);

    /* 6: a non-BKL klock across a user copy */
    b = g_ld_caught;
    klock_lock(&g_tk_hi); lockdep_check_copy_user(0); klock_unlock(&g_tk_hi);
    n++; pass += ld_st_case("klock held across a user copy", b);

    /* 7: SPIN_ASSERT_HELD / KLOCK_ASSERT_HELD on a lock not held */
    b = g_ld_caught;
    SPIN_ASSERT_HELD(&g_t_outer);
    n++; pass += ld_st_case("SPIN_ASSERT_HELD", b);
    b = g_ld_caught;
    KLOCK_ASSERT_HELD(&g_tk_hi);
    n++; pass += ld_st_case("KLOCK_ASSERT_HELD", b);

    /* 8: interrupts re-enabled while holding an irqsave spinlock (runs after
     * the IDT is up, so a tick in the window is harmless) */
    b = g_ld_caught;
    f = spin_lock_irqsave(&g_t_outer);
    __asm__ volatile ("sti" ::: "memory");
    spin_unlock(&g_t_outer);
    __asm__ volatile ("push %0; popfq" :: "r"(f) : "memory", "cc");
    n++; pass += ld_st_case("interrupts enabled while holding a spinlock", b);

    g_ld_expect = 0;
    g_nreports = 0;
    debugcon_writestring(pass == n ? "[LDTEST] all " : "[LDTEST] FAILED ");
    debugcon_print_hex((uint64_t)pass);
    debugcon_writestring("/");
    debugcon_print_hex((uint64_t)n);
    debugcon_writestring(" negative controls\n");
}

#endif /* SECOS_LOCKDEP */
