#ifndef SCHED_H
#define SCHED_H
#include "process.h"

void sched_init(void);
void sched_on_timer_tick(void); // chiamato da IRQ0
process_t* sched_get_current(void);
int sched_add_process(process_t* p); // opzionale (wrapper)
void sched_yield(void); // invocare per switch cooperativo (stub)

#endif
