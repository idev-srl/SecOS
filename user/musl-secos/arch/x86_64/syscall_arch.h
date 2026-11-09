/* SecOS overlay for musl: arch/x86_64/syscall_arch.h (replaces musl's).
 * SPDX-License-Identifier: MIT
 *
 * musl issues Linux x86-64 syscalls through __syscall0..6. On SecOS they all
 * go to __secos_syscall (src/secos/syscall.c), which translates the Linux
 * number, arguments and structs to the SecOS ABI (int $0x80) and returns what
 * Linux would (-errno on failure). docs/USERLAND_PLAN.md D1/D2 (option B1).
 * No vDSO on SecOS: VDSO_* are not defined, so clock_gettime is a syscall. */
#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

long __secos_syscall(long, long, long, long, long, long, long)
	__attribute__((__visibility__("hidden")));

static __inline long __syscall0(long n)
{
	return __secos_syscall(n, 0, 0, 0, 0, 0, 0);
}

static __inline long __syscall1(long n, long a1)
{
	return __secos_syscall(n, a1, 0, 0, 0, 0, 0);
}

static __inline long __syscall2(long n, long a1, long a2)
{
	return __secos_syscall(n, a1, a2, 0, 0, 0, 0);
}

static __inline long __syscall3(long n, long a1, long a2, long a3)
{
	return __secos_syscall(n, a1, a2, a3, 0, 0, 0);
}

static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
	return __secos_syscall(n, a1, a2, a3, a4, 0, 0);
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	return __secos_syscall(n, a1, a2, a3, a4, a5, 0);
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	return __secos_syscall(n, a1, a2, a3, a4, a5, a6);
}

#define IPC_64 0
