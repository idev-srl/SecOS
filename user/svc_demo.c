/* svc_demo — a trivial SecOS "service": announces itself, does a little work,
 * then exits. The init/service-manager (init.c) supervises and restarts it.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include "libsecos.h"

int main(void) {
    int pid = getpid();
    printf("[svc] service started pid=%d\n", pid);
    sleep_ticks(200);                 /* ~0.2s of "work" */
    printf("[svc] service pid=%d exiting\n", pid);
    return 0;
}
