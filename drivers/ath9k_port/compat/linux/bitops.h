#ifndef _C_BITOPS_H
#define _C_BITOPS_H
#include <linux/types.h>
#define BITS_PER_LONG 64
#define BITS_TO_LONGS(n) (((n)+63)/64)
#define DECLARE_BITMAP(name,bits) unsigned long name[BITS_TO_LONGS(bits)]
#define BIT(n) (1UL<<(n))
#define BIT_ULL(n) (1ULL<<(n))
#define GENMASK(h,l) (((~0UL)<<(l)) & (~0UL>>(63-(h))))
#define GENMASK_ULL(h,l) (((~0ULL)<<(l)) & (~0ULL>>(63-(h))))
static inline int test_bit(int n,const volatile unsigned long*a){return (a[n/64]>>(n%64))&1;}
static inline void set_bit(int n,volatile unsigned long*a){a[n/64]|=(1UL<<(n%64));}
static inline void clear_bit(int n,volatile unsigned long*a){a[n/64]&=~(1UL<<(n%64));}
static inline int fls(int x){return x?(32-__builtin_clz(x)):0;}
static inline int ffs(int x){return x?(__builtin_ctz(x)+1):0;}
static inline unsigned long hweight_long(unsigned long w){return __builtin_popcountl(w);}
#define hweight32(x) __builtin_popcount(x)
#define for_each_set_bit(bit,addr,size) for((bit)=0;(bit)<(int)(size);(bit)++) if(test_bit((bit),(addr)))
static inline int find_first_bit(const unsigned long*a,int n){for(int i=0;i<n;i++)if(test_bit(i,a))return i;return n;}
static inline int find_next_bit(const unsigned long*a,int n,int s){for(int i=s;i<n;i++)if(test_bit(i,a))return i;return n;}
#endif
