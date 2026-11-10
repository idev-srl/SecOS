/* SecOS overlay for musl: __clone (replaces musl's x86_64 clone.s).
 * int __clone(int (*func)(void *), void *stack, int flags, void *arg,
 *             pid_t *ptid, void *tls, pid_t *ctid)
 * SecOS clone = 512 + 56 (kernel/thread.c), SecOS ABI: rax = number, rdi =
 * flags, rsi = new stack, rdx = ptid, rcx = ctid, r8 = tls. The kernel only
 * accepts the thread flag set (pthread_create) or a plain fork. The new
 * thread starts on its stack with the caller's registers (r9 = func) and
 * pops its argument; func's return value is its exit(2) status (512 + 60).
 * SPDX-License-Identifier: MIT */
.text
.global __clone
.hidden __clone
.type   __clone,@function
__clone:
	mov %rdi,%r11           /* func */
	mov %rdx,%rdi           /* flags */
	mov %r8,%rdx            /* ptid */
	mov %r9,%r8             /* tls */
	mov %rcx,%r10           /* arg */
	mov 8(%rsp),%rcx        /* ctid */
	mov %r11,%r9            /* func, kept in the new thread */
	and $-16,%rsi
	sub $8,%rsi
	mov %r10,(%rsi)         /* arg, popped by the new thread */
	mov $568,%eax           /* SYS_LX_CLONE */
	int $0x80
	test %eax,%eax
	jnz 1f
	xor %ebp,%ebp
	pop %rdi
	call *%r9
	mov %eax,%edi
	mov $572,%eax           /* SYS_LX_EXIT: this thread */
	int $0x80
	hlt
1:	ret
.size __clone,.-__clone
