/*
 * SecOS Kernel - Trapframe-based syscall handler
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M5.1] C entry point called from syscall_entry asm stub.
 * Extracts syscall ABI registers from the trapframe and forwards
 * to the existing syscall_dispatch().
 */
#include "trapframe.h"
#include "syscall.h"

#ifdef SYSCALL_DEBUG
#include "terminal.h"
#include "sched.h"
#include "process.h"
#endif

extern uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4);

uint64_t syscall_handler(trapframe_t* tf) {
#ifdef SYSCALL_DEBUG
    {
        extern process_t* sched_get_current(void);
        process_t* cur = sched_get_current();
        terminal_writestring("[SYSCALL] pid=");
        char hx[] = "0123456789ABCDEF";
        uint32_t pid = cur ? cur->pid : 0;
        for (int i = 28; i >= 0; i -= 4)
            terminal_putchar(hx[(pid >> i) & 0xF]);
        terminal_writestring(" num=");
        for (int i = 60; i >= 0; i -= 4)
            terminal_putchar(hx[(tf->rax >> i) & 0xF]);
        terminal_writestring("\n");
    }
#endif
    return syscall_dispatch(tf->rax, tf->rdi, tf->rsi, tf->rdx, tf->rcx, tf->r8);
}
