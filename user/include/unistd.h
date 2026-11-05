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

/* [M39] single-user identity (signature is the trust boundary; everyone is root) */
int   getuid(void);
int   geteuid(void);
int   getgid(void);
int   getegid(void);
int   setuid(int u);
int   setgid(int g);
int   getgroups(int n, int* g);
int   setgroups(unsigned n, const int* g);
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

#endif
