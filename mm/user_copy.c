/*
 * user_copy.c — Safe kernel/user memory transfer primitives.
 * See user_copy.h for the contract.
 */
#include "user_copy.h"
#include <stdint.h>
#include <stddef.h>

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
    for (size_t i = 0; i < len; i++) d[i] = s[i];
    return 0;
}

int copy_to_user(void* user_dst, const void* src, size_t len) {
    if (!user_range_valid(user_dst, len)) return -EFAULT;

    const uint8_t* s = (const uint8_t*)src;
    uint8_t*       d = (uint8_t*)user_dst;
    for (size_t i = 0; i < len; i++) d[i] = s[i];
    return 0;
}
