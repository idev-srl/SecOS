#ifndef _PORT_PWD_H
#define _PORT_PWD_H
struct passwd { char* pw_name; char* pw_passwd; int pw_uid; int pw_gid; char* pw_gecos; char* pw_dir; char* pw_shell; };
struct passwd* getpwnam(const char*);
struct passwd* getpwuid(int);
struct passwd* getpwent(void);
void setpwent(void); void endpwent(void);
#endif
