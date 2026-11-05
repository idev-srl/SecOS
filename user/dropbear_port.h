/* SecOS - force-included shim for the Dropbear SSH port (tools/port-dropbear.sh).
 * Bridges Dropbear's POSIX assumptions and the SecOS libc. Implementations live
 * in user/dropbear_compat.c. Shims that are generic POSIX (not Dropbear
 * specific) are marked TODO(integration): move to libc.
 * SPDX-License-Identifier: MIT */
#ifndef SECOS_DROPBEAR_PORT_H
#define SECOS_DROPBEAR_PORT_H

/* Drops the non-POSIX 2-arg open()/1-arg waitpid() prototypes from libsecos.h
 * (the bash port uses the same switch); the POSIX ones are declared below. */
#define SECOS_BASH_PORT 1
#ifndef _POSIX_VERSION
#define _POSIX_VERSION 200809L
#endif

/* libsecos.h declares SecOS-native socket(type)/bind(fd,port)/... with
 * non-POSIX signatures. Rename them away while it is included; the POSIX API
 * comes from user/dropbear-include/sys/socket.h (db_* in the compat file). */
#include <sys/wait.h>     /* before libsecos.h: its W* macros are #ifndef-guarded */
#define socket   secos_native_socket
#define connect  secos_native_connect
#define bind     secos_native_bind
#define listen   secos_native_listen
#define accept   secos_native_accept
#define send     secos_native_send
#define recv     secos_native_recv
#define sendto   secos_native_sendto
#define recvfrom secos_native_recvfrom
#include "libsecos.h"
#undef socket
#undef connect
#undef bind
#undef listen
#undef accept
#undef send
#undef recv
#undef sendto
#undef recvfrom

#include <stdint.h>
#include <stdarg.h>
#include <sys/types.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <sys/stat.h>

/* ---- types ---- */
typedef unsigned long sigset_t;
typedef unsigned int  id_t;
typedef long          suseconds_t;
typedef unsigned long dev_t;
typedef unsigned long ino_t;
typedef unsigned long nlink_t;
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;

/* ---- scatter/gather I/O: writev() is a write() loop in the compat file ---- */
struct iovec { void* iov_base; size_t iov_len; };
#define IOV_MAX 1024
#define UIO_MAXIOV IOV_MAX
ssize_t writev(int fd, const struct iovec* iov, int cnt);
ssize_t readv(int fd, const struct iovec* iov, int cnt);
#ifndef _STRUCT_TIMESPEC
#define _STRUCT_TIMESPEC
struct timespec { long tv_sec; long tv_nsec; };
#endif
#ifndef CLOCK_MONOTONIC        /* older libc <time.h>: no struct tm / clocks */
#define DB_NEED_TIME_DECLS 1
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff; const char* tm_zone;
};
#endif

/* ---- errno values (Linux numbering, as the kernel returns -errno) ---- */
#ifndef ENXIO
#define ENXIO 6
#endif
#ifndef E2BIG
#define E2BIG 7
#endif
#ifndef ENOEXEC
#define ENOEXEC 8
#endif
#ifndef EBUSY
#define EBUSY 16
#endif
#ifndef ENODEV
#define ENODEV 19
#endif
#ifndef EXDEV
#define EXDEV 18
#endif
#ifndef ENFILE
#define ENFILE 23
#endif
#ifndef ESPIPE
#define ESPIPE 29
#endif
#ifndef EROFS
#define EROFS 30
#endif
#ifndef EDOM
#define EDOM 33
#endif
#ifndef ENAMETOOLONG
#define ENAMETOOLONG 36
#endif
#ifndef ENOTEMPTY
#define ENOTEMPTY 39
#endif
#ifndef ELOOP
#define ELOOP 40
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
#ifndef ENOTSOCK
#define ENOTSOCK 88
#endif
#ifndef EDESTADDRREQ
#define EDESTADDRREQ 89
#endif
#ifndef EMSGSIZE
#define EMSGSIZE 90
#endif
#ifndef EPROTOTYPE
#define EPROTOTYPE 91
#endif
#ifndef ENOPROTOOPT
#define ENOPROTOOPT 92
#endif
#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT 93
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 95
#endif
#ifndef ENOTSUP
#define ENOTSUP EOPNOTSUPP
#endif
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT 97
#endif
#ifndef EADDRINUSE
#define EADDRINUSE 98
#endif
#ifndef EADDRNOTAVAIL
#define EADDRNOTAVAIL 99
#endif
#ifndef ENETDOWN
#define ENETDOWN 100
#endif
#ifndef ENETUNREACH
#define ENETUNREACH 101
#endif
#ifndef ECONNABORTED
#define ECONNABORTED 103
#endif
#ifndef ECONNRESET
#define ECONNRESET 104
#endif
#ifndef ENOBUFS
#define ENOBUFS 105
#endif
#ifndef EISCONN
#define EISCONN 106
#endif
#ifndef ENOTCONN
#define ENOTCONN 107
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED 111
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH 113
#endif
#ifndef EALREADY
#define EALREADY 114
#endif
#ifndef EINPROGRESS
#define EINPROGRESS 115
#endif

/* ---- file modes / open flags ---- */
#ifndef S_IRUSR
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#endif
#ifndef S_IFIFO
#define S_IFIFO  0x1000
#define S_IFSOCK 0xC000
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(m)  (((m) & 0xF000) == S_IFIFO)
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(m)  (((m) & 0xF000) == S_IFSOCK)
#endif
#ifndef F_OK
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#endif
#ifndef O_EXCL
#define O_EXCL   0x80
#define O_NOCTTY 0x100
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x800
#endif
#define O_NDELAY O_NONBLOCK
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x20000
#endif
#define SOL_TCP 6

/* ---- O_NONBLOCK in user space ----
 * Dropbear drains its signal self-pipe with `while (read(p, &x, 1) > 0)` and
 * relies on O_NONBLOCK everywhere. The SecOS kernel does not honour O_NONBLOCK
 * on pipes yet (and has no fcntl syscall before SYS_FCNTL), so a read on an
 * empty pipe blocks forever. db_fcntl() records O_NONBLOCK per fd (and still
 * forwards to the libc fcntl); db_read()/db_write() on such an fd first
 * poll() with a zero timeout and return EAGAIN when it is not ready.
 * TODO(integration): drop once the kernel returns -EAGAIN for O_NONBLOCK pipes. */
int     db_fcntl(int fd, int cmd, ...);
ssize_t db_read(int fd, void* buf, size_t n);
ssize_t db_write(int fd, const void* buf, size_t n);
#ifdef DB_PORT_IO_WRAP
#define fcntl(...)    db_fcntl(__VA_ARGS__)
#define read(f,b,n)   db_read(f,b,n)
#define write(f,b,n)  db_write(f,b,n)
#endif

/* ---- POSIX functions with SecOS-incompatible prototypes ---- */
int open(const char* path, int flags, ...);
int db_open(const char* path, int flags, ...);          /* O_EXCL/O_NOFOLLOW/mode */
#define open(...) db_open(__VA_ARGS__)
int db_waitpid(pid_t pid, int* status, int options);   /* waitpid(pid,&st,opt) */
#define waitpid(p,s,o) db_waitpid(p,s,o)
/* Dropbear's non-multiuser mode requires getgroups() to fail with ENOSYS
 * (SecOS has no users/groups); see svr-runopts/common-session. */
int db_getgroups(int n, int* g);
#define getgroups(n,g) db_getgroups(n,g)

/* ---- signals ---- */
struct sigaction {
    void (*sa_handler)(int);
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};
#define SA_NOCLDSTOP 0x00000001
#define SA_SIGINFO   0x00000004
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000u
#define SIGWINCH 28
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGIO    29
#define SIGPWR   30
#define SIGSYS   31
int sigaction(int sig, const struct sigaction* act, struct sigaction* old);
int sigemptyset(sigset_t* s);
int sigfillset(sigset_t* s);
int sigaddset(sigset_t* s, int n);
int sigdelset(sigset_t* s, int n);
int sigismember(const sigset_t* s, int n);

/* ---- time (TODO(integration): move to libc) ---- */
#ifdef DB_NEED_TIME_DECLS
typedef int clockid_t;
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define CLOCK_BOOTTIME  7
#endif
int clock_gettime(clockid_t id, struct timespec* ts);
struct tm* gmtime(const time_t* t);
struct tm* localtime(const time_t* t);
struct tm* gmtime_r(const time_t* t, struct tm* out);
struct tm* localtime_r(const time_t* t, struct tm* out);
size_t strftime(char* s, size_t max, const char* fmt, const struct tm* tm);
time_t mktime(struct tm* tm);
unsigned int alarm(unsigned int s);
int nanosleep(const struct timespec* req, struct timespec* rem);

/* ---- misc POSIX (TODO(integration): move to libc) ---- */
struct stat;
int   fstat(int fd, struct stat* st);
int   fchmod(int fd, mode_t mode);
int   fchown(int fd, uid_t uid, gid_t gid);
int   fsync(int fd);
int   access(const char* path, int mode);
int   link(const char* oldp, const char* newp);
int   ftruncate(int fd, off_t len);
int   fileno(FILE* f);
char* strsignal(int sig);
int   sscanf(const char* s, const char* fmt, ...);   /* %d %u %x [width] only */
char* ttyname(int fd);
int   gethostname(char* name, size_t len);
long  sysconf(int name);
#define _SC_OPEN_MAX 4
#define _SC_CLK_TCK  2
#define _SC_PAGESIZE 30
int   getpagesize(void);
int   setegid(gid_t g);
int   seteuid(uid_t u);
int   initgroups(const char* user, gid_t g);
pid_t vfork(void);
int   execl(const char* path, const char* arg, ...);
char* crypt(const char* key, const char* salt);
char* getusershell(void);
void  setusershell(void);
void  endusershell(void);
int   setvbuf(FILE* f, char* buf, int mode, size_t size);
#ifndef _IOFBF
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#endif
void  clearerr(FILE* f);
int   strncasecmp(const char* a, const char* b, size_t n);
int   strcasecmp(const char* a, const char* b);
char* strerror(int e);
void* memmem(const void* h, size_t hl, const void* n, size_t nl);
size_t strlcat(char* d, const char* s, size_t n);
char* strsep(char** sp, const char* delim);
char* strndup(const char* s, size_t n);
long  random(void);
void  srandom(unsigned int seed);
int   isascii(int c);
int   setpgrp(void);
int   tcsendbreak(int fd, int dur);

/* ---- termios: Linux values. A <termios.h> that already has the full Linux
 * set (feat/pty) wins; each name is only defined when missing. ---- */
#ifndef VINTR
#define VINTR 0
#endif
#ifndef VQUIT
#define VQUIT 1
#endif
#ifndef VERASE
#define VERASE 2
#endif
#ifndef VKILL
#define VKILL 3
#endif
#ifndef VEOF
#define VEOF 4
#endif
#ifndef VSWTC
#define VSWTC 7
#endif
#ifndef VSTART
#define VSTART 8
#endif
#ifndef VSTOP
#define VSTOP 9
#endif
#ifndef VSUSP
#define VSUSP 10
#endif
#ifndef VEOL
#define VEOL 11
#endif
#ifndef VREPRINT
#define VREPRINT 12
#endif
#ifndef VDISCARD
#define VDISCARD 13
#endif
#ifndef VWERASE
#define VWERASE 14
#endif
#ifndef VLNEXT
#define VLNEXT 15
#endif
#ifndef VEOL2
#define VEOL2 16
#endif
#ifndef IGNBRK
#define IGNBRK 0000001
#endif
#ifndef BRKINT
#define BRKINT 0000002
#endif
#ifndef IGNPAR
#define IGNPAR 0000004
#endif
#ifndef PARMRK
#define PARMRK 0000010
#endif
#ifndef INPCK
#define INPCK  0000020
#endif
#ifndef ISTRIP
#define ISTRIP 0000040
#endif
#ifndef INLCR
#define INLCR  0000100
#endif
#ifndef IGNCR
#define IGNCR  0000200
#endif
#ifndef ICRNL
#define ICRNL  0000400
#endif
#ifndef IUCLC
#define IUCLC  0001000
#endif
#ifndef IXON
#define IXON   0002000
#endif
#ifndef IXANY
#define IXANY  0004000
#endif
#ifndef IXOFF
#define IXOFF  0010000
#endif
#ifndef IMAXBEL
#define IMAXBEL 0020000
#endif
#ifndef IUTF8
#define IUTF8  0040000
#endif
#ifndef OPOST
#define OPOST  0000001
#endif
#ifndef OLCUC
#define OLCUC  0000002
#endif
#ifndef ONLCR
#define ONLCR  0000004
#endif
#ifndef OCRNL
#define OCRNL  0000010
#endif
#ifndef ONOCR
#define ONOCR  0000020
#endif
#ifndef ONLRET
#define ONLRET 0000040
#endif
#ifndef CS7
#define CS7    0000040
#endif
#ifndef CS8
#define CS8    0000060
#endif
#ifndef CSIZE
#define CSIZE  0000060
#endif
#ifndef PARENB
#define PARENB 0000400
#endif
#ifndef PARODD
#define PARODD 0001000
#endif
#ifndef ECHOE
#define ECHOE  0000020
#endif
#ifndef ECHOK
#define ECHOK  0000040
#endif
#ifndef ECHONL
#define ECHONL 0000100
#endif
#ifndef NOFLSH
#define NOFLSH 0000200
#endif
#ifndef TOSTOP
#define TOSTOP 0000400
#endif
#ifndef ECHOCTL
#define ECHOCTL 0001000
#endif
#ifndef ECHOKE
#define ECHOKE 0004000
#endif
#ifndef IEXTEN
#define IEXTEN 0100000
#endif
#ifndef PENDIN
#define PENDIN 0040000
#endif
#ifndef XCASE
#define XCASE  0000004
#endif
typedef unsigned int speed_t;
speed_t cfgetospeed(const struct termios* t);
speed_t cfgetispeed(const struct termios* t);
int cfsetospeed(struct termios* t, speed_t s);
int cfsetispeed(struct termios* t, speed_t s);

/* PTY interface (kernel side pending: see docs/SSH.md). Until /dev/ptmx exists
 * these fail with ENOSYS/ENOENT and Dropbear refuses the pty-req cleanly. */
int   posix_openpt(int flags);
int   grantpt(int fd);
int   unlockpt(int fd);
char* ptsname(int fd);
#define TIOCSCTTY  0x540E
#define TIOCNOTTY  0x5422
#define TIOCSWINSZ 0x5414

#ifndef _PATH_TTY
#define _PATH_TTY "/dev/tty"
#endif
#ifndef _PATH_DEVNULL
#define _PATH_DEVNULL "/dev/null"
#endif

#endif /* SECOS_DROPBEAR_PORT_H */
