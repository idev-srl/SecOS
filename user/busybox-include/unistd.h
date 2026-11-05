/* SecOS BusyBox overlay: <unistd.h> = SecOS libc + the POSIX surface BusyBox
 * needs (shims in user/busybox_compat.c). SPDX-License-Identifier: MIT */
#ifndef _BB_UNISTD_H
#define _BB_UNISTD_H
#include <sys/types.h>
#include_next <unistd.h>
#include <getopt.h>

#ifndef F_OK
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#endif
#ifndef SEEK_DATA
#define SEEK_DATA 3
#define SEEK_HOLE 4
#endif

/* sysconf / pathconf names (Linux numbering) */
#define _SC_ARG_MAX        0
#define _SC_CHILD_MAX      1
#define _SC_CLK_TCK        2
#define _SC_NGROUPS_MAX    3
#define _SC_OPEN_MAX       4
#define _SC_PAGESIZE       30
#define _SC_PAGE_SIZE      30
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES     85
#define _SC_AVPHYS_PAGES   86
#define _SC_LINE_MAX       43
#define _SC_HOST_NAME_MAX  180
#define _SC_LOGIN_NAME_MAX 71
#define _SC_TTY_NAME_MAX   72
#define _SC_SYMLOOP_MAX    173
#define _PC_LINK_MAX       0
#define _PC_NAME_MAX       3
#define _PC_PATH_MAX       4
#define _PC_PIPE_BUF       5
#define _PC_VDISABLE       8
#define _POSIX_VDISABLE    '\0'
#define _POSIX_VERSION     200809L
#define _POSIX2_VERSION    200809L

long    sysconf(int name);
long    pathconf(const char* path, int name);
long    fpathconf(int fd, int name);
int     access(const char* path, int mode);
int     faccessat(int dirfd, const char* path, int mode, int flags);
int     eaccess(const char* path, int mode);
int     euidaccess(const char* path, int mode);
int     link(const char* oldp, const char* newp);
int     linkat(int od, const char* oldp, int nd, const char* newp, int fl);
int     unlinkat(int dirfd, const char* path, int flags);
int     symlinkat(const char* target, int dirfd, const char* linkpath);
ssize_t readlinkat(int dirfd, const char* path, char* buf, size_t len);
int     fchdir(int fd);
int     fchown(int fd, uid_t u, gid_t g);
int     lchown(const char* path, uid_t u, gid_t g);
int     fsync(int fd);
int     fdatasync(int fd);
void    sync(void);
int     syncfs(int fd);
int     truncate(const char* path, off_t len);
int     ftruncate(int fd, off_t len);
ssize_t pread(int fd, void* buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void* buf, size_t n, off_t off);
int     pipe2(int fds[2], int flags);
int     dup3(int oldfd, int newfd, int flags);
pid_t   vfork(void);
int     execl(const char* path, const char* arg, ...);
int     execlp(const char* file, const char* arg, ...);
int     execle(const char* path, const char* arg, ...);
int     execvpe(const char* file, char* const argv[], char* const envp[]);
int     fexecve(int fd, char* const argv[], char* const envp[]);
unsigned alarm(unsigned seconds);
int     pause(void);
int     nice(int inc);
int     gethostname(char* name, size_t len);
int     sethostname(const char* name, size_t len);
int     getdomainname(char* name, size_t len);
int     setdomainname(const char* name, size_t len);
char*   getlogin(void);
int     getlogin_r(char* buf, size_t len);
char*   ttyname(int fd);
int     ttyname_r(int fd, char* buf, size_t len);
char*   getpass(const char* prompt);
char*   crypt(const char* key, const char* salt);
int     setpgrp(void);
pid_t   tcgetsid(int fd);
int     seteuid(uid_t u);
int     setegid(gid_t g);
int     setreuid(uid_t r, uid_t e);
int     setregid(gid_t r, gid_t e);
int     setresuid(uid_t r, uid_t e, uid_t s);
int     setresgid(gid_t r, gid_t e, gid_t s);
int     getresuid(uid_t* r, uid_t* e, uid_t* s);
int     getresgid(gid_t* r, gid_t* e, gid_t* s);
int     chroot(const char* path);
int     getpagesize(void);
int     getdtablesize(void);
long    gethostid(void);
void*   sbrk(long incr);
int     brk(void* addr);
int     daemon(int nochdir, int noclose);
int     acct(const char* path);
int     lockf(int fd, int cmd, off_t len);
size_t  confstr(int name, char* buf, size_t len);
int     usleep(unsigned long usec);
ssize_t copy_file_range(int fi, off_t* oi, int fo, off_t* oo, size_t n, unsigned fl);
extern char** environ;
#endif
