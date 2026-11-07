/* Host stub of kernel/klock.h for the fuzz harness: fs/block.h embeds a
 * klock_t in block_dev_t (M50 B3); the harness is single-threaded and its
 * block_read/block_write stubs never take it. */
#ifndef KLOCK_H
#define KLOCK_H
#include <stdint.h>
typedef struct klock {
    void* volatile owner;
    uint32_t depth;
    uint16_t level;
    uint8_t  recursive, reg;
    const char* name;
    uint64_t t_acq, n_acq, n_contended, max_wait_cyc;
} klock_t;
#define KLOCK_ASSERT_HELD(l) do { (void)(l); } while (0)
#endif
