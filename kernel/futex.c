/*
 * futex.c — [U9] Linux futex(2): WAIT, WAKE, REQUEUE, CMP_REQUEUE,
 * WAIT_BITSET, WAKE_BITSET, timeouts, private and shared keys.
 * SPDX-License-Identifier: MIT
 *
 * Waiters are process_t (threads) on a 256-bucket hash of singly linked
 * chains under one spinlock (LK_S28_FUTEX: nothing is taken inside it, it
 * never covers a user copy or a block). Keys:
 *   - (mm, uaddr) for every private mapping, with or without
 *     FUTEX_PRIVATE_FLAG (Linux keys a shared futex on private anonymous
 *     memory by mm + address too, so the two always match);
 *   - (physical address | 1, 0) for device memory (VMA_TYPE_PHYS), the only
 *     memory two address spaces can share today. Anonymous MAP_SHARED and
 *     writable shared file maps (U4a) must key on their frame/inode+offset.
 *
 * Wait = the M16 block + re-run model. The FIRST run queues the task, THEN
 * reads the user word (no lock held): a FUTEX_WAKE that comes after our read
 * finds us queued; one that came before changed the word first (every correct
 * futex user changes the word before waking), so we see the new value and
 * return -EAGAIN. A waker dequeues the waiter, sets futex_woken and makes it
 * READY under proc_lock (sched_wake_task_sync); a wake that races the block
 * itself bumps the wake sequence and the block becomes an immediate re-run.
 * The RE-RUN returns 0 if futex_woken, -ETIMEDOUT past the deadline, -EINTR
 * for a pending signal; otherwise (a spurious re-run) it re-reads the word:
 * changed -> 0 (a permitted spurious wakeup), same -> block again.
 * Waiters are woken in FIFO order per key, so a newly queued waiter that then
 * sees a changed word never takes the wake-up an older sleeper needed.
 */
#include "futex.h"
#include "process.h"
#include "sched.h"
#include "signal.h"
#include "spinlock.h"
#include "lock_order.h"
#include "user_copy.h"
#include "kclock.h"
#include "debugcon.h"
#include "vma.h"

extern uint64_t timer_get_ticks(void);

#define FX_EAGAIN    11
#define FX_EFAULT    14
#define FX_EINVAL    22
#define FX_ENOSYS    38
#define FX_EINTR     4
#define FX_ETIMEDOUT 110

#define FX_BUCKETS 256
static process_t* g_fx_head[FX_BUCKETS];
static process_t* g_fx_tail[FX_BUCKETS];
static spinlock_t g_fx_lock = SPINLOCK_INIT_CLASS(LK_S28_FUTEX, "futex");
static uint64_t g_fx_waits, g_fx_wakes, g_fx_timeouts;

static inline uint32_t fx_hash(uint64_t k0, uint64_t k1) {
    uint64_t h = k0 * 0x9E3779B97F4A7C15ULL ^ (k1 >> 2) * 0xC2B2AE3D27D4EB4FULL;
    return (uint32_t)(h >> 56) & (FX_BUCKETS - 1);
}

static void fx_enqueue_locked(process_t* p) {
    uint32_t b = fx_hash(p->futex_key0, p->futex_key1);
    p->futex_next = NULL;
    if (g_fx_tail[b]) g_fx_tail[b]->futex_next = p; else g_fx_head[b] = p;
    g_fx_tail[b] = p;
    p->futex_queued = 1;
}
static void fx_dequeue_locked(process_t* p) {
    if (!p->futex_queued) return;
    uint32_t b = fx_hash(p->futex_key0, p->futex_key1);
    process_t* prev = NULL;
    for (process_t* q = g_fx_head[b]; q; prev = q, q = q->futex_next) {
        if (q != p) continue;
        if (prev) prev->futex_next = q->futex_next; else g_fx_head[b] = q->futex_next;
        if (g_fx_tail[b] == q) g_fx_tail[b] = prev;
        break;
    }
    p->futex_next = NULL;
    p->futex_queued = 0;
}

// The key of the 32-bit word at uaddr in cur's address space. 0 / -errno.
static int fx_key(process_t* cur, uint64_t uaddr, int priv, uint64_t* k0, uint64_t* k1) {
    if (uaddr & 3) return -FX_EINVAL;
    if (!user_range_valid((const void*)uaddr, 4)) return -FX_EFAULT;
    *k0 = (uint64_t)(uintptr_t)cur->mm; *k1 = uaddr;
    if (priv) return 0;
    uint64_t fl = mm_lock(cur);
    const vma_t* v = vma_find(&cur->mm->vmas, uaddr);
    if (v && v->type == VMA_TYPE_PHYS) {           // device memory: shared by address
        *k0 = (v->phys_base + (uaddr - v->start)) | 1ULL;
        *k1 = 0;
    }
    mm_unlock(cur, fl);
    return 0;
}

// Wake: collect (with a reference) up to `n` waiters matching (k0,k1,bits),
// then make them runnable outside the futex lock.
#define FX_BATCH 32
static int fx_wake_key(uint64_t k0, uint64_t k1, uint32_t bits, int n) {
    int woken = 0;
    while (n > 0) {
        process_t* got[FX_BATCH]; int ng = 0;
        uint64_t fl = spin_lock_irqsave(&g_fx_lock);
        uint32_t b = fx_hash(k0, k1);
        process_t* q = g_fx_head[b];
        while (q && ng < FX_BATCH && n > 0) {
            process_t* nx = q->futex_next;
            if (q->futex_key0 == k0 && q->futex_key1 == k1 && (q->futex_bits & bits)) {
                fx_dequeue_locked(q);
                __atomic_store_n(&q->futex_woken, 1, __ATOMIC_SEQ_CST);
                got[ng++] = process_get(q);    // alive: a queued task dequeues itself before exiting
                n--;
            }
            q = nx;
        }
        int more = (q != NULL && n > 0 && ng == FX_BATCH);
        spin_unlock_irqrestore(&g_fx_lock, fl);
        for (int i = 0; i < ng; i++) {
            sched_wake_task_sync(got[i]);
            process_put(got[i]);
        }
        woken += ng;
        __atomic_add_fetch(&g_fx_wakes, (uint64_t)ng, __ATOMIC_RELAXED);
        if (!more) break;
    }
    return woken;
}

int futex_wake_addr(process_t* cur, uint64_t uaddr, int n) {
    uint64_t k0, k1;
    if (!cur || fx_key(cur, uaddr, 0, &k0, &k1) != 0) return 0;
    return fx_wake_key(k0, k1, FUTEX_BITSET_MATCH_ANY, n);
}

void futex_exit(process_t* p) {
    if (!p || (!p->futex_queued && !p->futex_active)) return;
    uint64_t fl = spin_lock_irqsave(&g_fx_lock);
    fx_dequeue_locked(p);
    spin_unlock_irqrestore(&g_fx_lock, fl);
    p->futex_active = 0; p->futex_woken = 0;
}

void futex_stats(uint64_t* waits, uint64_t* wakes, uint64_t* timeouts) {
    *waits = g_fx_waits; *wakes = g_fx_wakes; *timeouts = g_fx_timeouts;
}

struct k_ts { int64_t tv_sec; int64_t tv_nsec; };

// End the wait in progress: off the queue, state cleared. Returns 1 if a
// waker had already taken us (the wait succeeded).
static int fx_finish(process_t* cur) {
    uint64_t fl = spin_lock_irqsave(&g_fx_lock);
    fx_dequeue_locked(cur);
    int w = cur->futex_woken;
    spin_unlock_irqrestore(&g_fx_lock, fl);
    cur->futex_active = 0; cur->futex_woken = 0; cur->futex_deadline = 0;
    cur->sleep_until = 0;
    return w;
}

// Block until woken, the deadline, or a signal (NORETURN for a user task).
static void fx_block(process_t* cur, trapframe_t* tf) {
    cur->sleep_until = 0;
    if (cur->futex_deadline) {
        uint64_t now = kclock_mono_ns();
        uint64_t left = cur->futex_deadline > now ? cur->futex_deadline - now : 0;
        uint64_t ticks = (left + 999999ULL) / 1000000ULL;
        cur->sleep_until = timer_get_ticks() + (ticks ? ticks : 1);
    }
    tf->rip -= 2;                                  // re-run `int 0x80` on wake
    sched_block_current(tf);                       // NORETURN
    tf->rip += 2;                                  // (not a user task: no block)
}

// Interrupted by a signal? (a pending stop stops us right here with the call
// rewound: it re-runs after SIGCONT and resumes the wait.)
static int fx_sig(process_t* cur, trapframe_t* tf) {
    int ss = signal_take_stop(cur);
    if (ss) { cur->stop_sig = ss; tf->rip -= 2; sched_stop_current(tf); }
    return signal_pending(cur);
}

static long fx_wait(process_t* cur, trapframe_t* tf, uint64_t uaddr, uint32_t val,
                    uint64_t utime, int abs, int realtime, uint32_t bits, int priv) {
    if (!bits) return -FX_EINVAL;
    if (!cur->futex_active) {
        // ---- first run ----
        uint64_t deadline = 0;
        if (utime) {
            struct k_ts ts;
            if (copy_from_user(&ts, (const void*)utime, sizeof(ts)) != 0) return -FX_EFAULT;
            if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return -FX_EINVAL;
            uint64_t ns = (uint64_t)ts.tv_sec > (1ULL << 33) ? (1ULL << 62)
                        : (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
            uint64_t mono = kclock_mono_ns();
            if (!abs) deadline = mono + ns;
            else if (!realtime) deadline = ns;
            else {                                 // CLOCK_REALTIME: converted once, here
                uint64_t real = kclock_real_ns();
                deadline = ns > real ? mono + (ns - real) : mono;
            }
            if (deadline == 0) deadline = 1;
            if (deadline <= mono) return -FX_ETIMEDOUT;
        }
        uint64_t k0, k1;
        int r = fx_key(cur, uaddr, priv, &k0, &k1);
        if (r) return r;
        uint64_t fl = spin_lock_irqsave(&g_fx_lock);
        cur->futex_key0 = k0; cur->futex_key1 = k1; cur->futex_bits = bits;
        cur->futex_woken = 0;
        fx_enqueue_locked(cur);
        spin_unlock_irqrestore(&g_fx_lock, fl);
        cur->futex_active = 1;
        cur->futex_deadline = deadline;
        __atomic_add_fetch(&g_fx_waits, 1, __ATOMIC_RELAXED);
        uint32_t cv;
        if (copy_from_user(&cv, (const void*)uaddr, sizeof(cv)) != 0) {
            return fx_finish(cur) ? 0 : -FX_EFAULT;
        }
        if (cv != val) return fx_finish(cur) ? 0 : -FX_EAGAIN;
        if (fx_sig(cur, tf)) return fx_finish(cur) ? 0 : -FX_EINTR;
        fx_block(cur, tf);
        return 0;                                  // not reached for a user task
    }
    // ---- re-run ----
    if (__atomic_load_n(&cur->futex_woken, __ATOMIC_SEQ_CST)) { fx_finish(cur); return 0; }
    if (cur->futex_deadline && kclock_mono_ns() >= cur->futex_deadline) {
        if (fx_finish(cur)) return 0;
        __atomic_add_fetch(&g_fx_timeouts, 1, __ATOMIC_RELAXED);
        return -FX_ETIMEDOUT;
    }
    if (fx_sig(cur, tf)) return fx_finish(cur) ? 0 : -FX_EINTR;
    uint32_t cv;
    if (copy_from_user(&cv, (const void*)uaddr, sizeof(cv)) != 0 || cv != val) {
        fx_finish(cur);
        return 0;                                  // spurious wakeup (permitted)
    }
    if (!cur->futex_queued) { fx_finish(cur); return 0; }   // dequeued by a requeue race
    fx_block(cur, tf);
    return 0;
}

static long fx_requeue(process_t* cur, uint64_t uaddr, int priv, int nwake, int nreq,
                       uint64_t uaddr2, int cmp, uint32_t val3) {
    if (nwake < 0 || nreq < 0) return -FX_EINVAL;
    uint64_t a0, a1, b0, b1;
    int r = fx_key(cur, uaddr, priv, &a0, &a1);
    if (r) return r;
    r = fx_key(cur, uaddr2, priv, &b0, &b1);
    if (r) return r;
    if (cmp) {
        // Linux compares under the hash lock; here the word is read just
        // before (a user copy is never made under a spinlock). A waker that
        // changes it in between is indistinguishable from one just after.
        uint32_t cv;
        if (copy_from_user(&cv, (const void*)uaddr, sizeof(cv)) != 0) return -FX_EFAULT;
        if (cv != val3) return -FX_EAGAIN;
    }
    int woken = fx_wake_key(a0, a1, FUTEX_BITSET_MATCH_ANY, nwake);
    int moved = 0;
    if (nreq > 0 && !(a0 == b0 && a1 == b1)) {
        uint64_t fl = spin_lock_irqsave(&g_fx_lock);
        uint32_t bk = fx_hash(a0, a1);
        process_t* q = g_fx_head[bk];
        while (q && moved < nreq) {
            process_t* nx = q->futex_next;
            if (q->futex_key0 == a0 && q->futex_key1 == a1) {
                fx_dequeue_locked(q);
                q->futex_key0 = b0; q->futex_key1 = b1;
                fx_enqueue_locked(q);
                moved++;
            }
            q = nx;
        }
        spin_unlock_irqrestore(&g_fx_lock, fl);
    }
    return cmp ? woken + moved : woken;
}

long futex_syscall(trapframe_t* tf, uint64_t uaddr, uint64_t op, uint64_t val,
                   uint64_t utime, uint64_t uaddr2, uint64_t val3) {
    process_t* cur = sched_get_current();
    if (!cur || !cur->mm) return -FX_ENOSYS;
    int priv = (op & FUTEX_PRIVATE_FLAG) != 0;
    int rt = (op & FUTEX_CLOCK_REALTIME) != 0;
    int cmd = (int)(op & ~(uint64_t)(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME));
    if (rt && cmd != FUTEX_WAIT_BITSET && cmd != FUTEX_WAIT) return -FX_ENOSYS;
    switch (cmd) {
    case FUTEX_WAIT:
        // (Linux ignores FUTEX_CLOCK_REALTIME for a relative FUTEX_WAIT.)
        return fx_wait(cur, tf, uaddr, (uint32_t)val, utime, 0, 0, FUTEX_BITSET_MATCH_ANY, priv);
    case FUTEX_WAIT_BITSET:
        return fx_wait(cur, tf, uaddr, (uint32_t)val, utime, 1, rt, (uint32_t)val3, priv);
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET: {
        uint32_t bits = cmd == FUTEX_WAKE ? FUTEX_BITSET_MATCH_ANY : (uint32_t)val3;
        if (!bits) return -FX_EINVAL;
        uint64_t k0, k1;
        int r = fx_key(cur, uaddr, priv, &k0, &k1);
        if (r) return r;
        int n = (int)(uint32_t)val; if (n < 0) n = 0x7fffffff;
        return fx_wake_key(k0, k1, bits, n);
    }
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE: {
        int nw = (int)(uint32_t)val, nr = (int)(uint32_t)utime;   // val2 is in the timeout slot
        if (nw < 0) nw = 0x7fffffff;
        if (nr < 0) nr = 0x7fffffff;
        return fx_requeue(cur, uaddr, priv, nw, nr, uaddr2, cmd == FUTEX_CMP_REQUEUE, (uint32_t)val3);
    }
    default:
        return -FX_ENOSYS;                         // PI, WAKE_OP, WAIT_REQUEUE_PI: not supported
    }
}
