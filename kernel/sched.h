#ifndef SCHED_H
#define SCHED_H
#include "process.h"

void sched_init(void);
void sched_on_timer_tick(void); // chiamato da IRQ0
process_t* sched_get_current(void);
void sched_set_current(process_t* p);
int sched_add_process(process_t* p); // opzionale (wrapper)
void sched_yield(void); // invocare per switch cooperativo (stub)
void sched_yield_from_syscall(trapframe_t* tf); // [M7] cooperative yield from INT 0x80

#endif
