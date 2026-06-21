#ifndef _C_TIMER_H
#define _C_TIMER_H
#include <linux/types.h>
struct timer_list { unsigned long expires; void (*function)(struct timer_list*); void* data; };
struct delayed_work { void* w; };
static inline void timer_setup(struct timer_list*t,void*f,unsigned x){(void)t;(void)f;(void)x;}
#endif
