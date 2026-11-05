/* SecOS BusyBox overlay: <pwd.h> (BusyBox's own libpwdgrp reads /etc/passwd).
 * SPDX-License-Identifier: MIT */
#ifndef _BB_PWD_H
#define _BB_PWD_H
#include <sys/types.h>
struct passwd { char* pw_name; char* pw_passwd; uid_t pw_uid; gid_t pw_gid;
                char* pw_gecos; char* pw_dir; char* pw_shell; };
struct passwd* getpwnam(const char* n);
struct passwd* getpwuid(uid_t u);
int getpwnam_r(const char* n, struct passwd* p, char* b, size_t l, struct passwd** r);
int getpwuid_r(uid_t u, struct passwd* p, char* b, size_t l, struct passwd** r);
struct passwd* getpwent(void);
void setpwent(void);
void endpwent(void);
#endif
