/* <unistd.h> — SecOS libc. SPDX-License-Identifier: MIT */
#ifndef _UNISTD_H
#define _UNISTD_H
#include <stddef.h>
#include "libsecos.h"   /* write/read/close/lseek/getpid/sbrk/fork/pipe/_exit/... */

#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

unsigned int sleep(unsigned int seconds);   /* whole-second sleep (via SYS_SLEEP) */
int   usleep(unsigned long usec);
int   isatty(int fd);
char* ttyname(int fd);                      /* "/dev/pts/N", "/dev/console" or NULL */
int   ttyname_r(int fd, char* buf, size_t len);

/* [M42] Credentials (kernel/cred.c). The signature decides which code runs;
 * these ids decide who it acts for (file permissions, signals). */
int   getuid(void);
int   geteuid(void);
int   getgid(void);
int   getegid(void);
int   setuid(int u);
int   setgid(int g);
int   seteuid(int e);
int   setegid(int e);
int   setreuid(int r, int e);
int   setregid(int r, int e);
int   setresuid(int r, int e, int s);
int   setresgid(int r, int e, int s);
int   getresuid(int* r, int* e, int* s);
int   getresgid(int* r, int* e, int* s);
int   getgroups(int n, int* g);
int   setgroups(unsigned n, const int* g);
int   fchown(int fd, unsigned uid, unsigned gid);
int   lchown(const char* path, unsigned uid, unsigned gid);
char* getpass(const char* prompt);          /* /dev/tty, echo off */
char* crypt(const char* key, const char* setting);   /* <crypt.h>; link user/libcrypt.a (musl: $6$/$5$/$2b$) */
unsigned umask(unsigned m);                 /* SYS_UMASK: applied to open/mkdir */

/* Filesystem calls (kernel syscalls 70-75). */
#ifndef F_OK
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#endif
int   access(const char* path, int mode);
void  sync(void);
int   syncfs(int fd);
int   fsync(int fd);
int   fdatasync(int fd);
int   link(const char* oldpath, const char* newpath);
int   truncate(const char* path, long length);
int   ftruncate(int fd, long length);

/* sysconf / pathconf names (Linux numbering). */
#define _SC_ARG_MAX        0
#define _SC_CHILD_MAX      1
#define _SC_CLK_TCK        2
#define _SC_NGROUPS_MAX    3
#define _SC_OPEN_MAX       4
#define _SC_PAGESIZE       30
#define _SC_PAGE_SIZE      30
#define _SC_LINE_MAX       43
#define _SC_LOGIN_NAME_MAX 71
#define _SC_TTY_NAME_MAX   72
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES     85
#define _SC_AVPHYS_PAGES   86
#define _SC_SYMLOOP_MAX    173
#define _SC_HOST_NAME_MAX  180
#define _PC_LINK_MAX       0
#define _PC_NAME_MAX       3
#define _PC_PATH_MAX       4
#define _PC_PIPE_BUF       5
#define _PC_VDISABLE       8
long  sysconf(int name);                    /* -1 + EINVAL for an unknown name */
long  pathconf(const char* path, int name);
long  fpathconf(int fd, int name);

#endif
