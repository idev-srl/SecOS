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

/* Socket descriptors live at SOCKFD_BASE + kernel sockfd, so they share one
 * namespace with file fds (< 32): read/write/close/poll work on them too. */
#define SOCKFD_BASE 64
#define IS_SOCKFD(fd) ((fd) >= SOCKFD_BASE && (fd) < SOCKFD_BASE + 64)
ssize_t write(int fd, const void* buf, size_t len) {
    if (IS_SOCKFD(fd)) return send(fd, buf, (long)len);
    return __se(secos_syscall(SYS_WRITE, fd, (long)buf, (long)len, 0, 0));
}
ssize_t read(int fd, void* buf, size_t len) {
    if (IS_SOCKFD(fd)) return recv(fd, buf, (long)len);
    return __se(secos_syscall(3 /*SYS_READ*/, fd, (long)buf, (long)len, 0, 0));
}
#include <stdarg.h>
int open(const char* path, int flags, ...) {
    unsigned mode = 0;
    if (flags & 0x40 /*O_CREAT*/) { va_list ap; va_start(ap, flags); mode = va_arg(ap, unsigned); va_end(ap); }
    return (int)__se(secos_syscall(4 /*SYS_OPEN*/, (long)path, flags, (long)mode, 0, 0));
}
int close(int fd) {
    if (IS_SOCKFD(fd)) return sockclose(fd);
    return (int)__se(secos_syscall(5 /*SYS_CLOSE*/, fd, 0, 0, 0, 0));
}
long lseek(int fd, long offset, int whence) { return __se(secos_syscall(19 /*SYS_LSEEK*/, fd, offset, whence, 0, 0)); }
static void stzero(struct stat* st){ char* p=(char*)st; for(unsigned i=0;i<sizeof(*st);i++) p[i]=0; }
int stat(const char* path, struct stat* st) { if(st) stzero(st); return (int)__se(secos_syscall(20 /*SYS_STAT*/, (long)path, (long)st, 0, 0, 0)); }
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
int kill(int pid, int sig){ return (int)__se(secos_syscall(SYS_KILL, pid, sig, 0, 0, 0)); }
int raise(int sig){ return (int)secos_syscall(SYS_KILL, getpid(), sig, 0, 0, 0); }
int sigprocmask(int how, const unsigned long* set, unsigned long* oldset){
    return (int)secos_syscall(SYS_SIGPROCMASK, how, set ? (long)*set : 0, (long)oldset, 0, 0);
}
int setpgid(int pid, int pgid){ return (int)__se(secos_syscall(SYS_SETPGID, pid, pgid, 0, 0, 0)); }
/* Job control: process-group queries (SYS_GETPGID 57) and group signalling.
 * Sessions (SYS_SETSID 85 / SYS_GETSID 86): setsid() starts a new session with
 * no controlling terminal (EPERM for a process-group leader, as POSIX says);
 * the first pty slave its leader opens without O_NOCTTY, or TIOCSCTTY,
 * becomes its controlling terminal. */
int getpgid(int pid){ return (int)__se(secos_syscall(57 /*SYS_GETPGID*/, pid, 0, 0, 0, 0)); }
int getpgrp(void){ return getpgid(0); }
/* Orderly shutdown (SYS_REBOOT 59): terminate all processes, sync + unmount
 * the filesystems, then power off / reset / halt. Returns only on failure. */
int secos_reboot(int cmd){ return (int)secos_syscall(59 /*SYS_REBOOT*/, cmd, 0, 0, 0, 0); }
int killpg(int pgrp, int sig){
    if(pgrp <= 1){ errno = EINVAL; return -1; }
    return kill(-pgrp, sig);
}
int setsid(void){ return (int)__se(secos_syscall(85 /*SYS_SETSID*/, 0, 0, 0, 0, 0)); }
int getsid(int pid){ return (int)__se(secos_syscall(86 /*SYS_GETSID*/, pid, 0, 0, 0, 0)); }
/* sigsuspend (SYS_SIGSUSPEND 58): atomically install 'mask' and block until a
 * signal is delivered; the previous mask is restored after the handler ran.
 * Always returns -1/EINTR, as POSIX requires. */
int sigsuspend(const unsigned long* mask){
    long r = secos_syscall(58 /*SYS_SIGSUSPEND*/, mask ? (long)*mask : 0, 0, 0, 0, 0);
    if(r >= 0) r = -EINTR;
    return (int)__se(r);
}

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
/* pipe2(fds, O_CLOEXEC): both ends close-on-exec. */
int pipe2(int fds[2], int flags){ return (int)__se(secos_syscall(31 /*SYS_PIPE*/, (long)fds, flags, 0, 0, 0)); }

/* [M26] VFS maturity: metadata + symlinks. */
int lstat(const char* path, struct stat* st){ if(st) stzero(st); return (int)__se(secos_syscall(37 /*SYS_LSTAT*/, (long)path, (long)st, 0, 0, 0)); }
int chmod(const char* path, unsigned mode){ return (int)secos_syscall(32 /*SYS_CHMOD*/, (long)path, (long)mode, 0, 0, 0); }
int chown(const char* path, unsigned uid, unsigned gid){ return (int)secos_syscall(33 /*SYS_CHOWN*/, (long)path, (long)uid, (long)gid, 0, 0); }
int utimes(const char* path, unsigned long atime, unsigned long mtime){ return (int)secos_syscall(34 /*SYS_UTIMES*/, (long)path, (long)atime, (long)mtime, 0, 0); }
long readlink(const char* path, char* buf, long len){ return secos_syscall(35 /*SYS_READLINK*/, (long)path, (long)buf, len, 0, 0); }
int symlink(const char* target, const char* linkpath){ return (int)secos_syscall(36 /*SYS_SYMLINK*/, (long)target, (long)linkpath, 0, 0, 0); }
int mount(const char* dev, const char* target, const char* fstype){ return (int)secos_syscall(38 /*SYS_MOUNT*/, (long)dev, (long)target, (long)fstype, 0, 0); }
int umount(const char* target){ return (int)secos_syscall(39 /*SYS_UMOUNT*/, (long)target, 0, 0, 0, 0); }

/* [M24] BSD-style sockets. Require CAP_NET in the signed manifest, else -1.
 * ip is network-order octets (octet0 in the low byte); port is host order. */
static long sk_in(int fd){ return fd >= SOCKFD_BASE ? fd - SOCKFD_BASE : -1; }
static int  sk_out(long r){ return r >= 0 ? (int)(r + SOCKFD_BASE) : (int)r; }
int socket(int type){ return sk_out(secos_syscall(SYS_SOCKET, type, 0, 0, 0, 0)); }
int connect(int fd, unsigned int ip, unsigned short port){ return (int)secos_syscall(SYS_CONNECT, sk_in(fd), (long)ip, port, 0, 0); }
int bind(int fd, unsigned short port){ return (int)secos_syscall(SYS_BIND, sk_in(fd), port, 0, 0, 0); }
int listen(int fd, int backlog){ return (int)secos_syscall(SYS_LISTEN, sk_in(fd), backlog, 0, 0, 0); }
int accept(int fd){ return sk_out(secos_syscall(SYS_ACCEPT, sk_in(fd), 0, 0, 0, 0)); }
long send(int fd, const void* buf, long len){ return secos_syscall(SYS_SEND, sk_in(fd), (long)buf, len, 0, 0); }
long recv(int fd, void* buf, long len){ return secos_syscall(SYS_RECV, sk_in(fd), (long)buf, len, 0, 0); }
long sendto(int fd, const void* buf, long len, const struct secos_sockaddr* sa){ return secos_syscall(SYS_SENDTO, sk_in(fd), (long)buf, len, (long)sa, 0); }
long recvfrom(int fd, void* buf, long len, struct secos_sockaddr* sa){ return secos_syscall(SYS_RECVFROM, sk_in(fd), (long)buf, len, (long)sa, 0); }
int sockclose(int fd){ return (int)secos_syscall(SYS_SOCKCLOSE, sk_in(fd), 0, 0, 0, 0); }

/* host-order port <-> network helpers and a dotted-quad parser for demos. */
unsigned int ip4(unsigned a, unsigned b, unsigned c, unsigned d){
    return (a & 0xFF) | ((b & 0xFF) << 8) | ((c & 0xFF) << 16) | ((d & 0xFF) << 24);
}

/* [M31] file management for a real userland. */
long getdents(const char* path, void* buf, long buflen){ return secos_syscall(40 /*SYS_GETDENTS*/, (long)path, (long)buf, buflen, 0, 0); }
int  creat_file(const char* path){ return (int)secos_syscall(41 /*SYS_CREATE*/, (long)path, 0, 0, 0, 0); }
int  mkdir(const char* path, unsigned mode){ return (int)__se(secos_syscall(42 /*SYS_MKDIR*/, (long)path, (long)mode, 0, 0, 0)); }
int  unlink(const char* path){ return (int)secos_syscall(43 /*SYS_UNLINK*/, (long)path, 0, 0, 0, 0); }
/* rmdir (SYS_RMDIR 74): empty directories only (ENOTDIR / ENOTEMPTY / EBUSY). */
int  rmdir(const char* path){ return (int)__se(secos_syscall(74 /*SYS_RMDIR*/, (long)path, 0, 0, 0, 0)); }
/* <stdio.h> remove: a file or an empty directory. */
int  remove(const char* path){
    struct stat st;
    if(lstat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) return rmdir(path);
    return unlink(path) == 0 ? 0 : -1;
}
/* rename (SYS_RENAME 71): atomic within one filesystem, replaces an existing
 * target; EXDEV across mounts (callers like mv then copy + unlink). */
int  rename(const char* from, const char* to){ return (int)__se(secos_syscall(71 /*SYS_RENAME*/, (long)from, (long)to, 0, 0, 0)); }

/* ---- kernel interfaces for userland tools (syscalls 65..79) ---- */
#include "include/time.h"
#include "include/sys/time.h"
#include "include/poll.h"
#include "include/sys/statfs.h"
#include "include/sys/utsname.h"
#include "include/sys/sysinfo.h"
#include "include/sys/klog.h"
int clock_gettime(clockid_t clk, struct timespec* ts){ return (int)__se(secos_syscall(65, clk, (long)ts, 0, 0, 0)); }
int clock_settime(clockid_t clk, const struct timespec* ts){ return (int)__se(secos_syscall(66, clk, (long)ts, 0, 0, 0)); }
int clock_getres(clockid_t clk, struct timespec* res){
    struct timespec t;
    if(clock_gettime(clk, &t) != 0) return -1;              /* validates clk */
    if(res){ res->tv_sec = 0; res->tv_nsec = (clk == CLOCK_REALTIME_COARSE || clk == CLOCK_MONOTONIC_COARSE ||
                                               clk == CLOCK_PROCESS_CPUTIME_ID || clk == CLOCK_THREAD_CPUTIME_ID) ? 1000000 : 1; }
    return 0;
}
int nanosleep(const struct timespec* req, struct timespec* rem){ return (int)__se(secos_syscall(67, (long)req, (long)rem, 0, 0, 0)); }
int klogctl(int type, char* buf, int len){ return (int)__se(secos_syscall(68, type, (long)buf, len, 0, 0)); }
int statfs(const char* path, struct statfs* b){ return (int)__se(secos_syscall(69, (long)path, (long)b, 0, 0, 0)); }
int fstatfs(int fd, struct statfs* b){ return (int)__se(secos_syscall(69, 0, (long)b, fd, 0, 0)); }
void sync(void){ secos_syscall(70, -1, 0, 0, 0, 0); }
int syncfs(int fd){ (void)fd; sync(); return 0; }
int fsync(int fd){ return (int)__se(secos_syscall(70, fd, 0, 0, 0, 0)); }
int fdatasync(int fd){ return fsync(fd); }
int link(const char* oldpath, const char* newpath){ return (int)__se(secos_syscall(72, (long)oldpath, (long)newpath, 0, 0, 0)); }
int truncate(const char* path, long len){ return (int)__se(secos_syscall(73, (long)path, len, 0, 0, 0)); }
int ftruncate(int fd, long len){ return (int)__se(secos_syscall(73, 0, len, fd, 0, 0)); }
int access(const char* path, int mode){ return (int)__se(secos_syscall(75, (long)path, mode, 0, 0, 0)); }
unsigned umask(unsigned m){ return (unsigned)secos_syscall(76, (long)(m & 0777), 0, 0, 0, 0); }
int uname(struct utsname* u){ return (int)__se(secos_syscall(77, (long)u, 0, 0, 0, 0)); }
int poll(struct pollfd* fds, nfds_t n, int timeout){ return (int)__se(secos_syscall(78, (long)fds, (long)n, timeout, 0, 0)); }
int sysinfo(struct sysinfo* si){ return (int)__se(secos_syscall(79, (long)si, 0, 0, 0, 0)); }

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
    return (int)__se(secos_syscall(53 /*SYS_IOCTL*/, fd, (long)request, (long)arg, 0, 0));
}
int getppid(void){ return (int)secos_syscall(54 /*SYS_GETPPID*/, 0, 0, 0, 0, 0); }

/* termios over ioctl. struct termios layout must match the kernel's. */
int tcgetattr(int fd, struct termios* t){ return ioctl(fd, TCGETS, t); }
int tcsetattr(int fd, int how, const struct termios* t){
    unsigned long req = how == TCSADRAIN ? TCSETSW : how == TCSAFLUSH ? TCSETSF : TCSETS;
    return ioctl(fd, req, (void*)t);
}
void cfmakeraw(struct termios* t){
    if(!t) return;
    t->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(CSIZE | PARENB);
    t->c_cflag |= CS8;
    t->c_cc[VMIN] = 1; t->c_cc[VTIME] = 0;
}
speed_t cfgetospeed(const struct termios* t){ return t->c_cflag & CBAUD; }
speed_t cfgetispeed(const struct termios* t){ return t->c_cflag & CBAUD; }
int cfsetospeed(struct termios* t, speed_t s){ if(s & ~CBAUD) return -1; t->c_cflag = (t->c_cflag & ~CBAUD) | s; return 0; }
int cfsetispeed(struct termios* t, speed_t s){ return s ? cfsetospeed(t, s) : 0; }
int cfsetspeed(struct termios* t, speed_t s){ return cfsetospeed(t, s); }
int tcflush(int fd, int queue){ return ioctl(fd, TCFLSH, (void*)(long)queue); }
int tcdrain(int fd){ return ioctl(fd, TCSBRK, (void*)1L); }
int tcflow(int fd, int action){ return ioctl(fd, TCXONC, (void*)(long)action); }
int tcsendbreak(int fd, int duration){ (void)duration; return ioctl(fd, TCSBRK, (void*)0L); }
int tcgetsid(int fd){ int sid = -1; if(ioctl(fd, TIOCGSID, &sid) < 0) return -1; return sid; }
int tcgetpgrp(int fd){ int pg = -1; ioctl(fd, 0x540F /*TIOCGPGRP*/, &pg); return pg; }
int tcsetpgrp(int fd, int pgrp){ return ioctl(fd, 0x5410 /*TIOCSPGRP*/, &pgrp); }

/* execve: the kernel replaces this process's image in place (SYS_EXECVE). pid,
 * parent, process group, cwd and every fd not marked FD_CLOEXEC survive; caught
 * signals revert to SIG_DFL. Returns only on failure, with errno set (ENOENT,
 * EACCES: unsigned/tampered or not a regular file, ENOEXEC: not an ELF, E2BIG,
 * EFAULT, ENOMEM) and the calling image untouched. */
extern char** environ;
char* getenv(const char* name);
int execve(const char* path, char* const argv[], char* const envp[]){
    return (int)__se(secos_syscall(60 /*SYS_EXECVE*/, (long)path, (long)argv, (long)envp, 0, 0));
}
int execv(const char* path, char* const argv[]){ return execve(path, argv, environ); }
/* PATH search (default "/bin:/usr/bin"). A name with a '/' is used as is. Keeps
 * searching past ENOENT/ENOTDIR; reports EACCES if some candidate was refused
 * and nothing ran (POSIX execvp). */
int execvpe(const char* file, char* const argv[], char* const envp[]){
    if(!file || !*file){ errno = ENOENT; return -1; }
    for(const char* p=file; *p; p++) if(*p=='/') return execve(file, argv, envp);
    const char* path = getenv("PATH");
    if(!path || !*path) path = "/bin:/usr/bin";
    int saw_eacces = 0;
    char buf[256];
    while(1){
        const char* e = path; while(*e && *e != ':') e++;
        int k = 0;
        if(e == path) buf[k++] = '.';                      /* empty element = cwd */
        for(const char* q=path; q<e && k<200; q++) buf[k++] = *q;
        if(k < 254) buf[k++] = '/';
        for(const char* q=file; *q && k<255; q++) buf[k++] = *q;
        buf[k] = 0;
        execve(buf, argv, envp);                           /* returns only on failure */
        if(errno == EACCES) saw_eacces = 1;
        else if(errno != ENOENT && errno != ENOTDIR) return -1;   /* real error: stop */
        if(!*e) break;
        path = e + 1;
    }
    errno = saw_eacces ? EACCES : ENOENT;
    return -1;
}
int execvp(const char* file, char* const argv[]){ return execvpe(file, argv, environ); }
