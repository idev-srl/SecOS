/*
 * SecOS Kernel - Driver Interface Definitions
 * Developed by iDev srl - Luigi De Astis <l.deastis@idev-srl.com>
 * Open License: Educational / permissive use.
 *
 * [M57] The M11 Driver Space mock (two synthetic devices, a kmalloc'd shadow
 * register file and a validation-only MAP_MEM, reached through SYS_DRIVER) is
 * retired: SYS_DRIVER returns -ENOSYS and ring-3 drivers use SYS_UDRV on real
 * PCI functions (kernel/udrv.h). What stays is the teardown hook every process
 * exit/exec/destroy path already calls.
 */
#pragma once
#include <stdint.h>
#include "process.h" // for process_t

// Drop every driver resource owned by p (its ring-3 driver claims). Called on
// process teardown and exec; idempotent; no spinlock may be held.
void driver_remove_all_bindings(process_t* p);
