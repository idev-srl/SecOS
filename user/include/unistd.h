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
unsigned umask(unsigned m);

#endif
