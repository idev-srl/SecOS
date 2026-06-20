/* SecOS user program — CPU stress spinner (SMP validation).
 *
 * A compute-bound ring-3 process: it busy-computes for a fixed, large number of
 * iterations (no syscalls in the hot loop, so it pegs a core), printing a start
 * and a done line so concurrent instances are visible interleaving. Spawn several
 * with the shell `stress` command to saturate multiple vCPUs.
 *
 * SPDX-License-Identifier: MIT
 */
#include "libsecos.h"

/* Append an unsigned decimal to buf at *pos. */
static void put_u(char* buf, int* pos, unsigned long v) {
    char t[24]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) buf[(*pos)++] = t[--n];
}

/* CPU-saturating run length, in timer ticks (1 kHz) → ~5 seconds. Duration-based
 * (not a fixed iteration count) so the wall-clock load is the same regardless of
 * how fast the core is — predictable to watch on a host CPU graph. */
#define SPIN_TICKS 5000UL

int main(void) {
    int pid = getpid();
    char buf[48]; int p = 0;
    for (const char* s = "[spin] pid="; *s; s++) buf[p++] = *s;
    put_u(buf, &p, (unsigned long)pid);
    for (const char* s = " start\n"; *s; s++) buf[p++] = *s;
    write(1, buf, p);

    /* Busy-compute until the deadline, checking the clock only once per large
     * batch so the hot loop stays in registers and pegs the core. `volatile`
     * keeps the compiler from optimizing the work away. */
    volatile unsigned long x = (unsigned long)pid * 2654435761UL + 1;
    unsigned long deadline = getticks() + SPIN_TICKS;
    unsigned long iters = 0;
    while (getticks() < deadline) {
        for (int b = 0; b < 2000000; b++) {
            x = x * 1103515245UL + 12345UL;
            x ^= x >> 13;
        }
        iters++;
    }

    p = 0;
    for (const char* s = "[spin] pid="; *s; s++) buf[p++] = *s;
    put_u(buf, &p, (unsigned long)pid);
    for (const char* s = " done batches="; *s; s++) buf[p++] = *s;
    put_u(buf, &p, iters);
    buf[p++] = '\n';
    write(1, buf, p);
    return 0;
}
