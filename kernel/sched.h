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
void sched_kill_current(int sig) __attribute__((noreturn)); // [M15] fault -> kill + switch away
void sched_block_current(trapframe_t* tf); // [M16/M17] block caller (re-runs syscall on wake)
void sched_stop_current(trapframe_t* tf) __attribute__((noreturn)); // [M30] SIGSTOP/SIGTSTP
int  sched_wake_waitpid(uint32_t pid, int code); // [M16] wake a SYS_WAIT blocker -> #woken
void sched_wake_parent_waitany(uint32_t ppid); // [M39] wake a waitpid(-1) blocker
void sched_wake_sleepers(uint64_t now);    // [M17] wake elapsed SYS_SLEEP blockers
void sched_wake_chan(int chan);            // [M17] wake SYS_MSG_RECV blockers on chan
void sched_wake_pipe(void* pp);            // [M25] wake pipe read/write blockers on pp
void sched_wake_pty(void* pty);            // wake pty read/write/poll blockers on pty
void sched_reap_zombies(void);             // free ZOMBIE processes (not current)
int  sched_count_alive_user(void);         // user processes not yet ZOMBIE
// [M48] SMP
int  sched_make_ready(process_t* p);       // NEW -> READY (publish a built task) + kick
int  sched_wake_task(process_t* p, int from_state); // CAS from_state -> READY + kick
void sched_kick_for(process_t* p);         // reschedule IPI to an idle CPU for p
void sched_wake_note(void);                // a wait condition changed (lost-wakeup guard)
uint64_t sched_wake_seq(void);             // current wake sequence (syscall entry snapshot)
void sched_on_resched_ipi(trapframe_t* tf);
// [U9] threads
int  sched_wake_task_sync(process_t* p);   // BLOCKED -> READY under proc_lock (futex wake)
void sched_thread_exit_current(int status); // Linux exit(2): this thread only (no return for a user task)
void sched_cpu_stats(uint32_t cpu, uint64_t* switches, uint64_t* migrations,
                     uint64_t* ipi_tlb, uint64_t* ipi_resched);

#endif
