BITS 64
GLOBAL syscall_entry
EXTERN syscall_dispatch

; Syscall convention:
; rax = number
; rdi, rsi, rdx, rcx, r8 = arg0..arg4
; Return value in rax
; C dispatcher prototype:
; uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
syscall_entry:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Save args in callee-saved registers (besides rbx already used)
    mov r15, r8      ; a4
    mov r14, rcx     ; a3
    mov r13, rdx     ; a2
    mov r12, rsi     ; a1
    mov rbx, rdi     ; a0

    ; Prepare call
    mov rdi, rax     ; num
    mov rsi, rbx     ; a0
    mov rdx, r12     ; a1
    mov rcx, r13     ; a2
    mov r8, r14      ; a3
    mov r9, r15      ; a4

    call syscall_dispatch

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    iretq
