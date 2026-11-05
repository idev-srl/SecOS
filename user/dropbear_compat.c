/* SecOS - Dropbear SSH port compatibility shim (tools/port-dropbear.sh).
 * Provides the POSIX symbols Dropbear references that the SecOS libc lacks,
 * mapped onto SecOS syscalls or honest single-user stubs. Functions that are
 * generic POSIX (useful to any port) are marked TODO(integration): move to libc.
 *
 * Kernel interfaces used (see docs/SSH.md):
 *   sockets 21..30 + SYS_SOCKNAME 80 (sockets are ordinary fds: read/write/
 *   close/dup2/fork work on them), SYS_POLL 78 (select/poll), SYS_WAIT 9 /
 *   SYS_WAITANY 55, SYS_FSTAT 56, SYS_CLOCK_GETTIME 65, SYS_NANOSLEEP 67,
 *   SYS_SYNC 70, SYS_LINK 72 (the 65+ ones degrade gracefully when absent).
 * SPDX-License-Identifier: MIT */
#include "dropbear_port.h"
#include <stddef.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>
#include <termios.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <syslog.h>

extern int errno;
/* Generic POSIX shims are weak: when the SecOS libc provides the real symbol
 * (poll/select/time/... landed there), the libc definition wins at link time.
 * Struct layouts here match user/include (fd_set, pollfd, tm, iovec). */
#define W __attribute__((weak))

#define SYS_WAIT_      9
#define SYS_SOCKET_    21
#define SYS_CONNECT_   22
#define SYS_BIND_      23
#define SYS_LISTEN_    24
#define SYS_ACCEPT_    25
#define SYS_SEND_      26
#define SYS_RECV_      27
#define SYS_SENDTO_    28
#define SYS_RECVFROM_  29
#define SYS_WAITANY_   55
#define SYS_FSTAT_     56
#define SYS_CLOCK_GETTIME_ 65
#define SYS_NANOSLEEP_ 67
#define SYS_SYNC_      70
#define SYS_LINK_      72
#define SYS_POLL_      78
#define SYS_SOCKNAME_  80

/* Kernel returns -errno; an unknown syscall number returns -1 (older kernels)
 * or -ENOSYS. */
static long sc_ret(long r){
    if(r < 0){ errno = (r > -134) ? (int)-r : EIO; return -1; }
    return r;
}
static int sc_missing(long r){ return r == -1 || r == -ENOSYS; }
static void zmem(void* d, unsigned long n){ unsigned char* p = d; while(n--) *p++ = 0; }

/* ===================== sockets (TODO(integration): move to libc) ===================== */
/* struct secos_sockaddr (libsecos.h): ip = network-order octets (octet0 in the
 * low byte, i.e. the in_addr.s_addr layout), port in host order. */
static int sa_to_secos(const struct sockaddr* sa, socklen_t len, unsigned int* ip, unsigned short* port){
    if(!sa || len < sizeof(struct sockaddr_in)){ errno = EINVAL; return -1; }
    if(sa->sa_family != AF_INET){ errno = EAFNOSUPPORT; return -1; }
    const struct sockaddr_in* in = (const struct sockaddr_in*)sa;
    *ip = in->sin_addr.s_addr; *port = ntohs(in->sin_port);
    return 0;
}
static void secos_to_sa(unsigned int ip, unsigned short port, struct sockaddr* sa, socklen_t* len){
    if(!sa || !len) return;
    struct sockaddr_in in; zmem(&in, sizeof(in));
    in.sin_family = AF_INET; in.sin_port = htons(port); in.sin_addr.s_addr = ip;
    socklen_t n = *len < sizeof(in) ? *len : (socklen_t)sizeof(in);
    memcpy(sa, &in, n);
    *len = sizeof(in);
}

int db_socket(int domain, int type, int protocol){
    (void)protocol;
    if(domain != AF_INET){ errno = EAFNOSUPPORT; return -1; }
    int base = type & 0xff;
    if(base != SOCK_STREAM && base != SOCK_DGRAM){ errno = EPROTONOSUPPORT; return -1; }
    /* SOCK_NONBLOCK / SOCK_CLOEXEC ride in the same word (Linux values). */
    return (int)sc_ret(secos_syscall(SYS_SOCKET_, type, 0, 0, 0, 0));
}
int db_bind(int fd, const struct sockaddr* sa, socklen_t len){
    unsigned int ip; unsigned short port;
    if(sa_to_secos(sa, len, &ip, &port) != 0) return -1;
    /* The SecOS stack has one interface: only the wildcard / its own address. */
    return (int)sc_ret(secos_syscall(SYS_BIND_, fd, port, 0, 0, 0));
}
int db_listen(int fd, int backlog){ return (int)sc_ret(secos_syscall(SYS_LISTEN_, fd, backlog, 0, 0, 0)); }
int db_accept(int fd, struct sockaddr* sa, socklen_t* len){
    int nfd = (int)sc_ret(secos_syscall(SYS_ACCEPT_, fd, 0, 0, 0, 0));
    if(nfd >= 0 && sa && len){
        struct secos_sockaddr k; zmem(&k, sizeof(k));
        if(secos_syscall(SYS_SOCKNAME_, nfd, (long)&k, 1, 0, 0) == 0) secos_to_sa(k.ip, k.port, sa, len);
        else secos_to_sa(0, 0, sa, len);
    }
    return nfd;
}
int db_connect(int fd, const struct sockaddr* sa, socklen_t len){
    unsigned int ip; unsigned short port;
    if(sa_to_secos(sa, len, &ip, &port) != 0) return -1;
    return (int)sc_ret(secos_syscall(SYS_CONNECT_, fd, ip, port, 0, 0));
}
ssize_t db_send(int fd, const void* buf, size_t len, int flags){ (void)flags; return write(fd, buf, len); }
ssize_t db_recv(int fd, void* buf, size_t len, int flags){
    if(flags & MSG_PEEK){ errno = EOPNOTSUPP; return -1; }
    return read(fd, buf, len);
}
ssize_t db_sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr* sa, socklen_t sl){
    (void)flags;
    if(!sa) return write(fd, buf, len);
    struct secos_sockaddr k; zmem(&k, sizeof(k));
    if(sa_to_secos(sa, sl, &k.ip, &k.port) != 0) return -1;
    return sc_ret(secos_syscall(SYS_SENDTO_, fd, (long)buf, (long)len, (long)&k, 0));
}
ssize_t db_recvfrom(int fd, void* buf, size_t len, int flags, struct sockaddr* sa, socklen_t* sl){
    (void)flags;
    struct secos_sockaddr k; zmem(&k, sizeof(k));
    long n = sc_ret(secos_syscall(SYS_RECVFROM_, fd, (long)buf, (long)len, (long)&k, 0));
    if(n >= 0) secos_to_sa(k.ip, k.port, sa, sl);
    return n;
}
/* No half-close in the SecOS socket layer: SHUT_WR/RDWR are accepted (the FIN
 * goes out on the last close()), SHUT_RD is a no-op. */
int db_shutdown(int fd, int how){ (void)fd; (void)how; return 0; }
/* Options are advisory on SecOS (no Nagle, no keepalive, no TOS). */
int db_setsockopt(int fd, int level, int opt, const void* val, socklen_t len){
    (void)fd; (void)level; (void)opt; (void)val; (void)len; return 0;
}
int db_getsockopt(int fd, int level, int opt, void* val, socklen_t* len){
    (void)fd;
    if(level == SOL_SOCKET && val && len && *len >= sizeof(int)){
        int v = 0;
        if(opt == SO_TYPE) v = SOCK_STREAM;
        memcpy(val, &v, sizeof(int)); *len = sizeof(int);
        return 0;
    }
    errno = ENOPROTOOPT; return -1;
}
static int sockname(int fd, struct sockaddr* sa, socklen_t* len, int peer){
    struct secos_sockaddr k; zmem(&k, sizeof(k));
    long r = secos_syscall(SYS_SOCKNAME_, fd, (long)&k, peer, 0, 0);
    if(r < 0){ if(sc_missing(r)) r = -ENOTSOCK; return (int)sc_ret(r); }
    secos_to_sa(k.ip, k.port, sa, len);
    return 0;
}
int db_getsockname(int fd, struct sockaddr* sa, socklen_t* len){ return sockname(fd, sa, len, 0); }
int db_getpeername(int fd, struct sockaddr* sa, socklen_t* len){ return sockname(fd, sa, len, 1); }
int db_socketpair(int domain, int type, int protocol, int sv[2]){
    (void)domain; (void)type; (void)protocol; (void)sv; errno = EOPNOTSUPP; return -1;
}

/* ---- inet helpers (IPv4 only) ---- */
W int inet_aton(const char* s, struct in_addr* out){
    unsigned int oct[4]; int n = 0;
    if(!s) return 0;
    while(n < 4){
        if(*s < '0' || *s > '9') return 0;
        unsigned int v = 0; int digits = 0;
        while(*s >= '0' && *s <= '9'){ v = v*10 + (unsigned)(*s - '0'); s++; if(++digits > 3 || v > 255) return 0; }
        oct[n++] = v;
        if(n < 4){ if(*s != '.') return 0; s++; }
    }
    if(*s) return 0;
    if(out) out->s_addr = oct[0] | (oct[1] << 8) | (oct[2] << 16) | (oct[3] << 24);
    return 1;
}
in_addr_t db_inet_addr(const char* s){ struct in_addr a; return inet_aton(s, &a) ? a.s_addr : INADDR_NONE; }
W const char* inet_ntop(int af, const void* src, char* dst, socklen_t size){
    if(af != AF_INET){ errno = EAFNOSUPPORT; return 0; }
    const unsigned char* b = (const unsigned char*)src;
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    if(strlen(tmp) + 1 > size){ errno = ENOSPC; return 0; }
    strcpy(dst, tmp);
    return dst;
}
char* db_inet_ntoa(struct in_addr in){ static char b[16]; inet_ntop(AF_INET, &in, b, sizeof(b)); return b; }
W int inet_pton(int af, const char* src, void* dst){
    if(af != AF_INET){ errno = EAFNOSUPPORT; return -1; }
    struct in_addr a; if(!inet_aton(src, &a)) return 0;
    memcpy(dst, &a, sizeof(a)); return 1;
}

/* ---- getaddrinfo: numeric IPv4 + "localhost"; no DNS resolver here ---- */
W int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res){
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    int flags = hints ? hints->ai_flags : 0;
    if(family != AF_UNSPEC && family != AF_INET) return EAI_FAMILY;
    struct in_addr addr;
    if(!node || !*node) addr.s_addr = (flags & AI_PASSIVE) ? INADDR_ANY : htonl(INADDR_LOOPBACK);
    else if(!strcmp(node, "localhost")) addr.s_addr = htonl(INADDR_LOOPBACK);
    else if(!inet_aton(node, &addr)) return EAI_NONAME;
    unsigned long port = 0;
    if(service && *service){
        char* end; port = strtoul(service, &end, 10);
        if(*end || port > 65535){
            if(!strcmp(service, "ssh")) port = 22; else return EAI_SERVICE;
        }
    }
    struct addrinfo* ai = (struct addrinfo*)calloc(1, sizeof(*ai) + sizeof(struct sockaddr_in));
    if(!ai) return EAI_MEMORY;
    struct sockaddr_in* sin = (struct sockaddr_in*)(ai + 1);
    sin->sin_family = AF_INET; sin->sin_port = htons((unsigned short)port); sin->sin_addr = addr;
    ai->ai_family = AF_INET;
    ai->ai_socktype = socktype ? socktype : SOCK_STREAM;
    ai->ai_protocol = ai->ai_socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
    ai->ai_addrlen = sizeof(*sin);
    ai->ai_addr = (struct sockaddr*)sin;
    *res = ai;
    return 0;
}
W void freeaddrinfo(struct addrinfo* ai){
    while(ai){ struct addrinfo* n = ai->ai_next; free(ai); ai = n; }
}
W int getnameinfo(const struct sockaddr* sa, socklen_t salen, char* host, socklen_t hostlen,
                char* serv, socklen_t servlen, int flags){
    (void)flags;
    if(!sa || salen < sizeof(struct sockaddr_in) || sa->sa_family != AF_INET) return EAI_FAMILY;
    const struct sockaddr_in* in = (const struct sockaddr_in*)sa;
    if(host && hostlen && !inet_ntop(AF_INET, &in->sin_addr, host, hostlen)) return EAI_FAIL;
    if(serv && servlen) snprintf(serv, servlen, "%u", (unsigned)ntohs(in->sin_port));
    return 0;
}
W const char* gai_strerror(int err){
    switch(err){
    case EAI_NONAME:  return "Name does not resolve (SecOS: numeric IPv4 only)";
    case EAI_FAMILY:  return "Address family not supported";
    case EAI_SERVICE: return "Unknown service";
    case EAI_MEMORY:  return "Out of memory";
    default:          return "getaddrinfo failed";
    }
}

/* ===================== poll / select (TODO(integration): move to libc) ===================== */
W int poll(struct pollfd* fds, nfds_t n, int timeout_ms){
    long r = secos_syscall(SYS_POLL_, (long)fds, (long)n, timeout_ms, 0, 0);
    if(sc_missing(r)) r = -ENOSYS;       /* kernel without SYS_POLL */
    return (int)sc_ret(r);
}
W int select(int nfds, fd_set* rs, fd_set* ws, fd_set* es, struct timeval* tv){
    struct pollfd p[FD_SETSIZE];
    int n = 0;
    if(nfds < 0 || nfds > FD_SETSIZE){ errno = EINVAL; return -1; }
    for(int fd = 0; fd < nfds; fd++){
        short ev = 0;
        if(rs && FD_ISSET(fd, rs)) ev |= POLLIN;
        if(ws && FD_ISSET(fd, ws)) ev |= POLLOUT;
        if(es && FD_ISSET(fd, es)) ev |= POLLPRI;
        if(ev){ p[n].fd = fd; p[n].events = ev; p[n].revents = 0; n++; }
    }
    int timeout = -1;
    if(tv){
        long ms = tv->tv_sec * 1000L + (tv->tv_usec + 999) / 1000;
        timeout = ms > 0x7fffffffL ? 0x7fffffff : (int)ms;
    }
    int r = poll(p, (nfds_t)n, timeout);
    if(r < 0) return -1;
    if(rs) FD_ZERO(rs);
    if(ws) FD_ZERO(ws);
    if(es) FD_ZERO(es);
    int count = 0;
    for(int i = 0; i < n; i++){
        short re = p[i].revents;
        if(re & POLLNVAL){ errno = EBADF; return -1; }
        if(rs && (p[i].events & POLLIN) && (re & (POLLIN|POLLHUP|POLLERR))){ FD_SET(p[i].fd, rs); count++; }
        if(ws && (p[i].events & POLLOUT) && (re & (POLLOUT|POLLHUP|POLLERR))){ FD_SET(p[i].fd, ws); count++; }
        if(es && (p[i].events & POLLPRI) && (re & POLLPRI)){ FD_SET(p[i].fd, es); count++; }
    }
    return count;
}

/* ===================== O_NONBLOCK emulation (see dropbear_port.h) ===================== */
static unsigned char g_nonblock[FD_SETSIZE];      /* copied by fork with the image */
static int nb_ready(int fd, short ev){
    struct pollfd p; p.fd = fd; p.events = ev; p.revents = 0;
    int r = poll(&p, 1, 0);
    if(r < 0) return errno == ENOSYS ? 1 : -1;     /* no poll: behave as before */
    return r > 0 && (p.revents & (ev | POLLHUP | POLLERR | POLLNVAL));
}
int db_fcntl(int fd, int cmd, ...){
    va_list ap; va_start(ap, cmd); long arg = va_arg(ap, long); va_end(ap);
    int r = (fcntl)(fd, cmd, arg);
    if(fd >= 0 && fd < FD_SETSIZE){
        if(cmd == F_SETFL){ g_nonblock[fd] = (arg & O_NONBLOCK) ? 1 : 0; if(r < 0) r = 0; }
        else if(cmd == F_GETFL && r >= 0 && g_nonblock[fd]) r |= O_NONBLOCK;
        else if((cmd == F_DUPFD) && r >= 0 && r < FD_SETSIZE) g_nonblock[r] = 0;
    }
    return r;
}
ssize_t db_read(int fd, void* buf, size_t n){
    if(fd >= 0 && fd < FD_SETSIZE && g_nonblock[fd]){
        int k = nb_ready(fd, POLLIN);
        if(k == 0){ errno = EAGAIN; return -1; }
    }
    return (read)(fd, buf, n);
}
ssize_t db_write(int fd, const void* buf, size_t n){
    if(fd >= 0 && fd < FD_SETSIZE && g_nonblock[fd]){
        int k = nb_ready(fd, POLLOUT);
        if(k == 0){ errno = EAGAIN; return -1; }
    }
    return (write)(fd, buf, n);
}

/* ===================== I/O ===================== */
W ssize_t writev(int fd, const struct iovec* iov, int cnt){
    ssize_t total = 0;
    for(int i = 0; i < cnt; i++){
        if(!iov[i].iov_len) continue;
        ssize_t w = write(fd, iov[i].iov_base, iov[i].iov_len);
        if(w < 0) return total ? total : -1;
        total += w;
        if((size_t)w < iov[i].iov_len) break;     /* short write: report progress */
    }
    return total;
}
W ssize_t readv(int fd, const struct iovec* iov, int cnt){
    ssize_t total = 0;
    for(int i = 0; i < cnt; i++){
        if(!iov[i].iov_len) continue;
        ssize_t r = read(fd, iov[i].iov_base, iov[i].iov_len);
        if(r < 0) return total ? total : -1;
        total += r;
        if((size_t)r < iov[i].iov_len) break;
    }
    return total;
}
/* open() with O_EXCL / O_NOFOLLOW / creation mode, which the SecOS kernel open
 * ignores today (it creates-or-opens and never follows a final symlink check).
 * Not atomic against a concurrent creator, but closes the host-key overwrite /
 * symlink-redirect holes for Dropbear's own key and pid files.
 * TODO(integration): move O_EXCL/O_NOFOLLOW/mode into the kernel open. */
int db_open(const char* path, int flags, ...){
    unsigned mode = 0;
    if(flags & O_CREAT){ va_list ap; va_start(ap, flags); mode = va_arg(ap, unsigned); va_end(ap); }
    struct stat st;
    int exists = lstat(path, &st) == 0;
    if(exists && (flags & O_CREAT) && (flags & O_EXCL)){ errno = EEXIST; return -1; }
    if(exists && (flags & O_NOFOLLOW) && S_ISLNK(st.st_mode)){ errno = ELOOP; return -1; }
    int fd = (open)(path, flags & ~(O_EXCL | O_NOFOLLOW));
    if(fd >= 0 && !exists && (flags & O_CREAT)) chmod(path, mode & 07777 & ~022u);
    return fd;
}
W int fstat(int fd, struct stat* st){
    if(!st){ errno = EFAULT; return -1; }
    zmem(st, sizeof(*st));
    return (int)sc_ret(secos_syscall(SYS_FSTAT_, fd, (long)st, 0, 0, 0));
}
W int fsync(int fd){
    long r = secos_syscall(SYS_SYNC_, fd, 0, 0, 0, 0);
    if(sc_missing(r)) return 0;          /* no SYS_SYNC: writes are already synchronous */
    return (int)sc_ret(r);
}
W int link(const char* oldp, const char* newp){
    long r = secos_syscall(SYS_LINK_, (long)oldp, (long)newp, 0, 0, 0);
    if(sc_missing(r)) r = -ENOSYS;       /* Dropbear falls back to a direct write */
    return (int)sc_ret(r);
}
W int fileno(FILE* f){ return f ? f->fd : (errno = EBADF, -1); }
W void clearerr(FILE* f){ (void)f; }
W int setvbuf(FILE* f, char* buf, int mode, size_t size){ (void)f; (void)buf; (void)mode; (void)size; return 0; }
W int fchmod(int fd, mode_t mode){ (void)fd; (void)mode; return 0; }
W int fchown(int fd, uid_t uid, gid_t gid){ (void)fd; (void)uid; (void)gid; return 0; }
W int ftruncate(int fd, off_t len){ (void)fd; (void)len; errno = ENOSYS; return -1; }
W int access(const char* path, int mode){ (void)mode; struct stat st; return stat(path, &st) == 0 ? 0 : -1; }

/* ===================== processes / signals ===================== */
/* waitpid(pid, &st, opt) over SYS_WAIT (one pid, blocking) / SYS_WAITANY. The
 * kernel returns Linux wait-status encoding (see user/bash_compat.c). */
int db_waitpid(pid_t pid, int* status, int options){
    if(pid == (pid_t)-1){
        int code = 0;
        long got = secos_syscall(SYS_WAITANY_, (long)&code, (long)options, 0, 0, 0);
        if(got == -ECHILD){ errno = ECHILD; return -1; }
        if(got == 0) return 0;
        if(got < 0){ errno = EINTR; return -1; }
        if(status) *status = code & 0xffff;
        return (int)got;
    }
    long code = secos_syscall(SYS_WAIT_, pid, 0, 0, 0, 0);
    if(code < 0){ errno = ECHILD; return -1; }
    if(status) *status = (int)(code & 0xffff);
    return pid;
}
int db_getgroups(int n, int* g){ (void)n; (void)g; errno = ENOSYS; return -1; }
static void (*g_sigdisp[NSIG])(int);
W int sigaction(int sig, const struct sigaction* act, struct sigaction* old){
    if(sig <= 0 || sig >= NSIG){ errno = EINVAL; return -1; }
    if(old){ zmem(old, sizeof(*old)); old->sa_handler = g_sigdisp[sig]; }
    if(act){
        if(signal(sig, act->sa_handler) == SIG_ERR){ errno = EINVAL; return -1; }
        g_sigdisp[sig] = act->sa_handler;
    }
    return 0;
}
W int sigemptyset(sigset_t* s){ if(s) *s = 0; return 0; }
W int sigfillset(sigset_t* s){ if(s) *s = ~0UL; return 0; }
W int sigaddset(sigset_t* s, int n){ if(s) *s |= (1UL << (n & 63)); return 0; }
W int sigdelset(sigset_t* s, int n){ if(s) *s &= ~(1UL << (n & 63)); return 0; }
W int sigismember(const sigset_t* s, int n){ return s ? (int)((*s >> (n & 63)) & 1) : 0; }
W unsigned int alarm(unsigned int s){ (void)s; return 0; }
W int setegid(gid_t g){ (void)g; return 0; }
W int seteuid(uid_t u){ (void)u; return 0; }
W int initgroups(const char* user, gid_t g){ (void)user; (void)g; return 0; }
W int setpgrp(void){ return setpgid(0, 0); }
W int getrlimit(int r, struct rlimit* l){ (void)r; if(l){ l->rlim_cur = RLIM_INFINITY; l->rlim_max = RLIM_INFINITY; } return 0; }
W int setrlimit(int r, const struct rlimit* l){ (void)r; (void)l; return 0; }
W long sysconf(int name){
    switch(name){ case _SC_OPEN_MAX: return 32; case _SC_CLK_TCK: return 1000; case _SC_PAGESIZE: return 4096; }
    errno = EINVAL; return -1;
}
W int getpagesize(void){ return 4096; }
W int gethostname(char* name, size_t len){
    const char* h = "secos";
    if(!name || !len){ errno = EINVAL; return -1; }
    strlcpy(name, h, len); return 0;
}

/* ===================== identity: one "root" account ===================== */
/* SecOS has no user database: the signature is the trust boundary. Dropbear
 * runs non-multiuser, so the only account that can authenticate is the one it
 * runs as. Any other user name does not exist (Dropbear rejects it before
 * looking at keys). The password field is locked: password auth is compiled
 * out anyway (DROPBEAR_SVR_PASSWORD_AUTH 0). */
static struct passwd g_root = { "root", "!", 0, 0, "SecOS root", "/root", "/bin/bash" };
struct passwd* getpwnam(const char* n){ return (n && !strcmp(n, "root")) ? &g_root : 0; }
struct passwd* getpwuid(int u){ return u == 0 ? &g_root : 0; }
struct passwd* getpwent(void){ return 0; }
void setpwent(void){}
void endpwent(void){}
static char* g_root_mem[] = { "root", 0 };
static struct group g_rootgrp = { "root", "!", 0, g_root_mem };
struct group* getgrnam(const char* n){ return (n && !strcmp(n, "root")) ? &g_rootgrp : 0; }
struct group* getgrgid(int g){ return g == 0 ? &g_rootgrp : 0; }
struct group* getgrent(void){ return 0; }
void setgrent(void){}
void endgrent(void){}
/* Valid login shells (there is no /etc/shells): the account's own shell. */
static const char* const g_shells[] = { "/bin/bash", 0 };
static int g_shell_i;
char* getusershell(void){ const char* s = g_shells[g_shell_i]; if(s) g_shell_i++; return (char*)s; }
void setusershell(void){ g_shell_i = 0; }
void endusershell(void){ g_shell_i = 0; }
W char* crypt(const char* key, const char* salt){ (void)key; (void)salt; errno = ENOSYS; return 0; }

/* ===================== PTY (kernel side pending; see docs/SSH.md) ===================== */
/* Standard Linux implementation over /dev/ptmx + TIOCGPTN/TIOCSPTLCK: it
 * starts working unchanged once the kernel provides them. Until then the
 * open of /dev/ptmx fails and Dropbear answers the pty-req with a failure, so
 * `ssh host cmd` (exec, no tty) works and `ssh -t` degrades to no tty. */
#define TIOCGPTN_   0x80045430
#define TIOCSPTLCK_ 0x40045431
W int posix_openpt(int flags){ return open("/dev/ptmx", flags); }
W int grantpt(int fd){ (void)fd; return 0; }   /* /dev/pts nodes are created owned + 0620 */
W int unlockpt(int fd){ int z = 0; return ioctl(fd, TIOCSPTLCK_, &z); }
W char* ptsname(int fd){
    static char name[24]; unsigned int n = 0;
    if(ioctl(fd, TIOCGPTN_, &n) != 0) return 0;
    snprintf(name, sizeof(name), "/dev/pts/%u", n);
    return name;
}
W int tcsendbreak(int fd, int dur){ (void)fd; (void)dur; return 0; }
W speed_t cfgetospeed(const void* t){ (void)t; return 38400; }
W speed_t cfgetispeed(const void* t){ (void)t; return 38400; }
W int cfsetospeed(void* t, speed_t s){ (void)t; (void)s; return 0; }
W int cfsetispeed(void* t, speed_t s){ (void)t; (void)s; return 0; }
W char* ttyname(int fd){ (void)fd; return 0; }

/* ===================== time (TODO(integration): move to libc) ===================== */
W int clock_gettime(clockid_t id, struct timespec* ts){
    if(!ts){ errno = EFAULT; return -1; }
    long r = secos_syscall(SYS_CLOCK_GETTIME_, id, (long)ts, 0, 0, 0);
    if(r == 0) return 0;
    if(!sc_missing(r)) return (int)sc_ret(r);
    /* No SYS_CLOCK_GETTIME: monotonic = uptime ticks (1 kHz); there is no wall
     * clock, so REALTIME is uptime too (log timestamps start at the epoch). */
    unsigned long ms = getticks();
    ts->tv_sec = (long)(ms / 1000); ts->tv_nsec = (long)(ms % 1000) * 1000000L;
    return 0;
}
W int nanosleep(const struct timespec* req, struct timespec* rem){
    if(!req){ errno = EFAULT; return -1; }
    long r = secos_syscall(SYS_NANOSLEEP_, (long)req, (long)rem, 0, 0, 0);
    if(r == 0) return 0;
    if(!sc_missing(r)) return (int)sc_ret(r);
    unsigned long ms = (unsigned long)req->tv_sec * 1000UL + (unsigned long)(req->tv_nsec / 1000000L);
    sleep_ticks((unsigned)(ms ? ms : 1));
    if(rem){ rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}
static int is_leap(long y){ return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const short g_mdays_cum[2][13] = {
    { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
    { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 } };
W struct tm* gmtime_r(const time_t* t, struct tm* tm){
    long s = (long)*t, days = s / 86400, rem = s % 86400;
    if(rem < 0){ rem += 86400; days--; }
    tm->tm_hour = (int)(rem / 3600); tm->tm_min = (int)(rem % 3600 / 60); tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)((4 + days) % 7); if(tm->tm_wday < 0) tm->tm_wday += 7;
    long y = 1970;
    while(days >= (is_leap(y) ? 366 : 365)){ days -= is_leap(y) ? 366 : 365; y++; }
    while(days < 0){ y--; days += is_leap(y) ? 366 : 365; }
    tm->tm_year = (int)(y - 1900); tm->tm_yday = (int)days;
    int l = is_leap(y), m = 0;
    while(days >= g_mdays_cum[l][m + 1]) m++;
    tm->tm_mon = m; tm->tm_mday = (int)(days - g_mdays_cum[l][m]) + 1;
    tm->tm_isdst = 0; tm->tm_gmtoff = 0; tm->tm_zone = "UTC";
    return tm;
}
W struct tm* localtime_r(const time_t* t, struct tm* tm){ return gmtime_r(t, tm); }   /* UTC only */
W struct tm* gmtime(const time_t* t){ static struct tm tm; return gmtime_r(t, &tm); }
W struct tm* localtime(const time_t* t){ return gmtime(t); }
W time_t mktime(struct tm* tm){
    long y = tm->tm_year + 1900, days = 0;
    for(long i = 1970; i < y; i++) days += is_leap(i) ? 366 : 365;
    days += g_mdays_cum[is_leap(y)][tm->tm_mon % 12] + tm->tm_mday - 1;
    return (time_t)(days * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec);
}
W size_t strftime(char* s, size_t max, const char* fmt, const struct tm* tm){
    static const char* mon[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
    static const char* wd[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    size_t o = 0;
    if(!max) return 0;
    for(; *fmt; fmt++){
        char b[32]; const char* add = b; b[0] = 0;
        if(*fmt != '%'){ b[0] = *fmt; b[1] = 0; }
        else switch(*++fmt){
            case 'Y': snprintf(b, sizeof b, "%d", tm->tm_year + 1900); break;
            case 'm': snprintf(b, sizeof b, "%02d", tm->tm_mon + 1); break;
            case 'd': snprintf(b, sizeof b, "%02d", tm->tm_mday); break;
            case 'e': snprintf(b, sizeof b, "%2d", tm->tm_mday); break;
            case 'H': snprintf(b, sizeof b, "%02d", tm->tm_hour); break;
            case 'M': snprintf(b, sizeof b, "%02d", tm->tm_min); break;
            case 'S': snprintf(b, sizeof b, "%02d", tm->tm_sec); break;
            case 'T': snprintf(b, sizeof b, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'b': case 'h': add = mon[tm->tm_mon % 12]; break;
            case 'a': add = wd[tm->tm_wday % 7]; break;
            case '%': b[0] = '%'; b[1] = 0; break;
            case 0: fmt--; break;
            default: b[0] = '%'; b[1] = *fmt; b[2] = 0; break;
        }
        for(; *add; add++){ if(o + 1 >= max) return 0; s[o++] = *add; }
    }
    s[o] = 0;
    return o;
}

/* ===================== strings / misc (TODO(integration): move to libc) ===================== */
/* sscanf: just the conversions Dropbear uses (%d %u %x with an optional width). */
W int sscanf(const char* s, const char* fmt, ...){
    va_list ap; va_start(ap, fmt);
    int n = 0;
    while(*fmt){
        if(*fmt != '%'){ if(*s != *fmt) break; s++; fmt++; continue; }
        fmt++;
        int width = 0; while(*fmt >= '0' && *fmt <= '9') width = width*10 + (*fmt++ - '0');
        char c = *fmt++;
        int base = (c == 'x') ? 16 : 10, neg = 0, digits = 0;
        unsigned long v = 0;
        if(c != 'd' && c != 'u' && c != 'x') break;
        while(*s == ' ') s++;
        if(c == 'd' && (*s == '-' || *s == '+')){ neg = (*s == '-'); s++; }
        for(;;){
            int d;
            if(width && digits >= width) break;
            if(*s >= '0' && *s <= '9') d = *s - '0';
            else if(base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if(base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else break;
            v = v * (unsigned)base + (unsigned)d; s++; digits++;
        }
        if(!digits) break;
        if(c == 'd') *va_arg(ap, int*) = neg ? -(int)v : (int)v;
        else *va_arg(ap, unsigned int*) = (unsigned int)v;
        n++;
    }
    va_end(ap);
    return n;
}
W char* strsignal(int sig){ static char b[24]; snprintf(b, sizeof b, "Signal %d", sig); return b; }

/* syslog: Dropbear is built with DISABLE_SYSLOG and run with -E (stderr). */
W void openlog(const char* ident, int opt, int facility){ (void)ident; (void)opt; (void)facility; }
W void syslog(int pri, const char* fmt, ...){ (void)pri; (void)fmt; }
W void closelog(void){}
