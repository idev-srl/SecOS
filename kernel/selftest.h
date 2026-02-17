/*
 * selftest.h — M4 minimal isolation selftest.
 *
 * Set M4_SELFTEST_ENABLE to 0 at build time to disable entirely:
 *   make CFLAGS_EXTRA=-DM4_SELFTEST_ENABLE=0
 *
 * When enabled (default), m4_run_selftests() is called from
 * kernel_main_phase2() and prints results to terminal + debugcon.
 */
#pragma once

#ifndef M4_SELFTEST_ENABLE
#define M4_SELFTEST_ENABLE 1
#endif

#if M4_SELFTEST_ENABLE
void m4_run_selftests(void);
#endif
