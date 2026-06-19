#ifndef SCHED_H
#define SCHED_H
#include "process.h"

void sched_init(void);
void sched_on_timer_tick(trapframe_t* tf); // chiamato da IRQ0 (M8: preemption)
process_t* sched_get_current(void);
void sched_set_current(process_t* p);
int sched_add_process(process_t* p); // opzionale (wrapper)
void sched_yield(void); // invocare per switch cooperativo (stub)
void sched_yield_from_syscall(trapframe_t* tf); // [M7] cooperative yield from INT 0x80

// [M8] process lifecycle + idle/preemption
void sched_set_idle(process_t* idle);     // register the kernel idle task
void sched_exit_current(trapframe_t* tf);  // SYS_EXIT: zombie + switch away (no return)
void sched_kill_current(int reason) __attribute__((noreturn)); // [M15] fault -> kill + switch away
void sched_block_current(trapframe_t* tf); // [M16/M17] block caller (re-runs syscall on wake)
void sched_wake_waitpid(uint32_t pid, int code); // [M16] wake a SYS_WAIT blocker
void sched_wake_sleepers(uint64_t now);    // [M17] wake elapsed SYS_SLEEP blockers
void sched_wake_chan(int chan);            // [M17] wake SYS_MSG_RECV blockers on chan
void sched_wake_pipe(void* pp);            // [M25] wake pipe read/write blockers on pp
void sched_reap_zombies(void);             // free ZOMBIE processes (not current)
int  sched_count_alive_user(void);         // user processes not yet ZOMBIE

#endif
