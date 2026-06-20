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

#endif
