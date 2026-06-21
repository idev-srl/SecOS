#ifndef _C_JIFFIES_H
#define _C_JIFFIES_H
#include <linux/types.h>
extern unsigned long jiffies;
#define HZ 100
#define msecs_to_jiffies(m) ((m)*HZ/1000)
#define time_after(a,b) ((long)((b)-(a))<0)
#define time_before(a,b) time_after(b,a)
#endif
