/* SecOS user CRT0 — entry point. SPDX-License-Identifier: MIT
 * The kernel enters at _start in ring 3 with RSP = stack top. */
.intel_syntax noprefix
.section .text
.global _start
_start:
    /* rdi/rsi/rdx = argc/argv/envp (System V, built by the kernel). Seed libc's
     * environ from envp, then call main(argc, argv, envp). rsp is 16-aligned. */
    xor     rbp, rbp
    mov     r12, rdi
    mov     r13, rsi
    mov     r14, rdx
    mov     rdi, rdx
    call    __libc_init_env
    mov     rdi, r12
    mov     rsi, r13
    mov     rdx, r14
    call    main
    mov     edi, eax        /* exit code = main() return */
    call    exit            /* libc: flush stdio, then SYS_EXIT */
1:  jmp     1b

/* [M30] Signal return trampoline. When a signal handler returns, it `ret`s here
 * (the kernel set this as the handler's return address); SYS_SIGRETURN restores
 * the interrupted context. Lives in the (RX) code segment, so no executable
 * stack is needed (W^X-clean). libc passes its address to SYS_SIGACTION. */
.global __sigreturn
__sigreturn:
    mov     rax, 45         /* SYS_SIGRETURN */
    int     0x80
2:  jmp     2b
