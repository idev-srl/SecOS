/*
 * user_copy.h — Safe kernel/user memory transfer primitives.
 *
 * Direct dereference of user pointers without range validation is forbidden.
 * All data crossing the user/kernel boundary must pass through these helpers.
 *
 * Range policy:
 *   Valid user address: [USER_ADDR_MIN, USER_ADDR_MAX)
 *   USER_ADDR_MIN = 0x0000000100000000 — the lowest address any user mapping can
 *     occupy (= vmm.h USER_CODE_BASE, 4 GB). This lower bound is a SECURITY GATE,
 *     not a nicety: until M44 every user address space kept the kernel-only low
 *     identity map (physical 0..512 MB mapped virt==phys, supervisor) live in
 *     PML4[0]. Those pages were supervisor (ring 3 could not touch them) but the
 *     kernel's OWN accesses in copy_from/to_user run with CPL 0 and WOULD succeed
 *     on them — so a user pointer below 4 GB handed to a syscall let the kernel
 *     read/write real physical memory (the kernel image, page tables, …) on the
 *     caller's behalf. [M44] User CR3s no longer contain the identity map (and
 *     the kernel retires it after early boot), so such a pointer now just
 *     faults; the floor stays as defence in depth (SMAP polices only USER pages,
 *     not supervisor aliases). The whole user layout (code @4 GB, data @8 GB,
 *     heap @10 GB, mmap @12 GB, stack @16 GB; see vmm.h) lives above it, so no
 *     legitimate access is lost.
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

/* Lowest valid user address (inclusive). Must sit above everything mapped in the
 * lower half of a user space — above all, above the kernel-only low identity map
 * (0..512 MB). Equals vmm.h USER_CODE_BASE (4 GB); kept as a literal so this
 * header stays dependency-free. See the range-policy note above. */
#define USER_ADDR_MIN  0x0000000100000000ULL

/* [M44] Set by cpu_enable_mitigations() when CR4.SMAP is on (the default when
 * the CPU supports it; -DSECOS_NO_SMAP opts out). */
extern int g_smap_enabled;

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
