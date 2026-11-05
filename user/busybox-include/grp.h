/* SecOS BusyBox overlay: <grp.h>. SPDX-License-Identifier: MIT */
#ifndef _BB_GRP_H
#define _BB_GRP_H
#include <sys/types.h>
struct group { char* gr_name; char* gr_passwd; gid_t gr_gid; char** gr_mem; };
struct group* getgrnam(const char* n);
struct group* getgrgid(gid_t g);
int getgrnam_r(const char* n, struct group* g, char* b, size_t l, struct group** r);
int getgrgid_r(gid_t gid, struct group* g, char* b, size_t l, struct group** r);
struct group* getgrent(void);
void setgrent(void);
void endgrent(void);
int getgrouplist(const char* user, gid_t g, gid_t* groups, int* n);
int initgroups(const char* user, gid_t g);
#endif
