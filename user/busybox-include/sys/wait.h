/* SecOS BusyBox overlay: <sys/wait.h>. The kernel returns the Linux wait-status
 * encoding (libsecos.h has the W* decoders). SPDX-License-Identifier: MIT */
#ifndef _BB_SYS_WAIT_H
#define _BB_SYS_WAIT_H
#include <sys/types.h>
#include <signal.h>
#include "libsecos.h"
#define WNOHANG    1
#define WUNTRACED  2
#define WSTOPPED   WUNTRACED
#define WEXITED    4
#define WCONTINUED 8
#define WNOWAIT    0x01000000
#ifndef WIFCONTINUED
#define WIFCONTINUED(s) ((s) == 0xffff)
#endif
typedef enum { P_ALL = 0, P_PID = 1, P_PGID = 2 } idtype_t;
struct rusage;
pid_t sbb_waitpid(pid_t pid, int* status, int options);
pid_t wait(int* status);
pid_t wait3(int* status, int options, struct rusage* ru);
pid_t wait4(pid_t pid, int* status, int options, struct rusage* ru);
int   waitid(idtype_t t, id_t id, siginfo_t* info, int options);
#endif
