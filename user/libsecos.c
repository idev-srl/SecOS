/* SecOS minimal user libc. SPDX-License-Identifier: MIT */
#include "libsecos.h"

#define SYS_YIELD  0
#define SYS_EXIT   1
#define SYS_WRITE  2
#define SYS_GETPID 6

long secos_syscall(long num, long a0, long a1, long a2, long a3, long a4) {
    long ret;
    register long r8 asm("r8") = a4;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "0"(num), "D"(a0), "S"(a1), "d"(a2), "c"(a3), "r"(r8)
                     : "memory");
    return ret;
}

ssize_t write(int fd, const void* buf, size_t len) {
    return secos_syscall(SYS_WRITE, fd, (long)buf, (long)len, 0, 0);
}
void _exit(int code) { secos_syscall(SYS_EXIT, code, 0, 0, 0, 0); for (;;) {} }
int  getpid(void)    { return (int)secos_syscall(SYS_GETPID, 0, 0, 0, 0, 0); }
void sched_yield(void){ secos_syscall(SYS_YIELD, 0, 0, 0, 0, 0); }

size_t strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
int puts(const char* s) { write(1, s, strlen(s)); write(1, "\n", 1); return 0; }
