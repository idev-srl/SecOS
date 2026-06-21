#ifndef _C_SLAB_H
#define _C_SLAB_H
#include <linux/types.h>
void* kmalloc(size_t,gfp_t); void* kzalloc(size_t,gfp_t); void kfree(const void*);
#define GFP_KERNEL 0
#define GFP_ATOMIC 0
static inline void* kcalloc(size_t n,size_t s,gfp_t f){ return kzalloc(n*s,f); }
#define kzalloc_obj(t) ((t*)kzalloc(sizeof(t),GFP_KERNEL))
#define kmalloc_array(n,s,f) kmalloc((n)*(s),f)
#endif
