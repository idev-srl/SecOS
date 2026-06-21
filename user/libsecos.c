/* SecOS minimal user libc. SPDX-License-Identifier: MIT */
#include "libsecos.h"
#include "secos_driver.h"
#include "include/termios.h"   /* [M39] struct termios for tcgetattr/tcsetattr */
#include "include/errno.h"     /* [M39] errno mapping for source ports (bash, …) */

/* [M39] Map a negative syscall return to errno and normalize to -1. The kernel
 * returns -errno for known errors (e.g. -ENOENT=-2, -EINTR=-4, -ECHILD=-10) and
 * -1 generically; bash and other ports branch on errno, so set it. */
static long __se(long r){
    if (r < 0) { errno = (r > -134) ? (int)(-r) : EIO; return -1; }
    return r;
}

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
#define SYS_SIGACTION   44
#define SYS_SIGRETURN   45
#define SYS_KILL        46
#define SYS_SIGPROCMASK 47
#define SYS_SETPGID     48

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
    return __se(secos_syscall(SYS_WRITE, fd, (long)buf, (long)len, 0, 0));
}
ssize_t read(int fd, void* buf, size_t len) {
    return __se(secos_syscall(3 /*SYS_READ*/, fd, (long)buf, (long)len, 0, 0));
}
int open(const char* path, int flags) { return (int)__se(secos_syscall(4 /*SYS_OPEN*/, (long)path, flags, 0, 0, 0)); }
int close(int fd) { return (int)__se(secos_syscall(5 /*SYS_CLOSE*/, fd, 0, 0, 0, 0)); }
long lseek(int fd, long offset, int whence) { return __se(secos_syscall(19 /*SYS_LSEEK*/, fd, offset, whence, 0, 0)); }
static void stzero(struct stat* st){ char* p=(char*)st; for(unsigned i=0;i<sizeof(*st);i++) p[i]=0; }
int stat(const char* path, struct stat* st) { if(st) stzero(st); return (int)secos_syscall(20 /*SYS_STAT*/, (long)path, (long)st, 0, 0, 0); }
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

/* [M30] Signals. signal() installs 'handler' for 'sig' and registers the libc
 * sigreturn trampoline (__sigreturn, in crt0.S) as the handler's return address.
 * SIG_DFL(0)/SIG_IGN(1) select the default/ignore dispositions. Returns 0 / -1. */
extern void __sigreturn(void);
sighandler_t signal(int sig, sighandler_t handler){
    long r = secos_syscall(SYS_SIGACTION, sig, (long)handler, (long)__sigreturn, 0, 0);
    return (r == 0) ? handler : (sighandler_t)(long)-1; /* SIG_ERR */
}
int kill(int pid, int sig){ return (int)secos_syscall(SYS_KILL, pid, sig, 0, 0, 0); }
int raise(int sig){ return (int)secos_syscall(SYS_KILL, getpid(), sig, 0, 0, 0); }
int sigprocmask(int how, const unsigned long* set, unsigned long* oldset){
    return (int)secos_syscall(SYS_SIGPROCMASK, how, set ? (long)*set : 0, (long)oldset, 0, 0);
}
int setpgid(int pid, int pgid){ return (int)secos_syscall(SYS_SETPGID, pid, pgid, 0, 0, 0); }

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

/* malloc/free/calloc/realloc live in libc.c (the full C library) so realloc can
 * read the block header. libc.o is linked into every user program. */

/* [M19] copy-on-write fork: returns child pid in the parent, 0 in the child. */
int fork(void){ return (int)secos_syscall(SYS_FORK, 0, 0, 0, 0, 0); }

/* [M25] anonymous pipe: fds[0]=read end, fds[1]=write end. Returns 0 or -1. */
int pipe(int fds[2]){ return (int)secos_syscall(31 /*SYS_PIPE*/, (long)fds, 0, 0, 0, 0); }

/* [M26] VFS maturity: metadata + symlinks. */
int lstat(const char* path, struct stat* st){ if(st) stzero(st); return (int)secos_syscall(37 /*SYS_LSTAT*/, (long)path, (long)st, 0, 0, 0); }
int chmod(const char* path, unsigned mode){ return (int)secos_syscall(32 /*SYS_CHMOD*/, (long)path, (long)mode, 0, 0, 0); }
int chown(const char* path, unsigned uid, unsigned gid){ return (int)secos_syscall(33 /*SYS_CHOWN*/, (long)path, (long)uid, (long)gid, 0, 0); }
int utimes(const char* path, unsigned long atime, unsigned long mtime){ return (int)secos_syscall(34 /*SYS_UTIMES*/, (long)path, (long)atime, (long)mtime, 0, 0); }
long readlink(const char* path, char* buf, long len){ return secos_syscall(35 /*SYS_READLINK*/, (long)path, (long)buf, len, 0, 0); }
int symlink(const char* target, const char* linkpath){ return (int)secos_syscall(36 /*SYS_SYMLINK*/, (long)target, (long)linkpath, 0, 0, 0); }
int mount(const char* dev, const char* target, const char* fstype){ return (int)secos_syscall(38 /*SYS_MOUNT*/, (long)dev, (long)target, (long)fstype, 0, 0); }
int umount(const char* target){ return (int)secos_syscall(39 /*SYS_UMOUNT*/, (long)target, 0, 0, 0, 0); }

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

/* [M31] file management for a real userland. */
long getdents(const char* path, void* buf, long buflen){ return secos_syscall(40 /*SYS_GETDENTS*/, (long)path, (long)buf, buflen, 0, 0); }
int  creat_file(const char* path){ return (int)secos_syscall(41 /*SYS_CREATE*/, (long)path, 0, 0, 0, 0); }
int  mkdir(const char* path, unsigned mode){ (void)mode; return (int)secos_syscall(42 /*SYS_MKDIR*/, (long)path, 0, 0, 0, 0); }
int  unlink(const char* path){ return (int)secos_syscall(43 /*SYS_UNLINK*/, (long)path, 0, 0, 0, 0); }
int  rmdir(const char* path){ return unlink(path); }

/* [M39] POSIX shell-from-source foundation: fd duplication, cwd, terminal ioctl,
 * exec. These bring the libc surface close to what a real shell (bash/dash) and
 * many open-source programs expect when compiled from source. */
int dup2(int oldfd, int newfd){ return (int)secos_syscall(49 /*SYS_DUP2*/, oldfd, newfd, 0, 0, 0); }
int dup(int oldfd){ return (int)secos_syscall(50 /*SYS_DUP*/, oldfd, 0, 0, 0, 0); }
int chdir(const char* path){ return (int)secos_syscall(51 /*SYS_CHDIR*/, (long)path, 0, 0, 0, 0); }
char* getcwd(char* buf, unsigned long size){
    char tmp[1024];
    long n = secos_syscall(52 /*SYS_GETCWD*/, (long)tmp, (long)sizeof(tmp), 0, 0, 0);
    if(n < 0) return 0;
    if(!buf){ buf = (char*)malloc((unsigned long)n + 1); if(!buf) return 0; }  /* GNU malloc form */
    else if(size && (unsigned long)n + 1 > size){ errno = ERANGE; return 0; }
    for(long i = 0; i <= n; i++) buf[i] = tmp[i];
    return buf;
}
int ioctl(int fd, unsigned long request, void* arg){
    return (int)secos_syscall(53 /*SYS_IOCTL*/, fd, (long)request, (long)arg, 0, 0);
}
int getppid(void){ return (int)secos_syscall(54 /*SYS_GETPPID*/, 0, 0, 0, 0, 0); }

/* termios over ioctl. struct termios layout must match the kernel's. */
int tcgetattr(int fd, struct termios* t){ return ioctl(fd, 0x5401 /*TCGETS*/, t); }
int tcsetattr(int fd, int how, const struct termios* t){ (void)how; return ioctl(fd, 0x5402 /*TCSETS*/, (void*)t); }
void cfmakeraw(struct termios* t){
    if(!t) return;
    t->c_iflag = 0; t->c_oflag = 0;
    t->c_lflag &= ~(0x0002u /*ICANON*/ | 0x0008u /*ECHO*/ | 0x0001u /*ISIG*/);
    t->c_cc[4] = 1; /*VMIN*/ t->c_cc[5] = 0; /*VTIME*/
}
int tcgetpgrp(int fd){ int pg = -1; ioctl(fd, 0x540F /*TIOCGPGRP*/, &pg); return pg; }
int tcsetpgrp(int fd, int pgrp){ return ioctl(fd, 0x5410 /*TIOCSPGRP*/, &pgrp); }

/* execve emulated via spawn+wait+exit: run the signed program and exit with its
 * status. Semantically matches the shell fork()+execve() pattern (the parent's
 * waitpid on the forked child gets the command's status). The command runs as a
 * fresh signed process (no in-place image replacement), so envp is not inherited
 * across the boundary yet — a documented limitation of the emulation. */
extern char** environ;
int execve(const char* path, char* const argv[], char* const envp[]){
    (void)envp;
    int pid = spawn(path, (char* const*)argv);
    if(pid < 0) return -1;
    int st = waitpid(pid);
    _exit(st & 0xff);
    return -1; /* unreached */
}
int execv(const char* path, char* const argv[]){ return execve(path, argv, environ); }
int execvp(const char* file, char* const argv[]){
    int has_slash = 0; for(const char* p=file; *p; p++) if(*p=='/'){ has_slash=1; break; }
    if(has_slash) return execve(file, argv, environ);
    char buf[160]; const char* dirs[] = { "/bin/", "/usr/bin/", 0 };
    for(int d=0; dirs[d]; d++){
        int k=0; for(const char* p=dirs[d]; *p && k<159; p++) buf[k++]=*p;
        for(const char* p=file; *p && k<159; p++) buf[k++]=*p; buf[k]=0;
        execve(buf, argv, environ); /* returns only on failure */
    }
    return -1;
}
