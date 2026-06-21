#ifndef _C_SPINLOCK_H
#define _C_SPINLOCK_H
#include <linux/types.h>
typedef struct { int x; } spinlock_t;
typedef struct { int x; } rwlock_t;
static inline void spin_lock_init(spinlock_t*l){(void)l;}
static inline void spin_lock(spinlock_t*l){(void)l;}
static inline void spin_unlock(spinlock_t*l){(void)l;}
static inline void spin_lock_bh(spinlock_t*l){(void)l;}
static inline void spin_unlock_bh(spinlock_t*l){(void)l;}
#define spin_lock_irqsave(l,f) do{(void)(l);(f)=0;}while(0)
#define spin_unlock_irqrestore(l,f) do{(void)(l);(void)(f);}while(0)
#endif
