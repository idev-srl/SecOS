/*
 * SecOS Kernel - Driver Interface Subsystem
 * Developed by iDev srl - Luigi De Astis <l.deastis@idev-srl.com>
 * Open License: Educational / permissive use.
 *
 * [M57] Only the teardown hook is left of the M11 mock (see driver_if.h).
 */
#include "driver_if.h"
#include "udrv.h"

void driver_remove_all_bindings(process_t* p) {
    udrv_release_all(p);
}
