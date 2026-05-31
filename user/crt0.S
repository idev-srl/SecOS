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
