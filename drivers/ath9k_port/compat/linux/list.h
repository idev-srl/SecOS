#ifndef _C_LIST_H
#define _C_LIST_H
#include <linux/types.h>
struct list_head { struct list_head *next,*prev; };
static inline void INIT_LIST_HEAD(struct list_head*l){l->next=l;l->prev=l;}
#define LIST_HEAD_INIT(n) { &(n), &(n) }
#define list_for_each_entry(p,h,m) for(p=NULL;0;)
#define list_empty(h) ((h)->next==(h))
#endif
