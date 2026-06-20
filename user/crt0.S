/* SecOS user CRT0 — entry point. SPDX-License-Identifier: MIT
 * The kernel enters at _start in ring 3 with RSP = stack top. */
.intel_syntax noprefix
.section .text
.global _start
_start:
    xor     rbp, rbp
    call    main
    mov     edi, eax        /* exit code = main() return */
    mov     rax, 1          /* SYS_EXIT */
    int     0x80
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
