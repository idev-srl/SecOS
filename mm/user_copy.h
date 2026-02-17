/*
 * user_copy.h — Safe kernel/user memory transfer primitives.
 *
 * Direct dereference of user pointers without range validation is forbidden.
 * All data crossing the user/kernel boundary must pass through these helpers.
 *
 * Range policy:
 *   Valid user address: [1, USER_ADDR_MAX)
 *   USER_ADDR_MAX = 0x0000800000000000 — top of x86-64 user canonical range.
 *   NULL (0) is always rejected.
 *
 * Without SMAP, enforcement is range-check only.  Accesses to valid but
 * unmapped user pages will fault in kernel context and be caught by the
 * IST2 page-fault handler.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* Top of the x86-64 user canonical address space (exclusive). */
#define USER_ADDR_MAX  0x0000800000000000ULL

/* POSIX EFAULT (14) returned on invalid user address. */
#define EFAULT 14

/*
 * user_range_valid — check that [ptr, ptr+len) is entirely within the user
 * canonical address range and that arithmetic does not overflow.
 * Returns 1 if valid, 0 if invalid (rejected).
 */
int user_range_valid(const void* ptr, size_t len);

/*
 * copy_from_user — copy len bytes from user-space src to kernel dst.
 * Returns 0 on success, -EFAULT if the range is invalid.
 * Does NOT allocate memory.
 */
int copy_from_user(void* dst, const void* user_src, size_t len);

/*
 * copy_to_user — copy len bytes from kernel src to user-space dst.
 * Returns 0 on success, -EFAULT if the range is invalid.
 * Does NOT allocate memory.
 */
int copy_to_user(void* user_dst, const void* src, size_t len);
