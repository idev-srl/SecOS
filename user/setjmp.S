/* SecOS libc - setjmp/longjmp for x86-64 (System V).
 * jmp_buf layout (8 longs): rbx, rbp, r12, r13, r14, r15, rsp(caller), rip. */
.text
.global setjmp
.type setjmp,@function
setjmp:
    movq %rbx,  0(%rdi)
    movq %rbp,  8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax        /* caller's rsp (above our return address) */
    movq %rax, 48(%rdi)
    movq (%rsp), %rax         /* return address */
    movq %rax, 56(%rdi)
    xorl %eax, %eax           /* setjmp returns 0 on the direct path */
    ret
.size setjmp,.-setjmp

.global longjmp
.type longjmp,@function
longjmp:
    movq  0(%rdi), %rbx
    movq  8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    movq 56(%rdi), %rdx       /* saved rip */
    movl %esi, %eax           /* return value */
    testl %eax, %eax
    jnz 1f
    movl $1, %eax             /* longjmp(env,0) must appear as 1 */
1:  jmp *%rdx
.size longjmp,.-longjmp

/* No executable stack. */
.section .note.GNU-stack,"",@progbits
