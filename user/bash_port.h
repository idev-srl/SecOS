/* SecOS - force-included shim for the bash port. Bridges the gaps between bash's
 * POSIX assumptions and SecOS's libc. SPDX-License-Identifier: MIT */
#ifndef SECOS_BASH_PORT_H
#define SECOS_BASH_PORT_H
#define SECOS_BASH_PORT 1
/* SecOS is POSIX-ish: make bash take the POSIX code paths (WAIT=int not union
 * wait, termios TTY driver not sgtty). Must be set before any bash header. */
#ifndef _POSIX_VERSION
#define _POSIX_VERSION 200809L
#endif
#ifndef TERMIOS_TTY_DRIVER
#define TERMIOS_TTY_DRIVER 1
#endif
#include <sys/types.h>
#include <time.h>

typedef unsigned long sigset_t;
#ifndef _STRUCT_TIMESPEC
#define _STRUCT_TIMESPEC
struct timespec { long tv_sec; long tv_nsec; };
#endif
struct itimerval { struct { long tv_sec, tv_usec; } it_interval, it_value; };
typedef long imaxdiv_t_lo; typedef struct { long quot; long rem; } imaxdiv_t;

/* errno values bash references that SecOS errno.h may lack */
#ifndef EINTR
#define EINTR 4
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef ESPIPE
#define ESPIPE 29
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef ENOEXEC
#define ENOEXEC 8
#endif
#ifndef EEXIST
#define EEXIST 17
#endif
#ifndef ENOTDIR
#define ENOTDIR 20
#endif
#ifndef EISDIR
#define EISDIR 21
#endif
#ifndef ERANGE
#define ERANGE 34
#endif

/* stdio buffering modes */
#ifndef _IOFBF
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#endif

/* access() modes + file mode bits bash's test/stat code needs */
#ifndef F_OK
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#endif
#ifndef S_ISUID
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#endif
#ifndef S_IRUSR
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_IRWXU 0700
#define S_IRWXG 0070
#define S_IRWXO 0007
#endif
#ifndef USEC_PER_SEC
#define USEC_PER_SEC 1000000
#endif

/* POSIX struct sigaction (SecOS libc has signal(), not sigaction) */
struct sigaction {
    void (*sa_handler)(int);
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};
#ifndef SA_RESTART
#define SA_RESTART 0x10000000
#define SA_SIGINFO 0x00000004
#define SIG_SETMASK 2
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#endif
int sigaction(int, const struct sigaction*, struct sigaction*);
int sigemptyset(sigset_t*);
int sigfillset(sigset_t*);
int sigaddset(sigset_t*, int);
int sigdelset(sigset_t*, int);
int sigismember(const sigset_t*, int);
int sigsuspend(const sigset_t*);

/* POSIX waitpid(pid,status,opt) — SecOS libc's waitpid is the 1-arg variant, so
 * remap to the bash shim (and the libsecos decl is guarded out below). */
int bash_waitpid(pid_t pid, int* status, int options);
#define waitpid bash_waitpid
/* variadic open() so bash's 3-arg open(path,flags,mode) compiles; the libsecos
 * 2-arg impl ignores the extra mode arg (ABI-compatible on x86-64 SysV). The
 * libsecos 2-arg decl is guarded out for the bash build. */
int open(const char* path, int flags, ...);

/* misc POSIX bits bash calls that no SecOS header declares (getcwd/pipe/isatty/
 * gettimeofday are provided by unistd.h/libsecos.h/sys/time.h — don't redeclare). */
int access(const char* path, int mode);
int setitimer(int, const struct itimerval*, struct itimerval*);
int getitimer(int, struct itimerval*);
unsigned int alarm(unsigned int);
long sysconf(int);
char* ttyname(int);
imaxdiv_t imaxdiv(long, long);
#ifndef MB_CUR_MAX
#define MB_CUR_MAX 1
#endif
#ifndef O_EXCL
#define O_EXCL 0x80
#define O_NOCTTY 0x100
#define O_SYNC 0x101000
#endif
#ifndef ITIMER_REAL
#define ITIMER_REAL 0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF 2
#endif
#ifndef E2BIG
#define E2BIG 7
#define ECHILD 10
#define ENOMEM 12
#define EACCES 13
#define EBADF 9
#define EINVAL 22
#define EMFILE 24
#define ENFILE 23
#define ENOSPC 28
#define EPERM 1
#define EPIPE 32
#define ENOSYS 38
#define ELOOP 40
#define ENAMETOOLONG 36
#define ENXIO 6
#define EIO 5
#define EWOULDBLOCK EAGAIN
#define ENOTEMPTY 39
#define EXDEV 18
#define EROFS 30
#define EFAULT 14
#define EDOM 33
#define ETIMEDOUT 110
#define EBUSY 16
#endif
#endif
