/* SecOS overlay for musl: cancellable syscall entry (replaces musl's).
 * __syscall_cp_asm(&cancel, nr, u, v, w, x, y, z) checks the cancel flag and
 * calls the translation layer __secos_syscall(nr, u, v, w, x, y, z). Without
 * threads (U9) nothing ever cancels; asynchronous cancellation needs the pc
 * inside [__cp_begin, __cp_end) and is revisited with pthreads.
 * SPDX-License-Identifier: MIT */
.text
.global __cp_begin
.hidden __cp_begin
.global __cp_end
.hidden __cp_end
.global __cp_cancel
.hidden __cp_cancel
.hidden __cancel
.hidden __secos_syscall
.global __syscall_cp_asm
.hidden __syscall_cp_asm
.type   __syscall_cp_asm,@function
__syscall_cp_asm:

__cp_begin:
	mov (%rdi),%eax
	test %eax,%eax
	jnz __cp_cancel
	mov %rsi,%rdi           /* nr */
	mov %rdx,%rsi           /* u  */
	mov %rcx,%rdx           /* v  */
	mov %r8,%rcx            /* w  */
	mov %r9,%r8             /* x  */
	mov 8(%rsp),%r9         /* y  */
	mov 16(%rsp),%rax       /* z  */
	push %rax               /* 7th argument; keeps rsp 16-aligned at the call */
	call __secos_syscall
	add $8,%rsp
__cp_end:
	ret
__cp_cancel:
	jmp __cancel
