/* SecOS M13 IPC consumer: reads a message from kernel channel 0 (non-blocking,
 * polls with sched_yield) and prints it. SPDX-License-Identifier: MIT */
#include "libsecos.h"

int main(void) {
    puts("[ipc_recv] consumer running");
    char buf[64];
    for (int tries = 0; tries < 1000000; tries++) {
        long n = msg_recv(0, buf, (long)sizeof(buf));
        if (n > 0) {
            write(1, "[ipc_recv] got: ", 16);
            write(1, buf, (size_t)n);
            write(1, "\n", 1);
            return 0;
        }
        sched_yield();
    }
    puts("[ipc_recv] timeout: no message");
    return 1;
}
