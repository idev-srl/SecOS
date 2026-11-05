; SecOS Kernel - Fault-tolerant user-memory access primitives
; SPDX-License-Identifier: MIT
;
; Every instruction in the kernel that may touch a user address lives here and
; is registered in the exception table (.ex_table: pairs {faulting rip, fixup
; rip}). On a ring-0 #PF on a user address, exception_handler first tries
; normal demand paging / COW; only if that fails does it look the faulting rip
; up in the table and resume at the fixup, which makes the primitive report an
; error instead of panicking the kernel (Linux's __ex_table model).
;
; These primitives do NOT range-check and do NOT toggle SMAP (stac/clac): the C
; wrappers in mm/user_copy.c do both around the call.
BITS 64

section .text

; uint64_t __uaccess_copy(void* dst, const void* src, uint64_t n)
; Copy n bytes; either side may be a user address. Returns the number of bytes
; NOT copied (0 = success). `rep movsb` is restartable and keeps rcx exact on a
; fault, so the fixup simply returns rcx.
global __uaccess_copy
__uaccess_copy:
    cld                         ; never trust DF inherited from ring 3
    mov rcx, rdx
.copy:
    rep movsb
    xor eax, eax
    ret
.copy_fault:
    mov rax, rcx
    ret

; long __uaccess_strncpy(char* dst, const char* usrc, uint64_t max)
; Copy bytes up to and including the first NUL, at most max bytes.
; Returns the string length (excluding NUL) if a NUL was found within max,
; max if none was found (dst NOT terminated), or -1 on a fault.
global __uaccess_strncpy
__uaccess_strncpy:
    xor eax, eax
.str_loop:
    cmp rax, rdx
    jae .str_done
.str_load:
    movzx ecx, byte [rsi + rax]
    mov [rdi + rax], cl
    test cl, cl
    jz .str_done
    inc rax
    jmp .str_loop
.str_done:
    ret
.str_fault:
    mov rax, -1
    ret

; Exception table: {insn, fixup} qword pairs, collected by linker.ld between
; __ex_table_start and __ex_table_end.
section .ex_table progbits alloc noexec nowrite align=8
    dq __uaccess_copy.copy,     __uaccess_copy.copy_fault
    dq __uaccess_strncpy.str_load, __uaccess_strncpy.str_fault
