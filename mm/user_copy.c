/*
 * user_copy.c — Safe kernel/user memory transfer primitives.
 * See user_copy.h for the contract.
 */
#include "user_copy.h"
#include <stdint.h>
#include <stddef.h>

/* [M36] SMAP: when enabled, the kernel may not touch user pages unless RFLAGS.AC
 * is set. These primitives are the sanctioned bridge, so they raise AC (stac)
 * around the access and clear it (clac) after. Dormant on CPUs without SMAP
 * (g_smap_enabled, set by cpu_enable_mitigations unless -DSECOS_NO_SMAP). */
extern int g_smap_enabled;
static inline void ua_begin(void){ if (g_smap_enabled) __asm__ volatile("stac" ::: "cc"); }
static inline void ua_end(void){ if (g_smap_enabled) __asm__ volatile("clac" ::: "cc"); }

int user_range_valid(const void* ptr, size_t len) {
    uint64_t start = (uint64_t)ptr;

    if (start == 0)              return 0;  /* NULL pointer */
    if (len == 0)                return 0;  /* zero-length transfer */
    if (start <  USER_ADDR_MIN)  return 0;  /* below the user window (aliases the
                                               kernel-only low identity map) */
    if (start >= USER_ADDR_MAX)  return 0;  /* starts in kernel range */
    if (len  >  USER_ADDR_MAX)   return 0;  /* length alone exceeds limit */
    /* Overflow check: start + len must not wrap or cross USER_ADDR_MAX.
     * (start >= USER_ADDR_MIN is already established, so the whole range is in
     * the user window once the upper bound holds.) */
    if (start + len < start)     return 0;  /* arithmetic overflow */
    if (start + len > USER_ADDR_MAX) return 0;

    return 1;
}

/* Fault-tolerant primitives (arch/x86/uaccess.asm), covered by .ex_table. */
extern uint64_t __uaccess_copy(void* dst, const void* src, uint64_t n);
extern long     __uaccess_strncpy(char* dst, const char* usrc, uint64_t max);

int copy_from_user(void* dst, const void* user_src, size_t len) {
    if (!user_range_valid(user_src, len)) return -EFAULT;
    ua_begin();
    uint64_t left = __uaccess_copy(dst, user_src, len);
    ua_end();
    return left ? -EFAULT : 0;
}

int copy_to_user(void* user_dst, const void* src, size_t len) {
    if (!user_range_valid(user_dst, len)) return -EFAULT;
    ua_begin();
    uint64_t left = __uaccess_copy(user_dst, src, len);
    ua_end();
    return left ? -EFAULT : 0;
}

long strncpy_from_user(char* dst, const char* user_src, size_t max) {
    uint64_t s = (uint64_t)user_src;
    if (max == 0) return -ENAMETOOLONG;
    if (s < USER_ADDR_MIN || s >= USER_ADDR_MAX) return -EFAULT;
    /* Never read past the top of the user range. */
    uint64_t lim = max;
    if (lim > USER_ADDR_MAX - s) lim = USER_ADDR_MAX - s;
    ua_begin();
    long n = __uaccess_strncpy(dst, user_src, lim);
    ua_end();
    if (n < 0) return -EFAULT;                       /* faulted mid-string */
    if ((uint64_t)n == lim) {                        /* no NUL within lim */
        dst[max - 1] = 0;
        return (lim < max) ? -EFAULT : -ENAMETOOLONG;
    }
    return n;
}

/* Exception-table search. The table is tiny (one entry per user-access insn),
 * so a linear scan is fine. Returns the fixup rip, or 0 if rip is not a
 * registered user-access instruction. */
struct ex_entry { uint64_t insn, fixup; };
extern const struct ex_entry __ex_table_start[], __ex_table_end[];

uint64_t uaccess_fixup_lookup(uint64_t rip) {
    for (const struct ex_entry* e = __ex_table_start; e < __ex_table_end; e++)
        if (e->insn == rip) return e->fixup;
    return 0;
}
