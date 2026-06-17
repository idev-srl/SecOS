/* SecOS minimal user libc (POSIX-friendly surface over the custom syscall ABI).
 * SPDX-License-Identifier: MIT */
#ifndef LIBSECOS_H
#define LIBSECOS_H
#include <stddef.h>

typedef long ssize_t;

/* Raw syscall: rax=num, args in rdi,rsi,rdx,rcx,r8; via int 0x80. */
long secos_syscall(long num, long a0, long a1, long a2, long a3, long a4);

/* POSIX-ish wrappers */
ssize_t write(int fd, const void* buf, size_t len);
void    _exit(int code) __attribute__((noreturn));
int     getpid(void);
void    sched_yield(void);

/* [M13] uptime + minimal IPC channels */
unsigned long getticks(void);                            /* uptime in timer ticks */
long    msg_send(int chan, const void* buf, long len);   /* bytes accepted, or <0 */
long    msg_recv(int chan, void* buf, long len);         /* bytes read (0 empty), or <0 */

/* tiny libc helpers */
size_t  strlen(const char* s);
int     puts(const char* s);   /* writes s + '\n' to stdout (fd 1) */

#endif /* LIBSECOS_H */
