; SecOS Kernel - Context switch assembly (trapframe restore + iretq)
; Copyright (c) 2025 iDev srl
; Author: Luigi De Astis <l.deastis@idev-srl.com>
; SPDX-License-Identifier: MIT
;
; [M6] Restores a full trapframe_t and executes iretq.
;
; trapframe_t layout (offsets in bytes from base):
;   0x00: r15  0x08: r14  0x10: r13  0x18: r12
;   0x20: r11  0x28: r10  0x30: r9   0x38: r8
;   0x40: rbp  0x48: rdi  0x50: rsi  0x58: rdx
;   0x60: rcx  0x68: rbx  0x70: rax
;   0x78: int_no  0x80: err_code
;   0x88: rip  0x90: cs   0x98: rflags  0xA0: rsp  0xA8: ss
BITS 64

GLOBAL arch_iret_to_tf

; void arch_iret_to_tf(trapframe_t* tf)  __attribute__((noreturn));
; rdi = pointer to trapframe_t struct (NOT on the stack — it's a heap copy)
;
; Strategy: build the iret frame on the current stack, then restore GPRs from
; the struct, and iretq.  We cannot just set RSP to &tf->r15 because the
; trapframe lives in heap memory that may be reclaimed after the switch.
arch_iret_to_tf:
    ; Push the CPU iret frame (SS, RSP, RFLAGS, CS, RIP) in reverse
    push qword [rdi + 0xA8]    ; ss
    push qword [rdi + 0xA0]    ; rsp
    push qword [rdi + 0x98]    ; rflags
    push qword [rdi + 0x90]    ; cs
    push qword [rdi + 0x88]    ; rip

    ; Restore GPRs from trapframe (rdi last since we need it as base pointer)
    mov r15, [rdi + 0x00]
    mov r14, [rdi + 0x08]
    mov r13, [rdi + 0x10]
    mov r12, [rdi + 0x18]
    mov r11, [rdi + 0x20]
    mov r10, [rdi + 0x28]
    mov r9,  [rdi + 0x30]
    mov r8,  [rdi + 0x38]
    mov rbp, [rdi + 0x40]
    ; skip rdi (0x48) — restore after we're done using it as base
    mov rsi, [rdi + 0x50]
    mov rdx, [rdi + 0x58]
    mov rcx, [rdi + 0x60]
    mov rbx, [rdi + 0x68]
    mov rax, [rdi + 0x70]
    ; Now restore rdi itself (last GPR using rdi as base)
    mov rdi, [rdi + 0x48]

    iretq
