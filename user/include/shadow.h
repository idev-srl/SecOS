/* <shadow.h> - SecOS libc: /etc/shadow (mode 0600, root only). [M42]
 * SPDX-License-Identifier: MIT */
#ifndef _SHADOW_H
#define _SHADOW_H
struct spwd {
    char* sp_namp;            /* login name */
    char* sp_pwdp;            /* hashed password ($6$... SHA-512-crypt), "!"/"*" = locked */
    long  sp_lstchg;          /* last change, days since 1970-01-01 */
    long  sp_min, sp_max, sp_warn, sp_inact, sp_expire;
    unsigned long sp_flag;
};
struct spwd* getspnam(const char* name);
struct spwd* getspent(void);
void setspent(void);
void endspent(void);
#endif
