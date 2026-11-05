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
 * Range validation alone does not prove the pages are mapped. The actual
 * user accesses happen inside fault-tolerant primitives (arch/x86/uaccess.asm)
 * registered in the exception table: a ring-0 #PF on a user address is first
 * served by demand paging / COW (M14/M19); if that fails, the faulting rip is
 * looked up in the table and execution resumes at a fixup, so the helper
 * returns -EFAULT instead of the kernel panicking. Any OTHER ring-0 access to a
 * user address that faults is a kernel bug and still panics loudly — so never
 * dereference a user pointer outside these helpers. They also bracket the
 * access with stac/clac, which makes them the only SMAP-safe user accesses.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* Top of the x86-64 user canonical address space (exclusive). */
#define USER_ADDR_MAX  0x0000800000000000ULL

/* POSIX EFAULT (14) returned on invalid user address. */
#define EFAULT 14
/* POSIX ENAMETOOLONG (36): user string longer than the kernel buffer. */
#define ENAMETOOLONG 36

/*
 * user_range_valid — check that [ptr, ptr+len) is entirely within the user
 * canonical address range and that arithmetic does not overflow.
 * Returns 1 if valid, 0 if invalid (rejected).
 */
int user_range_valid(const void* ptr, size_t len);

/*
 * copy_from_user — copy len bytes from user-space src to kernel dst.
 * Returns 0 on success, -EFAULT if the range is invalid or a page is
 * unmapped/inaccessible (and not demand-pageable).
 * Does NOT allocate memory.
 */
int copy_from_user(void* dst, const void* user_src, size_t len);

/*
 * copy_to_user — copy len bytes from kernel src to user-space dst.
 * Returns 0 on success, -EFAULT if the range is invalid or a page is
 * unmapped/inaccessible (and not demand-pageable).
 * Does NOT allocate memory.
 */
int copy_to_user(void* user_dst, const void* src, size_t len);

/*
 * strncpy_from_user — copy a NUL-terminated user string into dst[max].
 * Returns the length (excluding NUL, dst terminated) on success, -EFAULT on a
 * bad/unmapped pointer, -ENAMETOOLONG if no NUL within max bytes (dst is then
 * truncated + terminated, but the caller should treat it as an error).
 */
long strncpy_from_user(char* dst, const char* user_src, size_t max);

/*
 * uaccess_fixup_lookup — exception-table search for the #PF handler: returns
 * the fixup rip for a registered user-access instruction, or 0.
 */
uint64_t uaccess_fixup_lookup(uint64_t rip);
