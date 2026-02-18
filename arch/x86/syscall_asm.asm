; SecOS Kernel - Syscall Entry Stub (INT 0x80 path)
; Copyright (c) 2025 iDev srl
; Author: Luigi De Astis <l.deastis@idev-srl.com>
; SPDX-License-Identifier: MIT
;
; [M5.1] Full trapframe save — layout matches isr_common / struct trapframe:
;   CPU pushes: SS, RSP, RFLAGS, CS, RIP  (on privilege change)
;   stub pushes: int_no(0x80), err_code(0), rax..r15
;   RDI = pointer to trapframe on stack -> syscall_handler(tf)
;   Return value in RAX patched into trapframe before restore.
BITS 64
GLOBAL syscall_entry
EXTERN syscall_handler

syscall_entry:
    ; Build int_no / err_code slots (same as ISR_NOERRCODE convention)
    push 0              ; dummy err_code
    push 0x80           ; int_no = 0x80 (syscall marker)

    ; Save all GPRs in trapframe order (must match struct trapframe)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Pass trapframe pointer to C handler
    mov rdi, rsp
    call syscall_handler

    ; Patch return value into trapframe->rax
    mov [rsp + 14*8], rax   ; offset of rax in GPR block (14 pushes from top)

    ; Restore all GPRs
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Remove int_no + err_code
    add rsp, 16

    iretq
