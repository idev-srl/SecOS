/*
 * SecOS Kernel - Trapframe-based syscall handler
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M5.1] C entry point called from syscall_entry asm stub.
 * Extracts syscall ABI registers from the trapframe and forwards
 * to the existing syscall_dispatch().
 *
 * [M6] Saves trapframe snapshot into current process for context switch.
 */
#include "trapframe.h"
#include "syscall.h"
#include "sched.h"
#include "process.h"
#include "debugcon.h"

#ifdef SYSCALL_DEBUG
#include "terminal.h"
#endif

extern uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4);

uint64_t syscall_handler(trapframe_t* tf) {
    process_t* cur = sched_get_current();
    uint64_t num = tf->rax;

    // [M7] Debugcon: log pid + syscall number
    debugcon_writestring("[SYSCALL] pid=");
    debugcon_print_hex(cur ? cur->pid : 0);
    debugcon_writestring(" num=");
    debugcon_print_hex(num);
    debugcon_writestring("\n");

#ifdef SYSCALL_DEBUG
    {
        terminal_writestring("[SYSCALL] pid=");
        char hx[] = "0123456789ABCDEF";
        uint32_t pid = cur ? cur->pid : 0;
        for (int i = 28; i >= 0; i -= 4)
            terminal_putchar(hx[(pid >> i) & 0xF]);
        terminal_writestring(" num=");
        for (int i = 60; i >= 0; i -= 4)
            terminal_putchar(hx[(num >> i) & 0xF]);
        terminal_writestring("\n");
    }
#endif

    // [M7] SYS_YIELD: cooperative context switch.
    // sched_yield_from_syscall does NOT return if it switches to another process.
    // If no other process is ready, it returns and we fall through (rax=0).
    if (num == SYS_YIELD) {
        sched_yield_from_syscall(tf);
        // Reached only if no switch occurred — return 0 to caller
        return 0;
    }

    uint64_t ret = syscall_dispatch(num, tf->rdi, tf->rsi, tf->rdx, tf->rcx, tf->r8);

    // Save trapframe snapshot into current process (persistent copy)
    if (cur && cur->tf) {
        const uint8_t* s = (const uint8_t*)tf;
        uint8_t* d = (uint8_t*)cur->tf;
        for (int i = 0; i < (int)sizeof(trapframe_t); i++) d[i] = s[i];
        cur->tf->rax = ret; // patch return value
    }

    return ret;
}
