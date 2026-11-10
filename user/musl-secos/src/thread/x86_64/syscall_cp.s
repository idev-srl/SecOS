/* SecOS overlay for musl: the cancellation-point trap (replaces musl's
 * syscall_cp.s). musl's __syscall_cp_asm is in C here (src/secos/syscall.c:
 * it translates the Linux call); the one kernel call of a translated
 * cancellable syscall that may block goes through
 *     long __secos_cp_trap(volatile int *cancel, long secos_nr,
 *                          long a, long b, long c, long d, long e, long f)
 * so that, as on Linux, the pc of a thread interrupted before or in that
 * kernel call lies in [__cp_begin, __cp_end): musl's SIGCANCEL handler
 * (pthread_cancel.c) then moves it to __cp_cancel. The kernel's signal frame
 * gives the handler the interrupted pc (ucontext_t, U9). A thread whose call
 * already returned (pc == __cp_end) is cancelled by __syscall_cp_c when the
 * result is -EINTR. SPDX-License-Identifier: MIT */
.text
.global __cp_begin
.hidden __cp_begin
.global __cp_end
.hidden __cp_end
.global __cp_cancel
.hidden __cp_cancel
.hidden __cancel
.global __secos_cp_trap
.hidden __secos_cp_trap
.type   __secos_cp_trap,@function
__secos_cp_trap:
__cp_begin:
	mov (%rdi),%eax
	test %eax,%eax
	jnz __cp_cancel
	mov %rsi,%rax           /* SecOS number */
	mov %rdx,%rdi           /* a */
	mov %rcx,%rsi           /* b */
	mov %r8,%rdx            /* c */
	mov %r9,%rcx            /* d (4th SecOS argument: rcx) */
	mov 8(%rsp),%r8         /* e */
	mov 16(%rsp),%r9        /* f */
	int $0x80
__cp_end:
	ret
__cp_cancel:
	jmp __cancel
.size __secos_cp_trap,.-__secos_cp_trap
