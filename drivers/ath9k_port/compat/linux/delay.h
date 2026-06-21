#ifndef _C_DELAY_H
#define _C_DELAY_H
#include <linux/types.h>
static inline void udelay(unsigned long us){ for(volatile unsigned long i=0;i<us*300UL;i++){} }
static inline void mdelay(unsigned long ms){ udelay(ms*1000); }
static inline void usleep_range(unsigned long a,unsigned long b){ (void)b; udelay(a); }
static inline void ndelay(unsigned long ns){ (void)ns; }
#endif
