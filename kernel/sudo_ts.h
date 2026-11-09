/* sudo_ts.h - per-terminal-session sudo credential timestamps (kernel side).
 * See kernel/sudo_ts.c and docs/USERS.md. SPDX-License-Identifier: MIT */
#ifndef SECOS_SUDO_TS_H
#define SECOS_SUDO_TS_H
#include <stdint.h>

struct process;

/* prctl(PR_SECOS_SUDO_TS, op, arg) operations (kernel/syscall.h). */
#define SUDO_TS_CHECK      1   /* arg = timeout (s): 1 = a fresh record exists, 0 = none */
#define SUDO_TS_UPDATE     2   /* create / refresh this session's record */
#define SUDO_TS_INVALIDATE 3   /* sudo -k: drop this session's record */
#define SUDO_TS_REMOVE_ALL 4   /* sudo -K: drop every record of the real uid */

/* 0/1 (CHECK), 0, or a negative errno: -EPERM (euid != 0), -ENOTTY (no
 * controlling terminal, CHECK returns 0 instead), -EINVAL. */
long sudo_ts_op(struct process* c, uint64_t op, uint64_t arg);

#endif
