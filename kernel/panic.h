#ifndef PANIC_H
#define PANIC_H
/*
 * SecOS Kernel - Panic & Exception Handling (Header)
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

// Register snapshot captured on exception/trap
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

// Panic - halt the system printing a message
void kernel_panic(const char* message, const char* file, uint32_t line);

// Panic macro convenience
#define PANIC(msg) kernel_panic(msg, __FILE__, __LINE__)

// Assert macro triggers PANIC if condition is false
#define ASSERT(cond) do { if (!(cond)) kernel_panic("Assertion failed: " #cond, __FILE__, __LINE__); } while (0)

// Generic exception handler
void exception_handler(struct registers* regs);

// CPU exception names array
extern const char* exception_messages[];

#endif