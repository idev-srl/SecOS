/*
 * SecOS overlay for musl: the Linux -> SecOS syscall translation layer.
 * SPDX-License-Identifier: MIT
 *
 * musl calls __secos_syscall(n, a..f) with a Linux x86-64 syscall number and
 * Linux arguments (arch/x86_64/syscall_arch.h). This file maps each call onto
 * the SecOS ABI (int $0x80; rax = number, rdi, rsi, rdx, rcx, r8, r9) and
 * returns what Linux would return: the result, or -errno.
 *
 *   direct      same arguments, SecOS number (and its error convention);
 *   converting  structs or flags differ (stat, sigaction masks, dirents, ...);
 *   emulated    built from other calls (wait4, select, getdents64, ...);
 *   Linux-shaped  SecOS numbers 512 + Linux number (arch_prctl, ...): passed on.
 *   missing     -ENOSYS. With SECOS_SYSCALL_TRACE=1 in the environment each
 *               one is reported on stderr: the porting worklist (U2/U3).
 *               SECOS_SYSCALL_TRACE=2 also logs every call and its result to
 *               /tmp/secos-trace.<pid> (a debugging aid: full-screen programs
 *               own stderr).
 *
 * docs/USERLAND_PLAN.md D1/D2, docs/devlog/U1-musl-base.md.
 *
 * Constraints: this code runs before TLS exists (static_init_tls may mmap) and
 * is built without the stack protector (tools/port-musl.sh); it never touches
 * errno and calls no libc function. Bad user pointers fault here, in the
 * process, instead of returning EFAULT (Linux returns EFAULT): a gap, not a
 * security issue (the kernel checks every pointer it is given).
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/random.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include "syscall.h"

/* ---- the SecOS ABI (kernel/syscall.h) ------------------------------------ */
enum {
    S_YIELD = 0, S_EXIT = 1, S_WRITE = 2, S_READ = 3, S_OPEN = 4, S_CLOSE = 5,
    S_GETPID = 6, S_WAIT = 9, S_MMAP = 14, S_MUNMAP = 15, S_BRK = 16,
    S_MPROTECT = 17, S_FORK = 18, S_LSEEK = 19, S_STAT = 20, S_PIPE = 31,
    S_CHMOD = 32, S_CHOWN = 33, S_UTIMES = 34, S_READLINK = 35, S_SYMLINK = 36,
    S_LSTAT = 37, S_MOUNT = 38, S_UMOUNT = 39, S_GETDENTS = 40, S_MKDIR = 42,
    S_UNLINK = 43, S_SIGACTION = 44, S_KILL = 46, S_SIGPROCMASK = 47,
    S_SETPGID = 48, S_DUP2 = 49, S_DUP = 50, S_CHDIR = 51, S_GETCWD = 52,
    S_IOCTL = 53, S_GETPPID = 54, S_WAITANY = 55, S_FSTAT = 56, S_GETPGID = 57,
    S_SIGSUSPEND = 58, S_REBOOT = 59, S_EXECVE = 60, S_FCNTL = 61,
    S_CLOCK_GETTIME = 65, S_CLOCK_SETTIME = 66, S_NANOSLEEP = 67,
    S_STATFS = 69, S_SYNC = 70, S_RENAME = 71, S_LINK = 72, S_TRUNCATE = 73,
    S_RMDIR = 74, S_ACCESS = 75, S_UMASK = 76, S_UNAME = 77, S_POLL = 78,
    S_SYSINFO = 79, S_SETSID = 85, S_GETSID = 86, S_PRCTL = 90,
    S_SCHED_GETAFFINITY = 91,
    S_GETUID = 100, S_GETEUID = 101, S_GETGID = 102, S_GETEGID = 103,
    S_SETUID = 104, S_SETGID = 105, S_SETRESUID = 106, S_SETRESGID = 107,
    S_GETGROUPS = 108, S_SETGROUPS = 109,
    S_LX = 512,                       /* Linux-shaped: 512 + Linux number */
};

/* struct secos_stat (kernel/syscall.h), 88 bytes */
struct secos_stat {
    uint64_t size; uint32_t mode, nlink, uid, gid;
    uint64_t atime, mtime, ctime, dev, ino, rdev;
    int64_t blksize, blocks;
};

/* Linux x86-64 struct stat as the kernel writes it (musl arch/x86_64/kstat.h) */
struct lx_stat {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad0;
    uint64_t rdev;
    int64_t size, blksize, blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    int64_t unused[3];
};

struct lx_dirent64 { uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[]; };

struct lx_ksigaction { uint64_t handler; uint64_t flags; uint64_t restorer; uint64_t mask; };

#define SECOS_PATH 256               /* kernel path buffers (incl. the NUL) */
#define RAW_IOV_MAX 1024

static long sc(long n, long a, long b, long c, long d, long e, long f)
{
    long ret;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "0"(n), "D"(a), "S"(b), "d"(c), "c"(d), "r"(r8), "r"(r9)
                         : "memory", "cc");
    return ret;
}
/* [U9] A kernel call that is musl's cancellation point: with cp (the
 * thread's cancel flag, from __syscall_cp_asm) it goes through the asm trap
 * whose pc range [__cp_begin, __cp_end) musl's SIGCANCEL handler knows
 * (src/thread/x86_64/syscall_cp.s); without, the plain trap. */
long __secos_cp_trap(volatile int *cancel, long n, long a, long b, long c, long d, long e, long f)
    __attribute__((__visibility__("hidden")));
static long scp(volatile int *cp, long n, long a, long b, long c, long d, long e, long f)
{
    return cp ? __secos_cp_trap(cp, n, a, b, c, d, e, f) : sc(n, a, b, c, d, e, f);
}
#define sc0(n)             sc(n, 0, 0, 0, 0, 0, 0)
#define sc1(n, a)          sc(n, (long)(a), 0, 0, 0, 0, 0)
#define sc2(n, a, b)       sc(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define sc3(n, a, b, c)    sc(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

/* A SecOS result: -errno passes through; a bare -1 (some calls still use it
 * for "failed") becomes -dflt. */
static long er(long r, int dflt)
{
    if (r == -1) return -dflt;
    return r;
}

/* ---- tiny helpers (no libc: this runs before TLS, and libc may call us) -- */
static size_t xstrlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void xmemset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; }
static void xmemcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; }

static void wr2(const char *s) { sc3(S_WRITE, 2, s, xstrlen(s)); }
static void wr2num(long v)
{
    char b[24]; int i = 23; b[i] = 0;
    int neg = v < 0; unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    wr2(b + i);
}

/* SECOS_SYSCALL_TRACE=1: report every call this layer cannot serve. */
extern char **__environ;
static int g_trace = -1;
static int trace_on(void)
{
    if (g_trace < 0) {
        g_trace = 0;
        for (char **e = __environ; e && *e; e++) {
            const char *k = "SECOS_SYSCALL_TRACE=";
            const char *p = *e; int i = 0;
            while (k[i] && p[i] == k[i]) i++;
            if (!k[i]) { g_trace = !p[i] || p[i] == '0' ? 0 : p[i] == '2' ? 2 : 1; break; }
        }
    }
    return g_trace;
}
static long nosys(long n, const char *what)
{
    if (trace_on()) {
        wr2("secos-musl: ENOSYS linux syscall "); wr2num(n);
        if (what) { wr2(" ("); wr2(what); wr2(")"); }
        wr2("\n");
    }
    return -ENOSYS;
}
/* A call that works, but only partly as on Linux (also under the trace). */
static void partial(long n, const char *what)
{
    if (trace_on()) {
        wr2("secos-musl: partial linux syscall "); wr2num(n);
        wr2(" ("); wr2(what); wr2(")\n");
    }
}

/* ---- paths ----------------------------------------------------------------
 * The kernel takes absolute or cwd-relative paths of at most 255 bytes. The
 * *at() calls with a real directory fd resolve relative to that directory:
 * its path comes from /proc/self/fd/N (the kernel keeps the inode's path). */
static long fd_path(int fd, char *out, size_t outsz)
{
    char link[32] = "/proc/self/fd/";
    int i = 14; char t[12]; int k = 0; unsigned u = (unsigned)fd;
    do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (k) link[i++] = t[--k];
    link[i] = 0;
    char buf[SECOS_PATH];
    long n = sc3(S_READLINK, link, buf, sizeof buf - 1);
    if (n <= 0) return -EBADF;
    if ((size_t)n >= outsz) return -ENAMETOOLONG;
    buf[n] = 0;
    if (buf[0] != '/') return -ENOTDIR;           /* a pipe, socket, ... */
    xmemcpy(out, buf, (size_t)n + 1);
    return 0;
}

/* Resolve (dirfd, path) to a path the kernel accepts in 'out'. */
static long at_path(int dirfd, const char *path, char *out)
{
    if (!path) return -EFAULT;
    size_t pl = xstrlen(path);
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        if (pl >= SECOS_PATH) return -ENAMETOOLONG;
        xmemcpy(out, path, pl + 1);
        return 0;
    }
    char d[SECOS_PATH];
    long r = fd_path(dirfd, d, sizeof d);
    if (r) return r;
    size_t dl = xstrlen(d);
    if (dl + 1 + pl >= SECOS_PATH) return -ENAMETOOLONG;
    xmemcpy(out, d, dl);
    if (dl == 0 || out[dl - 1] != '/') out[dl++] = '/';
    xmemcpy(out + dl, path, pl + 1);
    return 0;
}
#define AT_PATH(dirfd, p, buf) do { long _r = at_path((dirfd), (const char *)(p), (buf)); if (_r) return _r; } while (0)

/* ---- stat ----------------------------------------------------------------- */
static void stat_conv(const struct secos_stat *k, struct lx_stat *l)
{
    xmemset(l, 0, sizeof *l);
    l->dev = k->dev; l->ino = k->ino; l->nlink = k->nlink;
    l->mode = k->mode; l->uid = k->uid; l->gid = k->gid;
    l->rdev = k->rdev; l->size = (int64_t)k->size;
    l->blksize = k->blksize; l->blocks = k->blocks;
    l->atime = (int64_t)k->atime; l->mtime = (int64_t)k->mtime; l->ctime = (int64_t)k->ctime;
}
static long k_stat(const char *p, struct secos_stat *k, int follow)
{
    xmemset(k, 0, sizeof *k);
    return er(sc2(follow ? S_STAT : S_LSTAT, p, k), ENOENT);
}
static long k_fstat(int fd, struct secos_stat *k)
{
    xmemset(k, 0, sizeof *k);
    return er(sc2(S_FSTAT, fd, k), EBADF);
}
static long x_fstatat(int dirfd, const char *path, struct lx_stat *st, int flag)
{
    struct secos_stat k;
    long r;
    if ((flag & AT_EMPTY_PATH) && path && !path[0]) r = k_fstat(dirfd, &k);
    else {
        char p[SECOS_PATH];
        AT_PATH(dirfd, path, p);
        if (!p[0]) return -ENOENT;
        r = k_stat(p, &k, !(flag & AT_SYMLINK_NOFOLLOW));
    }
    if (r == 0) stat_conv(&k, st);
    return r;
}
static int is_dir_fd(int fd)
{
    struct secos_stat k;
    return k_fstat(fd, &k) == 0 && S_ISDIR(k.mode);
}

/* ---- files ---------------------------------------------------------------- */
#define K_OPEN_FLAGS (O_ACCMODE | O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC | O_APPEND | O_NONBLOCK | O_CLOEXEC)

static long x_openat(int dirfd, const char *path, int flags, int mode)
{
    char p[SECOS_PATH];
    AT_PATH(dirfd, path, p);
    if (!p[0]) return -ENOENT;
    if ((flags & O_DIRECTORY) && (flags & O_CREAT)) return -EINVAL;
    if (flags & O_NOFOLLOW) {
        struct secos_stat k;
        if (k_stat(p, &k, 0) == 0 && S_ISLNK(k.mode)) return -ELOOP;
    }
    if (flags & ~(K_OPEN_FLAGS | O_DIRECTORY | O_NOFOLLOW | O_LARGEFILE | O_PATH | O_NOATIME | O_DIRECT | O_SYNC | O_DSYNC))
        partial(SYS_openat, "open flags ignored");
    int kf = flags & K_OPEN_FLAGS;
    if (flags & O_PATH) kf = (kf & ~O_ACCMODE) | O_RDONLY;   /* lookup-only fd: needs r (U2) */
    long fd = er(sc3(S_OPEN, p, kf, mode), EMFILE);
    if (fd < 0) return fd;
    if ((flags & O_DIRECTORY) && !is_dir_fd((int)fd)) {
        sc1(S_CLOSE, fd);
        return -ENOTDIR;
    }
    return fd;
}

/* read/write: SecOS refuses a zero length and takes an int count. */
#define IO_MAX 0x7ffff000L
static long io_err(int fd, int writing)
{
    struct secos_stat k;
    if (k_fstat(fd, &k) != 0) return -EBADF;
    if (writing && S_ISFIFO(k.mode)) return -EPIPE;
    return -EIO;
}
static long x_read(int fd, void *buf, size_t len, volatile int *cp)
{
    if (len == 0) return 0;
    if (len > IO_MAX) len = IO_MAX;
    long r = scp(cp, S_READ, fd, (long)buf, (long)len, 0, 0, 0);
    return r == -1 ? io_err(fd, 0) : r;
}
static long x_write(int fd, const void *buf, size_t len, volatile int *cp)
{
    if (len == 0) return 0;
    if (len > IO_MAX) len = IO_MAX;
    long r = scp(cp, S_WRITE, fd, (long)buf, (long)len, 0, 0, 0);
    return r == -1 ? io_err(fd, 1) : r;
}
struct lx_iovec { void *base; size_t len; };
static long x_rwv(int fd, const struct lx_iovec *iov, int cnt, int writing)
{
    if (cnt < 0 || cnt > RAW_IOV_MAX) return -EINVAL;
    long total = 0;
    for (int i = 0; i < cnt; i++) {
        if (!iov[i].len) continue;
        long r = writing ? x_write(fd, iov[i].base, iov[i].len, 0) : x_read(fd, iov[i].base, iov[i].len, 0);
        if (r < 0) return total ? total : r;
        total += r;
        if ((size_t)r < iov[i].len) break;        /* short: stop like the kernel would */
    }
    return total;
}
/* pread/pwrite: seek, transfer, seek back. Not atomic against another user of
 * the same open file (U2 gives the kernel real positional I/O). */
static long x_prw(int fd, void *buf, size_t len, long off, int writing)
{
    if (off < 0) return -EINVAL;
    long cur = sc3(S_LSEEK, fd, 0, SEEK_CUR);
    if (cur < 0) return er(cur, ESPIPE);
    long r = sc3(S_LSEEK, fd, off, SEEK_SET);
    if (r < 0) return er(r, EINVAL);
    r = writing ? x_write(fd, buf, len, 0) : x_read(fd, buf, len, 0);
    sc3(S_LSEEK, fd, cur, SEEK_SET);
    return r;
}

/* getdents64 over the path-based SYS_GETDENTS: the directory's path comes from
 * /proc/self/fd/N, the position (entry index) is the fd's file offset, so
 * rewinddir/seekdir (lseek) work, and it survives dup/fork/exec. "." and ".."
 * come first, as on Linux. d_ino/d_type come from lstat of each entry. */
#define GD_REC 256
static long x_getdents64(int fd, void *buf, size_t len)
{
    char dir[SECOS_PATH];
    long r = fd_path(fd, dir, sizeof dir);
    if (r) return r == -ENOTDIR ? -ENOTDIR : r;
    if (!is_dir_fd(fd)) return -ENOTDIR;
    long pos = sc3(S_LSEEK, fd, 0, SEEK_CUR);
    if (pos < 0) return er(pos, EINVAL);
    /* The whole listing (the kernel fills at most 1 MiB = 4096 entries). */
    size_t cap = 64 * 1024;
    char *recs = 0; long n = 0;
    for (;;) {
        recs = (char *)sc(S_MMAP, 0, (long)cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if ((long)recs <= 0 && (long)recs > -4096) return -ENOMEM;
        n = sc3(S_GETDENTS, dir, recs, cap);
        if (n < 0) { sc2(S_MUNMAP, recs, cap); return er(n, EACCES); }
        if ((size_t)n + GD_REC <= cap || cap >= (1u << 20)) break;
        sc2(S_MUNMAP, recs, cap);
        cap *= 4;
    }
    long nent = n / GD_REC + 2;
    size_t dl = xstrlen(dir);
    long out = 0, i = pos;
    for (; i < nent; i++) {
        const char *name;
        if (i == 0) name = ".";
        else if (i == 1) name = "..";
        else name = recs + (i - 2) * GD_REC + 1;
        size_t nl = xstrlen(name);
        if (!nl) continue;
        size_t reclen = (offsetof(struct lx_dirent64, d_name) + nl + 1 + 7) & ~(size_t)7;
        if ((size_t)out + reclen > len) break;
        char p[SECOS_PATH];
        struct secos_stat k;
        uint64_t ino = 0; unsigned type = DT_UNKNOWN;
        if (i == 1 && dl == 1) { p[0] = '/'; p[1] = 0; }       /* "/.." is "/" */
        else if (dl + 1 + nl < sizeof p) {
            xmemcpy(p, dir, dl);
            size_t o = dl; if (o == 0 || p[o - 1] != '/') p[o++] = '/';
            xmemcpy(p + o, name, nl + 1);
        } else p[0] = 0;
        if (i == 1 && dl > 1) {                                /* parent: drop the last component */
            size_t o = dl; while (o > 1 && p[o - 1] != '/') o--;
            if (o > 1) o--;
            p[o] = 0;
        }
        if (p[0] && k_stat(p, &k, 0) == 0) {
            ino = k.ino;
            type = S_ISDIR(k.mode) ? DT_DIR : S_ISLNK(k.mode) ? DT_LNK : S_ISCHR(k.mode) ? DT_CHR :
                   S_ISBLK(k.mode) ? DT_BLK : S_ISFIFO(k.mode) ? DT_FIFO : S_ISSOCK(k.mode) ? DT_SOCK : DT_REG;
        }
        struct lx_dirent64 *d = (struct lx_dirent64 *)((char *)buf + out);
        d->d_ino = ino ? ino : (uint64_t)(i + 1);
        d->d_off = i + 1;
        d->d_reclen = (uint16_t)reclen;
        d->d_type = (uint8_t)type;
        xmemcpy(d->d_name, name, nl + 1);
        for (size_t z = offsetof(struct lx_dirent64, d_name) + nl + 1; z < reclen; z++) ((char *)d)[z] = 0;
        out += (long)reclen;
    }
    sc2(S_MUNMAP, recs, cap);
    if (out == 0 && i < nent) return -EINVAL;                 /* buffer too small for one entry */
    sc3(S_LSEEK, fd, i, SEEK_SET);
    return out;
}

static long x_unlinkat(int dirfd, const char *path, int flag)
{
    char p[SECOS_PATH];
    AT_PATH(dirfd, path, p);
    if (flag & ~AT_REMOVEDIR) return -EINVAL;
    struct secos_stat k;
    long r = k_stat(p, &k, 0);
    if (r) return r;
    if (flag & AT_REMOVEDIR) {
        if (!S_ISDIR(k.mode)) return -ENOTDIR;
        return er(sc1(S_RMDIR, p), EACCES);
    }
    if (S_ISDIR(k.mode)) return -EISDIR;
    return er(sc1(S_UNLINK, p), EACCES);
}

static long x_mkdirat(int dirfd, const char *path, int mode)
{
    char p[SECOS_PATH];
    AT_PATH(dirfd, path, p);
    long r = sc2(S_MKDIR, p, mode);
    if (r != -1) return r;
    struct secos_stat k;
    if (k_stat(p, &k, 0) == 0) return -EEXIST;
    return -EACCES;
}

static long x_renameat(int od, const char *a, int nd, const char *b, unsigned flags)
{
    if (flags) return nosys(SYS_renameat2, "renameat2 flags");
    char pa[SECOS_PATH], pb[SECOS_PATH];
    AT_PATH(od, a, pa);
    AT_PATH(nd, b, pb);
    return er(sc2(S_RENAME, pa, pb), EACCES);
}

static long x_linkat(int od, const char *a, int nd, const char *b)
{
    char pa[SECOS_PATH], pb[SECOS_PATH];
    AT_PATH(od, a, pa);
    AT_PATH(nd, b, pb);
    return er(sc2(S_LINK, pa, pb), EPERM);
}

static long x_symlinkat(const char *target, int dirfd, const char *path)
{
    char p[SECOS_PATH];
    AT_PATH(dirfd, path, p);
    long r = sc2(S_SYMLINK, target, p);
    if (r != -1) return r;
    struct secos_stat k;
    return k_stat(p, &k, 0) == 0 ? -EEXIST : -EPERM;
}

static long x_readlinkat(int dirfd, const char *path, char *buf, size_t len)
{
    char p[SECOS_PATH];
    AT_PATH(dirfd, path, p);
    if ((long)len <= 0) return -EINVAL;
    char t[SECOS_PATH];
    long n = sc3(S_READLINK, p, t, sizeof t);
    if (n < 0) {
        struct secos_stat k;
        if (k_stat(p, &k, 0) != 0) return -ENOENT;
        if (!S_ISLNK(k.mode)) return -EINVAL;
        return er(n, EIO);
    }
    if ((size_t)n > len) n = (long)len;                       /* truncate, no NUL (Linux) */
    xmemcpy(buf, t, (size_t)n);
    return n;
}

static long x_fchmodat(int dirfd, const char *path, int mode, int flag)
{
    if (flag & AT_SYMLINK_NOFOLLOW) return -EOPNOTSUPP;       /* as Linux: no symlink modes */
    char p[SECOS_PATH];
    AT_PATH(dirfd, path, p);
    return er(sc3(S_CHMOD, p, mode & 07777, 0), EPERM);
}

static long x_fchownat(int dirfd, const char *path, unsigned uid, unsigned gid, int flag)
{
    struct secos_stat k;
    if ((flag & AT_EMPTY_PATH) && path && !path[0])
        return er(sc(S_CHOWN, 0, uid, gid, dirfd, 0, 0), EPERM);
    char p[SECOS_PATH];
    AT_PATH(dirfd, path, p);
    long r = k_stat(p, &k, !(flag & AT_SYMLINK_NOFOLLOW));
    if (r) return r;
    return er(sc(S_CHOWN, (long)p, uid, gid, 0, (flag & AT_SYMLINK_NOFOLLOW) ? 1 : 0, 0), EPERM);
}

/* utimensat: SecOS stores whole seconds (atime, mtime). */
static long now_sec(void)
{
    struct timespec ts = { 0, 0 };
    sc2(S_CLOCK_GETTIME, CLOCK_REALTIME, &ts);
    return (long)ts.tv_sec;
}
static long x_utimensat(int dirfd, const char *path, const struct timespec *t, int flag)
{
    struct secos_stat k;
    char p[SECOS_PATH];
    int by_fd = (path == 0);                                  /* futimens */
    long r;
    if (by_fd) r = k_fstat(dirfd, &k);
    else {
        AT_PATH(dirfd, path, p);
        r = k_stat(p, &k, !(flag & AT_SYMLINK_NOFOLLOW));
    }
    if (r) return r;
    long a, m, now = now_sec();
    if (!t) a = m = now;
    else {
        if (t[0].tv_nsec == UTIME_OMIT && t[1].tv_nsec == UTIME_OMIT) return 0;
        a = t[0].tv_nsec == UTIME_NOW ? now : t[0].tv_nsec == UTIME_OMIT ? (long)k.atime : (long)t[0].tv_sec;
        m = t[1].tv_nsec == UTIME_NOW ? now : t[1].tv_nsec == UTIME_OMIT ? (long)k.mtime : (long)t[1].tv_sec;
    }
    if (by_fd) return er(sc(S_UTIMES, 0, a, m, dirfd, 0, 0), EPERM);
    return er(sc3(S_UTIMES, p, a, m), EPERM);
}

static long x_faccessat(int dirfd, const char *path, int mode, int flag)
{
    char p[SECOS_PATH];
    AT_PATH(dirfd, path, p);
    if (flag & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW)) return -EINVAL;
    if (flag) partial(SYS_faccessat2, "flags ignored (real ids, symlinks followed)");
    return er(sc2(S_ACCESS, p, mode), EACCES);
}

static long x_getcwd(char *buf, size_t size)
{
    char t[SECOS_PATH];
    long n = sc2(S_GETCWD, t, sizeof t);
    if (n < 0) return er(n, ERANGE);
    if ((size_t)n + 1 > size) return -ERANGE;
    xmemcpy(buf, t, (size_t)n);
    buf[n] = 0;
    return n + 1;                                             /* Linux: bytes incl. the NUL */
}

static long x_chdir(const char *path)
{
    long r = sc1(S_CHDIR, path);
    if (r != -1) return r;
    struct secos_stat k;
    if (k_stat(path, &k, 1) != 0) return -ENOENT;
    if (!S_ISDIR(k.mode)) return -ENOTDIR;
    return -EACCES;
}

static long x_dup3(int o, int n, int flags)
{
    if (flags & ~O_CLOEXEC) return -EINVAL;
    if (sc3(S_FCNTL, o, F_GETFD, 0) < 0) return -EBADF;
    if (n < 0 || n >= 32) return -EBADF;                      /* the kernel's 32-entry fd table */
    long r = sc2(S_DUP2, o, n);
    if (r < 0) return er(r, EBADF);
    if (flags & O_CLOEXEC) sc3(S_FCNTL, n, F_SETFD, FD_CLOEXEC);
    return r;
}

static long x_fcntl(int fd, int cmd, long arg)
{
    switch (cmd) {
    case F_DUPFD: case F_DUPFD_CLOEXEC: case F_GETFD: case F_SETFD:
    case F_GETFL: case F_SETFL:
        return er(sc3(S_FCNTL, fd, cmd, arg), EBADF);
    case F_GETLK: case F_SETLK: case F_SETLKW:
        return nosys(SYS_fcntl, "file locks");
    default:
        return nosys(SYS_fcntl, "fcntl command");
    }
}

/* ---- memory --------------------------------------------------------------- */
static long x_mmap(long addr, size_t len, int prot, int flags, int fd, long off)
{
    if (!len) return -EINVAL;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) return -EACCES;   /* W^X */
    if (off & 0xfff) return -EINVAL;
    int anon = (flags & MAP_ANONYMOUS) != 0;
    int type = flags & MAP_TYPE;
    if (type != MAP_PRIVATE && type != MAP_SHARED) return -EINVAL;
    if (flags & MAP_FIXED) {
        /* Only what mallocng does: a PROT_NONE guard over memory already
         * mapped (its fresh brk pages). Anything else needs a real MAP_FIXED
         * (U4a). */
        if (anon && prot == PROT_NONE && !(addr & 0xfff) &&
            sc3(S_MPROTECT, addr, len, PROT_NONE) == 0)
            return addr;
        return nosys(SYS_mmap, "MAP_FIXED");
    }
    if (flags & MAP_FIXED_NOREPLACE) return nosys(SYS_mmap, "MAP_FIXED_NOREPLACE");
    if (anon && type == MAP_SHARED) return nosys(SYS_mmap, "anonymous MAP_SHARED");
    if (!anon && off) {
        /* The kernel maps files from offset 0 only; a device (fb0) takes it. */
        struct secos_stat k;
        if (k_fstat(fd, &k) != 0) return -EBADF;
        if (!S_ISCHR(k.mode)) return nosys(SYS_mmap, "file offset");
    }
    if (!anon && type == MAP_SHARED && (prot & PROT_WRITE)) {
        struct secos_stat k;
        if (k_fstat(fd, &k) != 0) return -EBADF;
        if (!S_ISCHR(k.mode)) return nosys(SYS_mmap, "writable MAP_SHARED file");
    }
    int kflags = (flags & (MAP_ANONYMOUS | MAP_TYPE));
    long r = sc(S_MMAP, 0, (long)len, prot, kflags, anon ? -1 : fd, off);
    if (r == -1 || r == 0) return -ENOMEM;
    return r;
}

static long x_madvise(long addr, size_t len, int advice)
{
    (void)addr; (void)len;
    switch (advice) {
    case MADV_NORMAL: case MADV_RANDOM: case MADV_SEQUENTIAL:
    case MADV_WILLNEED: case MADV_FREE:
        return 0;                    /* hints: keeping the pages is a valid answer */
    default:
        return nosys(SYS_madvise, "madvise advice (DONTNEED needs zeroing)");
    }
}

/* ---- signals ---------------------------------------------------------------
 * SecOS has signals 1..63 ([U9]; Linux 1..64: SIGRTMAX 64 is refused), masks
 * with bit n = signal n (Linux: bit n-1), one handler address per signal and
 * one process-wide restorer. sa_mask, the sa_flags and the old action are
 * kept here (a process-local shadow, shared by the threads: a disposition
 * inherited across exec reads as SIG_DFL until set again). SA_SIGINFO
 * handlers get siginfo_t / ucontext_t from the kernel (U9). */
static struct lx_ksigaction g_sa[64];

static uint64_t mask_to_k(uint64_t l) { return (l << 1) & ~1ull; }
static uint64_t mask_to_l(uint64_t k) { return (k >> 1) & 0x7fffffffffffffffull; }

static long x_rt_sigaction(int sig, const struct lx_ksigaction *act, struct lx_ksigaction *old, size_t ss)
{
    if (ss != 8) return -EINVAL;
    if (sig < 1 || sig >= 65) return -EINVAL;
    if (sig >= 64) {
        if (act) return nosys(SYS_rt_sigaction, "signal 64");
        if (old) xmemset(old, 0, sizeof *old);
        return 0;
    }
    if (act) {
        if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;
        if (act->flags & (SA_RESETHAND | SA_NODEFER | SA_ONSTACK))
            partial(SYS_rt_sigaction, "SA_RESETHAND/NODEFER/ONSTACK not honoured");
        uint64_t restorer = (act->flags & SA_RESTORER) ? act->restorer : 0;
        long r = sc3(S_SIGACTION, sig, act->handler, restorer);
        if (r < 0) return er(r, EINVAL);
    }
    if (old) *old = g_sa[sig];
    if (act) g_sa[sig] = *act;
    return 0;
}

static long x_rt_sigprocmask(int how, const uint64_t *set, uint64_t *old, size_t ss)
{
    if (ss != 8) return -EINVAL;
    if (how < 0 || how > 2) return -EINVAL;
    uint64_t kold = 0;
    long r;
    if (set) r = sc3(S_SIGPROCMASK, how, mask_to_k(*set), &kold);
    else     r = sc3(S_SIGPROCMASK, SIG_BLOCK, 0, &kold);
    if (r < 0) return er(r, EINVAL);
    if (old) *old = mask_to_l(kold);
    return 0;
}

static long x_kill(int pid, int sig)
{
    if (sig < 0 || sig >= 65) return -EINVAL;
    if (sig >= 64) return nosys(SYS_kill, "signal 64");
    return er(sc2(S_KILL, pid, sig), EPERM);
}

/* ---- processes ------------------------------------------------------------
 * wait4: SYS_WAIT(pid) blocks for one child; SYS_WAITANY(&st, options) takes
 * any child and handles WNOHANG/WUNTRACED/WCONTINUED. A child reaped while
 * waiting for another pid is stashed for its own waitpid. Process-group waits
 * (pid 0 / < -1) wait for any child. rusage is not tracked (zeroed). */
static struct { int pid, st; } g_stash[16];
/* [U9] The stash is process-wide: threads may wait concurrently. */
static volatile int g_stash_lock;
static void stash_lock(void) { while (__atomic_exchange_n(&g_stash_lock, 1, __ATOMIC_ACQUIRE)) sc0(S_YIELD); }
static void stash_unlock(void) { __atomic_store_n(&g_stash_lock, 0, __ATOMIC_RELEASE); }
static int stash_take(int pid, int *st)
{
    stash_lock();
    for (int i = 0; i < 16; i++)
        if (g_stash[i].pid && (pid <= 0 || g_stash[i].pid == pid)) {
            int p = g_stash[i].pid;
            if (st) *st = g_stash[i].st;
            g_stash[i].pid = 0;
            stash_unlock();
            return p;
        }
    stash_unlock();
    return 0;
}
static void stash_put(int pid, int st)
{
    stash_lock();
    for (int i = 0; i < 16; i++) if (!g_stash[i].pid) { g_stash[i].pid = pid; g_stash[i].st = st; break; }
    stash_unlock();
}
static long x_wait4(int pid, int *status, int options, void *ru, volatile int *cp)
{
    if (options & ~(WNOHANG | WUNTRACED | WCONTINUED | __WALL | __WCLONE)) return -EINVAL;
    if (ru) { xmemset(ru, 0, 144); partial(SYS_wait4, "rusage not tracked"); }
    if (pid == 0 || pid < -1) partial(SYS_wait4, "process-group wait = any child");
    int p = stash_take(pid, status);
    if (p) return p;
    if (pid > 0 && !(options & (WNOHANG | WUNTRACED | WCONTINUED))) {
        long code = scp(cp, S_WAIT, pid, 0, 0, 0, 0, 0);
        if (code == -EINTR) return -EINTR;
        if (code < 0) return -ECHILD;
        if (status) *status = (int)(code & 0xffff);
        return pid;
    }
    for (;;) {
        int code = 0;
        long got = scp(cp, S_WAITANY, (long)&code, options & (WNOHANG | WUNTRACED | WCONTINUED), 0, 0, 0, 0);
        if (got == -ECHILD) return -ECHILD;
        if (got == 0) return 0;
        if (got < 0) return got == -1 ? -EINTR : got;
        if (pid > 0 && got != pid) { stash_put((int)got, code & 0xffff); continue; }
        if (status) *status = code & 0xffff;
        return got;
    }
}

/* setreuid/setregid on top of setresuid (POSIX/Linux: the saved id follows
 * the new effective id when the real id is set or the effective id changes
 * away from the real one). */
static long x_setre(int is_gid, unsigned r, unsigned e)
{
    uint32_t cur[3];
    long rr = sc1(is_gid ? S_GETGID : S_GETUID, cur);
    if (rr < 0) return er(rr, EPERM);
    unsigned s = (unsigned)-1;
    if (r != (unsigned)-1 || (e != (unsigned)-1 && e != cur[0]))
        s = e != (unsigned)-1 ? e : cur[1];
    return er(sc3(is_gid ? S_SETRESGID : S_SETRESUID, r, e, s), EPERM);
}

static long x_getres(int is_gid, unsigned *r, unsigned *e, unsigned *s)
{
    uint32_t v[3];
    long rr = sc1(is_gid ? S_GETGID : S_GETUID, v);
    if (rr < 0) return er(rr, EFAULT);
    if (r) *r = v[0];
    if (e) *e = v[1];
    if (s) *s = v[2];
    return 0;
}

/* ---- time ----------------------------------------------------------------- */
static long x_clock_getres(int clk, struct timespec *res)
{
    struct timespec t;
    long r = sc2(S_CLOCK_GETTIME, clk, &t);                   /* validates clk */
    if (r < 0) return er(r, EINVAL);
    if (res) {
        res->tv_sec = 0;
        res->tv_nsec = (clk == CLOCK_REALTIME_COARSE || clk == CLOCK_MONOTONIC_COARSE ||
                        clk == CLOCK_PROCESS_CPUTIME_ID || clk == CLOCK_THREAD_CPUTIME_ID) ? 1000000 : 1;
    }
    return 0;
}

static long x_clock_nanosleep(int clk, int flags, const struct timespec *req, struct timespec *rem,
                              volatile int *cp)
{
    if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC && clk != CLOCK_BOOTTIME) return -EINVAL;
    if (!(flags & TIMER_ABSTIME)) return er(scp(cp, S_NANOSLEEP, (long)req, (long)rem, 0, 0, 0, 0), EINVAL);
    struct timespec now, d;
    long r = sc2(S_CLOCK_GETTIME, clk, &now);
    if (r < 0) return er(r, EINVAL);
    d.tv_sec = req->tv_sec - now.tv_sec;
    d.tv_nsec = req->tv_nsec - now.tv_nsec;
    if (d.tv_nsec < 0) { d.tv_nsec += 1000000000L; d.tv_sec--; }
    if (d.tv_sec < 0) return 0;
    return er(scp(cp, S_NANOSLEEP, (long)&d, 0, 0, 0, 0, 0), EINVAL);   /* abstime: no rem */
}

/* ---- poll / select ---------------------------------------------------------
 * ppoll and pselect6 apply their signal mask around the wait (not atomically:
 * a signal arriving between the mask change and the poll is handled late). */
static int ts_to_ms(const struct timespec *ts)
{
    if (!ts) return -1;
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L) return -2;
    long long ms = (long long)ts->tv_sec * 1000 + (ts->tv_nsec + 999999) / 1000000;
    return ms > 0x7fffffff ? 0x7fffffff : (int)ms;
}
static long x_poll_mask(struct pollfd *fds, unsigned long n, int ms, const uint64_t *sigmask, size_t ss,
                        volatile int *cp)
{
    if (sigmask && ss != 8) return -EINVAL;
    uint64_t old = 0;
    if (sigmask) x_rt_sigprocmask(SIG_SETMASK, sigmask, &old, 8);
    long r = er(scp(cp, S_POLL, (long)fds, (long)n, ms, 0, 0, 0), EINVAL);
    if (sigmask) x_rt_sigprocmask(SIG_SETMASK, &old, 0, 8);
    return r;
}
static long x_select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, int ms,
                     const uint64_t *sigmask, volatile int *cp)
{
    if (nfds < 0 || nfds > FD_SETSIZE) return -EINVAL;
    struct pollfd pf[64];
    int map[64], n = 0;
    for (int fd = 0; fd < nfds; fd++) {
        short ev = 0;
        if (rd && FD_ISSET(fd, rd)) ev |= POLLIN;
        if (wr && FD_ISSET(fd, wr)) ev |= POLLOUT;
        if (ex && FD_ISSET(fd, ex)) ev |= POLLPRI;
        if (!ev) continue;
        if (n >= 64) return -EINVAL;              /* the kernel has 32 fds anyway */
        pf[n].fd = fd; pf[n].events = ev; pf[n].revents = 0; map[n] = fd; n++;
    }
    long r = x_poll_mask(pf, (unsigned long)n, ms, sigmask, 8, cp);
    if (r < 0) return r;
    for (int i = 0; i < n; i++)
        if (pf[i].revents & POLLNVAL) return -EBADF;
    if (rd) FD_ZERO(rd);
    if (wr) FD_ZERO(wr);
    if (ex) FD_ZERO(ex);
    long cnt = 0;
    for (int i = 0; i < n; i++) {
        short re = pf[i].revents;
        if (rd && (re & (POLLIN | POLLHUP | POLLERR)) && (pf[i].events & POLLIN)) { FD_SET(map[i], rd); cnt++; }
        if (wr && (re & (POLLOUT | POLLERR)) && (pf[i].events & POLLOUT)) { FD_SET(map[i], wr); cnt++; }
        if (ex && (re & POLLPRI) && (pf[i].events & POLLPRI)) { FD_SET(map[i], ex); cnt++; }
    }
    return cnt;
}

/* ---- misc ----------------------------------------------------------------- */
static long x_getrandom(void *buf, size_t len, unsigned flags)
{
    if (flags & ~(GRND_NONBLOCK | GRND_RANDOM | 4 /*GRND_INSECURE*/)) return -EINVAL;
    long fd = sc3(S_OPEN, "/dev/urandom", O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return nosys(SYS_getrandom, "/dev/urandom unavailable");
    long got = 0;
    while ((size_t)got < len) {
        long r = x_read((int)fd, (char *)buf + got, len - (size_t)got, 0);
        if (r <= 0) break;
        got += r;
    }
    sc1(S_CLOSE, fd);
    return got ? got : -EIO;
}

/* mount(2): flags MS_RDONLY / MS_REMOUNT become leading "ro" / "remount"
 * option tokens, as BusyBox's mount does on SecOS (busybox_compat.c). */
#define MS_RDONLY_  1
#define MS_REMOUNT_ 32
#define MS_BIND_    4096
#define MS_MOVE_    8192
static long x_mount(const char *src, const char *tgt, const char *type, unsigned long fl, const char *data)
{
    if (fl & (MS_BIND_ | MS_MOVE_)) return -EINVAL;
    if (fl & MS_REMOUNT_) { if (!src) src = ""; if (!type) type = ""; }
    else if (!type || !*type) return -EINVAL;
    char opt[128]; size_t n = 0;
    if (fl & MS_REMOUNT_) { const char *r = "remount,"; while (*r) opt[n++] = *r++; }
    if (fl & MS_RDONLY_) { opt[n++] = 'r'; opt[n++] = 'o'; }
    else if (n) n--;
    if (data && *data) {
        if (n) opt[n++] = ',';
        while (*data && n < sizeof opt - 1) opt[n++] = *data++;
        if (*data) return -EINVAL;
    }
    opt[n] = 0;
    return er(sc(S_MOUNT, (long)src, (long)tgt, (long)type, n ? (long)opt : 0, 0, 0), EINVAL);
}

static long x_reboot(unsigned m1, unsigned m2, unsigned cmd)
{
    if (m1 != 0xfee1dead) return -EINVAL;
    (void)m2;
    int k;
    switch (cmd) {
    case 0x4321FEDC: k = 1; break;               /* POWER_OFF */
    case 0x01234567: k = 2; break;               /* RESTART */
    case 0xCDEF0123: k = 3; break;               /* HALT */
    default: return nosys(SYS_reboot, "reboot command");
    }
    return er(sc1(S_REBOOT, k), EPERM);
}

/* ---- SECOS_SYSCALL_TRACE=2: one line per call --------------------------- */
static long dispatch(long n, long a, long b, long c, long d, long e, long f, volatile int *cp);
static void tr_put(char *buf, size_t *o, const char *s)
{
    while (*s && *o < 400) buf[(*o)++] = *s++;
}
static void tr_num(char *buf, size_t *o, long v)
{
    char b[24]; int i = 23; b[i] = 0;
    int neg = v < 0; unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    tr_put(buf, o, b + i);
}
static int tr_path_arg(long n)      /* which argument is a path (1-based), 0 = none */
{
    switch (n) {
    case SYS_open: case SYS_stat: case SYS_lstat: case SYS_access: case SYS_unlink:
    case SYS_rename: case SYS_mkdir: case SYS_rmdir: case SYS_readlink: case SYS_chdir:
    case SYS_chmod: case SYS_chown: case SYS_lchown: case SYS_truncate: case SYS_creat:
    case SYS_symlink: case SYS_link: case SYS_statfs: case SYS_execve:
        return 1;
    case SYS_openat: case SYS_newfstatat: case SYS_faccessat: case SYS_unlinkat:
    case SYS_renameat: case SYS_mkdirat: case SYS_readlinkat: case SYS_fchmodat:
    case SYS_fchownat: case SYS_utimensat: case SYS_faccessat2: case SYS_renameat2:
        return 2;
    }
    return 0;
}
static void tr_log(long n, long a, long b, long c, long d, long r)
{
    char buf[420]; size_t o = 0;
    tr_num(buf, &o, n); tr_put(buf, &o, "(");
    tr_num(buf, &o, a); tr_put(buf, &o, ", "); tr_num(buf, &o, b); tr_put(buf, &o, ", ");
    tr_num(buf, &o, c); tr_put(buf, &o, ", "); tr_num(buf, &o, d); tr_put(buf, &o, ")");
    int pa = tr_path_arg(n);
    const char *path = pa == 1 ? (const char *)a : pa == 2 ? (const char *)b : 0;
    if (path) { tr_put(buf, &o, " \""); tr_put(buf, &o, path); tr_put(buf, &o, "\""); }
    tr_put(buf, &o, " = "); tr_num(buf, &o, r); buf[o++] = '\n';
    char name[40] = "/tmp/secos-trace.";
    size_t k = xstrlen(name); long pid = sc0(S_GETPID);
    char t[24]; int i = 0; do { t[i++] = (char)('0' + pid % 10); pid /= 10; } while (pid && i < 20);
    while (i) name[k++] = t[--i];
    name[k] = 0;
    long fd = sc3(S_OPEN, name, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    sc3(S_WRITE, fd, buf, o);
    sc1(S_CLOSE, fd);
}
long __secos_syscall(long n, long a, long b, long c, long d, long e, long f)
{
    long r = dispatch(n, a, b, c, d, e, f, 0);
    if (g_trace == 2 || (g_trace < 0 && __environ && trace_on() == 2)) tr_log(n, a, b, c, d, r);
    return r;
}

/* [U9] musl's cancellable syscall entry (pthread_cancel.c __syscall_cp_c):
 * a cancel already pending is acted on here; the translated call's blocking
 * kernel call goes through __secos_cp_trap (scp() with cp set). */
long __cancel(void) __attribute__((__visibility__("hidden")));
__attribute__((__visibility__("hidden")))
long __syscall_cp_asm(volatile int *cancel, long n, long a, long b, long c, long d, long e, long f)
{
    if (*cancel) return __cancel();
    long r = dispatch(n, a, b, c, d, e, f, cancel);
    if (g_trace == 2) tr_log(n, a, b, c, d, r);
    return r;
}

/* ---- the table ------------------------------------------------------------ */
static long dispatch(long n, long a, long b, long c, long d, long e, long f, volatile int *cp)
{
    switch (n) {
    /* files */
    case SYS_read:       return x_read((int)a, (void *)b, (size_t)c, cp);
    case SYS_write:      return x_write((int)a, (const void *)b, (size_t)c, cp);
    case SYS_readv:      return x_rwv((int)a, (const struct lx_iovec *)b, (int)c, 0);
    case SYS_writev:     return x_rwv((int)a, (const struct lx_iovec *)b, (int)c, 1);
    case SYS_pread64:    return x_prw((int)a, (void *)b, (size_t)c, d, 0);
    case SYS_pwrite64:   return x_prw((int)a, (void *)b, (size_t)c, d, 1);
    case SYS_open:       return x_openat(AT_FDCWD, (const char *)a, (int)b, (int)c);
    case SYS_openat:     return x_openat((int)a, (const char *)b, (int)c, (int)d);
    case SYS_creat:      return x_openat(AT_FDCWD, (const char *)a, O_CREAT | O_WRONLY | O_TRUNC, (int)b);
    case SYS_close:      return er(sc1(S_CLOSE, a), EBADF);
    case SYS_lseek:      return er(sc3(S_LSEEK, a, b, c), EINVAL);
    case SYS_stat:       return x_fstatat(AT_FDCWD, (const char *)a, (struct lx_stat *)b, 0);
    case SYS_lstat:      return x_fstatat(AT_FDCWD, (const char *)a, (struct lx_stat *)b, AT_SYMLINK_NOFOLLOW);
    case SYS_newfstatat: return x_fstatat((int)a, (const char *)b, (struct lx_stat *)c, (int)d);
    case SYS_fstat: {
        struct secos_stat k;
        long r = k_fstat((int)a, &k);
        if (r == 0) stat_conv(&k, (struct lx_stat *)b);
        return r;
    }
    case SYS_statx:      return nosys(n, "statx");      /* musl falls back to fstatat */
    case SYS_getdents64: return x_getdents64((int)a, (void *)b, (size_t)c);
    case SYS_pipe:       return er(sc2(S_PIPE, a, 0), EMFILE);
    case SYS_pipe2:      return er(sc2(S_PIPE, a, b), EINVAL);
    case SYS_dup: {
        if (sc3(S_FCNTL, a, F_GETFD, 0) < 0) return -EBADF;
        return er(sc1(S_DUP, a), EMFILE);
    }
    case SYS_dup2:
        if (a == b) return sc3(S_FCNTL, a, F_GETFD, 0) < 0 ? -EBADF : b;
        return x_dup3((int)a, (int)b, 0);
    case SYS_dup3:
        if (a == b) return -EINVAL;
        return x_dup3((int)a, (int)b, (int)c);
    case SYS_fcntl:      return x_fcntl((int)a, (int)b, c);
    case SYS_ioctl:
        if (b == FIOCLEX || b == FIONCLEX)
            return er(sc3(S_FCNTL, a, F_SETFD, b == FIOCLEX ? FD_CLOEXEC : 0), EBADF);
        return er(sc3(S_IOCTL, a, b, c), ENOTTY);
    case SYS_mkdir:      return x_mkdirat(AT_FDCWD, (const char *)a, (int)b);
    case SYS_mkdirat:    return x_mkdirat((int)a, (const char *)b, (int)c);
    case SYS_rmdir:      return x_unlinkat(AT_FDCWD, (const char *)a, AT_REMOVEDIR);
    case SYS_unlink:     return x_unlinkat(AT_FDCWD, (const char *)a, 0);
    case SYS_unlinkat:   return x_unlinkat((int)a, (const char *)b, (int)c);
    case SYS_rename:     return x_renameat(AT_FDCWD, (const char *)a, AT_FDCWD, (const char *)b, 0);
    case SYS_renameat:   return x_renameat((int)a, (const char *)b, (int)c, (const char *)d, 0);
    case SYS_renameat2:  return x_renameat((int)a, (const char *)b, (int)c, (const char *)d, (unsigned)e);
    case SYS_link:       return x_linkat(AT_FDCWD, (const char *)a, AT_FDCWD, (const char *)b);
    case SYS_linkat:     return x_linkat((int)a, (const char *)b, (int)c, (const char *)d);
    case SYS_symlink:    return x_symlinkat((const char *)a, AT_FDCWD, (const char *)b);
    case SYS_symlinkat:  return x_symlinkat((const char *)a, (int)b, (const char *)c);
    case SYS_readlink:   return x_readlinkat(AT_FDCWD, (const char *)a, (char *)b, (size_t)c);
    case SYS_readlinkat: return x_readlinkat((int)a, (const char *)b, (char *)c, (size_t)d);
    case SYS_chmod:      return x_fchmodat(AT_FDCWD, (const char *)a, (int)b, 0);
    case SYS_fchmodat:   return x_fchmodat((int)a, (const char *)b, (int)c, (int)d);
    case SYS_fchmod:     return er(sc3(S_CHMOD, 0, b & 07777, a), EPERM);
    case SYS_chown:      return x_fchownat(AT_FDCWD, (const char *)a, (unsigned)b, (unsigned)c, 0);
    case SYS_lchown:     return x_fchownat(AT_FDCWD, (const char *)a, (unsigned)b, (unsigned)c, AT_SYMLINK_NOFOLLOW);
    case SYS_fchownat:   return x_fchownat((int)a, (const char *)b, (unsigned)c, (unsigned)d, (int)e);
    case SYS_fchown:     return er(sc(S_CHOWN, 0, b, c, a, 0, 0), EPERM);
    case SYS_utimensat:  return x_utimensat((int)a, (const char *)b, (const struct timespec *)c, (int)d);
    case SYS_access:     return x_faccessat(AT_FDCWD, (const char *)a, (int)b, 0);
    case SYS_faccessat:  return x_faccessat((int)a, (const char *)b, (int)c, 0);
    case SYS_faccessat2: return x_faccessat((int)a, (const char *)b, (int)c, (int)d);
    case SYS_truncate:   return er(sc3(S_TRUNCATE, a, b, 0), EACCES);
    case SYS_ftruncate:  return er(sc3(S_TRUNCATE, 0, b, a), EBADF);
    case SYS_fsync:
    case SYS_fdatasync:  return er(sc1(S_SYNC, a), EBADF);
    case SYS_sync:       sc1(S_SYNC, -1); return 0;
    case SYS_syncfs:     sc1(S_SYNC, -1); return 0;
    case SYS_statfs:     return er(sc3(S_STATFS, a, b, 0), ENOENT);
    case SYS_fstatfs:    return er(sc3(S_STATFS, 0, b, a), EBADF);
    case SYS_getcwd:     return x_getcwd((char *)a, (size_t)b);
    case SYS_chdir:      return x_chdir((const char *)a);
    case SYS_fchdir: {
        char p[SECOS_PATH];
        long r = fd_path((int)a, p, sizeof p);
        if (r) return r;
        return x_chdir(p);
    }
    case SYS_umask:      return sc1(S_UMASK, a & 0777);
    case SYS_fadvise64:  return 0;              /* advice: ignoring it is valid */
    case SYS_mount:      return x_mount((const char *)a, (const char *)b, (const char *)c, (unsigned long)d, (const char *)e);
    case SYS_umount2:    return er(sc1(S_UMOUNT, a), EINVAL);

    /* memory */
    case SYS_mmap:       return x_mmap(a, (size_t)b, (int)c, (int)d, (int)e, f);
    case SYS_munmap:     return er(sc2(S_MUNMAP, a, b), EINVAL);
    case SYS_mprotect:
        if ((c & PROT_WRITE) && (c & PROT_EXEC)) return -EACCES;      /* W^X */
        return er(sc3(S_MPROTECT, a, b, c), ENOMEM);
    case SYS_brk:        return sc1(S_BRK, a);  /* Linux semantics: the new or old break */
    case SYS_madvise:    return x_madvise(a, (size_t)b, (int)c);
    case SYS_msync:      return 0;              /* no shared file mappings to write back */
    case SYS_mlock: case SYS_munlock: case SYS_mlockall: case SYS_munlockall:
        return 0;                               /* no swap: memory never pages out */
    case SYS_mremap:     return nosys(n, "mremap");

    /* signals */
    case SYS_rt_sigaction:   return x_rt_sigaction((int)a, (const struct lx_ksigaction *)b, (struct lx_ksigaction *)c, (size_t)d);
    case SYS_rt_sigprocmask: return x_rt_sigprocmask((int)a, (const uint64_t *)b, (uint64_t *)c, (size_t)d);
    case SYS_rt_sigsuspend: {
        if (b != 8) return -EINVAL;
        long r = scp(cp, S_SIGSUSPEND, (long)mask_to_k(*(const uint64_t *)a), 0, 0, 0, 0, 0);
        return r >= 0 ? -EINTR : r;
    }
    case SYS_pause: {                                /* [U10] = sigsuspend(current mask) */
        uint64_t cur = 0;
        long r = x_rt_sigprocmask(SIG_BLOCK, 0, &cur, 8);
        if (r < 0) return r;
        r = scp(cp, S_SIGSUSPEND, (long)mask_to_k(cur), 0, 0, 0, 0, 0);   /* [U9] a cancellation point */
        return r >= 0 ? -EINTR : r;
    }
    case SYS_kill:       return x_kill((int)a, (int)b);
    /* [U9] thread-directed: Linux-shaped kernel calls */
    case SYS_tkill:      return (b < 0 || b >= 64) ? -EINVAL : sc2(S_LX + SYS_tkill, a, b);
    case SYS_tgkill:     return (c < 0 || c >= 64) ? -EINVAL : sc3(S_LX + SYS_tgkill, a, b, c);

    /* processes */
    case SYS_getpid:     return sc0(S_GETPID);
    case SYS_gettid:     return sc0(S_LX + SYS_gettid);
    case SYS_getppid:    return sc0(S_GETPPID);
    case SYS_fork:
    case SYS_vfork:      return er(sc0(S_FORK), EAGAIN);
    case SYS_execve:     return er(sc3(S_EXECVE, a, b, c), ENOENT);
    case SYS_exit:       sc1(S_LX + SYS_exit, a); for (;;) ;   /* [U9] this thread */
    case SYS_exit_group: sc1(S_LX + SYS_exit_group, a); for (;;) ;
    case SYS_wait4:      return x_wait4((int)a, (int *)b, (int)c, (void *)d, cp);
    case SYS_set_tid_address: return sc1(S_LX + SYS_set_tid_address, a);
    case SYS_arch_prctl: return sc2(S_LX + SYS_arch_prctl, a, b);
    case SYS_sched_yield: sc0(S_YIELD); return 0;
    case SYS_sched_getaffinity: return er(sc3(S_SCHED_GETAFFINITY, a, b, c), EINVAL);
    case SYS_prctl:      return er(sc2(S_PRCTL, a, b), EINVAL);
    case SYS_setpgid:    return er(sc2(S_SETPGID, a, b), ESRCH);
    case SYS_getpgid:    return er(sc1(S_GETPGID, a), ESRCH);
    case SYS_getpgrp:    return er(sc1(S_GETPGID, 0), ESRCH);
    case SYS_setsid:     return er(sc0(S_SETSID), EPERM);
    case SYS_getsid:     return er(sc1(S_GETSID, a), ESRCH);
    case SYS_reboot:     return x_reboot((unsigned)a, (unsigned)b, (unsigned)c);

    /* credentials */
    case SYS_getuid:     return sc1(S_GETUID, 0);
    case SYS_geteuid:    return sc0(S_GETEUID);
    case SYS_getgid:     return sc1(S_GETGID, 0);
    case SYS_getegid:    return sc0(S_GETEGID);
    case SYS_setuid:     return er(sc1(S_SETUID, (unsigned)a), EPERM);
    case SYS_setgid:     return er(sc1(S_SETGID, (unsigned)a), EPERM);
    case SYS_setreuid:   return x_setre(0, (unsigned)a, (unsigned)b);
    case SYS_setregid:   return x_setre(1, (unsigned)a, (unsigned)b);
    case SYS_setresuid:  return er(sc3(S_SETRESUID, (unsigned)a, (unsigned)b, (unsigned)c), EPERM);
    case SYS_setresgid:  return er(sc3(S_SETRESGID, (unsigned)a, (unsigned)b, (unsigned)c), EPERM);
    case SYS_getresuid:  return x_getres(0, (unsigned *)a, (unsigned *)b, (unsigned *)c);
    case SYS_getresgid:  return x_getres(1, (unsigned *)a, (unsigned *)b, (unsigned *)c);
    case SYS_getgroups:  return er(sc2(S_GETGROUPS, a, b), EINVAL);
    case SYS_setgroups:  return er(sc2(S_SETGROUPS, a, b), EPERM);

    /* time */
    case SYS_clock_gettime: return er(sc2(S_CLOCK_GETTIME, a, b), EINVAL);
    case SYS_clock_settime: return er(sc2(S_CLOCK_SETTIME, a, b), EPERM);
    case SYS_clock_getres:  return x_clock_getres((int)a, (struct timespec *)b);
    case SYS_clock_nanosleep: return x_clock_nanosleep((int)a, (int)b, (const struct timespec *)c, (struct timespec *)d, cp);
    case SYS_nanosleep:  return er(scp(cp, S_NANOSLEEP, a, b, 0, 0, 0, 0), EINVAL);
    case SYS_gettimeofday: {
        struct timespec ts;
        long r = sc2(S_CLOCK_GETTIME, CLOCK_REALTIME, &ts);
        if (r < 0) return er(r, EINVAL);
        if (a) { long *tv = (long *)a; tv[0] = ts.tv_sec; tv[1] = ts.tv_nsec / 1000; }
        if (b) { int *tz = (int *)b; tz[0] = 0; tz[1] = 0; }   /* UTC, no DST */
        return 0;
    }
    case SYS_time: {
        long t = now_sec();
        if (a) *(long *)a = t;
        return t;
    }

    /* poll / select */
    case SYS_poll:       return er(scp(cp, S_POLL, a, b, c, 0, 0, 0), EINVAL);
    case SYS_ppoll: {
        int ms = ts_to_ms((const struct timespec *)c);
        if (ms == -2) return -EINVAL;
        return x_poll_mask((struct pollfd *)a, (unsigned long)b, ms, (const uint64_t *)d, (size_t)e, cp);
    }
    case SYS_select: {
        int ms = -1;
        if (e) {
            const long *tv = (const long *)e;
            if (tv[0] < 0 || tv[1] < 0) return -EINVAL;
            long long v = (long long)tv[0] * 1000 + (tv[1] + 999) / 1000;
            ms = v > 0x7fffffff ? 0x7fffffff : (int)v;
        }
        return x_select((int)a, (fd_set *)b, (fd_set *)c, (fd_set *)d, ms, 0, cp);
    }
    case SYS_pselect6: {
        int ms = ts_to_ms((const struct timespec *)e);
        if (ms == -2) return -EINVAL;
        const uint64_t *mask = 0;
        if (f) {
            const long *data = (const long *)f;         /* { const sigset_t *ss; size_t len; } */
            if (data[0]) { if (data[1] != 8) return -EINVAL; mask = (const uint64_t *)data[0]; }
        }
        return x_select((int)a, (fd_set *)b, (fd_set *)c, (fd_set *)d, ms, mask, cp);
    }

    /* system */
    case SYS_uname:      return er(sc1(S_UNAME, a), EFAULT);
    case SYS_sysinfo:    return er(sc1(S_SYSINFO, a), EFAULT);
    case SYS_getrandom:  return x_getrandom((void *)a, (size_t)b, (unsigned)c);

    /* [U9] threads (kernel/thread.c, kernel/futex.c) */
    case SYS_futex:      return scp(cp, S_LX + SYS_futex, a, b, c, d, e, f);
    case SYS_set_robust_list: return sc2(S_LX + SYS_set_robust_list, a, b);
    case SYS_get_robust_list: return sc3(S_LX + SYS_get_robust_list, a, b, c);
    case SYS_clone:
        /* Threads come through __clone (src/thread/x86_64/clone.s): a raw
         * clone() returning on a new stack inside this C code cannot work.
         * The fork form (no new stack) can. */
        if (b == 0) return er(sc(S_LX + SYS_clone, a, 0, c, d, e, 0), EAGAIN);
        return nosys(n, "clone with a stack (use pthread_create / __clone)");
    case SYS_membarrier: return nosys(n, "membarrier (musl falls back to signals)");

    /* missing in the kernel: the U2/U3/U10 worklist */
    case SYS_rt_sigpending: return nosys(n, "rt_sigpending (U2)");
    case SYS_rt_sigtimedwait: return nosys(n, "rt_sigtimedwait (U2)");
    case SYS_rt_sigqueueinfo: return nosys(n, "rt_sigqueueinfo (U2)");
    case SYS_sigaltstack: return nosys(n, "sigaltstack (U2)");
    case SYS_rt_sigreturn: return nosys(n, "rt_sigreturn (the trampoline calls SYS_SIGRETURN)");
    case SYS_getrlimit: case SYS_setrlimit: case SYS_prlimit64:
                         return nosys(n, "rlimits (U2)");
    case SYS_getrusage:  return nosys(n, "getrusage (U2)");
    case SYS_times:      return nosys(n, "times (U2)");
    case SYS_getitimer: case SYS_setitimer:       /* [U10] ITIMER_REAL; musl's alarm() uses setitimer */
                         if (a != 0 /* ITIMER_REAL */) return nosys(n, "ITIMER_VIRTUAL/PROF");
                         return sc(S_LX + n, a, b, c, 0, 0, 0);
    case SYS_alarm: {
        long it[4] = { 0, 0, (long)(unsigned)a, 0 }, old[4];
        long r = sc(S_LX + SYS_setitimer, 0, (long)it, (long)old, 0, 0, 0);
        if (r < 0) return r;
        return old[2] + (old[3] ? 1 : 0);
    }
    /* [U10] Linux-shaped in the kernel (kernel/lx_net.c, kernel/lx_event.c):
     * sockets (AF_INET TCP/UDP/ICMP/raw, AF_UNIX), epoll, eventfd, timerfd. */
    /* [U9] The blocking ones are pthread cancellation points: through scp(). */
    case SYS_connect: case SYS_accept: case SYS_accept4: case SYS_sendto:
    case SYS_recvfrom: case SYS_sendmsg: case SYS_recvmsg:
    case SYS_epoll_wait: case SYS_epoll_pwait:
#ifdef SYS_epoll_pwait2
    case SYS_epoll_pwait2:
#endif
                         return scp(cp, S_LX + n, a, b, c, d, e, f);
    case SYS_socket: case SYS_socketpair: case SYS_bind:
    case SYS_listen: case SYS_shutdown:
    case SYS_getsockname: case SYS_getpeername: case SYS_setsockopt:
    case SYS_getsockopt:
    case SYS_epoll_create: case SYS_epoll_create1: case SYS_epoll_ctl:
    case SYS_eventfd: case SYS_eventfd2:
    case SYS_timerfd_create: case SYS_timerfd_settime: case SYS_timerfd_gettime:
                         return sc(S_LX + n, a, b, c, d, e, f);
    default:             return nosys(n, 0);
    }
}
