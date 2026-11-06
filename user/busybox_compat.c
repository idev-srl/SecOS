/* SecOS - BusyBox port compatibility layer (see docs/BUSYBOX_PORT.md).
 *
 * Everything BusyBox needs beyond the SecOS libc lives here, so the port does
 * not touch user/libc.c:
 *   1. the entry point (flushes the private stdio when an applet returns);
 *   2. errno-setting wrappers for path/fd syscalls, POSIX open/wait/signal;
 *   3. a POSIX socket layer over the SecOS socket syscalls;
 *   4. process helpers (execvp with PATH, system, popen, wait3, ...);
 *   5. time/calendar (UTC; the clock is uptime until a wall clock lands);
 *   6. strings, wide-char (C locale), netdb (numeric + tiny DNS client), misc;
 *   7. a private buffered stdio with printf/scanf engines (bbio_* symbols).
 * Functions that are generic libc material (not BusyBox-specific) are marked
 * "TODO(integration): move to libc" for the libc owners to pick up.
 *
 * Compiled with -DBB_COMPAT_IMPL and the force-included busybox_port.h, so the
 * public names below resolve to the same sbb_* / bbio_* link symbols BusyBox
 * calls. The real libsecos/libc functions are reached through sys_* aliases
 * (asm labels naming the original symbols).
 * SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <termios.h>
#include <poll.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>
#include <mntent.h>
#include <netdb.h>
#include <resolv.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <sched.h>
#include <pwd.h>

/* ---- the original libsecos/libc entry points (bypassing the relabels) ---- */
extern long    secos_syscall(long, long, long, long, long, long);
extern ssize_t sys_read(int, void*, size_t)              __asm__("read");
extern ssize_t sys_write(int, const void*, size_t)       __asm__("write");
extern int     sys_close(int)                            __asm__("close");
extern int     sys_dup(int)                              __asm__("dup");
extern int     sys_dup2(int, int)                        __asm__("dup2");
extern int     sys_open(const char*, int)                __asm__("open");
extern int     sys_stat(const char*, struct stat*)       __asm__("stat");
extern int     sys_lstat(const char*, struct stat*)      __asm__("lstat");
extern int     sys_chmod(const char*, unsigned)          __asm__("chmod");
extern int     sys_chown(const char*, unsigned, unsigned) __asm__("chown");
extern long    sys_readlink(const char*, char*, long)    __asm__("readlink");
extern int     sys_symlink(const char*, const char*)     __asm__("symlink");
extern int     sys_mkdir(const char*, unsigned)          __asm__("mkdir");
extern int     sys_unlink(const char*)                   __asm__("unlink");
extern int     sys_chdir(const char*)                    __asm__("chdir");
extern int     sys_rename(const char*, const char*)      __asm__("rename");
extern int     sys_utimes(const char*, unsigned long, unsigned long) __asm__("utimes");
extern int     sys_umount(const char*)                   __asm__("umount");
extern void*   sys_mmap(void*, size_t, int, int)         __asm__("mmap");
extern void*   sys_mmap_file(void*, size_t, int, int, int) __asm__("mmap_file");
extern int     sys_ioctl(int, unsigned long, void*)      __asm__("ioctl");
extern void    sys_exit(int) __asm__("exit") __attribute__((noreturn));
extern char*   sys_strerror(int)                         __asm__("strerror");
extern sighandler_t sys_signal(int, sighandler_t)        __asm__("signal");
extern int     sys_socket(int)                           __asm__("socket");
extern int     sys_connect(int, unsigned, unsigned short) __asm__("connect");
extern int     sys_bind(int, unsigned short)             __asm__("bind");
extern int     sys_listen(int, int)                      __asm__("listen");
extern int     sys_accept(int)                           __asm__("accept");
extern long    sys_send(int, const void*, long)          __asm__("send");
extern long    sys_recv(int, void*, long)                __asm__("recv");
extern long    sys_sendto(int, const void*, long, const struct secos_sockaddr*) __asm__("sendto");
extern long    sys_recvfrom(int, void*, long, struct secos_sockaddr*) __asm__("recvfrom");
extern int     sys_snprintf(char*, size_t, const char*, ...) __asm__("snprintf");
extern double  sys_strtod(const char*, char**)           __asm__("strtod");
extern int     sys_poll(struct pollfd*, nfds_t, int)     __asm__("poll");
extern int     sys_execve(const char*, char* const[], char* const[]) __asm__("execve");
extern int     sys_rename2(const char*, const char*)     __asm__("rename");
extern int     sys_rmdir(const char*)                    __asm__("rmdir");
extern int     sys_uname(struct utsname*)                __asm__("uname");

/* syscall result -> POSIX: negative = -errno (or a bare -1, mapped to dflt) */
static long se(long r, int dflt) {
    if (r >= 0) return r;
    errno = (r < -1 && r > -134) ? (int)-r : dflt;
    return -1;
}

/* ===================================================================== */
/* 1. entry point                                                         */
/* ===================================================================== */
extern int sbb_main(int argc, char** argv) __asm__("sbb_main");
int main(int argc, char** argv) {
    exit(sbb_main(argc, argv));
}

/* ===================================================================== */
/* 2. files, fds, errno                                                   */
/* ===================================================================== */
static void sock_forget(int fd);
static void sock_copy(int from, int to);
static int sock_ioctl(int fd, unsigned long req, void* arg);
static int is_sock_fd(int fd);

/* Sockets are kernel fds: read/write go straight through; close/dup/dup2 also
 * keep this layer's per-fd socket metadata (type, addresses) in step. */
ssize_t read(int fd, void* buf, size_t len) { return sys_read(fd, buf, len); }
ssize_t write(int fd, const void* buf, size_t len) { return sys_write(fd, buf, len); }
int close(int fd) { sock_forget(fd); return sys_close(fd); }
int stat(const char* p, struct stat* st)  { return (int)se(sys_stat(p, st), ENOENT); }
int lstat(const char* p, struct stat* st) { return (int)se(sys_lstat(p, st), ENOENT); }
int fstat(int fd, struct stat* st) {
    if (!st) { errno = EFAULT; return -1; }
    memset(st, 0, sizeof(*st));
    return (int)se(secos_syscall(56 /*SYS_FSTAT*/, fd, (long)st, 0, 0, 0), EBADF);
}
int fstatat(int dirfd, const char* p, struct stat* st, int fl) {
    (void)dirfd;
    return (fl & AT_SYMLINK_NOFOLLOW) ? lstat(p, st) : stat(p, st);
}
static int path_exists(const char* p) { struct stat st; return sys_lstat(p, &st) >= 0; }
static int path_is_dir(const char* p) { struct stat st; return sys_stat(p, &st) >= 0 && S_ISDIR(st.st_mode); }
/* Only open/stat/lstat/chdir resolve relative paths in the kernel; every other
 * path syscall needs an absolute path, so make one (lexically normalised). */
static const char* ap(const char* p, char* out, size_t n) {
    char tmp[PATH_MAX];
    if (!p) return p;
    if (p[0] == '/') { strncpy(tmp, p, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0; }
    else {
        if (!getcwd(tmp, sizeof(tmp))) return p;
        size_t l = strlen(tmp);
        if (l + strlen(p) + 2 >= sizeof(tmp)) return p;
        tmp[l++] = '/'; strcpy(tmp + l, p);
    }
    size_t ol = 0; const char* q = tmp;
    while (*q) {
        while (*q == '/') q++;
        if (!*q) break;
        const char* e = q; while (*e && *e != '/') e++;
        size_t cl = (size_t)(e - q);
        if (cl == 1 && q[0] == '.') { q = e; continue; }
        if (cl == 2 && q[0] == '.' && q[1] == '.') { while (ol > 0 && out[ol - 1] != '/') ol--; if (ol > 0) ol--; q = e; continue; }
        if (ol + cl + 2 >= n) return p;
        out[ol++] = '/'; memcpy(out + ol, q, cl); ol += cl; q = e;
    }
    if (!ol) out[ol++] = '/';
    out[ol] = 0;
    return out;
}
#define AP(p) ap((p), (char[PATH_MAX]){ 0 }, PATH_MAX)
/* chmod/chown/utimes: the kernel enforces the owner/root rules and returns
 * -errno (a filesystem without on-disk metadata keeps the change in memory or
 * ignores it, and reports success), so -1 is a real EPERM. */
static int meta_rc(long r) { return (int)se(r, EPERM); }

int chmod(const char* p, unsigned m) {
    if (!path_exists(p)) { errno = ENOENT; return -1; }
    return meta_rc(sys_chmod(AP(p), m & 07777));
}
/* fchmod/fchown/lchown: the SecOS libc (real SYS_CHMOD/SYS_CHOWN fd and no-follow forms). */
int fchmodat(int d, const char* p, mode_t m, int f) { (void)d; (void)f; return chmod(p, m); }
int chown(const char* p, unsigned u, unsigned g) {
    if (!path_exists(p)) { errno = ENOENT; return -1; }
    return meta_rc(sys_chown(AP(p), u, g));
}
long readlink(const char* p, char* buf, long len) {
    struct stat st;
    if (sys_lstat(p, &st) < 0) { errno = ENOENT; return -1; }
    if (!S_ISLNK(st.st_mode)) { errno = EINVAL; return -1; }
    long r = sys_readlink(AP(p), buf, len);
    return se(r, EINVAL);
}
ssize_t readlinkat(int d, const char* p, char* b, size_t l) { (void)d; return readlink(p, b, (long)l); }
int symlink(const char* t, const char* l) {
    if (path_exists(l)) { errno = EEXIST; return -1; }
    return (int)se(sys_symlink(t, AP(l)), EPERM);
}
int symlinkat(const char* t, int d, const char* l) { (void)d; return symlink(t, l); }
/* mkdir/open/rmdir keep the kernel's errno (EACCES, EROFS, ENOSPC, ...);
 * only a bare -1 falls back to a best guess. */
int mkdir(const char* p, unsigned m) {
    if (path_exists(p)) { errno = EEXIST; return -1; }
    if (sys_mkdir(AP(p), m) < 0) {                       /* libsecos set errno */
        if (errno != EPERM) return -1;                    /* EPERM = a bare -1: guess below */
        char par[PATH_MAX]; strncpy(par, p, sizeof(par) - 1); par[sizeof(par) - 1] = 0;
        char* s = strrchr(par, '/');
        if (s && s != par) { *s = 0; if (!path_is_dir(par)) { errno = ENOENT; return -1; } }
        errno = EACCES; return -1;
    }
    return 0;
}
int mkdirat(int d, const char* p, mode_t m) { (void)d; return mkdir(p, m); }
int unlink(const char* p) {
    struct stat st;
    if (sys_lstat(p, &st) < 0) { errno = ENOENT; return -1; }
    if (S_ISDIR(st.st_mode)) { errno = EISDIR; return -1; }
    return (int)se(sys_unlink(AP(p)), EACCES);
}
int rmdir(const char* p) { return sys_rmdir(AP(p)); }   /* libsecos sets errno */
int unlinkat(int d, const char* p, int fl) { (void)d; return (fl & AT_REMOVEDIR) ? rmdir(p) : unlink(p); }
int remove(const char* p) { return path_is_dir(p) ? rmdir(p) : unlink(p); }
int chdir(const char* p) {
    struct stat st;
    if (sys_stat(p, &st) < 0) { errno = ENOENT; return -1; }
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return (int)se(sys_chdir(p), EACCES);
}
int fchdir(int fd) { (void)fd; errno = ENOSYS; return -1; }
/* rename/link: the kernel's atomic SYS_RENAME / SYS_LINK (libc sets errno). */
int rename(const char* a, const char* b) { return sys_rename2(AP(a), AP(b)); }
int renameat(int od, const char* a, int nd, const char* b) { (void)od; (void)nd; return rename(a, b); }
int linkat(int od, const char* a, int nd, const char* b, int f) { (void)od; (void)nd; (void)f; return link(a, b); }
int mknod(const char* p, mode_t m, dev_t d) { (void)p; (void)m; (void)d; errno = EPERM; return -1; }
int mknodat(int d, const char* p, mode_t m, dev_t dv) { (void)d; return mknod(p, m, dv); }
int mkfifo(const char* p, mode_t m) { (void)p; (void)m; errno = EPERM; return -1; }

int open(const char* path, int flags, ...) {
    mode_t mode = 0666;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }
    struct stat st;
    int exists = sys_stat(path, &st) >= 0;
    int serr = exists ? 0 : errno;
    if (exists && (flags & O_CREAT) && (flags & O_EXCL)) { errno = EEXIST; return -1; }
    /* Keep a search-permission denial on the way (EACCES) instead of ENOENT. */
    if (!exists && !(flags & O_CREAT)) { errno = (serr == EACCES || serr == ENOTDIR) ? serr : ENOENT; return -1; }
    if (exists && (flags & O_DIRECTORY) && !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    if (exists && S_ISDIR(st.st_mode) && (flags & O_ACCMODE) != O_RDONLY) { errno = EISDIR; return -1; }
    int fd = sys_open(path, flags & (O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND));
    if (fd < 0) {
        if (errno != EACCES && errno != EPERM) return -1;  /* the kernel's errno (EROFS, ENOSPC, ...); EPERM = a bare -1 */
        if (!exists && (flags & O_CREAT)) {
            char par[PATH_MAX]; strncpy(par, path, sizeof(par) - 1); par[sizeof(par) - 1] = 0;
            char* s = strrchr(par, '/');
            if (s && s != par) { *s = 0; if (!path_is_dir(par)) { errno = ENOENT; return -1; } }
        }
        errno = EACCES; return -1;
    }
    if (!exists && (flags & O_CREAT)) { unsigned um = umask(0); umask(um); sys_chmod(AP(path), mode & ~um & 07777); }
    return fd;
}
int openat(int dirfd, const char* path, int flags, ...) {
    (void)dirfd;
    mode_t mode = 0666;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }
    return open(path, flags, mode);
}
int creat(const char* p, mode_t m) { return open(p, O_WRONLY | O_CREAT | O_TRUNC, m); }
/* umask: libc keeps it; reading it is set-and-restore */
int isatty(int fd) {
    struct termios t;
    if (is_sock_fd(fd)) { errno = ENOTTY; return 0; }
    if (sys_ioctl(fd, TCGETS, &t) < 0) { errno = ENOTTY; return 0; }
    return 1;
}
/* ioctl: terminal requests go to the kernel TTY/pty layer (termios, window
 * size, FIONREAD, FIONBIO, job control, sessions); a socket of this layer
 * only understands FIONBIO/FIONREAD. */
int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req); void* arg = va_arg(ap, void*); va_end(ap);
    if (is_sock_fd(fd)) return sock_ioctl(fd, req, arg);
    if (req == FIOCLEX || req == FIONCLEX)
        return fcntl(fd, F_SETFD, req == FIOCLEX ? FD_CLOEXEC : 0) < 0 ? -1 : 0;
    return (int)se(sys_ioctl(fd, req, arg), ENOTTY);
}
/* eaccess/euidaccess: the SecOS libc (effective ids). */
int faccessat(int d, const char* p, int m, int f) { (void)d; return (f & AT_EACCESS) ? eaccess(p, m) : access(p, m); }
int utimes(const char* p, const struct timeval tv[2]) {
    unsigned long a, m;
    if (tv) { a = (unsigned long)tv[0].tv_sec; m = (unsigned long)tv[1].tv_sec; }
    else a = m = (unsigned long)time(NULL);
    if (!path_exists(p)) { errno = ENOENT; return -1; }
    return meta_rc(sys_utimes(AP(p), a, m));
}
int lutimes(const char* p, const struct timeval tv[2]) { return utimes(p, tv); }
int futimes(int fd, const struct timeval tv[2]) { (void)fd; (void)tv; return 0; }
int utimensat(int d, const char* p, const struct timespec ts[2], int fl) {
    (void)d; (void)fl;
    time_t now = time(NULL);
    struct stat st;
    if (sys_stat(p, &st) < 0) { errno = ENOENT; return -1; }
    unsigned long a = now, m = now;
    if (ts) {
        a = ts[0].tv_nsec == UTIME_OMIT ? st.st_atime : ts[0].tv_nsec == UTIME_NOW ? (unsigned long)now : (unsigned long)ts[0].tv_sec;
        m = ts[1].tv_nsec == UTIME_OMIT ? st.st_mtime : ts[1].tv_nsec == UTIME_NOW ? (unsigned long)now : (unsigned long)ts[1].tv_sec;
    }
    return meta_rc(sys_utimes(AP(p), a, m));
}
int futimens(int fd, const struct timespec ts[2]) { (void)fd; (void)ts; return 0; }
/* truncate/ftruncate, sync/fsync/fdatasync/syncfs: libc (SYS_TRUNCATE, SYS_SYNC). */
ssize_t pread(int fd, void* b, size_t n, off_t off) {
    long cur = lseek(fd, 0, SEEK_CUR);
    if (cur < 0 || lseek(fd, off, SEEK_SET) < 0) { errno = ESPIPE; return -1; }
    ssize_t r = read(fd, b, n); lseek(fd, cur, SEEK_SET); return r;
}
ssize_t pwrite(int fd, const void* b, size_t n, off_t off) {
    long cur = lseek(fd, 0, SEEK_CUR);
    if (cur < 0 || lseek(fd, off, SEEK_SET) < 0) { errno = ESPIPE; return -1; }
    ssize_t r = write(fd, b, n); lseek(fd, cur, SEEK_SET); return r;
}
ssize_t readv(int fd, const struct iovec* v, int n) {
    ssize_t t = 0;
    for (int i = 0; i < n; i++) { ssize_t r = read(fd, v[i].iov_base, v[i].iov_len); if (r < 0) return t ? t : r; t += r; if ((size_t)r < v[i].iov_len) break; }
    return t;
}
ssize_t writev(int fd, const struct iovec* v, int n) {
    ssize_t t = 0;
    for (int i = 0; i < n; i++) { ssize_t r = write(fd, v[i].iov_base, v[i].iov_len); if (r < 0) return t ? t : r; t += r; }
    return t;
}
int dup3(int o, int n, int f) { (void)f; return dup2(o, n); }   /* dup2() is the socket-aware one */
int flock(int fd, int op) { (void)fd; (void)op; return 0; }
int posix_fadvise(int fd, off_t o, off_t l, int a) { (void)fd; (void)o; (void)l; (void)a; return 0; }
int posix_fallocate(int fd, off_t o, off_t l) { (void)fd; (void)o; (void)l; return 0; }
ssize_t sendfile(int out, int in, off_t* off, size_t n) { (void)out; (void)in; (void)off; (void)n; errno = ENOSYS; return -1; }
ssize_t copy_file_range(int fi, off_t* oi, int fo, off_t* oo, size_t n, unsigned fl) {
    (void)fi; (void)oi; (void)fo; (void)oo; (void)n; (void)fl; errno = ENOSYS; return -1;
}
/* statfs/fstatfs/statvfs/fstatvfs: libc (SYS_STATFS). */
void* mmap(void* a, size_t l, int p, int f, int fd, off_t o) {
    void* r;
    if (o != 0) { errno = EINVAL; return MAP_FAILED; }
    if ((f & MAP_ANONYMOUS) || fd < 0) r = sys_mmap(a, l, p, (f & ~MAP_SHARED) | MAP_PRIVATE);
    else r = sys_mmap_file(a, l, p, (f & ~MAP_SHARED) | MAP_PRIVATE, fd);
    if ((long)r < 0 && (long)r > -4096) { errno = (int)-(long)r; return MAP_FAILED; }
    if (!r) { errno = ENOMEM; return MAP_FAILED; }
    return r;
}
int msync(void* a, size_t l, int f) { (void)a; (void)l; (void)f; return 0; }
int mlock(const void* a, size_t l) { (void)a; (void)l; return 0; }
int munlock(const void* a, size_t l) { (void)a; (void)l; return 0; }
int mlockall(int f) { (void)f; return 0; }
int munlockall(void) { return 0; }
int madvise(void* a, size_t l, int v) { (void)a; (void)l; (void)v; return 0; }
void* mremap(void* o, size_t ol, size_t nl, int f, ...) { (void)o; (void)ol; (void)nl; (void)f; errno = ENOMEM; return MAP_FAILED; }
int mount(const char* src, const char* tgt, const char* type, unsigned long fl, const void* data) {
    if (fl & (MS_REMOUNT | MS_BIND | MS_MOVE)) { errno = EINVAL; return -1; }
    if (!type || !*type) { errno = EINVAL; return -1; }
    /* `data` (-o uid=,gid=,umask=,... for vfat/exfat/ntfs) goes to the kernel
     * as SYS_MOUNT a3; MS_RDONLY is passed as a leading "ro". */
    char opt[128]; size_t n = 0; const char* d = (const char*)data;
    if (fl & MS_RDONLY) { opt[n++] = 'r'; opt[n++] = 'o'; }
    if (d && *d) {
        if (n) opt[n++] = ',';
        while (*d && n < sizeof(opt) - 1) opt[n++] = *d++;
        if (*d) { errno = EINVAL; return -1; }
    }
    opt[n] = 0;
    return (int)se(secos_syscall(38 /*SYS_MOUNT*/, (long)src, (long)AP(tgt), (long)type, n ? (long)opt : 0, 0), EINVAL);
}
int umount2(const char* t, int fl) { (void)fl; return (int)se(sys_umount(AP(t)), EINVAL); }
int sbb_umount2(const char* t, int fl) __asm__("sbb_umount2");
int sbb_umount2(const char* t, int fl) { return umount2(t, fl); }

/* opendir: the libc one reads at most 16 KB (64 entries) of a directory and
 * passes relative paths to SYS_GETDENTS, which does not resolve the cwd. */
DIR* opendir(const char* path) {
    struct stat st;
    if (sys_stat(path, &st) < 0) { errno = ENOENT; return NULL; }
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return NULL; }
    const char* abs = AP(path);
    DIR* d = malloc(sizeof(DIR));
    if (!d) { errno = ENOMEM; return NULL; }
    for (long cap = 64 * 1024; cap <= (1L << 20); cap *= 4) {
        d->buf = malloc((size_t)cap);
        if (!d->buf) break;
        long n = secos_syscall(40 /*SYS_GETDENTS*/, (long)abs, (long)d->buf, cap, 0, 0);
        if (n < 0) { free(d->buf); free(d); errno = EACCES; return NULL; }
        if (n + 256 <= cap || cap == (1L << 20)) { d->len = (int)n; d->pos = 0; return d; }
        free(d->buf);
    }
    free(d); errno = ENOMEM; return NULL;
}
struct dirent* readdir(DIR* d) {
    for (;;) {
        if (!d || d->pos + 256 > d->len) return NULL;
        char* rec = d->buf + d->pos; d->pos += 256;
        char t = rec[0];
        d->ent.d_type = t == 'd' ? DT_DIR : t == 'l' ? DT_LNK : t == 'c' ? DT_CHR : t == 'b' ? DT_BLK : DT_REG;
        strlcpy(d->ent.d_name, rec + 1, sizeof(d->ent.d_name));
        if (d->ent.d_name[0]) return &d->ent;
    }
}
int closedir(DIR* d) { if (d) { free(d->buf); free(d); } return 0; }
void rewinddir(DIR* d) { if (d) d->pos = 0; }
long telldir(DIR* d) { return d ? d->pos : -1; }
void seekdir(DIR* d, long p) { if (d) d->pos = (int)p; }
int dirfd(DIR* d) { (void)d; errno = ENOTSUP; return -1; }
DIR* fdopendir(int fd) { (void)fd; errno = ENOSYS; return NULL; }
int alphasort(const struct dirent** a, const struct dirent** b) { return strcmp((*a)->d_name, (*b)->d_name); }
int versionsort(const struct dirent** a, const struct dirent** b) { return strverscmp((*a)->d_name, (*b)->d_name); }

/* realpath: absolute, normalised, symlinks resolved (bounded).
 * TODO(integration): move to libc */
char* realpath(const char* path, char* resolved) {
    char buf[PATH_MAX], out[PATH_MAX];
    int hops = 0;
    if (!path || !*path) { errno = ENOENT; return NULL; }
    if (path[0] == '/') { strncpy(buf, path, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0; }
    else {
        if (!getcwd(buf, sizeof(buf))) return NULL;
        size_t l = strlen(buf);
        if (l + 1 + strlen(path) >= sizeof(buf)) { errno = ENAMETOOLONG; return NULL; }
        if (l > 1) buf[l++] = '/';
        strcpy(buf + l, path);
    }
again:
    out[0] = 0;
    {
        size_t ol = 0;
        char* p = buf;
        while (*p) {
            while (*p == '/') p++;
            if (!*p) break;
            char* e = p; while (*e && *e != '/') e++;
            size_t cl = (size_t)(e - p);
            if (cl == 1 && p[0] == '.') { p = e; continue; }
            if (cl == 2 && p[0] == '.' && p[1] == '.') {
                while (ol > 0 && out[ol - 1] != '/') ol--;
                if (ol > 0) ol--;
                out[ol] = 0; p = e; continue;
            }
            if (ol + 1 + cl >= sizeof(out)) { errno = ENAMETOOLONG; return NULL; }
            out[ol++] = '/'; memcpy(out + ol, p, cl); ol += cl; out[ol] = 0;
            struct stat st;
            if (sys_lstat(out, &st) < 0) {
                if (*e) { errno = ENOENT; return NULL; }
            } else if (S_ISLNK(st.st_mode)) {
                char tgt[PATH_MAX]; long n = sys_readlink(out, tgt, sizeof(tgt) - 1);
                if (n > 0 && ++hops < 8) {
                    tgt[n] = 0;
                    char rest[PATH_MAX]; strncpy(rest, e, sizeof(rest) - 1); rest[sizeof(rest) - 1] = 0;
                    if (tgt[0] == '/') snprintf(buf, sizeof(buf), "%s%s", tgt, rest);
                    else { while (ol > 0 && out[ol - 1] != '/') ol--; out[ol] = 0;
                           snprintf(buf, sizeof(buf), "%s%s%s", out, tgt, rest); }
                    goto again;
                }
            }
            p = e;
        }
        if (!out[0]) strcpy(out, "/");
    }
    if (!resolved) return strdup(out);
    strcpy(resolved, out);
    return resolved;
}
char* dirname(char* p) {
    static char dot[] = ".";
    if (!p || !*p) return dot;
    size_t l = strlen(p);
    while (l > 1 && p[l - 1] == '/') l--;
    while (l > 0 && p[l - 1] != '/') l--;
    if (!l) return dot;
    while (l > 1 && p[l - 1] == '/') l--;
    p[l] = 0;
    return p;
}

/* ===================================================================== */
/* 3. sockets (AF_INET over the SecOS socket syscalls)                    */
/* ===================================================================== */
/* A socket is a kernel fd (sockets share the fd table with files and pipes,
 * so dup2/fork/exec and poll work on them). This layer only remembers what the
 * SecOS socket syscalls do not report back: the type and the addresses, per
 * fd (lost across exec, where getsockname/getpeername still work through
 * SYS_SOCKNAME). */
#define SOCK_IO_MAX 2048   /* datagram calls bounce through a 2 KB kernel buffer */
struct bbsock { int used, type, connected; struct sockaddr_in local, peer; };
static struct bbsock g_skm[32];
static struct bbsock* sk(int fd) {
    if (fd < 0 || fd >= 32) { errno = EBADF; return NULL; }
    if (!g_skm[fd].used) {   /* inherited across exec: ask the kernel */
        struct stat st;
        if (secos_syscall(56 /*SYS_FSTAT*/, fd, (long)&st, 0, 0, 0) < 0 || !S_ISSOCK(st.st_mode)) { errno = ENOTSOCK; return NULL; }
        memset(&g_skm[fd], 0, sizeof(g_skm[fd]));
        g_skm[fd].used = 1; g_skm[fd].type = SOCK_STREAM; g_skm[fd].connected = 1;
        g_skm[fd].local.sin_family = g_skm[fd].peer.sin_family = AF_INET;
    }
    return &g_skm[fd];
}
static int is_sock_fd(int fd) { return fd >= 0 && fd < 32 && g_skm[fd].used; }
static void sock_forget(int fd) { if (fd >= 0 && fd < 32) g_skm[fd].used = 0; }
static void sock_copy(int from, int to) {
    if (to < 0 || to >= 32) return;
    if (from >= 0 && from < 32 && g_skm[from].used) g_skm[to] = g_skm[from]; else g_skm[to].used = 0;
}
static int sock_track(int fd, int type) {
    if (fd < 0) return fd;
    if (fd >= 32) return fd;
    memset(&g_skm[fd], 0, sizeof(g_skm[fd]));
    g_skm[fd].used = 1; g_skm[fd].type = type;
    g_skm[fd].local.sin_family = g_skm[fd].peer.sin_family = AF_INET;
    return fd;
}
static void sa_from_secos(const struct secos_sockaddr* k, struct sockaddr_in* in) {
    memset(in, 0, sizeof(*in));
    in->sin_family = AF_INET; in->sin_addr.s_addr = k->ip; in->sin_port = htons(k->port);
}
int socket(int domain, int type, int proto) {
    (void)proto;
    int t = type & 0xf;
    if (domain != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    if (t != SOCK_STREAM && t != SOCK_DGRAM) { errno = EPROTONOSUPPORT; return -1; }  /* no raw/ICMP sockets */
    /* SOCK_NONBLOCK / SOCK_CLOEXEC ride in the same word (Linux values). */
    int fd = (int)se(secos_syscall(21 /*SYS_SOCKET*/, type, 0, 0, 0, 0), EACCES);
    return sock_track(fd, t);
}
int connect(int fd, const struct sockaddr* a, socklen_t l) {
    struct bbsock* s = sk(fd); const struct sockaddr_in* in = (const struct sockaddr_in*)a;
    if (!s) return -1;
    if (!a || l < sizeof(*in) || in->sin_family != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    if (se(sys_connect(fd, in->sin_addr.s_addr, ntohs(in->sin_port)), ECONNREFUSED) < 0) return -1;
    s->peer = *in; s->connected = 1;
    return 0;
}
int bind(int fd, const struct sockaddr* a, socklen_t l) {
    struct bbsock* s = sk(fd); const struct sockaddr_in* in = (const struct sockaddr_in*)a;
    if (!s) return -1;
    if (!a || l < sizeof(*in) || in->sin_family != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    if (in->sin_port && se(sys_bind(fd, ntohs(in->sin_port)), EADDRINUSE) < 0) return -1;
    s->local = *in;
    return 0;
}
int listen(int fd, int bl) { if (!sk(fd)) return -1; return (int)se(sys_listen(fd, bl), EOPNOTSUPP); }
int accept(int fd, struct sockaddr* a, socklen_t* l) {
    struct bbsock* s = sk(fd); if (!s) return -1;
    int nfd = (int)se(sys_accept(fd), ECONNABORTED);
    if (nfd < 0) return -1;
    sock_track(nfd, SOCK_STREAM);
    if (nfd < 32) {
        g_skm[nfd].connected = 1; g_skm[nfd].local = s->local;
        struct secos_sockaddr k; memset(&k, 0, sizeof(k));
        if (secos_syscall(80 /*SYS_SOCKNAME*/, nfd, (long)&k, 1, 0, 0) == 0) sa_from_secos(&k, &g_skm[nfd].peer);
    }
    if (a && l) {
        size_t n = *l < sizeof(struct sockaddr_in) ? *l : sizeof(struct sockaddr_in);
        if (nfd < 32) memcpy(a, &g_skm[nfd].peer, n);
        *l = sizeof(struct sockaddr_in);
    }
    return nfd;
}
int accept4(int fd, struct sockaddr* a, socklen_t* l, int f) {
    int n = accept(fd, a, l);
    if (n >= 0 && (f & SOCK_CLOEXEC)) fcntl(n, F_SETFD, FD_CLOEXEC);
    if (n >= 0 && (f & SOCK_NONBLOCK)) fcntl(n, F_SETFL, O_NONBLOCK);
    return n;
}
ssize_t send(int fd, const void* b, size_t n, int fl) {
    struct bbsock* s = sk(fd); (void)fl; if (!s) return -1;
    if (s->type == SOCK_DGRAM && !s->connected) { errno = EDESTADDRREQ; return -1; }
    return write(fd, b, n);
}
ssize_t recv(int fd, void* b, size_t n, int fl) {
    (void)fl; if (!sk(fd)) return -1;
    return read(fd, b, n);
}
ssize_t sendto(int fd, const void* b, size_t n, int fl, const struct sockaddr* a, socklen_t l) {
    struct bbsock* s = sk(fd); if (!s) return -1;
    if (!a || s->type == SOCK_STREAM) return send(fd, b, n, fl);
    const struct sockaddr_in* in = (const struct sockaddr_in*)a;
    if (l < sizeof(*in) || in->sin_family != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    struct secos_sockaddr sa; sa.ip = in->sin_addr.s_addr; sa.port = ntohs(in->sin_port); sa._pad = 0;
    long r = -1;
    for (int t = 0; t < 4 && r < 0; t++) {   /* a send fails while the next hop's ARP resolves */
        if (t) sleep_ticks(100);
        r = sys_sendto(fd, b, (long)(n > SOCK_IO_MAX ? SOCK_IO_MAX : n), &sa);
    }
    return se(r, ENETUNREACH);
}
ssize_t recvfrom(int fd, void* b, size_t n, int fl, struct sockaddr* a, socklen_t* l) {
    struct bbsock* s = sk(fd); if (!s) return -1;
    if (s->type == SOCK_STREAM || !a) {
        ssize_t r = recv(fd, b, n, fl);
        if (r >= 0 && a && l) { size_t k = *l < sizeof(s->peer) ? *l : sizeof(s->peer); memcpy(a, &s->peer, k); *l = sizeof(s->peer); }
        return r;
    }
    struct secos_sockaddr sa; memset(&sa, 0, sizeof(sa));
    long r = se(sys_recvfrom(fd, b, (long)(n > SOCK_IO_MAX ? SOCK_IO_MAX : n), &sa), EAGAIN);
    if (r < 0) return -1;
    struct sockaddr_in in; sa_from_secos(&sa, &in);
    if (l) { size_t k = *l < sizeof(in) ? *l : sizeof(in); memcpy(a, &in, k); *l = sizeof(in); }
    return r;
}
ssize_t sendmsg(int fd, const struct msghdr* m, int fl) {
    if (!m || m->msg_iovlen < 1) { errno = EINVAL; return -1; }
    return sendto(fd, m->msg_iov[0].iov_base, m->msg_iov[0].iov_len, fl, (const struct sockaddr*)m->msg_name, m->msg_namelen);
}
ssize_t recvmsg(int fd, struct msghdr* m, int fl) {
    if (!m || m->msg_iovlen < 1) { errno = EINVAL; return -1; }
    m->msg_controllen = 0; m->msg_flags = 0;
    return recvfrom(fd, m->msg_iov[0].iov_base, m->msg_iov[0].iov_len, fl, (struct sockaddr*)m->msg_name, m->msg_name ? &m->msg_namelen : NULL);
}
static int sock_ioctl(int fd, unsigned long req, void* arg) {
    if (req == FIONBIO) {
        int fl = fcntl(fd, F_GETFL); if (fl < 0) return -1;
        return fcntl(fd, F_SETFL, (arg && *(int*)arg) ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) < 0 ? -1 : 0;
    }
    if (req == FIONREAD) { if (arg) *(int*)arg = 0; return 0; }
    errno = ENOTTY; return -1;
}
int dup(int fd) {
    int n = (int)se(sys_dup(fd), EBADF);
    if (n >= 0) sock_copy(fd, n);
    return n;
}
int dup2(int o, int n) {
    int r = (int)se(sys_dup2(o, n), EBADF);
    if (r >= 0 && o != n) sock_copy(o, n);
    return r;
}
int shutdown(int fd, int how) { (void)how; return sk(fd) ? 0 : -1; }
static int sockname(int fd, struct sockaddr* a, socklen_t* l, int peer) {
    if (!sk(fd)) return -1;
    struct secos_sockaddr k; memset(&k, 0, sizeof(k));
    if (se(secos_syscall(80 /*SYS_SOCKNAME*/, fd, (long)&k, peer, 0, 0), ENOTCONN) < 0) return -1;
    struct sockaddr_in in; sa_from_secos(&k, &in);
    if (a && l) { size_t n = *l < sizeof(in) ? *l : sizeof(in); memcpy(a, &in, n); *l = sizeof(in); }
    return 0;
}
int getsockname(int fd, struct sockaddr* a, socklen_t* l) { return sockname(fd, a, l, 0); }
int getpeername(int fd, struct sockaddr* a, socklen_t* l) { return sockname(fd, a, l, 1); }
int getsockopt(int fd, int lvl, int opt, void* v, socklen_t* l) {
    struct bbsock* s = sk(fd); if (!s) return -1;
    (void)lvl;
    if (v && l && *l >= sizeof(int)) { *(int*)v = (opt == SO_TYPE) ? s->type : 0; *l = sizeof(int); }
    return 0;
}
int setsockopt(int fd, int lvl, int opt, const void* v, socklen_t l) { (void)lvl; (void)opt; (void)v; (void)l; return sk(fd) ? 0 : -1; }
int socketpair(int d, int t, int p, int sv[2]) { (void)d; (void)t; (void)p; (void)sv; errno = EOPNOTSUPP; return -1; }
unsigned int if_nametoindex(const char* n) { (void)n; errno = ENODEV; return 0; }
char* if_indextoname(unsigned int i, char* n) { (void)i; (void)n; errno = ENXIO; return NULL; }

/* poll/select: the kernel's SYS_POLL (libc); sockets are kernel fds. */
int poll(struct pollfd* fds, nfds_t n, int timeout) { return sys_poll(fds, n, timeout); }
static int select_ms(int nfd, fd_set* r, fd_set* w, fd_set* e, int ms) {
    struct pollfd pf[64];
    int n = 0;
    if (nfd < 0 || nfd > FD_SETSIZE) { errno = EINVAL; return -1; }
    for (int fd = 0; fd < nfd; fd++) {
        short ev = 0;
        if (r && FD_ISSET(fd, r)) ev |= POLLIN;
        if (w && FD_ISSET(fd, w)) ev |= POLLOUT;
        if (e && FD_ISSET(fd, e)) ev |= POLLPRI;
        if (!ev) continue;
        if (n == 64) { errno = EINVAL; return -1; }
        pf[n].fd = fd; pf[n].events = ev; pf[n].revents = 0; n++;
    }
    if (poll(pf, (nfds_t)n, ms) < 0) return -1;
    for (int i = 0; i < n; i++) if (pf[i].revents & POLLNVAL) { errno = EBADF; return -1; }
    if (r) FD_ZERO(r);
    if (w) FD_ZERO(w);
    if (e) FD_ZERO(e);
    int ready = 0;
    for (int i = 0; i < n; i++) {
        int fd = pf[i].fd, rv = pf[i].revents;
        if (r && (rv & (POLLIN | POLLHUP | POLLERR)) && (pf[i].events & POLLIN)) { FD_SET(fd, r); ready++; }
        if (w && (rv & (POLLOUT | POLLERR)) && (pf[i].events & POLLOUT)) { FD_SET(fd, w); ready++; }
        if (e && (rv & POLLPRI)) { FD_SET(fd, e); ready++; }
    }
    return ready;
}
int select(int nfd, fd_set* r, fd_set* w, fd_set* e, struct timeval* tv) {
    int ms = -1;
    if (tv) {
        if (tv->tv_sec < 0 || tv->tv_usec < 0) { errno = EINVAL; return -1; }
        ms = (int)(tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000);
    }
    int rv = select_ms(nfd, r, w, e, ms);
    if (tv && rv == 0) tv->tv_sec = tv->tv_usec = 0;
    return rv;
}
int pselect(int nfd, fd_set* r, fd_set* w, fd_set* e, const struct timespec* ts, const unsigned long* m) {
    int ms = ts ? (int)(ts->tv_sec * 1000 + (ts->tv_nsec + 999999) / 1000000) : -1;
    unsigned long old = 0;
    if (m) sigprocmask(SIG_SETMASK, m, &old);
    int rv = select_ms(nfd, r, w, e, ms);
    int er = errno;
    if (m) sigprocmask(SIG_SETMASK, &old, 0);
    errno = er;
    return rv;
}

/* ===================================================================== */
/* 4. processes, signals, identity                                       */
/* ===================================================================== */
/* waitpid: SYS_WAIT(pid) blocks; SYS_WAITANY handles -1 and WNOHANG. A child
 * reaped by WAITANY while waiting for another pid is stashed for later. */
static struct { int pid, st; } g_stash[16];
static int stash_take(pid_t pid, int* status) {
    for (int i = 0; i < 16; i++)
        if (g_stash[i].pid && (pid <= 0 || g_stash[i].pid == pid)) {
            int p = g_stash[i].pid; if (status) *status = g_stash[i].st; g_stash[i].pid = 0; return p;
        }
    return 0;
}
static void stash_put(int pid, int st) {
    for (int i = 0; i < 16; i++) if (!g_stash[i].pid) { g_stash[i].pid = pid; g_stash[i].st = st; return; }
}
pid_t waitpid(pid_t pid, int* status, int options) {
    int p = stash_take(pid, status);
    if (p) return p;
    if (pid > 0 && !(options & WNOHANG)) {
        long code = secos_syscall(9 /*SYS_WAIT*/, pid, 0, 0, 0, 0);
        if (code == -4) { errno = EINTR; return -1; }
        if (code < 0) { errno = ECHILD; return -1; }
        if (status) *status = (int)(code & 0xffff);
        return pid;
    }
    for (;;) {
        int code = 0;
        long got = secos_syscall(55 /*SYS_WAITANY*/, (long)&code, (long)(options & (WNOHANG | WUNTRACED | WCONTINUED)), 0, 0, 0);
        if (got == -10) { errno = ECHILD; return -1; }
        if (got == 0) return 0;
        if (got < 0) { errno = EINTR; return -1; }
        if (pid > 0 && got != pid) { stash_put((int)got, code & 0xffff); continue; }
        if (status) *status = code & 0xffff;
        return (pid_t)got;
    }
}
pid_t wait(int* st) { return waitpid(-1, st, 0); }
pid_t wait3(int* st, int o, struct rusage* ru) { if (ru) memset(ru, 0, sizeof(*ru)); return waitpid(-1, st, o); }
pid_t wait4(pid_t p, int* st, int o, struct rusage* ru) { if (ru) memset(ru, 0, sizeof(*ru)); return waitpid(p, st, o); }
pid_t vfork(void) { return fork(); }

static sighandler_t g_disp[NSIG];
sighandler_t signal(int sig, sighandler_t h) {
    if (sig <= 0 || sig >= NSIG) { errno = EINVAL; return SIG_ERR; }
    if (sys_signal(sig, h) == SIG_ERR) { errno = EINVAL; return SIG_ERR; }
    sighandler_t old = g_disp[sig]; g_disp[sig] = h;
    return old;
}
sighandler_t bsd_signal(int s, sighandler_t h) { return signal(s, h); }
sighandler_t sysv_signal(int s, sighandler_t h) { return signal(s, h); }
int sigaction(int sig, const struct sigaction* act, struct sigaction* old) {
    if (sig <= 0 || sig >= NSIG) { errno = EINVAL; return -1; }
    if (old) { memset(old, 0, sizeof(*old)); old->sa_handler = g_disp[sig]; }
    if (act && signal(sig, act->sa_handler) == SIG_ERR) return -1;
    return 0;
}
int sigemptyset(sigset_t* s) { *s = 0; return 0; }
int sigfillset(sigset_t* s) { *s = ~0UL; return 0; }
int sigaddset(sigset_t* s, int n) { if (n <= 0 || n >= 64) { errno = EINVAL; return -1; } *s |= 1UL << n; return 0; }
int sigdelset(sigset_t* s, int n) { if (n <= 0 || n >= 64) { errno = EINVAL; return -1; } *s &= ~(1UL << n); return 0; }
int sigismember(const sigset_t* s, int n) { return (n > 0 && n < 64) ? (int)((*s >> n) & 1) : 0; }
int sigpending(sigset_t* s) { *s = 0; return 0; }
int siginterrupt(int s, int f) { (void)s; (void)f; return 0; }
int sigaltstack(const stack_t* a, stack_t* o) { (void)a; if (o) memset(o, 0, sizeof(*o)); return 0; }
int sigwait(const sigset_t* s, int* sig) { (void)s; (void)sig; errno = ENOSYS; return ENOSYS; }
int sigqueue(pid_t p, int sig, const union sigval v) { (void)v; return kill(p, sig); }
/* No timer signals on SecOS yet: alarm/setitimer are accepted but never fire.
 * TODO(integration): SIGALRM delivery. */
unsigned alarm(unsigned s) { (void)s; return 0; }
int setitimer(int w, const struct itimerval* n, struct itimerval* o) { (void)w; (void)n; if (o) memset(o, 0, sizeof(*o)); return 0; }
int getitimer(int w, struct itimerval* v) { (void)w; memset(v, 0, sizeof(*v)); return 0; }
int pause(void) { return sigsuspend(NULL); }
void psignal(int sig, const char* s) { if (s && *s) fprintf(stderr, "%s: ", s); fprintf(stderr, "%s\n", strsignal(sig)); }

/* identity: the real credentials of the SecOS libc (kernel/cred.c):
 * get/set[res][ug]id, setgroups/getgroups and initgroups are not shadowed here.
 * getlogin: the name of the real uid (there is no utmp). */
int chroot(const char* p) { (void)p; errno = EPERM; return -1; }
char* getlogin(void) { struct passwd* pw = getpwuid(getuid()); return pw ? pw->pw_name : NULL; }
int getlogin_r(char* b, size_t l) {
    const char* n = getlogin();
    if (!n) return ENXIO;
    if (strlen(n) + 1 > l) return ERANGE;
    strcpy(b, n); return 0;
}
int nice(int inc) { (void)inc; return 0; }
int getpriority(int w, id_t who) { (void)w; (void)who; return 0; }
int setpriority(int w, id_t who, int p) { (void)w; (void)who; (void)p; return 0; }
int getrlimit(int r, struct rlimit* l) { (void)r; l->rlim_cur = l->rlim_max = RLIM_INFINITY; if (r == RLIMIT_NOFILE) l->rlim_cur = l->rlim_max = 32; return 0; }
int setrlimit(int r, const struct rlimit* l) { (void)r; (void)l; return 0; }
int prlimit(pid_t p, int r, const struct rlimit* n, struct rlimit* o) { (void)p; (void)n; return o ? getrlimit(r, o) : 0; }
int getrusage(int w, struct rusage* u) { (void)w; memset(u, 0, sizeof(*u)); return 0; }
clock_t times(struct tms* t) {
    clock_t now = (clock_t)(getticks() / 10);   /* sysconf(_SC_CLK_TCK) = 100 */
    if (t) { t->tms_utime = now; t->tms_stime = t->tms_cutime = t->tms_cstime = 0; }
    return now;
}
int prctl(int op, ...) { (void)op; return 0; }
int sched_getaffinity(pid_t p, size_t sz, cpu_set_t* s) { (void)p; (void)sz; CPU_ZERO(s); CPU_SET(0, s); return 0; }
int sched_setaffinity(pid_t p, size_t sz, const cpu_set_t* s) { (void)p; (void)sz; (void)s; return 0; }
int sched_getscheduler(pid_t p) { (void)p; return SCHED_OTHER; }
int sched_setscheduler(pid_t p, int pol, const struct sched_param* sp) { (void)p; (void)pol; (void)sp; errno = EPERM; return -1; }
int sched_getparam(pid_t p, struct sched_param* sp) { (void)p; sp->sched_priority = 0; return 0; }
int sched_get_priority_max(int p) { (void)p; return 0; }
int sched_get_priority_min(int p) { (void)p; return 0; }
long syscall(long nr, ...) { (void)nr; errno = ENOSYS; return -1; }
/* exec: the kernel's in-place SYS_EXECVE (libc's execve; pid, fds and the
 * environment carry over). execvp adds the PATH search and POSIX's ENOEXEC
 * rule: a file without an ELF header is run by the shell (GNU bash). */
static int exec_path(const char* path, char* const argv[], char* const envp[]) {
    fflush(NULL);                     /* the private stdio would lose buffered output */
    sys_execve(path, argv, envp);
    if (errno == ENOEXEC) {
        char* av[256]; int n = 0;
        av[n++] = (char*)"bash"; av[n++] = (char*)path;
        for (int i = 1; argv && argv[i] && n < 255; i++) av[n++] = argv[i];
        av[n] = NULL;
        sys_execve("/bin/bash", av, envp);
        errno = ENOEXEC;
    }
    return -1;
}
int execve(const char* p, char* const a[], char* const e[]) { fflush(NULL); return sys_execve(p, a, e); }
int execv(const char* p, char* const a[]) { return execve(p, a, environ); }
int execvpe(const char* file, char* const argv[], char* const envp[]) {
    if (!file || !*file) { errno = ENOENT; return -1; }
    if (strchr(file, '/')) return exec_path(file, argv, envp);
    const char* path = getenv("PATH");
    if (!path || !*path) path = "/bin:/usr/bin";
    char buf[PATH_MAX];
    int saw_eacces = 0;
    while (*path) {
        const char* e = strchr(path, ':'); size_t l = e ? (size_t)(e - path) : strlen(path);
        if (l + strlen(file) + 2 < sizeof(buf)) {
            if (l) { memcpy(buf, path, l); buf[l] = '/'; strcpy(buf + l + 1, file); }
            else strcpy(buf, file);
            struct stat st;
            if (sys_stat(buf, &st) >= 0 && S_ISREG(st.st_mode)) {
                exec_path(buf, argv, envp);
                if (errno == EACCES) saw_eacces = 1;
                else return -1;
            }
        }
        if (!e) break;
        path = e + 1;
    }
    /* SecOS has no /bin/sh: GNU bash is the POSIX shell (watch, xargs sh -c, ...) */
    if (!strcmp(file, "sh")) return exec_path("/bin/bash", argv, envp);
    errno = saw_eacces ? EACCES : ENOENT; return -1;
}
int execvp(const char* f, char* const a[]) { return execvpe(f, a, environ); }
static int exec_list(int vp, const char* p, const char* arg0, va_list ap, int with_env) {
    char* argv[1024]; int n = 0;
    argv[n++] = (char*)arg0;
    while (arg0 && (argv[n] = va_arg(ap, char*)) != NULL) {
        if (++n == 1023) { errno = E2BIG; return -1; }
    }
    argv[n] = NULL;
    char* const* envp = with_env ? va_arg(ap, char* const*) : environ;
    if (vp) return execvpe(p, argv, envp);
    return execve(p, argv, envp);
}
int execl(const char* p, const char* a, ...) { va_list ap; va_start(ap, a); int r = exec_list(0, p, a, ap, 0); va_end(ap); return r; }
int execlp(const char* p, const char* a, ...) { va_list ap; va_start(ap, a); int r = exec_list(1, p, a, ap, 0); va_end(ap); return r; }
int execle(const char* p, const char* a, ...) { va_list ap; va_start(ap, a); int r = exec_list(0, p, a, ap, 1); va_end(ap); return r; }
int fexecve(int fd, char* const a[], char* const e[]) { (void)fd; (void)a; (void)e; errno = ENOSYS; return -1; }
int daemon(int nc, int no) { (void)nc; (void)no; errno = ENOSYS; return -1; }
int setpgrp(void) { return setpgid(0, 0); }

/* ===================================================================== */
/* 5. time. The wall clock and the UTC calendar functions are libc's;     */
/*    only strptime and a few setters live here.                          */
/* ===================================================================== */
int stime(const time_t* t) { struct timespec ts = { t ? *t : 0, 0 }; return clock_settime(CLOCK_REALTIME, &ts); }
int adjtime(const struct timeval* d, struct timeval* o) { (void)d; if (o) memset(o, 0, sizeof(*o)); return 0; }
time_t timelocal(struct tm* tm) { return timegm(tm); }   /* UTC */
static const char* const g_wday[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char* const g_mon[12] = { "January", "February", "March", "April", "May", "June", "July",
                                        "August", "September", "October", "November", "December" };
static const char* stp_num(const char* s, int lo, int hi, int w, int* out) {
    int v = 0, k = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-' || *s == '+') { neg = *s == '-'; s++; }
    while (k < w && isdigit((unsigned char)*s)) { v = v * 10 + (*s++ - '0'); k++; }
    if (!k) return NULL;
    if (neg) v = -v;
    if (v < lo || v > hi) return NULL;
    *out = v; return s;
}
static const char* stp_name(const char* s, const char* const* tab, int n, int* out) {
    for (int i = 0; i < n; i++) {
        size_t l = strlen(tab[i]);
        if (!strncasecmp(s, tab[i], l)) { *out = i; return s + l; }
        if (!strncasecmp(s, tab[i], 3)) { *out = i; return s + 3; }
    }
    return NULL;
}
char* strptime(const char* s, const char* f, struct tm* tm) {
    int v;
    for (; *f && s; f++) {
        if (isspace((unsigned char)*f)) { while (isspace((unsigned char)*s)) s++; continue; }
        if (*f != '%') { if (*s != *f) return NULL; s++; continue; }
        f++;
        if (*f == 'E' || *f == 'O') f++;
        switch (*f) {
        case 'a': case 'A': s = stp_name(s, g_wday, 7, &tm->tm_wday); break;
        case 'b': case 'B': case 'h': s = stp_name(s, g_mon, 12, &tm->tm_mon); break;
        case 'd': case 'e': s = stp_num(s, 1, 31, 2, &tm->tm_mday); break;
        case 'H': case 'k': s = stp_num(s, 0, 23, 2, &tm->tm_hour); break;
        case 'I': case 'l': s = stp_num(s, 1, 12, 2, &tm->tm_hour); break;
        case 'j': s = stp_num(s, 1, 366, 3, &v); if (s) tm->tm_yday = v - 1; break;
        case 'm': s = stp_num(s, 1, 12, 2, &v); if (s) tm->tm_mon = v - 1; break;
        case 'M': s = stp_num(s, 0, 59, 2, &tm->tm_min); break;
        case 'S': s = stp_num(s, 0, 61, 2, &tm->tm_sec); break;
        case 'y': s = stp_num(s, 0, 99, 2, &v); if (s) tm->tm_year = v < 69 ? v + 100 : v; break;
        case 'Y': s = stp_num(s, 0, 9999, 4, &v); if (s) tm->tm_year = v - 1900; break;
        case 'C': s = stp_num(s, 0, 99, 2, &v); if (s) tm->tm_year = v * 100 - 1900; break;
        case 'p': if (!strncasecmp(s, "PM", 2)) { if (tm->tm_hour < 12) tm->tm_hour += 12; s += 2; }
                  else if (!strncasecmp(s, "AM", 2)) { if (tm->tm_hour == 12) tm->tm_hour = 0; s += 2; }
                  else s = NULL;
                  break;
        case 'n': case 't': while (isspace((unsigned char)*s)) s++; break;
        case 'D': s = strptime(s, "%m/%d/%y", tm); break;
        case 'F': s = strptime(s, "%Y-%m-%d", tm); break;
        case 'R': s = strptime(s, "%H:%M", tm); break;
        case 'T': s = strptime(s, "%H:%M:%S", tm); break;
        case 's': { long t = strtol(s, (char**)&s, 10); time_t tt = (time_t)t; gmtime_r(&tt, tm); break; }
        case 'z': if (*s == 'Z') s++; else if (*s == '+' || *s == '-') { s++; for (int i = 0; i < 4 && isdigit((unsigned char)*s); i++) s++; } break;
        case 'Z': while (isalpha((unsigned char)*s)) s++; break;
        case '%': if (*s != '%') return NULL; s++; break;
        default: return NULL;
        }
    }
    return (char*)s;
}

/* ===================================================================== */
/* 6. system information                                                  */
/* ===================================================================== */
static long read_small(const char* p, char* buf, size_t n) {
    int fd = sys_open(p, O_RDONLY);
    if (fd < 0) return -1;
    long t = 0, r;
    while ((size_t)t < n - 1 && (r = sys_read(fd, buf + t, n - 1 - (size_t)t)) > 0) t += r;
    sys_close(fd); buf[t] = 0; return t;
}
long get_phys_pages(void) { return sysconf(_SC_PHYS_PAGES); }
long get_avphys_pages(void) { return sysconf(_SC_AVPHYS_PAGES); }
int getpagesize(void) { return 4096; }
int getdtablesize(void) { return 32; }
long gethostid(void) { return 0; }
int gethostname(char* name, size_t len) {   /* /etc/hostname, else "secos" */
    char buf[HOST_NAME_MAX + 1];
    long r = read_small("/etc/hostname", buf, sizeof(buf));
    const char* h = "secos";
    if (r > 0) { buf[strcspn(buf, " \t\r\n")] = 0; if (buf[0]) h = buf; }
    if (strlen(h) + 1 > len) { errno = ENAMETOOLONG; return -1; }
    strcpy(name, h);
    return 0;
}
int sethostname(const char* name, size_t len) {
    int fd = open("/etc/hostname", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { errno = EPERM; return -1; }
    write(fd, name, len); write(fd, "\n", 1); close(fd);
    return 0;
}
int getdomainname(char* n, size_t l) { if (l < 7) { errno = EINVAL; return -1; } strcpy(n, "(none)"); return 0; }
int setdomainname(const char* n, size_t l) { (void)n; (void)l; errno = EPERM; return -1; }
/* uname: the kernel's (SYS_UNAME); the node name follows /etc/hostname. */
int uname(struct utsname* u) {
    if (sys_uname(u) < 0) return -1;
    gethostname(u->nodename, sizeof(u->nodename));
    return 0;
}
char* ctermid(char* s) { static char b[] = "/dev/tty"; if (s) { strcpy(s, b); return s; } return b; }
/* getpass (echo off on /dev/tty) and crypt (musl, user/libcrypt.a): the SecOS libc. */
int acct(const char* p) { (void)p; errno = ENOSYS; return -1; }
int lockf(int fd, int c, off_t l) { (void)fd; (void)c; (void)l; return 0; }
size_t confstr(int n, char* b, size_t l) { (void)n; const char* v = "/bin:/usr/bin"; if (b && l) { strncpy(b, v, l - 1); b[l - 1] = 0; } return strlen(v) + 1; }
int brk(void* a) { return (brk_set((unsigned long)a) >= (unsigned long)a) ? 0 : (errno = ENOMEM, -1); }
ssize_t getrandom(void* buf, size_t len, unsigned fl) {
    (void)fl;
    int fd = sys_open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { ssize_t r = sys_read(fd, buf, len); sys_close(fd); if (r > 0) return r; }
    unsigned long x = getticks() * 6364136223846793005UL + (unsigned long)getpid();
    for (size_t i = 0; i < len; i++) { x = x * 6364136223846793005UL + 1442695040888963407UL; ((unsigned char*)buf)[i] = (unsigned char)(x >> 56); }
    return (ssize_t)len;
}
ssize_t getxattr(const char* p, const char* n, void* v, size_t s) { (void)p; (void)n; (void)v; (void)s; errno = ENODATA; return -1; }
int setxattr(const char* p, const char* n, const void* v, size_t s, int f) { (void)p; (void)n; (void)v; (void)s; (void)f; errno = ENOTSUP; return -1; }

/* mntent over /proc/mounts */
FILE* setmntent(const char* f, const char* m) { return fopen(f, m); }
int endmntent(FILE* f) { if (f) fclose(f); return 1; }
struct mntent* getmntent_r(FILE* f, struct mntent* m, char* buf, int len) {
    while (fgets(buf, len, f)) {
        char* p = buf; char* fld[6] = { 0 }; int n = 0;
        while (*p && n < 6) {
            while (*p == ' ' || *p == '\t' || *p == '\n') *p++ = 0;
            if (!*p || *p == '#') break;
            fld[n++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        }
        if (*p == '\n') *p = 0;
        if (n < 2 || fld[0][0] == '#') continue;
        m->mnt_fsname = fld[0]; m->mnt_dir = fld[1];
        if (n == 3) { fld[3] = fld[2]; fld[2] = fld[0]; n = 4; }   /* SecOS /proc/mounts: "fs dir opts" */
        m->mnt_type = n > 2 ? fld[2] : (char*)"auto";
        m->mnt_opts = n > 3 ? fld[3] : (char*)"rw";
        m->mnt_freq = n > 4 ? atoi(fld[4]) : 0; m->mnt_passno = n > 5 ? atoi(fld[5]) : 0;
        return m;
    }
    return NULL;
}
struct mntent* getmntent(FILE* f) { static struct mntent m; static char b[512]; return getmntent_r(f, &m, b, sizeof(b)); }
int addmntent(FILE* f, const struct mntent* m) {
    return fprintf(f, "%s %s %s %s %d %d\n", m->mnt_fsname, m->mnt_dir, m->mnt_type, m->mnt_opts, m->mnt_freq, m->mnt_passno) < 0;
}
char* hasmntopt(const struct mntent* m, const char* opt) {
    size_t l = strlen(opt); char* p = m->mnt_opts;
    while (p && *p) { if (!strncmp(p, opt, l) && (p[l] == 0 || p[l] == ',' || p[l] == '=')) return p; p = strchr(p, ','); if (p) p++; }
    return NULL;
}
void openlog(const char* i, int o, int f) { (void)i; (void)o; (void)f; }
void vsyslog(int pri, const char* fmt, va_list ap) { (void)pri; vfprintf(stderr, fmt, ap); fputc('\n', stderr); }
void syslog(int pri, const char* fmt, ...) { va_list ap; va_start(ap, fmt); vsyslog(pri, fmt, ap); va_end(ap); }
void closelog(void) {}
int setlogmask(int m) { return m; }

/* ===================================================================== */
/* 7. strings, stdlib, ctype, wide chars (C locale)                       */
/*    TODO(integration): move these generic ones to libc                  */
/* ===================================================================== */
char* strerror(int e) {
    switch (e) {
    case 0: return (char*)"Success";
    case ENXIO: return (char*)"No such device or address";
    case E2BIG: return (char*)"Argument list too long";
    case ENOEXEC: return (char*)"Exec format error";
    case EBUSY: return (char*)"Device or resource busy";
    case EXDEV: return (char*)"Invalid cross-device link";
    case ENODEV: return (char*)"No such device";
    case ENFILE: return (char*)"Too many open files in system";
    case ETXTBSY: return (char*)"Text file busy";
    case EFBIG: return (char*)"File too large";
    case ESPIPE: return (char*)"Illegal seek";
    case EROFS: return (char*)"Read-only file system";
    case EMLINK: return (char*)"Too many links";
    case EDOM: return (char*)"Numerical argument out of domain";
    case ENAMETOOLONG: return (char*)"File name too long";
    case ENOTEMPTY: return (char*)"Directory not empty";
    case ELOOP: return (char*)"Too many levels of symbolic links";
    case ENODATA: return (char*)"No data available";
    case EOVERFLOW: return (char*)"Value too large for defined data type";
    case EILSEQ: return (char*)"Invalid or incomplete multibyte or wide character";
    case ENOTSOCK: return (char*)"Socket operation on non-socket";
    case EDESTADDRREQ: return (char*)"Destination address required";
    case EPROTONOSUPPORT: return (char*)"Protocol not supported";
    case EOPNOTSUPP: return (char*)"Operation not supported";
    case EAFNOSUPPORT: return (char*)"Address family not supported by protocol";
    case EADDRINUSE: return (char*)"Address already in use";
    case ENETUNREACH: return (char*)"Network is unreachable";
    case ECONNABORTED: return (char*)"Software caused connection abort";
    case ECONNRESET: return (char*)"Connection reset by peer";
    case ENOTCONN: return (char*)"Transport endpoint is not connected";
    case ETIMEDOUT: return (char*)"Connection timed out";
    case ECONNREFUSED: return (char*)"Connection refused";
    case EHOSTUNREACH: return (char*)"No route to host";
    case EINPROGRESS: return (char*)"Operation now in progress";
    case EEXIST: return (char*)"File exists";
    default: return sys_strerror(e);
    }
}
int strerror_r(int e, char* b, size_t l) { strncpy(b, strerror(e), l); if (l) b[l - 1] = 0; return 0; }
char* strndup(const char* s, size_t n) { size_t l = strnlen(s, n); char* p = malloc(l + 1); if (p) { memcpy(p, s, l); p[l] = 0; } return p; }
char* strchrnul(const char* s, int c) { while (*s && *s != (char)c) s++; return (char*)s; }
char* strsep(char** sp, const char* d) {
    char* s = *sp; if (!s) return NULL;
    char* e = s + strcspn(s, d);
    if (*e) { *e = 0; *sp = e + 1; } else *sp = NULL;
    return s;
}
char* strcasestr(const char* h, const char* n) {
    size_t l = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, l)) return (char*)h;
    return l ? NULL : (char*)h;
}
char* stpcpy(char* d, const char* s) { while ((*d = *s)) { d++; s++; } return d; }
char* stpncpy(char* d, const char* s, size_t n) { size_t l = strnlen(s, n); memcpy(d, s, l); memset(d + l, 0, n - l); return d + l; }
void* mempcpy(void* d, const void* s, size_t n) { return (char*)memcpy(d, s, n) + n; }
void* memrchr(const void* s, int c, size_t n) { const unsigned char* p = (const unsigned char*)s + n; while (n--) if (*--p == (unsigned char)c) return (void*)p; return NULL; }
void* rawmemchr(const void* s, int c) { const unsigned char* p = s; while (*p != (unsigned char)c) p++; return (void*)p; }
void* memmem(const void* h, size_t hl, const void* n, size_t nl) {
    if (!nl) return (void*)h;
    for (const char* p = h; hl >= nl; p++, hl--) if (!memcmp(p, n, nl)) return (void*)p;
    return NULL;
}
void* memccpy(void* d, const void* s, int c, size_t n) {
    unsigned char* a = d; const unsigned char* b = s;
    while (n--) { if ((*a++ = *b++) == (unsigned char)c) return a; }
    return NULL;
}
size_t strlcat(char* d, const char* s, size_t n) { size_t l = strnlen(d, n); if (l == n) return n + strlen(s); return l + strlcpy(d + l, s, n - l); }
char* strtok_r(char* s, const char* d, char** save) {
    if (!s) s = *save;
    s += strspn(s, d);
    if (!*s) { *save = s; return NULL; }
    char* e = s + strcspn(s, d);
    if (*e) { *e = 0; *save = e + 1; } else *save = e;
    return s;
}
int strverscmp(const char* a, const char* b) {
    const unsigned char* l = (const void*)a; const unsigned char* r = (const void*)b;
    size_t i, dp, j; int z = 1;
    for (dp = i = 0; l[i] == r[i]; i++) {
        int c = l[i]; if (!c) return 0;
        if (!isdigit(c)) dp = i + 1, z = 1; else if (c != '0') z = 0;
    }
    if (l[dp] - '1' < 9U && r[dp] - '1' < 9U) {
        for (j = i; isdigit(l[j]); j++) if (!isdigit(r[j])) return 1;
        if (isdigit(r[j])) return -1;
    } else if (z && dp < i && (isdigit(l[i]) || isdigit(r[i]))) {
        return (unsigned char)(l[i] - '0') - (unsigned char)(r[i] - '0');
    }
    return l[i] - r[i];
}
char* basename(const char* p) { const char* s = strrchr(p, '/'); return (char*)(s ? s + 1 : p); }
void bzero(void* p, size_t n) { memset(p, 0, n); }
int bcmp(const void* a, const void* b, size_t n) { return memcmp(a, b, n); }
void explicit_bzero(void* p, size_t n) { volatile unsigned char* q = p; while (n--) *q++ = 0; }
char* index(const char* s, int c) { return strchr(s, c); }
char* rindex(const char* s, int c) { return strrchr(s, c); }
int ffs(int v) { return __builtin_ffs(v); }
int ffsl(long v) { return __builtin_ffsl(v); }
long long llabs(long long x) { return x < 0 ? -x : x; }
div_t div(int n, int d) { div_t r; r.quot = n / d; r.rem = n % d; return r; }
ldiv_t ldiv(long n, long d) { ldiv_t r; r.quot = n / d; r.rem = n % d; return r; }
intmax_t strtoimax(const char* s, char** e, int b) { return strtoll(s, e, b); }
uintmax_t strtoumax(const char* s, char** e, int b) { return strtoull(s, e, b); }
intmax_t imaxabs(intmax_t v) { return v < 0 ? -v : v; }
long double strtold(const char* s, char** e) { return sys_strtod(s, e); }
static unsigned long g_rnd = 1;
long random(void) { g_rnd = g_rnd * 6364136223846793005UL + 1442695040888963407UL; return (long)(g_rnd >> 33); }
void srandom(unsigned int s) { g_rnd = s ? s : 1; }
char* initstate(unsigned s, char* st, size_t n) { (void)n; srandom(s); return st; }
int rand_r(unsigned* s) { *s = *s * 1103515245U + 12345U; return (int)((*s >> 16) & 0x7fff); }
long lrand48(void) { return random(); }
double drand48(void) { return (double)(random() & 0x7fffffff) / 2147483648.0; }
void srand48(long s) { srandom((unsigned)s); }
static void (*g_atexit[16])(void);
static int g_natexit;
int atexit(void (*fn)(void)) { if (g_natexit >= 16) return -1; g_atexit[g_natexit++] = fn; return 0; }
int clearenv(void) { while (environ && environ[0]) { char n[256]; size_t k = strcspn(environ[0], "="); if (k > 255) k = 255; memcpy(n, environ[0], k); n[k] = 0; unsetenv(n); } return 0; }
char* secure_getenv(const char* n) { return getenv(n); }
static unsigned long g_tmpseq;
static int fill_tmpl(char* t, int suffix) {
    size_t l = strlen(t);
    if (l < 6 + (size_t)suffix) { errno = EINVAL; return -1; }
    char* x = t + l - suffix - 6;
    if (memcmp(x, "XXXXXX", 6)) { errno = EINVAL; return -1; }
    unsigned long v = getticks() * 2654435761UL + (unsigned long)getpid() * 40503UL + ++g_tmpseq * 977UL;
    for (int i = 0; i < 6; i++) { x[i] = "abcdefghijklmnopqrstuvwxyz0123456789"[v % 36]; v /= 36; }
    return 0;
}
int mkstemps(char* t, int suffix) {
    char save[PATH_MAX]; strncpy(save, t, sizeof(save) - 1); save[sizeof(save) - 1] = 0;
    for (int tries = 0; tries < 100; tries++) {
        strcpy(t, save);
        if (fill_tmpl(t, suffix) < 0) return -1;
        int fd = open(t, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    errno = EEXIST; return -1;
}
int mkstemp(char* t) { return mkstemps(t, 0); }
char* mkdtemp(char* t) {
    char save[PATH_MAX]; strncpy(save, t, sizeof(save) - 1); save[sizeof(save) - 1] = 0;
    for (int tries = 0; tries < 100; tries++) {
        strcpy(t, save);
        if (fill_tmpl(t, 0) < 0) return NULL;
        if (mkdir(t, 0700) == 0) return t;
        if (errno != EEXIST) return NULL;
    }
    return NULL;
}
char* mktemp(char* t) { if (fill_tmpl(t, 0) < 0) t[0] = 0; return t; }
char* tmpnam(char* s) { static char b[32]; char* p = s ? s : b; strcpy(p, "/tmp/tmpXXXXXX"); return mktemp(p); }
char* tempnam(const char* d, const char* pfx) { char b[PATH_MAX]; snprintf(b, sizeof(b), "%s/%sXXXXXX", d ? d : "/tmp", pfx ? pfx : "tmp"); mktemp(b); return strdup(b); }
void* aligned_alloc(size_t al, size_t n) { void* p; return posix_memalign(&p, al, n) ? NULL : p; }
int posix_memalign(void** out, size_t al, size_t n) {
    /* libc malloc returns 8-aligned blocks; over-allocate (blocks are never freed back to the OS) */
    if (al <= 8) { *out = malloc(n); return *out ? 0 : ENOMEM; }
    char* p = malloc(n + al);
    if (!p) return ENOMEM;
    *out = (void*)(((uintptr_t)p + al - 1) & ~(uintptr_t)(al - 1));
    return 0;
}
void* memalign(size_t al, size_t n) { return aligned_alloc(al, n); }
void* valloc(size_t n) { return aligned_alloc(4096, n); }
int getsubopt(char** opt, char* const* tok, char** val) {
    char* s = *opt; char* e = strchrnul(s, ',');
    *opt = *e ? e + 1 : e; *e = 0;
    char* eq = strchr(s, '='); if (eq) { *eq = 0; *val = eq + 1; } else *val = NULL;
    for (int i = 0; tok[i]; i++) if (!strcmp(tok[i], s)) return i;
    *val = s; return -1;
}
static void qsr_swap(char* a, char* b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }
void qsort_r(void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*, void*), void* arg) {
    char* b = base;   /* insertion sort: only used for small sets */
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && cmp(b + (j - 1) * sz, b + j * sz, arg) > 0; j--) qsr_swap(b + (j - 1) * sz, b + j * sz, sz);
}

/* wide chars: C locale, one byte = one character */
int mbtowc(wchar_t* wc, const char* s, size_t n) { if (!s) return 0; if (!n) return -1; if (wc) *wc = (unsigned char)*s; return *s ? 1 : 0; }
int wctomb(char* s, wchar_t wc) { if (!s) return 0; if ((unsigned)wc > 255) { errno = EILSEQ; return -1; } *s = (char)wc; return 1; }
int mblen(const char* s, size_t n) { return mbtowc(NULL, s, n); }
size_t mbstowcs(wchar_t* d, const char* s, size_t n) { size_t i = 0; for (; s[i] && (!d || i < n); i++) if (d) d[i] = (unsigned char)s[i]; if (d && i < n) d[i] = 0; return i; }
size_t wcstombs(char* d, const wchar_t* s, size_t n) { size_t i = 0; for (; s[i] && (!d || i < n); i++) if (d) d[i] = (char)s[i]; if (d && i < n) d[i] = 0; return i; }
size_t mbrtowc(wchar_t* wc, const char* s, size_t n, mbstate_t* st) { (void)st; if (!s) return 0; if (!n) return (size_t)-2; if (wc) *wc = (unsigned char)*s; return *s ? 1 : 0; }
size_t wcrtomb(char* s, wchar_t wc, mbstate_t* st) { (void)st; if (!s) return 1; *s = (char)wc; return 1; }
int mbsinit(const mbstate_t* st) { (void)st; return 1; }
size_t mbrlen(const char* s, size_t n, mbstate_t* st) { return mbrtowc(NULL, s, n, st); }
size_t mbsrtowcs(wchar_t* d, const char** s, size_t n, mbstate_t* st) { (void)st; size_t r = mbstowcs(d, *s, n); if (d && r < n) *s = NULL; else if (d) *s += r; return r; }
size_t wcsrtombs(char* d, const wchar_t** s, size_t n, mbstate_t* st) { (void)st; size_t r = wcstombs(d, *s, n); if (d && r < n) *s = NULL; else if (d) *s += r; return r; }
int wcwidth(wchar_t c) { return c == 0 ? 0 : (c < 32 || c == 127) ? -1 : 1; }
int wcswidth(const wchar_t* s, size_t n) { int w = 0; for (size_t i = 0; i < n && s[i]; i++) { int k = wcwidth(s[i]); if (k < 0) return -1; w += k; } return w; }
size_t wcslen(const wchar_t* s) { size_t n = 0; while (s[n]) n++; return n; }
wchar_t* wcschr(const wchar_t* s, wchar_t c) { for (; *s; s++) if (*s == c) return (wchar_t*)s; return c ? NULL : (wchar_t*)s; }
int wcscmp(const wchar_t* a, const wchar_t* b) { while (*a && *a == *b) { a++; b++; } return *a < *b ? -1 : *a > *b; }
int wcsncmp(const wchar_t* a, const wchar_t* b, size_t n) { for (; n && *a && *a == *b; n--) { a++; b++; } return n ? (*a < *b ? -1 : *a > *b) : 0; }
wchar_t* wcscpy(wchar_t* d, const wchar_t* s) { wchar_t* r = d; while ((*d++ = *s++)) {} return r; }
wint_t btowc(int c) { return c == EOF ? WEOF : (wint_t)(unsigned char)c; }
int wctob(wint_t c) { return c < 256 ? (int)c : EOF; }
int iswalnum(wint_t c) { return c < 128 && isalnum((int)c); }
int iswalpha(wint_t c) { return c < 128 && isalpha((int)c); }
int iswblank(wint_t c) { return c == ' ' || c == '\t'; }
int iswcntrl(wint_t c) { return c < 32 || c == 127; }
int iswdigit(wint_t c) { return c >= '0' && c <= '9'; }
int iswgraph(wint_t c) { return c > 32 && c < 127; }
int iswlower(wint_t c) { return c >= 'a' && c <= 'z'; }
int iswprint(wint_t c) { return c >= 32 && c < 127; }
int iswpunct(wint_t c) { return c < 128 && ispunct((int)c); }
int iswspace(wint_t c) { return c < 128 && isspace((int)c); }
int iswupper(wint_t c) { return c >= 'A' && c <= 'Z'; }
int iswxdigit(wint_t c) { return c < 128 && isxdigit((int)c); }
wint_t towlower(wint_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
wint_t towupper(wint_t c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static const char* const g_wct[] = { "", "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower",
                                      "print", "punct", "space", "upper", "xdigit", 0 };
wctype_t wctype(const char* n) { for (int i = 1; g_wct[i]; i++) if (!strcmp(n, g_wct[i])) return (wctype_t)i; return 0; }
int iswctype(wint_t c, wctype_t t) {
    switch (t) {
    case 1: return iswalnum(c); case 2: return iswalpha(c); case 3: return iswblank(c);
    case 4: return iswcntrl(c); case 5: return iswdigit(c); case 6: return iswgraph(c);
    case 7: return iswlower(c); case 8: return iswprint(c); case 9: return iswpunct(c);
    case 10: return iswspace(c); case 11: return iswupper(c); case 12: return iswxdigit(c);
    default: return 0;
    }
}

/* termios speed helpers (stored in the CBAUD bits of c_cflag) */
/* cf*speed / tcflush / tcdrain / tcflow / tcsendbreak / tcgetsid: libc. */

/* ===================================================================== */
/* 8. netdb: numeric addresses, /etc/hosts, and a minimal UDP DNS client  */
/* ===================================================================== */
int h_errno;
struct __res_state _res;
int res_init(void) {
    char buf[1024];
    memset(&_res, 0, sizeof(_res));
    _res.retrans = 2; _res.retry = 2;
    if (read_small("/etc/resolv.conf", buf, sizeof(buf)) > 0) {
        char* p = buf;
        while (p && *p && _res.nscount < MAXNS) {
            if (!strncmp(p, "nameserver", 10)) {
                char* a = p + 10; while (*a == ' ' || *a == '\t') a++;
                char ip[32]; size_t k = strcspn(a, " \t\r\n"); if (k > 31) k = 31; memcpy(ip, a, k); ip[k] = 0;
                struct in_addr in;
                if (inet_aton(ip, &in)) {
                    _res.nsaddr_list[_res.nscount].sin_family = AF_INET;
                    _res.nsaddr_list[_res.nscount].sin_port = htons(53);
                    _res.nsaddr_list[_res.nscount++].sin_addr = in;
                }
            }
            p = strchr(p, '\n'); if (p) p++;
        }
    }
    if (!_res.nscount) {   /* QEMU user-mode networking's resolver */
        _res.nsaddr_list[0].sin_family = AF_INET;
        _res.nsaddr_list[0].sin_port = htons(53);
        _res.nsaddr_list[0].sin_addr.s_addr = htonl(0x0a000203);
        _res.nscount = 1;
    }
    _res.options |= RES_INIT;
    return 0;
}
in_addr_t inet_addr(const char* s) { struct in_addr a; return inet_aton(s, &a) ? a.s_addr : INADDR_NONE; }
int inet_aton(const char* s, struct in_addr* a) {
    unsigned long v[4]; int n = 0; const char* p = s;
    while (n < 4) {
        char* e; if (!isdigit((unsigned char)*p)) return 0;
        v[n++] = strtoul(p, &e, 0); p = e;
        if (*p == '.') { p++; continue; }
        break;
    }
    if (*p) return 0;
    unsigned long ip;
    switch (n) {
    case 1: ip = v[0]; break;
    case 2: if (v[0] > 255 || v[1] > 0xffffff) return 0; ip = v[0] << 24 | v[1]; break;
    case 3: if (v[0] > 255 || v[1] > 255 || v[2] > 0xffff) return 0; ip = v[0] << 24 | v[1] << 16 | v[2]; break;
    default: for (int i = 0; i < 4; i++) if (v[i] > 255) return 0; ip = v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]; break;
    }
    if (ip > 0xffffffffUL) return 0;
    if (a) a->s_addr = htonl((uint32_t)ip);
    return 1;
}
char* inet_ntoa(struct in_addr a) {
    static char b[16]; const unsigned char* p = (const unsigned char*)&a.s_addr;
    snprintf(b, sizeof(b), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]); return b;
}
int inet_pton(int af, const char* s, void* d) {
    if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    int dots = 0; for (const char* p = s; *p; p++) { if (*p == '.') dots++; else if (!isdigit((unsigned char)*p)) return 0; }
    if (dots != 3) return 0;
    return inet_aton(s, (struct in_addr*)d);
}
const char* inet_ntop(int af, const void* s, char* d, socklen_t l) {
    if (af != AF_INET) { errno = EAFNOSUPPORT; return NULL; }
    const unsigned char* p = s; char b[16];
    snprintf(b, sizeof(b), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    if (strlen(b) + 1 > l) { errno = ENOSPC; return NULL; }
    strcpy(d, b); return d;
}
in_addr_t inet_network(const char* s) { struct in_addr a; return inet_aton(s, &a) ? ntohl(a.s_addr) : INADDR_NONE; }
struct in_addr inet_makeaddr(in_addr_t n, in_addr_t h) { struct in_addr a; a.s_addr = htonl(n << 8 | h); return a; }
in_addr_t inet_lnaof(struct in_addr a) { return ntohl(a.s_addr) & 0xff; }
in_addr_t inet_netof(struct in_addr a) { return ntohl(a.s_addr) >> 8; }
const struct in6_addr in6addr_any, in6addr_loopback;

static int hosts_lookup(const char* name, struct in_addr* out) {
    char buf[2048];
    if (!strcmp(name, "localhost")) { out->s_addr = htonl(INADDR_LOOPBACK); return 1; }
    if (read_small("/etc/hosts", buf, sizeof(buf)) <= 0) return 0;
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char* hash = strchr(line, '#'); if (hash) *hash = 0;
        char ip[32]; int n = 0; while (*line && !isspace((unsigned char)*line) && n < 31) ip[n++] = *line++; ip[n] = 0;
        while (*line) {
            while (isspace((unsigned char)*line)) line++;
            char* w = line; while (*line && !isspace((unsigned char)*line)) line++;
            if (line > w && (size_t)(line - w) == strlen(name) && !strncasecmp(w, name, strlen(name)) && inet_aton(ip, out)) return 1;
        }
    }
    return 0;
}
/* One A query over UDP to the first nameserver. Returns 1 and the address. */
static int dns_query_a(const char* name, struct in_addr* out) {
    unsigned char q[512], r[512];
    if (!(_res.options & RES_INIT)) res_init();
    size_t nl = strlen(name);
    if (nl > 250) return 0;
    uint16_t id = (uint16_t)(getticks() ^ (unsigned)getpid());
    memset(q, 0, 12);
    q[0] = id >> 8; q[1] = id & 0xff; q[2] = 0x01; q[5] = 1;   /* RD, QDCOUNT=1 */
    size_t k = 12; const char* p = name;
    while (*p) {
        const char* e = strchrnul(p, '.'); size_t l = (size_t)(e - p);
        if (!l || l > 63) return 0;
        q[k++] = (unsigned char)l; memcpy(q + k, p, l); k += l;
        p = *e ? e + 1 : e;
    }
    q[k++] = 0; q[k++] = 0; q[k++] = 1; q[k++] = 0; q[k++] = 1;   /* QTYPE=A, QCLASS=IN */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    int ok = 0;
    for (int tries = 0; tries < 4 && !ok; tries++) {
        /* the first datagram to an unresolved next hop fails while ARP runs */
        if (sendto(fd, q, k, 0, (struct sockaddr*)&_res.nsaddr_list[0], sizeof(struct sockaddr_in)) < 0) {
            sleep_ticks(250); continue;
        }
        ssize_t n = recvfrom(fd, r, sizeof(r), 0, NULL, NULL);
        if (n < 12 || r[0] != q[0] || r[1] != q[1] || (r[3] & 0x0f)) continue;
        int an = r[6] << 8 | r[7];
        size_t off = 12;
        for (int qd = r[4] << 8 | r[5]; qd > 0 && off < (size_t)n; qd--) {   /* skip questions */
            while (off < (size_t)n && r[off] && !(r[off] & 0xc0)) off += r[off] + 1;
            off += (off < (size_t)n && (r[off] & 0xc0)) ? 2 : 1; off += 4;
        }
        for (; an > 0 && off + 10 <= (size_t)n; an--) {
            while (off < (size_t)n && r[off] && !(r[off] & 0xc0)) off += r[off] + 1;
            off += (off < (size_t)n && (r[off] & 0xc0)) ? 2 : 1;
            if (off + 10 > (size_t)n) break;
            int type = r[off] << 8 | r[off + 1], rdl = r[off + 8] << 8 | r[off + 9];
            off += 10;
            if (type == 1 && rdl == 4 && off + 4 <= (size_t)n) { memcpy(&out->s_addr, r + off, 4); ok = 1; break; }
            off += (size_t)rdl;
        }
    }
    close(fd);
    return ok;
}
static int resolve4(const char* name, struct in_addr* out) {
    if (inet_aton(name, out)) return 1;
    if (hosts_lookup(name, out)) return 1;
    return dns_query_a(name, out);
}
struct hostent* gethostbyname(const char* name) {
    static struct hostent h; static struct in_addr a; static char* list[2]; static char* al[1]; static char nm[256];
    if (!resolve4(name, &a)) { h_errno = HOST_NOT_FOUND; return NULL; }
    strncpy(nm, name, sizeof(nm) - 1);
    list[0] = (char*)&a; list[1] = NULL; al[0] = NULL;
    h.h_name = nm; h.h_aliases = al; h.h_addrtype = AF_INET; h.h_length = 4; h.h_addr_list = list;
    return &h;
}
struct hostent* gethostbyname2(const char* n, int af) { if (af != AF_INET) { h_errno = HOST_NOT_FOUND; return NULL; } return gethostbyname(n); }
struct hostent* gethostbyaddr(const void* a, socklen_t l, int t) {
    static struct hostent h; static struct in_addr ad; static char* list[2]; static char* al[1]; static char nm[32];
    if (t != AF_INET || l != 4) { h_errno = HOST_NOT_FOUND; return NULL; }
    memcpy(&ad, a, 4); inet_ntop(AF_INET, &ad, nm, sizeof(nm));
    list[0] = (char*)&ad; list[1] = NULL; al[0] = NULL;
    h.h_name = nm; h.h_aliases = al; h.h_addrtype = AF_INET; h.h_length = 4; h.h_addr_list = list;
    return &h;
}
static const struct { const char* n; int port; } g_services[] = {
    { "echo", 7 }, { "discard", 9 }, { "daytime", 13 }, { "ftp-data", 20 }, { "ftp", 21 }, { "ssh", 22 },
    { "telnet", 23 }, { "smtp", 25 }, { "time", 37 }, { "domain", 53 }, { "tftp", 69 }, { "gopher", 70 },
    { "finger", 79 }, { "http", 80 }, { "www", 80 }, { "pop3", 110 }, { "ntp", 123 }, { "imap", 143 },
    { "https", 443 }, { "http-alt", 8080 }, { 0, 0 } };
struct servent* getservbyname(const char* name, const char* proto) {
    static struct servent s; static char* al[1];
    for (int i = 0; g_services[i].n; i++) if (!strcmp(g_services[i].n, name)) {
        s.s_name = (char*)g_services[i].n; s.s_aliases = al; s.s_port = htons(g_services[i].port);
        s.s_proto = (char*)(proto ? proto : "tcp"); return &s;
    }
    return NULL;
}
struct servent* getservbyport(int port, const char* proto) {
    static struct servent s; static char* al[1];
    for (int i = 0; g_services[i].n; i++) if (htons(g_services[i].port) == port) {
        s.s_name = (char*)g_services[i].n; s.s_aliases = al; s.s_port = port; s.s_proto = (char*)(proto ? proto : "tcp"); return &s;
    }
    return NULL;
}
struct protoent* getprotobyname(const char* n) {
    static struct protoent p; static char* al[1];
    int v = !strcmp(n, "tcp") ? 6 : !strcmp(n, "udp") ? 17 : !strcmp(n, "icmp") ? 1 : !strcmp(n, "ip") ? 0 : -1;
    if (v < 0) return NULL;
    p.p_name = (char*)n; p.p_aliases = al; p.p_proto = v; return &p;
}
struct protoent* getprotobynumber(int n) { return getprotobyname(n == 6 ? "tcp" : n == 17 ? "udp" : n == 1 ? "icmp" : n == 0 ? "ip" : "?"); }
int getaddrinfo(const char* node, const char* svc, const struct addrinfo* hints, struct addrinfo** res) {
    int fam = hints ? hints->ai_family : AF_UNSPEC, st = hints ? hints->ai_socktype : 0, fl = hints ? hints->ai_flags : 0;
    if (fam != AF_UNSPEC && fam != AF_INET) return EAI_FAMILY;
    struct in_addr a;
    if (!node) a.s_addr = (fl & AI_PASSIVE) ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    else if ((fl & AI_NUMERICHOST) ? !inet_aton(node, &a) : !resolve4(node, &a)) return EAI_NONAME;
    int port = 0;
    if (svc && *svc) {
        char* e; long v = strtol(svc, &e, 10);
        if (!*e) port = (int)v;
        else { struct servent* s = (fl & AI_NUMERICSERV) ? NULL : getservbyname(svc, NULL); if (!s) return EAI_SERVICE; port = ntohs(s->s_port); }
    }
    struct { struct addrinfo ai; struct sockaddr_in sin; char name[256]; }* r = calloc(1, sizeof(*r));
    if (!r) return EAI_MEMORY;
    r->sin.sin_family = AF_INET; r->sin.sin_port = htons((uint16_t)port); r->sin.sin_addr = a;
    r->ai.ai_family = AF_INET; r->ai.ai_socktype = st ? st : SOCK_STREAM;
    r->ai.ai_protocol = r->ai.ai_socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
    r->ai.ai_addrlen = sizeof(r->sin); r->ai.ai_addr = (struct sockaddr*)&r->sin;
    if (fl & AI_CANONNAME) { strncpy(r->name, node ? node : "localhost", sizeof(r->name) - 1); r->ai.ai_canonname = r->name; }
    *res = &r->ai;
    return 0;
}
void freeaddrinfo(struct addrinfo* ai) { free(ai); }
const char* gai_strerror(int e) {
    switch (e) {
    case EAI_NONAME: return "Name or service not known";
    case EAI_AGAIN: return "Temporary failure in name resolution";
    case EAI_FAIL: return "Non-recoverable failure in name resolution";
    case EAI_FAMILY: return "Address family not supported";
    case EAI_SERVICE: return "Service not supported";
    case EAI_MEMORY: return "Memory allocation failure";
    default: return "Unknown error";
    }
}
int getnameinfo(const struct sockaddr* sa, socklen_t sl, char* host, socklen_t hl, char* serv, socklen_t svl, int fl) {
    (void)fl;
    if (!sa || sa->sa_family != AF_INET || sl < sizeof(struct sockaddr_in)) return EAI_FAMILY;
    const struct sockaddr_in* in = (const struct sockaddr_in*)sa;
    if (host && hl && !inet_ntop(AF_INET, &in->sin_addr, host, hl)) return EAI_OVERFLOW;
    if (serv && svl) snprintf(serv, svl, "%u", ntohs(in->sin_port));
    return 0;
}
const char* hstrerror(int e) { return e == HOST_NOT_FOUND ? "Unknown host" : e == TRY_AGAIN ? "Host name lookup failure" : "Unknown resolver error"; }
void herror(const char* s) { fprintf(stderr, "%s%s%s\n", s ? s : "", s ? ": " : "", hstrerror(h_errno)); }
struct ether_addr;
char* ether_ntoa(const struct ether_addr* a) {
    static char b[18]; const unsigned char* p = (const unsigned char*)a;
    snprintf(b, sizeof(b), "%x:%x:%x:%x:%x:%x", p[0], p[1], p[2], p[3], p[4], p[5]); return b;
}

/* ===================================================================== */
/* 9. private stdio (bbio_*)                                              */
/* ===================================================================== */
static struct bbio_file g_stdin_f  = { 0, BBIO_R, _IOLBF, 0, NULL, 0, 0, 0, 0, -1, 0, NULL, { 0 } };
static struct bbio_file g_stdout_f = { 1, BBIO_W, _IOLBF, 0, NULL, 0, 0, 0, 0, -1, 0, NULL, { 0 } };
static struct bbio_file g_stderr_f = { 2, BBIO_W, _IONBF, 0, NULL, 0, 0, 0, 0, -1, 0, NULL, { 0 } };
FILE* stdin  = &g_stdin_f;
FILE* stdout = &g_stdout_f;
FILE* stderr = &g_stderr_f;
static FILE* g_files;   /* heap-allocated streams, for fflush(NULL) */

static int bbio_ensure_buf(FILE* f) {
    if (f->buf) return 0;
    if (f->mode == _IONBF && (f->flags & BBIO_W) && !(f->flags & BBIO_R)) return 0;
    f->size = BUFSIZ;
    f->buf = malloc(f->size);
    if (!f->buf) { f->size = 0; return -1; }
    f->own_buf = 1;
    return 0;
}
static int bbio_flush_w(FILE* f) {
    size_t off = 0;
    while (off < f->wlen) {
        ssize_t r = write(f->fd, f->buf + off, f->wlen - off);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; f->flags |= BBIO_ERR; f->wlen = 0; return EOF; }
        off += (size_t)r;
    }
    f->wlen = 0;
    return 0;
}
/* Discard read-ahead so the fd offset matches the logical position. */
static void bbio_drop_r(FILE* f) {
    if (f->rlen > f->rpos) lseek(f->fd, -(long)(f->rlen - f->rpos), SEEK_CUR);
    f->rpos = f->rlen = 0;
    f->ungot = -1;
}
int fflush(FILE* f) {
    if (!f) {
        fflush(stdout); fflush(stderr);
        for (FILE* p = g_files; p; p = p->next) fflush(p);
        return 0;
    }
    if (f->wlen) return bbio_flush_w(f);
    return 0;
}
static int bbio_fill(FILE* f) {
    if (f->wlen) bbio_flush_w(f);
    if (f == stdin) fflush(stdout);   /* interactive prompt before a read */
    if (bbio_ensure_buf(f) < 0) return EOF;
    for (;;) {
        ssize_t r = read(f->fd, f->buf, f->size);
        if (r > 0) { f->rpos = 0; f->rlen = (size_t)r; return 0; }
        if (r < 0 && errno == EINTR) { f->flags |= BBIO_ERR; return EOF; }
        if (r == 0) f->flags |= BBIO_EOF; else f->flags |= BBIO_ERR;
        return EOF;
    }
}
int fgetc(FILE* f) {
    if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
    if (f->rpos >= f->rlen && bbio_fill(f) == EOF) return EOF;
    return f->buf[f->rpos++];
}
int getc(FILE* f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }
int ungetc(int c, FILE* f) {
    if (c == EOF) return EOF;
    if (f->buf && f->rpos > 0 && f->rpos <= f->rlen) f->buf[--f->rpos] = (unsigned char)c;
    else f->ungot = (unsigned char)c;
    f->flags &= ~BBIO_EOF;
    return (unsigned char)c;
}
char* fgets(char* s, int n, FILE* f) {
    int i = 0;
    if (n <= 0) return NULL;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (!i) return NULL;
    s[i] = 0;
    return s;
}
ssize_t getdelim(char** line, size_t* n, int delim, FILE* f) {
    size_t i = 0;
    if (!line || !n) { errno = EINVAL; return -1; }
    if (!*line || !*n) { *n = 128; *line = malloc(*n); if (!*line) return -1; }
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) break;
        if (i + 2 > *n) {
            size_t nn = *n * 2; char* p = realloc(*line, nn);
            if (!p) return -1;
            *line = p; *n = nn;
        }
        (*line)[i++] = (char)c;
        if (c == delim) break;
    }
    (*line)[i] = 0;
    return i ? (ssize_t)i : -1;
}
ssize_t getline(char** l, size_t* n, FILE* f) { return getdelim(l, n, '\n', f); }
size_t fread(void* p, size_t sz, size_t nm, FILE* f) {
    size_t want = sz * nm, got = 0;
    unsigned char* d = p;
    if (!want) return 0;
    if (f->ungot >= 0) { d[got++] = (unsigned char)f->ungot; f->ungot = -1; }
    while (got < want) {
        if (f->rpos < f->rlen) {
            size_t k = f->rlen - f->rpos; if (k > want - got) k = want - got;
            memcpy(d + got, f->buf + f->rpos, k); f->rpos += k; got += k; continue;
        }
        if (want - got >= BUFSIZ) {   /* large read: bypass the buffer */
            if (f->wlen) bbio_flush_w(f);
            ssize_t r = read(f->fd, d + got, want - got);
            if (r > 0) { got += (size_t)r; continue; }
            f->flags |= r == 0 ? BBIO_EOF : BBIO_ERR; break;
        }
        if (bbio_fill(f) == EOF) break;
    }
    return got / sz;
}
static int bbio_putn(FILE* f, const char* s, size_t n) {
    if (f->rlen) bbio_drop_r(f);
    if (bbio_ensure_buf(f) < 0 || !f->buf) {   /* unbuffered */
        size_t off = 0;
        while (off < n) { ssize_t r = write(f->fd, s + off, n - off); if (r <= 0) { if (r < 0 && errno == EINTR) continue; f->flags |= BBIO_ERR; return EOF; } off += (size_t)r; }
        return 0;
    }
    if (f->mode == _IONBF) {
        if (f->wlen && bbio_flush_w(f)) return EOF;
        size_t off = 0;
        while (off < n) { ssize_t r = write(f->fd, s + off, n - off); if (r <= 0) { f->flags |= BBIO_ERR; return EOF; } off += (size_t)r; }
        return 0;
    }
    while (n) {
        size_t room = f->size - f->wlen;
        if (!room) { if (bbio_flush_w(f)) return EOF; room = f->size; }
        size_t k = n < room ? n : room;
        memcpy(f->buf + f->wlen, s, k); f->wlen += k; s += k; n -= k;
    }
    if (f->mode == _IOLBF && f->wlen && memchr(f->buf, '\n', f->wlen)) return bbio_flush_w(f);
    return 0;
}
size_t fwrite(const void* p, size_t sz, size_t nm, FILE* f) {
    size_t n = sz * nm;
    if (!n) return 0;
    if (f == stdout && stdin->rlen == 0) {}   /* nothing */
    return bbio_putn(f, p, n) == 0 ? nm : 0;
}
int fputc(int c, FILE* f) { char ch = (char)c; return bbio_putn(f, &ch, 1) == 0 ? (unsigned char)c : EOF; }
int putc(int c, FILE* f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }
int fputs(const char* s, FILE* f) { return bbio_putn(f, s, strlen(s)) == 0 ? 1 : EOF; }
int puts(const char* s) { if (fputs(s, stdout) == EOF) return EOF; return fputc('\n', stdout) == EOF ? EOF : 1; }
int fileno(FILE* f) { return f->fd; }
int feof(FILE* f) { return !!(f->flags & BBIO_EOF); }
int ferror(FILE* f) { return !!(f->flags & BBIO_ERR); }
void clearerr(FILE* f) { f->flags &= ~(BBIO_EOF | BBIO_ERR); }
int setvbuf(FILE* f, char* b, int m, size_t sz) {
    if (f->wlen) bbio_flush_w(f);
    f->mode = m;
    if (b && sz) {
        if (f->own_buf) free(f->buf);
        f->buf = (unsigned char*)b; f->size = sz; f->own_buf = 0;
    }
    return 0;
}
void setbuf(FILE* f, char* b) { setvbuf(f, b, b ? _IOFBF : _IONBF, BUFSIZ); }
void setbuffer(FILE* f, char* b, size_t s) { setvbuf(f, b, b ? _IOFBF : _IONBF, s); }
void setlinebuf(FILE* f) { setvbuf(f, NULL, _IOLBF, 0); }
static int parse_mode(const char* m) {
    int fl;
    switch (m[0]) {
    case 'r': fl = O_RDONLY; break;
    case 'w': fl = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': fl = O_WRONLY | O_CREAT | O_APPEND; break;
    default: errno = EINVAL; return -1;
    }
    if (strchr(m, '+')) fl = (fl & ~O_ACCMODE) | O_RDWR;
    if (strchr(m, 'x')) fl |= O_EXCL;
    return fl;
}
static FILE* bbio_new(int fd, int oflags) {
    FILE* f = calloc(1, sizeof(*f));
    if (!f) { errno = ENOMEM; return NULL; }
    f->fd = fd;
    int acc = oflags & O_ACCMODE;
    f->flags = (acc == O_RDONLY ? BBIO_R : acc == O_WRONLY ? BBIO_W : BBIO_R | BBIO_W);
    f->mode = isatty(fd) ? _IOLBF : _IOFBF;
    f->ungot = -1;
    f->next = g_files; g_files = f;
    return f;
}
FILE* fopen(const char* path, const char* mode) {
    int fl = parse_mode(mode);
    if (fl < 0) return NULL;
    int fd = open(path, fl, 0666);
    if (fd < 0) return NULL;
    FILE* f = bbio_new(fd, fl);
    if (!f) close(fd);
    return f;
}
FILE* fdopen(int fd, const char* mode) {
    int fl = parse_mode(mode);
    if (fl < 0) return NULL;
    return bbio_new(fd, fl);
}
static void bbio_unlink_list(FILE* f) {
    for (FILE** pp = &g_files; *pp; pp = &(*pp)->next) if (*pp == f) { *pp = f->next; return; }
}
int fclose(FILE* f) {
    int r = fflush(f);
    if (f->rlen) bbio_drop_r(f);
    int pid = f->pid;
    if (close(f->fd) < 0) r = EOF;
    if (f == stdin || f == stdout || f == stderr) { f->fd = -1; return r; }
    bbio_unlink_list(f);
    if (f->own_buf) free(f->buf);
    free(f);
    (void)pid;
    return r;
}
FILE* freopen(const char* path, const char* mode, FILE* f) {
    int fl = parse_mode(mode);
    if (fl < 0) return NULL;
    fflush(f);
    if (!path) return f;
    int fd = open(path, fl, 0666);
    if (fd < 0) return NULL;
    if (f->fd >= 0 && f->fd != fd) { dup2(fd, f->fd); close(fd); fd = f->fd; }
    f->fd = fd;
    int acc = fl & O_ACCMODE;
    f->flags = (acc == O_RDONLY ? BBIO_R : acc == O_WRONLY ? BBIO_W : BBIO_R | BBIO_W);
    f->rpos = f->rlen = f->wlen = 0; f->ungot = -1;
    return f;
}
FILE* popen(const char* cmd, const char* mode) {
    int fds[2];
    int rd = mode[0] == 'r';
    if (pipe(fds) < 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return NULL; }
    if (pid == 0) {
        if (rd) { dup2(fds[1], 1); } else { dup2(fds[0], 0); }
        close(fds[0]); close(fds[1]);
        execl("/bin/bash", "bash", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    close(rd ? fds[1] : fds[0]);
    FILE* f = bbio_new(rd ? fds[0] : fds[1], rd ? O_RDONLY : O_WRONLY);
    if (f) f->pid = pid;
    return f;
}
int pclose(FILE* f) {
    int pid = f->pid, st = -1;
    fflush(f);
    close(f->fd); f->fd = -1;
    bbio_unlink_list(f);
    if (f->own_buf) free(f->buf);
    free(f);
    if (pid > 0) while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return st;
}
FILE* tmpfile(void) {
    char t[] = "/tmp/tmpfXXXXXX";
    int fd = mkstemp(t);
    if (fd < 0) return NULL;
    unlink(t);
    return bbio_new(fd, O_RDWR);
}
FILE* fmemopen(void* b, size_t s, const char* m) { (void)b; (void)s; (void)m; errno = ENOSYS; return NULL; }
FILE* open_memstream(char** p, size_t* s) { (void)p; (void)s; errno = ENOSYS; return NULL; }
off_t ftello(FILE* f) {
    long pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0) { errno = ESPIPE; return -1; }
    return pos - (long)(f->rlen - f->rpos) - (f->ungot >= 0) + (long)f->wlen;
}
long ftell(FILE* f) { return (long)ftello(f); }
int fseeko(FILE* f, off_t off, int wh) {
    if (f->wlen && bbio_flush_w(f)) return -1;
    if (wh == SEEK_CUR) off -= (off_t)(f->rlen - f->rpos) + (f->ungot >= 0);
    f->rpos = f->rlen = 0; f->ungot = -1;
    if (lseek(f->fd, off, wh) < 0) { errno = ESPIPE; return -1; }
    f->flags &= ~BBIO_EOF;
    return 0;
}
int fseek(FILE* f, long off, int wh) { return fseeko(f, off, wh); }
void rewind(FILE* f) { fseeko(f, 0, SEEK_SET); clearerr(f); }
int fgetpos(FILE* f, fpos_t* p) { off_t o = ftello(f); if (o < 0) return -1; *p = o; return 0; }
int fsetpos(FILE* f, const fpos_t* p) { return fseeko(f, *p, SEEK_SET); }
void perror(const char* s) {
    if (s && *s) { fputs(s, stderr); fputs(": ", stderr); }
    fputs(strerror(errno), stderr); fputc('\n', stderr);
}
__attribute__((noreturn)) void exit(int code) {
    while (g_natexit > 0) g_atexit[--g_natexit]();
    fflush(NULL);
    sys_exit(code);
}

/* ---- printf engine ---- */
typedef struct { char* buf; size_t cap, len; FILE* f; int fd; int err; } pout_t;
static void po_put(pout_t* o, const char* s, size_t n) {
    if (o->f) { if (bbio_putn(o->f, s, n)) o->err = 1; }
    else if (o->fd >= 0) { if (write(o->fd, s, n) < 0) o->err = 1; }
    else if (o->buf) { for (size_t i = 0; i < n; i++) { if (o->len + 1 < o->cap) o->buf[o->len] = s[i]; o->len++; } return; }
    o->len += n;
}
static void po_pad(pout_t* o, char c, int n) { char b[32]; memset(b, c, sizeof(b)); while (n > 0) { int k = n > 32 ? 32 : n; po_put(o, b, (size_t)k); n -= k; } }
static int bb_vformat(pout_t* o, const char* fmt, va_list ap) {
    int saved_errno = errno;
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            const char* s = fmt; while (fmt[1] && fmt[1] != '%') fmt++;
            po_put(o, s, (size_t)(fmt - s + 1)); continue;
        }
        const char* spec = fmt++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1; else if (*fmt == '0') zero = 1; else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1; else if (*fmt == '#') alt = 1; else if (*fmt == '\'') {} else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
        else while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; if (prec < 0) prec = -1; }
            else while (isdigit((unsigned char)*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        int len = 0;   /* 0 int, 1 long, 2 long long, -1 short, -2 char, 3 size_t/ptrdiff/intmax, 4 long double */
        for (;; fmt++) {
            if (*fmt == 'l') len = len == 1 ? 2 : 1;
            else if (*fmt == 'h') len = len == -1 ? -2 : -1;
            else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j' || *fmt == 'q') len = 3;
            else if (*fmt == 'L') len = 4;
            else break;
        }
        char conv = *fmt;
        char nb[80]; const char* str = NULL; size_t sl = 0;
        const char* pfx = ""; int neg = 0;
        unsigned long long uv = 0; int base = 10, upper = 0, isnum = 0;
        switch (conv) {
        case 'd': case 'i': {
            long long v;
            if (len == 0) v = va_arg(ap, int); else if (len == -1) v = (short)va_arg(ap, int);
            else if (len == -2) v = (signed char)va_arg(ap, int); else v = va_arg(ap, long long);
            if (v < 0) { neg = 1; uv = (unsigned long long)(-(v + 1)) + 1; } else uv = (unsigned long long)v;
            isnum = 1; break;
        }
        case 'u': case 'o': case 'x': case 'X': case 'b':
            if (len == 0) uv = va_arg(ap, unsigned); else if (len == -1) uv = (unsigned short)va_arg(ap, unsigned);
            else if (len == -2) uv = (unsigned char)va_arg(ap, unsigned); else uv = va_arg(ap, unsigned long long);
            base = conv == 'o' ? 8 : conv == 'u' ? 10 : conv == 'b' ? 2 : 16; upper = conv == 'X'; isnum = 1;
            if (alt && uv) pfx = conv == 'o' ? "0" : conv == 'x' ? "0x" : conv == 'X' ? "0X" : "";
            break;
        case 'p': uv = (uintptr_t)va_arg(ap, void*); base = 16; pfx = "0x"; isnum = 1; break;
        case 'c': nb[0] = (char)va_arg(ap, int); str = nb; sl = 1; break;
        case 's': str = va_arg(ap, const char*); if (!str) str = "(null)";
                  sl = prec >= 0 ? strnlen(str, (size_t)prec) : strlen(str); break;
        case 'm': str = strerror(saved_errno); sl = strlen(str); break;
        case '%': str = "%"; sl = 1; break;
        case 'n': { int* p = va_arg(ap, int*); if (p) *p = (int)o->len; continue; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v = len == 4 ? (double)va_arg(ap, long double) : va_arg(ap, double);
            char sub[32]; size_t k = 0; sub[k++] = '%';
            if (left) sub[k++] = '-';
            if (zero) sub[k++] = '0';
            if (plus) sub[k++] = '+';
            if (space) sub[k++] = ' ';
            if (alt) sub[k++] = '#';
            sub[k++] = '*'; if (prec >= 0) { sub[k++] = '.'; sub[k++] = '*'; }
            sub[k++] = (conv == 'a' || conv == 'A') ? 'g' : conv; sub[k] = 0;
            char fb[512];
            int n = prec >= 0 ? sys_snprintf(fb, sizeof(fb), sub, width, prec > 300 ? 300 : prec, v)
                              : sys_snprintf(fb, sizeof(fb), sub, width, v);
            if (n > (int)sizeof(fb) - 1) n = (int)sizeof(fb) - 1;
            if (n > 0) po_put(o, fb, (size_t)n);
            continue;
        }
        case 0: fmt--; po_put(o, spec, (size_t)(fmt - spec + 1)); continue;
        default: po_put(o, spec, (size_t)(fmt - spec + 1)); continue;
        }
        if (isnum) {
            const char* digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
            char* q = nb + sizeof(nb);
            while (uv) { *--q = digs[uv % (unsigned)base]; uv /= (unsigned)base; }
            if (q == nb + sizeof(nb) && prec != 0) *--q = '0';
            int nd = (int)(nb + sizeof(nb) - q);
            int zp = prec > nd ? prec - nd : 0;
            if (conv == 'o' && alt && pfx[0] && zp) pfx = "";
            const char* sign = neg ? "-" : plus && (conv == 'd' || conv == 'i') ? "+" : space && (conv == 'd' || conv == 'i') ? " " : "";
            int tot = nd + zp + (int)strlen(sign) + (int)strlen(pfx);
            int pad = width > tot ? width - tot : 0;
            if (!left && !(zero && prec < 0)) po_pad(o, ' ', pad);
            po_put(o, sign, strlen(sign)); po_put(o, pfx, strlen(pfx));
            if (!left && zero && prec < 0) po_pad(o, '0', pad);
            po_pad(o, '0', zp);
            po_put(o, q, (size_t)nd);
            if (left) po_pad(o, ' ', pad);
        } else {
            int pad = width > (int)sl ? width - (int)sl : 0;
            if (!left) po_pad(o, zero ? '0' : ' ', pad);
            po_put(o, str, sl);
            if (left) po_pad(o, ' ', pad);
        }
    }
    return o->err ? -1 : (int)o->len;
}
int vfprintf(FILE* f, const char* fmt, va_list ap) { pout_t o = { NULL, 0, 0, f, -1, 0 }; return bb_vformat(&o, fmt, ap); }
int fprintf(FILE* f, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r; }
int vprintf(const char* fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int printf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(stdout, fmt, ap); va_end(ap); return r; }
int vdprintf(int fd, const char* fmt, va_list ap) {
    char b[1024]; va_list c; va_copy(c, ap);
    int n = vsnprintf(b, sizeof(b), fmt, c); va_end(c);
    if (n < (int)sizeof(b)) return write(fd, b, (size_t)n) < 0 ? -1 : n;
    pout_t o = { NULL, 0, 0, NULL, fd, 0 }; return bb_vformat(&o, fmt, ap);
}
int dprintf(int fd, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vdprintf(fd, fmt, ap); va_end(ap); return r; }
int vsnprintf(char* s, size_t n, const char* fmt, va_list ap) {
    pout_t o = { s, n, 0, NULL, -1, 0 };
    int r = bb_vformat(&o, fmt, ap);
    if (n) s[o.len < n ? o.len : n - 1] = 0;
    return r;
}
int snprintf(char* s, size_t n, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(s, n, fmt, ap); va_end(ap); return r; }
int vsprintf(char* s, const char* fmt, va_list ap) { return vsnprintf(s, (size_t)INT_MAX, fmt, ap); }
int sprintf(char* s, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsprintf(s, fmt, ap); va_end(ap); return r; }
int vasprintf(char** out, const char* fmt, va_list ap) {
    va_list c; va_copy(c, ap);
    int n = vsnprintf(NULL, 0, fmt, c); va_end(c);
    if (n < 0) return -1;
    *out = malloc((size_t)n + 1);
    if (!*out) return -1;
    return vsnprintf(*out, (size_t)n + 1, fmt, ap);
}
int asprintf(char** out, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vasprintf(out, fmt, ap); va_end(ap); return r; }

/* ---- scanf engine (string source; FILE source reads one line) ---- */
int vsscanf(const char* s, const char* fmt, va_list ap) {
    const char* p = s; int n = 0;
    if (!*p) return EOF;
    for (; *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) { while (isspace((unsigned char)*p)) p++; continue; }
        if (*fmt != '%') { if (*p != *fmt) return n; p++; continue; }
        fmt++;
        if (*fmt == '%') { while (isspace((unsigned char)*p)) p++; if (*p != '%') return n; p++; continue; }
        int skip = 0, width = 0, len = 0;
        if (*fmt == '*') { skip = 1; fmt++; }
        while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        for (;; fmt++) {
            if (*fmt == 'l') len = len == 1 ? 2 : 1; else if (*fmt == 'h') len = len == -1 ? -2 : -1;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'q') len = 2; else if (*fmt == 'L') len = 3; else break;
        }
        char c = *fmt;
        if (c != 'c' && c != '[' && c != 'n') while (isspace((unsigned char)*p)) p++;
        if (!*p && c != 'n') return n ? n : EOF;
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            char tmp[64]; int k = 0, lim = width ? width : 63;
            if (lim > 63) lim = 63;
            if ((*p == '-' || *p == '+') && k < lim) tmp[k++] = *p++;
            int base = c == 'x' || c == 'X' ? 16 : c == 'o' ? 8 : c == 'i' ? 0 : 10;
            while (k < lim && (isxdigit((unsigned char)*p) || ((*p == 'x' || *p == 'X') && base != 10 && base != 8))) {
                if (base == 10 && !isdigit((unsigned char)*p)) break;
                tmp[k++] = *p++;
            }
            tmp[k] = 0;
            if (!k || (k == 1 && (tmp[0] == '-' || tmp[0] == '+'))) return n;
            char* e; unsigned long long v;
            if (c == 'd' || c == 'i') v = (unsigned long long)strtoll(tmp, &e, base); else v = strtoull(tmp, &e, base);
            if (e == tmp) return n;
            if (!skip) {
                void* dst = va_arg(ap, void*);
                if (len == 2 || len == 1) *(unsigned long long*)dst = v;   /* long == long long on LP64 */
                else if (len == -1) *(unsigned short*)dst = (unsigned short)v;
                else if (len == -2) *(unsigned char*)dst = (unsigned char)v;
                else *(unsigned*)dst = (unsigned)v;
                n++;
            }
            break;
        }
        case 'f': case 'e': case 'g': case 'E': case 'G': case 'a': {
            char* e; double v = sys_strtod(p, &e);
            if (e == p) return n;
            p = e;
            if (!skip) { void* dst = va_arg(ap, void*); if (len >= 1) *(double*)dst = v; else *(float*)dst = (float)v; n++; }
            break;
        }
        case 's': {
            char* d = skip ? NULL : va_arg(ap, char*); int k = 0;
            while (*p && !isspace((unsigned char)*p) && (!width || k < width)) { if (d) d[k] = *p; k++; p++; }
            if (d) { d[k] = 0; n++; }
            break;
        }
        case 'c': {
            char* d = skip ? NULL : va_arg(ap, char*); int k = 0, w = width ? width : 1;
            while (*p && k < w) { if (d) d[k] = *p; k++; p++; }
            if (k < w) return n;
            if (d) n++;
            break;
        }
        case '[': {
            int neg = 0; fmt++;
            if (*fmt == '^') { neg = 1; fmt++; }
            const char* set = fmt;
            if (*fmt == ']') fmt++;
            while (*fmt && *fmt != ']') fmt++;
            size_t setl = (size_t)(fmt - set);
            char* d = skip ? NULL : va_arg(ap, char*); int k = 0;
            while (*p && (!width || k < width)) {
                int in = 0;
                for (size_t i = 0; i < setl; i++) {
                    if (i + 2 < setl && set[i + 1] == '-') { if ((unsigned char)*p >= (unsigned char)set[i] && (unsigned char)*p <= (unsigned char)set[i + 2]) in = 1; i += 2; }
                    else if (*p == set[i]) in = 1;
                }
                if (in == neg) break;
                if (d) d[k] = *p;
                k++; p++;
            }
            if (!k) return n;
            if (d) { d[k] = 0; n++; }
            break;
        }
        case 'n': if (!skip) *va_arg(ap, int*) = (int)(p - s); break;
        default: return n;
        }
    }
    return n;
}
int sscanf(const char* s, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsscanf(s, fmt, ap); va_end(ap); return r; }
int vfscanf(FILE* f, const char* fmt, va_list ap) {
    char line[1024];
    if (!fgets(line, sizeof(line), f)) return EOF;
    return vsscanf(line, fmt, ap);
}
int fscanf(FILE* f, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfscanf(f, fmt, ap); va_end(ap); return r; }
int vscanf(const char* fmt, va_list ap) { return vfscanf(stdin, fmt, ap); }
int scanf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfscanf(stdin, fmt, ap); va_end(ap); return r; }
