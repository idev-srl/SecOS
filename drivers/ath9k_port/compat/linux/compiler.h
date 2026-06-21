#ifndef _C_COMPILER_H
#define _C_COMPILER_H
#define __printf(a,b)
#define __packed __attribute__((packed))
#define __aligned(n) __attribute__((aligned(n)))
#define __force
#define __iomem
#define __user
#define __must_check
#define __maybe_unused __attribute__((unused))
#define __always_inline inline
#define __read_mostly
#define __init
#define __exit
#define __bitwise
#define __rcu
#define __percpu
#define fallthrough do{}while(0)
#define struct_group(name,...) struct { __VA_ARGS__ } name; struct { __VA_ARGS__ }
#define struct_group_tagged(tag,name,...) struct { __VA_ARGS__ } name
#define offsetof(t,m) __builtin_offsetof(t,m)
#endif
