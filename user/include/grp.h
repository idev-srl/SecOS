/* <grp.h> - SecOS libc: /etc/group (root when the file is missing). SPDX: MIT */
#ifndef _GRP_H
#define _GRP_H
struct group { char* gr_name; char* gr_passwd; int gr_gid; char** gr_mem; };
struct group* getgrnam(const char*);
struct group* getgrgid(int);
struct group* getgrent(void);
void setgrent(void); void endgrent(void);
int getgrouplist(const char* user, int group, int* groups, int* ngroups);
int initgroups(const char* user, int group);
#endif
