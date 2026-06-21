/*
 * user_copy.c — Safe kernel/user memory transfer primitives.
 * See user_copy.h for the contract.
 */
#include "user_copy.h"
#include <stdint.h>
#include <stddef.h>

/* [M36] SMAP: when enabled, the kernel may not touch user pages unless RFLAGS.AC
 * is set. These primitives are the sanctioned bridge, so they raise AC (stac)
 * around the access and clear it (clac) after. Dormant until SMAP is enabled
 * (g_smap_enabled, set by cpu_enable_mitigations under -DSECOS_SMAP). */
extern int g_smap_enabled;
static inline void ua_begin(void){ if (g_smap_enabled) __asm__ volatile("stac" ::: "cc"); }
static inline void ua_end(void){ if (g_smap_enabled) __asm__ volatile("clac" ::: "cc"); }

int user_range_valid(const void* ptr, size_t len) {
    uint64_t start = (uint64_t)ptr;

    if (start == 0)              return 0;  /* NULL pointer */
    if (len == 0)                return 0;  /* zero-length transfer */
    if (start >= USER_ADDR_MAX)  return 0;  /* starts in kernel range */
    if (len  >  USER_ADDR_MAX)   return 0;  /* length alone exceeds limit */
    /* Overflow check: start + len must not wrap or cross USER_ADDR_MAX. */
    if (start + len < start)     return 0;  /* arithmetic overflow */
    if (start + len > USER_ADDR_MAX) return 0;

    return 1;
}

int copy_from_user(void* dst, const void* user_src, size_t len) {
    if (!user_range_valid(user_src, len)) return -EFAULT;

    const uint8_t* s = (const uint8_t*)user_src;
    uint8_t*       d = (uint8_t*)dst;
    ua_begin();
    for (size_t i = 0; i < len; i++) d[i] = s[i];
    ua_end();
    return 0;
}

int copy_to_user(void* user_dst, const void* src, size_t len) {
    if (!user_range_valid(user_dst, len)) return -EFAULT;

    const uint8_t* s = (const uint8_t*)src;
    uint8_t*       d = (uint8_t*)user_dst;
    ua_begin();
    for (size_t i = 0; i < len; i++) d[i] = s[i];
    ua_end();
    return 0;
}
