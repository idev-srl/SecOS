/*
 * klock.c — task-owned kernel locks (M50). See klock.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "klock.h"
#include "lockdep.h"
#include "percpu.h"
#include "process.h"
#include "spinlock.h"   /* cpu_relax_poll */
#include "debugcon.h"
#include "bkl.h"     /* bkl_wait_poll */

extern uint64_t tsc_read(void);
extern uint64_t tsc_hz(void);

#define RF_IF (1ULL << 9)

static inline uint64_t kl_save_cli(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void kl_restore(uint64_t f) {
    __asm__ volatile ("push %0; popfq" :: "r"(f) : "memory", "cc");
}

/* The calling task (or this CPU before its scheduler runs) and its klock
 * stack. Valid across an IF-on wait: a task in ring 0 is never preempted, and
 * the only preemptible ring-0 task (the idle task) never leaves its CPU. */
static inline klock_task_t* kl_self(cpu_t* c, void** owner) {
    process_t* p = c->current;
    if (p) { *owner = p; return &p->klk; }
    *owner = c;
    return &c->klk;
}

/* ---- reporting ------------------------------------------------------------ */
void klock_bug(const char* what, const char* name) {
#ifdef SECOS_LOCKDEP
    lockdep_report(what, name);
#else
    /* One line per distinct problem: a release build must keep running. */
    static const char* volatile seen[16];
    static volatile uint32_t nseen;
    uint32_t n = nseen;
    for (uint32_t i = 0; i < n && i < 16; i++) if (seen[i] == what) return;
    if (n < 16) { seen[n] = what; nseen = n + 1; }
    debugcon_writestring("[LOCK] BUG: ");
    debugcon_writestring(what);
    debugcon_writestring(" (");
    debugcon_writestring(name ? name : "?");
    debugcon_writestring(")\n");
#endif
}

/* ---- /proc/lockstat registry (klocks are few and static) ------------------ */
#define KL_REG_MAX 64
static klock_t* g_kl_reg[KL_REG_MAX];
static volatile uint32_t g_kl_nreg;

static void kl_register(klock_t* l) {          /* called by the owner */
    uint32_t i = __atomic_fetch_add(&g_kl_nreg, 1, __ATOMIC_SEQ_CST);
    if (i >= KL_REG_MAX) { l->reg = 0xFF; return; }   /* unlisted */
    g_kl_reg[i] = l;
    l->reg = (uint8_t)(i + 1);
}

/* ---- the task's held stack ------------------------------------------------ */
static void kl_push(klock_task_t* t, klock_t* l) {
    if (t->depth < KLOCK_STACK) t->held[t->depth] = l;
    t->depth++;
}
static void kl_forget(klock_task_t* t, klock_t* l) {
    uint32_t n = t->depth < KLOCK_STACK ? t->depth : KLOCK_STACK;
    for (uint32_t i = n; i-- > 0;) {
        if (t->held[i] != l) continue;
        for (uint32_t k = i; k + 1 < n; k++) t->held[k] = t->held[k + 1];
        t->held[n - 1] = 0;
        break;
    }
    if (t->depth) t->depth--;
}

void klock_init(klock_t* l, uint16_t level, const char* name, int recursive) {
    l->owner = 0; l->depth = 0; l->level = level; l->recursive = recursive ? 1 : 0;
    l->reg = 0; l->name = name; l->t_acq = 0;
    l->n_acq = l->n_contended = l->max_wait_cyc = 0;
}

/* ---- acquire / release ---------------------------------------------------- */
/* Every ownership change happens with interrupts off on this CPU, so the tick
 * (which reads klock_depth to decide whether it may preempt the idle task)
 * never sees a lock owned but not yet counted. */
static void kl_acquire(klock_t* l, int irq_on) {
    uint64_t rf = kl_save_cli();
    cpu_t* c = this_cpu();
    void* me;
    klock_task_t* t = kl_self(c, &me);
    if (c->irq_depth) klock_bug("klock taken in IRQ context", l->name);
    if (l->owner == me) {
        if (!l->recursive) klock_bug("recursive acquisition of a non-recursive klock", l->name);
        l->depth++;                       /* pairs with the caller's unlock */
        kl_restore(rf);
        return;
    }
#ifdef SECOS_LOCKDEP
    lockdep_klock_before(l, t);
#endif
    if (!__sync_bool_compare_and_swap(&l->owner, (void*)0, me)) {
        /* Contended: wait with interrupts on when the caller had them on (or
         * asked for it), so this CPU keeps taking ticks and IPIs; claim with
         * them off; return with the caller's IF exactly as it was. */
        uint64_t t0 = tsc_read();
        int spin_on = irq_on || (rf & RF_IF);
#ifdef SECOS_LOCKDEP
        int reported = 0;
#endif
        for (;;) {
            if (spin_on) __asm__ volatile ("sti" ::: "memory");
            while (l->owner) {
                cpu_relax_poll();
                if (spin_on && l->recursive) bkl_wait_poll();   /* only the BKL */
#ifdef SECOS_LOCKDEP
                lockdep_klock_wait(l, t0, &reported);
#endif
            }
            __asm__ volatile ("cli" ::: "memory");
            if (__sync_bool_compare_and_swap(&l->owner, (void*)0, me)) break;
        }
        uint64_t w = tsc_read() - t0;
        l->n_contended++;
        if (w > l->max_wait_cyc) l->max_wait_cyc = w;
    }
    l->depth = 1;
    l->t_acq = tsc_read();
    l->n_acq++;
    if (!l->reg) kl_register(l);
    kl_push(t, l);
    kl_restore(rf);
}

void klock_lock(klock_t* l)        { kl_acquire(l, 0); }
void klock_lock_irq_on(klock_t* l) { kl_acquire(l, 1); }

int klock_trylock(klock_t* l) {
    uint64_t rf = kl_save_cli();
    cpu_t* c = this_cpu();
    void* me;
    klock_task_t* t = kl_self(c, &me);
    if (c->irq_depth) klock_bug("klock taken in IRQ context", l->name);
    if (l->owner == me) {
        if (!l->recursive) { kl_restore(rf); return 0; }
        l->depth++;
        kl_restore(rf);
        return 1;
    }
    if (!__sync_bool_compare_and_swap(&l->owner, (void*)0, me)) { kl_restore(rf); return 0; }
    l->depth = 1;
    l->t_acq = tsc_read();
    l->n_acq++;
    if (!l->reg) kl_register(l);
    kl_push(t, l);
    kl_restore(rf);
    return 1;
}

static void kl_release_locked(klock_t* l, klock_task_t* t) {
    kl_forget(t, l);
    l->depth = 0;
    __atomic_store_n(&l->owner, (void*)0, __ATOMIC_RELEASE);
}

void klock_unlock(klock_t* l) {
    uint64_t rf = kl_save_cli();
    cpu_t* c = this_cpu();
    void* me;
    klock_task_t* t = kl_self(c, &me);
    if (l->owner != me) {
        klock_bug("unlock of a klock by a non-owner", l->name);
        kl_restore(rf);
        return;
    }
    if (l->depth > 1) { l->depth--; kl_restore(rf); return; }
    kl_release_locked(l, t);
    kl_restore(rf);
}

void klock_unlock_to_user(klock_t* l) {
    uint64_t rf = kl_save_cli();
    cpu_t* c = this_cpu();
    void* me;
    klock_task_t* t = kl_self(c, &me);
    if (l->owner != me) {
        klock_bug("unlock of a klock by a non-owner", l->name);
    } else if (l->depth > 1) {
        l->depth--;
    } else {
        kl_release_locked(l, t);
    }
    uint32_t left = t->depth;
    const char* nm = (left && t->held[0]) ? t->held[0]->name : "?";
    kl_restore(rf);
    if (left) klock_bug("klock held on return to ring 3", nm);
#ifdef SECOS_LOCKDEP
    if (lockdep_cpu_spin_depth()) lockdep_report("spinlock held on return to ring 3", "syscall exit");
#endif
}

int klock_held(const klock_t* l) {
    uint64_t rf = kl_save_cli();
    void* me;
    kl_self(this_cpu(), &me);
    int r = (l->owner == me);
    kl_restore(rf);
    return r;
}

uint32_t klock_depth(const struct process* p) {
    if (p) return p->klk.depth;
    uint64_t rf = kl_save_cli();
    void* me;
    uint32_t d = kl_self(this_cpu(), &me)->depth;
    kl_restore(rf);
    return d;
}

int klock_owned_by(const klock_t* l, const struct process* p) {
    if (p) return l->owner == (const void*)p;
    return klock_held(l);
}

uint32_t klock_suspend(klock_t* l) {
    uint64_t rf = kl_save_cli();
    void* me;
    klock_task_t* t = kl_self(this_cpu(), &me);
    if (l->owner != me) { kl_restore(rf); return 0; }
    uint32_t d = l->depth;
    kl_release_locked(l, t);
    kl_restore(rf);
    return d;
}

void klock_resume(klock_t* l, uint32_t depth) {
    if (!depth) return;
    kl_acquire(l, 0);
    l->depth = depth;
}

void klock_drop_owner(klock_t* l, struct process* p) {
    if (!p || l->owner != (void*)p) return;
    uint64_t rf = kl_save_cli();
    if (l->owner == (void*)p) kl_release_locked(l, &p->klk);
    kl_restore(rf);
}

const char* klock_owner_desc(const klock_t* l, uint64_t* id) {
    void* o = l->owner;
    *id = 0;
    if (!o) return "none";
    uint32_t n = smp_cpu_count();
    if (!n) n = 1;
    for (uint32_t i = 0; i < n; i++)
        if ((void*)cpu_by_index(i) == o) { *id = i; return "cpu"; }
    *id = ((process_t*)o)->pid;
    return "pid";
}

#ifdef SECOS_LOCKDEP
/* Only the BKL (the one level-K0 klock) may stay held across a wait. */
static int kl_only_bkl(const klock_task_t* t) {
    uint32_t n = t->depth < KLOCK_STACK ? t->depth : KLOCK_STACK;
    if (t->depth > KLOCK_STACK) return 0;
    for (uint32_t i = 0; i < n; i++)
        if (!t->held[i] || t->held[i]->level != LK_K0_BKL) return 0;
    return 1;
}

/* ---- waits, IRQ nesting, switch, return to user --------------------------- */
static void kl_check_wait(const char* where) {
    if (lockdep_cpu_spin_depth()) lockdep_report("spinlock held across", where);
    uint64_t rf = kl_save_cli();
    void* me;
    klock_task_t* t = kl_self(this_cpu(), &me);
    int bad = !kl_only_bkl(t);
    const char* nm = (t->depth && t->held[0]) ? t->held[t->depth > KLOCK_STACK ? 0 : t->depth - 1]->name : "?";
    kl_restore(rf);
    if (bad) lockdep_report("klock (other than the BKL) held across cpu_wait_irq", nm);
}
#endif

void cpu_wait_irq(void) {
#ifdef SECOS_LOCKDEP
    kl_check_wait("cpu_wait_irq");
#endif
    __asm__ volatile ("sti; hlt" ::: "memory");
}

void irq_enter(void) {
    this_cpu()->irq_depth++;
#ifdef SECOS_LOCKDEP
    lockdep_irq_frame_push();
#endif
}

void irq_exit(void) {
#ifdef SECOS_LOCKDEP
    lockdep_irq_frame_pop();
#endif
    cpu_t* c = this_cpu();
    if (c->irq_depth) c->irq_depth--;
}

int in_irq(void) { return this_cpu()->irq_depth != 0; }

void klock_on_switch(struct process* prev, struct process* idle) {
    cpu_t* c = this_cpu();
    /* switch_to dropped the BKL for a task; the idle task is never switched
     * away while it holds a klock (the tick checks klock_depth), so anything
     * still held here would deadlock the next task that needs it. */
    (void)idle;
    if (prev && prev->klk.depth)
        klock_bug("klock held across switch_to",
                  prev->klk.held[0] ? prev->klk.held[0]->name : "?");
#ifdef SECOS_LOCKDEP
    lockdep_cpu_reset();
#endif
    c->irq_depth = 0;
}

void klock_check_user_return(const char* where) {
    uint64_t rf = kl_save_cli();
    cpu_t* c = this_cpu();
    process_t* p = c->current;
    uint32_t d = p ? p->klk.depth : 0;
    const char* nm = (d && p->klk.held[0]) ? p->klk.held[0]->name : where;
    kl_restore(rf);
    if (d) klock_bug("klock held on return to ring 3", nm);
#ifdef SECOS_LOCKDEP
    if (lockdep_cpu_spin_depth()) lockdep_report("spinlock held on return to ring 3", where);
#endif
}

void klock_dump_held(void) {
    cpu_t* c = this_cpu();
    void* me;
    klock_task_t* t = kl_self(c, &me);
    debugcon_writestring("[LOCK] held by ");
    if (c->current) { debugcon_writestring("pid "); debugcon_print_hex(c->current->pid); }
    else { debugcon_writestring("cpu "); debugcon_print_hex(c->index); }
    debugcon_writestring(":");
    uint32_t n = t->depth < KLOCK_STACK ? t->depth : KLOCK_STACK;
    if (!t->depth) debugcon_writestring(" no klock");
    for (uint32_t i = 0; i < n; i++) {
        debugcon_writestring(" ");
        debugcon_writestring(t->held[i] && t->held[i]->name ? t->held[i]->name : "?");
    }
    if (c->irq_depth) debugcon_writestring(" [in IRQ]");
    debugcon_writestring("\n");
#ifdef SECOS_LOCKDEP
    lockdep_dump_cpu();
#endif
}

/* ---- /proc/lockstat ------------------------------------------------------- */
static void ls_u64(char* b, int* n, uint64_t v, int width) {
    char t[24]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = k; i < width; i++) b[(*n)++] = ' ';
    while (k) b[(*n)++] = t[--k];
}
static void ls_str(char* b, int* n, const char* s, int width) {
    int k = 0;
    while (s && *s && k < 31) { b[(*n)++] = *s++; k++; }
    for (; k < width; k++) b[(*n)++] = ' ';
}
static uint64_t ls_us(uint64_t cyc) {
    uint64_t hz = tsc_hz();
    if (!hz) return 0;
    return cyc / (hz / 1000000 ? hz / 1000000 : 1);
}

/* One formatted line for a lock (shared with lockdep.c via lockstat_line). */
void lockstat_line(void (*emit)(void*, const char*), void* ctx, const char* kind,
                   const char* name, uint16_t level, uint64_t acq, uint64_t cont,
                   uint64_t max_cyc) {
    char b[128]; int n = 0;
    ls_str(b, &n, kind, 6);
    ls_str(b, &n, name, 18);
    if (level == LK_NONE) ls_str(b, &n, "-", 6); else ls_u64(b, &n, level, 5), b[n++] = ' ';
    ls_u64(b, &n, acq, 12);
    ls_u64(b, &n, cont, 11);
    ls_u64(b, &n, ls_us(max_cyc), 12);
    b[n++] = '\n'; b[n] = 0;
    emit(ctx, b);
}

#ifdef SECOS_LOCKDEP
void lockdep_stat_spin(void (*emit)(void*, const char*), void* ctx);
#endif

void lockstat_generate(void (*emit)(void* ctx, const char* s), void* ctx) {
    emit(ctx, "kind  name              level    acquired  contended max_wait_us\n");
    uint32_t n = g_kl_nreg < KL_REG_MAX ? g_kl_nreg : KL_REG_MAX;
    for (uint32_t i = 0; i < n; i++) {
        klock_t* l = g_kl_reg[i];
        if (!l) continue;
        lockstat_line(emit, ctx, "klock", l->name, l->level, l->n_acq, l->n_contended,
                      l->max_wait_cyc);
    }
#ifdef SECOS_LOCKDEP
    lockdep_stat_spin(emit, ctx);
#else
    emit(ctx, "(spinlock statistics need a -DSECOS_LOCKDEP build)\n");
#endif
}
