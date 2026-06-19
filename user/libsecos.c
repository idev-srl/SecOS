/* SecOS minimal user libc. SPDX-License-Identifier: MIT */
#include "libsecos.h"
#include "secos_driver.h"

#define SYS_YIELD    0
#define SYS_EXIT     1
#define SYS_WRITE    2
#define SYS_GETPID   6
#define SYS_DRIVER   7
#define SYS_SPAWN    8
#define SYS_WAIT     9
#define SYS_GETTICKS 10
#define SYS_MSG_SEND 11
#define SYS_MSG_RECV 12
#define SYS_SLEEP    13
#define SYS_MMAP     14
#define SYS_MUNMAP   15
#define SYS_BRK      16
#define SYS_MPROTECT 17
#define SYS_FORK     18
#define SYS_SOCKET   21
#define SYS_CONNECT  22
#define SYS_BIND     23
#define SYS_LISTEN   24
#define SYS_ACCEPT   25
#define SYS_SEND     26
#define SYS_RECV     27
#define SYS_SENDTO   28
#define SYS_RECVFROM 29
#define SYS_SOCKCLOSE 30

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
ssize_t read(int fd, void* buf, size_t len) {
    return secos_syscall(3 /*SYS_READ*/, fd, (long)buf, (long)len, 0, 0);
}
int open(const char* path, int flags) { return (int)secos_syscall(4 /*SYS_OPEN*/, (long)path, flags, 0, 0, 0); }
int close(int fd) { return (int)secos_syscall(5 /*SYS_CLOSE*/, fd, 0, 0, 0, 0); }
long lseek(int fd, long offset, int whence) { return secos_syscall(19 /*SYS_LSEEK*/, fd, offset, whence, 0, 0); }
int stat(const char* path, struct stat* st) { return (int)secos_syscall(20 /*SYS_STAT*/, (long)path, (long)st, 0, 0, 0); }
void _exit(int code) { secos_syscall(SYS_EXIT, code, 0, 0, 0, 0); for (;;) {} }
int  getpid(void)    { return (int)secos_syscall(SYS_GETPID, 0, 0, 0, 0, 0); }
void sched_yield(void){ secos_syscall(SYS_YIELD, 0, 0, 0, 0, 0); }
long secos_driver(driver_call_t* call){ return secos_syscall(SYS_DRIVER, (long)call, 0, 0, 0, 0); }

unsigned long getticks(void){ return (unsigned long)secos_syscall(SYS_GETTICKS, 0, 0, 0, 0, 0); }
long msg_send(int chan, const void* buf, long len){ return secos_syscall(SYS_MSG_SEND, chan, (long)buf, len, 0, 0); }
long msg_recv(int chan, void* buf, long len){ return secos_syscall(SYS_MSG_RECV, chan, (long)buf, len, 0, 0); }

/* [M16] spawn a signed child program from a VFS path with argv (NULL-terminated,
 * may be NULL). Returns the child pid (>0) or <0. */
int spawn(const char* path, char* const argv[]){ return (int)secos_syscall(SYS_SPAWN, (long)path, (long)argv, 0, 0, 0); }
/* [M16] block until child 'pid' exits; returns its status (0=normal, 128+vec=killed). */
int waitpid(int pid){ return (int)secos_syscall(SYS_WAIT, pid, 0, 0, 0, 0); }
/* [M17] block the caller for 'ticks' timer ticks. */
void sleep_ticks(unsigned ticks){ secos_syscall(SYS_SLEEP, (long)ticks, 0, 0, 0, 0); }

size_t strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
int puts(const char* s) { write(1, s, strlen(s)); write(1, "\n", 1); return 0; }

/* [M18] dynamic memory */
void* mmap(void* addr, size_t len, int prot, int flags){ return (void*)secos_syscall(SYS_MMAP,(long)addr,(long)len,prot,flags,-1); }
/* [M20] file-backed mmap (MAP_PRIVATE via the page cache), from file offset 0. */
void* mmap_file(void* addr, size_t len, int prot, int flags, int fd){ return (void*)secos_syscall(SYS_MMAP,(long)addr,(long)len,prot,flags,fd); }
int   munmap(void* addr, size_t len){ return (int)secos_syscall(SYS_MUNMAP,(long)addr,(long)len,0,0,0); }
int   mprotect(void* addr, size_t len, int prot){ return (int)secos_syscall(SYS_MPROTECT,(long)addr,(long)len,prot,0,0); }
unsigned long brk_set(unsigned long addr){ return (unsigned long)secos_syscall(SYS_BRK,(long)addr,0,0,0,0); }

void* sbrk(long incr){
    unsigned long old = brk_set(0);                 /* query current break */
    if (incr == 0) return (void*)old;
    unsigned long neu = old + (unsigned long)incr;
    unsigned long got = brk_set(neu);
    if (got < neu) return (void*)-1L;               /* grow failed */
    return (void*)old;
}

/* Minimal first-fit malloc on sbrk. Block header carries size + free flag; the
 * free list is singly linked. No coalescing/splitting yet — enough for ports to
 * allocate, and the free list proves reuse. */
typedef struct mblock { size_t size; struct mblock* next; int is_free; } mblock_t;
static mblock_t* g_heap = 0;

void* malloc(size_t size){
    if (!size) return 0;
    size = (size + 7UL) & ~7UL;                      /* 8-byte align */
    mblock_t* prev = 0; mblock_t* b = g_heap;
    while (b) { if (b->is_free && b->size >= size) { b->is_free = 0; return (void*)(b + 1); } prev = b; b = b->next; }
    mblock_t* nb = (mblock_t*)sbrk((long)(sizeof(mblock_t) + size));
    if ((long)nb == -1L) return 0;
    nb->size = size; nb->is_free = 0; nb->next = 0;
    if (prev) prev->next = nb; else g_heap = nb;
    return (void*)(nb + 1);
}
void free(void* p){ if (!p) return; mblock_t* b = (mblock_t*)p - 1; b->is_free = 1; }

/* [M19] copy-on-write fork: returns child pid in the parent, 0 in the child. */
int fork(void){ return (int)secos_syscall(SYS_FORK, 0, 0, 0, 0, 0); }

/* [M24] BSD-style sockets. Require CAP_NET in the signed manifest, else -1.
 * ip is network-order octets (octet0 in the low byte); port is host order. */
int socket(int type){ return (int)secos_syscall(SYS_SOCKET, type, 0, 0, 0, 0); }
int connect(int fd, unsigned int ip, unsigned short port){ return (int)secos_syscall(SYS_CONNECT, fd, (long)ip, port, 0, 0); }
int bind(int fd, unsigned short port){ return (int)secos_syscall(SYS_BIND, fd, port, 0, 0, 0); }
int listen(int fd, int backlog){ return (int)secos_syscall(SYS_LISTEN, fd, backlog, 0, 0, 0); }
int accept(int fd){ return (int)secos_syscall(SYS_ACCEPT, fd, 0, 0, 0, 0); }
long send(int fd, const void* buf, long len){ return secos_syscall(SYS_SEND, fd, (long)buf, len, 0, 0); }
long recv(int fd, void* buf, long len){ return secos_syscall(SYS_RECV, fd, (long)buf, len, 0, 0); }
long sendto(int fd, const void* buf, long len, const struct secos_sockaddr* sa){ return secos_syscall(SYS_SENDTO, fd, (long)buf, len, (long)sa, 0); }
long recvfrom(int fd, void* buf, long len, struct secos_sockaddr* sa){ return secos_syscall(SYS_RECVFROM, fd, (long)buf, len, (long)sa, 0); }
int sockclose(int fd){ return (int)secos_syscall(SYS_SOCKCLOSE, fd, 0, 0, 0, 0); }

/* host-order port <-> network helpers and a dotted-quad parser for demos. */
unsigned int ip4(unsigned a, unsigned b, unsigned c, unsigned d){
    return (a & 0xFF) | ((b & 0xFF) << 8) | ((c & 0xFF) << 16) | ((d & 0xFF) << 24);
}
