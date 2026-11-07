/* Host stub of kernel/klock.h for the fuzz harnesses: fs/block.h embeds a
 * klock_t in block_dev_t (M50 B3), and since M51 C4 the exFAT/NTFS drivers
 * take their own K30 klock around every operation. The harness is
 * single-threaded, so the stub only checks the discipline: taking a lock the
 * caller already holds (a driver op re-entering itself instead of calling the
 * _locked variant), releasing one it does not hold, or a KLOCK_ASSERT_HELD
 * helper reached without the lock aborts the run. */
#ifndef KLOCK_H
#define KLOCK_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define LK_K30_FS 30
typedef struct klock {
    void* volatile owner;
    uint32_t depth;
    uint16_t level;
    uint8_t  recursive, reg;
    const char* name;
    uint64_t t_acq, n_acq, n_contended, max_wait_cyc;
} klock_t;
#define KLOCK_INIT(lvl, nm) { .level = (lvl), .name = (nm) }
static inline void klock_init(klock_t* l, uint16_t level, const char* name, int recursive) {
    l->owner = 0; l->depth = 0; l->level = level; l->name = name; l->recursive = (uint8_t)recursive;
}
static inline void klock_lock(klock_t* l) {
    if (l->owner) { fprintf(stderr, "klock stub: recursive lock of %s\n", l->name); abort(); }
    l->owner = (void*)l; l->depth = 1; l->n_acq++;
}
static inline void klock_unlock(klock_t* l) {
    if (!l->owner) { fprintf(stderr, "klock stub: unlock of free %s\n", l->name); abort(); }
    l->owner = 0; l->depth = 0;
}
static inline int klock_held(const klock_t* l) { return l->owner != 0; }
#define KLOCK_ASSERT_HELD(l) \
    do { if (!klock_held(l)) { fprintf(stderr, "klock stub: %s not held in %s\n", (l)->name, __func__); abort(); } } while (0)
#endif
