/* SecOS: stand-in for musl's internal locale_impl.h (C locale only), used to
 * compile musl's regex/fnmatch/getopt sources. SPDX-License-Identifier: MIT */
#ifndef _BB_MUSL_LOCALE_IMPL_H
#define _BB_MUSL_LOCALE_IMPL_H
#include <stdlib.h>
#define LCTRANS_CUR(s) (s)
#define __lctrans_cur(s) (s)
#ifndef weak_alias
#define weak_alias(old, new) extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))
#endif
#endif
