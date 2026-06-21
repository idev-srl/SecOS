#ifndef _C_IO_H
#define _C_IO_H
#include <linux/types.h>
/* MMIO is wired to the driver's reg_ops in the ath9k port; these are unused
 * fallbacks. */
static inline u32 ioread32(const volatile void* a){ return *(const volatile u32*)a; }
static inline void iowrite32(u32 v, volatile void* a){ *(volatile u32*)a = v; }
#endif
