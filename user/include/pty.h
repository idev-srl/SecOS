/* <pty.h> — SecOS libc (libutil-style pty helpers). SPDX-License-Identifier: MIT */
#ifndef _PTY_H
#define _PTY_H
#include <termios.h>

/* Open a pty pair: *amaster = master fd, *aslave = slave fd (both O_NOCTTY).
 * name (if not NULL, >= 32 bytes) receives the slave path; termp/winp (if not
 * NULL) are applied to the slave. 0, or -1 with errno. */
int openpty(int* amaster, int* aslave, char* name,
            const struct termios* termp, const struct winsize* winp);
/* openpty + fork: the child gets the slave as its controlling terminal and
 * stdin/stdout/stderr (login_tty) and returns 0; the parent gets the child's
 * pid and *amaster. -1 with errno on failure. */
int forkpty(int* amaster, char* name,
            const struct termios* termp, const struct winsize* winp);

#endif
