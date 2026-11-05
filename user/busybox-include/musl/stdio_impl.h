/* SecOS: stand-in for musl's internal stdio_impl.h (single-threaded). */
#ifndef _BB_MUSL_STDIO_IMPL_H
#define _BB_MUSL_STDIO_IMPL_H
#include <stdio.h>
#define FLOCK(f) ((void)0)
#define FUNLOCK(f) ((void)0)
#ifndef weak_alias
#define weak_alias(old, new) extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))
#endif
#endif
