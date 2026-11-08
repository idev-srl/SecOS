/*
 * lockdep.h — run-time lock validator (M50), built with -DSECOS_LOCKDEP.
 * See docs/BKL_REMOVAL_PLAN.md §3.3 and docs/devlog/M50-lock-core.md.
 *
 * With SECOS_LOCKDEP every spinlock_t carries a class {level, name}
 * (SPINLOCK_INIT_CLASS) and spin_lock/unlock go through the hooks below; each
 * CPU keeps a stack of the spinlocks it holds and each task a stack of its
 * klocks (klock_task_t). Checked:
 *   - order (lock_order.h levels strictly increasing) and recursion, for
 *     spinlocks (within one interrupt frame) and klocks;
 *   - a spinlock taken both with IF on and from IRQ context (self-deadlock);
 *   - interrupts re-enabled while holding a spinlock taken with them off;
 *   - a klock taken under a spinlock or in IRQ context;
 *   - anything held at syscall exit / return to ring 3 / switch_to /
 *     cpu_wait_irq, and klocks (but the BKL) across copy_to/from_user;
 *   - SPIN_ASSERT_HELD / KLOCK_ASSERT_HELD in internal helpers;
 *   - a wait longer than 2 s: "[LOCKDEP] stuck on <name> owner=... mine: ...".
 * A violation prints "[LOCKDEP] ..." with this CPU's held locks and panics
 * (boot word `lockdep=warn`: report each distinct violation once and go on).
 * kfree poisons freed memory with 0x6B (mm/heap.c) and /proc/lockstat lists
 * per-lock acquire / contended / max-wait counts.
 *
 * Without SECOS_LOCKDEP nothing here costs anything: the hooks are not called,
 * spinlock_t stays one word, the assertions compile away. -DSECOS_LOCKDEP_LITE
 * (the M47 net-only checker) now means SECOS_LOCKDEP.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LOCKDEP_H
#define LOCKDEP_H
#include <stdint.h>

#if defined(SECOS_LOCKDEP_LITE) && !defined(SECOS_LOCKDEP)
#define SECOS_LOCKDEP 1
#endif

struct spinlock;
struct klock;
struct klock_task;

#ifdef SECOS_LOCKDEP
#include "lockdep_wait.h"

/* spinlock.h hooks (the whole acquisition / release, with checks + stats). */
void lockdep_spin_lock(struct spinlock* l);
int  lockdep_spin_trylock(struct spinlock* l);
void lockdep_spin_unlock(struct spinlock* l);
int  lockdep_spin_held(const struct spinlock* l);       /* by this CPU */

/* The stuck-lock watchdog state of one wait (ld_wait_t, lockdep_wait.h). */
void lockdep_wait_init(ld_wait_t* w, uint64_t progress);
void lockdep_stuck_expect(int on);       /* forced test: count reports, print [LDTEST] */
uint32_t lockdep_stuck_caught(void);

/* klock.c hooks. */
void lockdep_klock_before(struct klock* l, struct klock_task* t);
void lockdep_klock_wait(struct klock* l, ld_wait_t* w);

/* Called by lockdep's checkers, panic and the IRQ stubs. */
uint32_t lockdep_cpu_spin_depth(void);   /* spinlocks held by this CPU */
void lockdep_irq_frame_push(void);
void lockdep_irq_frame_pop(void);
void lockdep_cpu_reset(void);            /* switch_to: a fresh frame for the next task */
void lockdep_check_copy_user(void* caller);  /* copy_to/from_user, strncpy_from_user */
void lockdep_dump_cpu(void);             /* this CPU's spinlocks (panic path) */
void lockdep_report(const char* what, const char* name);   /* panics unless lockdep=warn */
void lockdep_selftest(void);             /* boot: negative controls, "[LDTEST]" (no "LOCKDEP": gates grep for it) */

#define SPIN_ASSERT_HELD(l) \
    do { if (!lockdep_spin_held(l)) lockdep_report("spinlock not held in " __FILE__, (l)->name); } while (0)
#else
#define SPIN_ASSERT_HELD(l) do { (void)(l); } while (0)
static inline void lockdep_check_copy_user(void* caller) { (void)caller; }
#endif

/* /proc/lockstat (fs/procfs.c): klocks always, spinlocks with SECOS_LOCKDEP.
 * Calls emit(buf) for each line. */
void lockstat_generate(void (*emit)(void* ctx, const char* s), void* ctx);

#endif /* LOCKDEP_H */
