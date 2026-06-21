#ifndef _C_GPIOD_H
#define _C_GPIOD_H
#include <linux/types.h>
enum gpiod_flags { GPIOD_ASIS=0, GPIOD_IN, GPIOD_OUT_LOW, GPIOD_OUT_HIGH };
struct gpio_desc;
static inline struct gpio_desc* gpiod_get(void*a,const char*b,enum gpiod_flags f){(void)a;(void)b;(void)f;return 0;}
static inline int gpiod_get_value(const struct gpio_desc*d){(void)d;return 0;}
static inline void gpiod_set_value(struct gpio_desc*d,int v){(void)d;(void)v;}
#endif
