#ifndef _C_SKBUFF_H
#define _C_SKBUFF_H
#include <linux/types.h>
struct sk_buff { void* data; unsigned int len; void* head; void* tail; struct sk_buff* next; };
static inline void* skb_put(struct sk_buff*s,unsigned n){void*p=s->tail;s->len+=n;return p;}
static inline void* skb_push(struct sk_buff*s,unsigned n){s->data=(char*)s->data-n;s->len+=n;return s->data;}
static inline void skb_reserve(struct sk_buff*s,int n){(void)s;(void)n;}
#endif
