#ifndef _C_KERNEL_H
#include <linux/compiler.h>
#include <linux/bitops.h>
#include <asm/byteorder.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/list.h>
#define _C_KERNEL_H
#include <linux/types.h>
#define BIT(n) (1UL<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define max_t(t,a,b) ((t)(a)>(t)(b)?(t)(a):(t)(b))
#define clamp(v,lo,hi) ((v)<(lo)?(lo):((v)>(hi)?(hi):(v)))
#define abs(x) ((x)<0?-(x):(x))
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define do_div(n,d) ({ u32 __r=(n)%(d); (n)/=(d); __r; })
#define swap(a,b) do{ typeof(a) __t=(a); (a)=(b); (b)=__t; }while(0)
#define container_of(p,t,m) ((t*)((char*)(p)-(unsigned long)&((t*)0)->m))
#define printk(...) do{}while(0)
#define pr_err(...) do{}while(0)
#define WARN_ON(c) (!!(c))
#define WARN_ON_ONCE(c) (!!(c))
#define BUG_ON(c) do{}while(0)
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define __init
#define __exit
#define KERN_ERR ""
#define KERN_CRIT ""
#define KERN_WARNING ""
#define KERN_INFO ""
#define KERN_DEBUG ""
#define KERN_NOTICE ""
#define KERN_CONT ""
#define unlikely(x) (x)
#define likely(x) (x)
#endif
