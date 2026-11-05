/* SecOS BusyBox overlay: <shadow.h>. SPDX-License-Identifier: MIT */
#ifndef _BB_SHADOW_H
#define _BB_SHADOW_H
#include <stddef.h>
struct spwd { char* sp_namp; char* sp_pwdp; long sp_lstchg, sp_min, sp_max, sp_warn,
              sp_inact, sp_expire; unsigned long sp_flag; };
struct spwd* getspnam(const char* n);
int getspnam_r(const char* n, struct spwd* s, char* b, size_t l, struct spwd** r);
#endif
