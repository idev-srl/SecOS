/*
 * SecOS Kernel UEFI Entry Shim
 * Entry point for UEFI boot path. Receives boot info from UEFI bootloader,
 * initializes terminal, and delegates to main kernel initialization.
 *
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "config.h"
#if CONFIG_UEFI
#include "bootinfo.h"
#include "terminal.h"
#include "panic.h"

// Forward declaration of main kernel entry
extern void kernel_main(uint32_t multiboot_magic, uint64_t multiboot_info);

// UEFI entry point - called by bootloader after ExitBootServices
// Signature matches what bootloader calls: kernel_main(0, &bootinfo)
// So this is actually called as kernel_main but we handle UEFI case
// The real entry is in kernel.c which detects magic=0 and boots UEFI path

#endif // CONFIG_UEFI
