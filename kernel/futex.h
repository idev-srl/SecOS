/*
 * futex.h — [U9] Linux futex(2) for SecOS threads (SecOS 512 + 202).
 * SPDX-License-Identifier: MIT
 */
#ifndef FUTEX_H
#define FUTEX_H
#include <stdint.h>
#include "trapframe.h"

struct process;

#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET     10
#define FUTEX_PRIVATE_FLAG    128
#define FUTEX_CLOCK_REALTIME  256
#define FUTEX_BITSET_MATCH_ANY 0xffffffffu

// futex(uaddr, op, val, timeout|val2, uaddr2, val3) for the current task.
// May block (rewinds tf->rip and does not return), like the other blocking
// syscalls; the re-run resumes the wait. Returns the Linux result (-errno).
long futex_syscall(trapframe_t* tf, uint64_t uaddr, uint64_t op, uint64_t val,
                   uint64_t utime, uint64_t uaddr2, uint64_t val3);
// Wake up to n waiters on cur's private word at uaddr (CLONE_CHILD_CLEARTID).
int  futex_wake_addr(struct process* cur, uint64_t uaddr, int n);
// The task is exiting / exec'ing: take it off any futex queue.
void futex_exit(struct process* p);
// Counters for /proc/smp-style diagnostics (waits, wakes, timeouts).
void futex_stats(uint64_t* waits, uint64_t* wakes, uint64_t* timeouts);

#endif
