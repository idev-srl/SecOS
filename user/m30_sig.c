/* SecOS [M30] signals demo — signed, runs in ring 3.
 * SPDX-License-Identifier: MIT
 *
 * Proves the signal machinery end-to-end from user space:
 *   1. install a SIGUSR1 handler, raise(SIGUSR1) -> handler runs -> sigreturn
 *      restores the interrupted context (execution continues after raise()).
 *   2. SIG_IGN of SIGPIPE: writing to a pipe whose read end is closed returns
 *      EPIPE (-1) WITHOUT terminating the process.
 * All output goes to fd 1 (console + debugcon), so the selftest can assert on it.
 */
#include "libsecos.h"
#include <signal.h>

#define P(s) write(1, s, strlen(s))

static volatile int got_usr1 = 0;

static void on_usr1(int sig) {
    (void)sig;
    got_usr1 = 1;
    P("[m30] caught SIGUSR1\n");
}

int main(void) {
    P("[m30] start\n");

    signal(SIGUSR1, on_usr1);
    raise(SIGUSR1);                 /* delivered on the kill() syscall's return */
    P(got_usr1 ? "[m30] sigreturn ok\n" : "[m30] FAIL no handler\n");

    /* Ignore SIGPIPE so the EPIPE write does not terminate us. */
    signal(SIGPIPE, SIG_IGN);
    int fds[2];
    if (pipe(fds) == 0) {
        close(fds[0]);              /* no readers remain */
        long r = write(fds[1], "x", 1);
        P(r < 0 ? "[m30] sigpipe epipe ok\n" : "[m30] sigpipe FAIL\n");
        close(fds[1]);
    }

    P("[m30] done\n");
    return 0;
}
