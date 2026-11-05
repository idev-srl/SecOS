/*
 * SecOS Kernel - Orderly power-off / reboot / halt (SYS_REBOOT). See power.c.
 * SPDX-License-Identifier: MIT
 */
#ifndef SECOS_POWER_H
#define SECOS_POWER_H

/* SYS_REBOOT commands (mirrored in user/libsecos.h). */
#define SECOS_REBOOT_POWEROFF 1
#define SECOS_REBOOT_RESTART  2
#define SECOS_REBOOT_HALT     3

struct process;
/* One step of the shutdown state machine; 1 = block and re-run, <0 = error. */
long power_syscall_step(struct process* cur, int cmd);

#endif
