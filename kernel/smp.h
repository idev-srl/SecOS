/*
 * smp.h — SMP application-processor bring-up (M29-2).
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef SMP_H
#define SMP_H
#include <stdint.h>

/* Bring up the application processors discovered via ACPI (INIT-SIPI-SIPI). Each
 * AP enables its Local APIC, registers itself online, and (M29-2) parks. Must be
 * called after the APIC switchover and TSC calibration. No-op without SMP/ACPI. */
void smp_init(void);

/* Number of CPUs currently marked online (including the BSP). */
uint32_t smp_online_count(void);

/* The 64-bit C entry every AP jumps to from the trampoline. Not called directly. */
void ap_entry(void);

#endif /* SMP_H */
