/* <utmp.h> — SecOS libc. Only login_tty(); there is no utmp database.
 * SPDX-License-Identifier: MIT */
#ifndef _UTMP_H
#define _UTMP_H

/* Make fd the controlling terminal and stdin/stdout/stderr of the caller:
 * setsid(), TIOCSCTTY, dup2 onto 0/1/2, close fd (if > 2). 0 or -1. */
int login_tty(int fd);

#endif
