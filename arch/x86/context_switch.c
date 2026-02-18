/*
 * SecOS Kernel - Context switch glue (C side)
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M6] Loads the target address space and jumps into the saved trapframe
 * via arch_iret_to_tf (assembly).
 */
#include "process.h"
#include "vmm.h"
#include "terminal.h"
#include "panic.h"

extern void arch_iret_to_tf(trapframe_t* tf) __attribute__((noreturn));

void arch_switch_to_process(process_t* next) {
    if (!next->tf)
        kernel_panic("[M6] arch_switch_to_process: next->tf is NULL", __FILE__, __LINE__);

    if (vmm_switch_space(next->space) != 0)
        kernel_panic("[M6] arch_switch_to_process: vmm_switch_space failed", __FILE__, __LINE__);

    arch_iret_to_tf(next->tf);
    /* never reached */
}
