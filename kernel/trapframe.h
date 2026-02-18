#ifndef TRAPFRAME_H
#define TRAPFRAME_H
/*
 * SecOS Kernel - Canonical trap frame layout
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * This layout matches the push order in isr_common / syscall_entry (idt_asm.asm,
 * syscall_asm.asm).  On a ring-3 -> ring-0 transition the CPU pushes SS, RSP,
 * RFLAGS, CS, RIP automatically; the asm stub then pushes int_no + err_code
 * (or dummy values) followed by all GPRs.
 *
 * Stack grows downward, so the first push (rax) is at the highest GPR offset
 * and r15 is at the lowest address (top of struct when RSP points here).
 */
#include <stdint.h>

typedef struct trapframe {
    /* pushed by asm stub (reverse order — r15 is at lowest address) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    /* pushed by CPU on interrupt/trap entry */
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) trapframe_t;

#endif /* TRAPFRAME_H */
