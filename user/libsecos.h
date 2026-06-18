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
long    msg_recv(int chan, void* buf, long len);         /* blocks if empty (M17); bytes read or <0 */

/* [M16] process control */
int     spawn(const char* path, char* const argv[]);     /* signed child + argv -> pid, or <0 */
int     waitpid(int pid);                                /* block until child exits -> status */
/* [M17] blocking sleep */
void    sleep_ticks(unsigned ticks);

/* [M18] dynamic memory (POSIX-ish). PROT and MAP flags match the kernel ABI. */
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
void*   mmap(void* addr, size_t len, int prot, int flags);
int     munmap(void* addr, size_t len);
int     mprotect(void* addr, size_t len, int prot);
void*   sbrk(long incr);
unsigned long brk_set(unsigned long addr);
void*   malloc(size_t size);
void    free(void* p);

/* [M19] copy-on-write fork */
int     fork(void);

/* tiny libc helpers */
size_t  strlen(const char* s);
int     puts(const char* s);   /* writes s + '\n' to stdout (fd 1) */

#endif /* LIBSECOS_H */
