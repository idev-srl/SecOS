#ifndef _PORT_GRP_H
#define _PORT_GRP_H
struct group { char* gr_name; char* gr_passwd; int gr_gid; char** gr_mem; };
struct group* getgrnam(const char*);
struct group* getgrgid(int);
struct group* getgrent(void);
void setgrent(void); void endgrent(void);
#endif
