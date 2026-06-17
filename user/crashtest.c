/* SecOS user program — deliberately faults to prove the kernel KILLS the
 * offending ring-3 process instead of halting (M15).
 * SPDX-License-Identifier: MIT */
#include "libsecos.h"

int main(void) {
    puts("[crash] about to fault (wild write to NULL)");
    /* Address 0 lies below USER_CODE_BASE — no VMA backs it, so this write is a
     * genuine not-present user fault with no demand-paging match. The kernel
     * must terminate THIS process and keep running. */
    volatile unsigned int* p = (volatile unsigned int*)0;
    *p = 0xdeadbeef;
    /* Unreachable: we were killed by the fault above. If this prints, M15 failed. */
    puts("[crash] STILL ALIVE — M15 FAILED");
    return 0;
}
