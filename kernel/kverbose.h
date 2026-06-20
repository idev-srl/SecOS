/* kverbose.h — kernel verbosity gate for per-spawn / teardown debug logs.
 *
 * g_kverbose defaults to 0 (quiet): a clean shell shows only program output.
 * The shell `verbose on|off` command toggles it; the boot self-tests are
 * unaffected (they log to debugcon, not via these helpers). SPDX: MIT */
#ifndef KVERBOSE_H
#define KVERBOSE_H
#include <stdint.h>

extern int g_kverbose;            /* 0 = quiet (default), 1 = verbose */

/* Console logging that only prints when verbose — used for the noisy
 * process-create/ELF-load/teardown lines. */
void kvlog(const char* s);
void kvhex(uint64_t v);
void kvputc(char c);

#endif
