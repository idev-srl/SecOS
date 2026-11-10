/*
 * lx_net.c — [U10] the Linux-shaped socket calls (512 + Linux number):
 * socket, socketpair, bind, listen, connect, accept/accept4, sendto,
 * recvfrom, sendmsg, recvmsg, shutdown, getsockname, getpeername,
 * setsockopt, getsockopt. Linux arguments, Linux structs (sockaddr_in,
 * sockaddr_un, msghdr, cmsghdr), -errno results, so musl's translation layer
 * passes them straight through.
 * SPDX-License-Identifier: MIT
 *
 * Families: AF_INET (net/socket.c: TCP, UDP, ICMP ping sockets, raw IP) and
 * AF_UNIX (kernel/unix_sock.c); AF_INET6 and the rest -EAFNOSUPPORT.
 *
 * Who may do what (the same gates as the M24 calls, plus raw):
 *  - AF_INET needs network access: CAP_NET in the signed manifest;
 *  - SOCK_RAW also needs root (euid 0) or the explicit manifest capability
 *    CAP_NET_RAW (never ambient, audited, like CAP_INPUT/CAP_DISPLAY); a ping
 *    socket does not (that is what it is for: unprivileged ping);
 *  - AF_UNIX needs CAP_IPC (ambient by default); path names are checked like
 *    files (kernel/unix_sock.c);
 *  - binding a TCP/UDP port below 1024 needs root (M43).
 *
 * Blocking: every lower layer is non-blocking; a blocking descriptor (no
 * O_NONBLOCK, no MSG_DONTWAIT) waits in lx_wait() (kernel/syscall_trap.c) and
 * the call re-runs: AF_UNIX sockets are woken by their peer (anonfd_wake),
 * AF_INET ones re-check every 2 ms (the RX path has no wake hook yet), both
 * bounded by SO_RCVTIMEO / SO_SNDTIMEO (EAGAIN on expiry; a connect gives
 * EINPROGRESS) and ended by a signal (EINTR). Data moves through kernel
 * bounce buffers (no net or unix lock is held across a user copy).
 *
 * Not yet: SCM_RIGHTS/SCM_CREDENTIALS (refused with EOPNOTSUPP), IP_PKTINFO
 * delivery, sendmmsg/recvmmsg, AF_INET6, the loopback interface.
 */
#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "process.h"
#include "trapframe.h"
#include "user_copy.h"
#include "anonfd.h"
#include "cap.h"
#include "cred.h"
#include "signal.h"
#include "heap.h"
#include "unix_sock.h"
#include "../net/socket.h"

extern process_t* sched_get_current(void);
extern long lx_wait(trapframe_t* tf, void* wake_obj, uint32_t recheck_ticks,
                    uint64_t timeout_ns, long timeout_ret);
extern int  ksys_sock_install(process_t* c, int sx, uint32_t flags, int cloexec);
extern int  ksys_close(int fd);

#define LX(n) (SYS_LX_BASE + (n))
enum {
    LXN_SOCKET = 41, LXN_CONNECT = 42, LXN_ACCEPT = 43, LXN_SENDTO = 44,
    LXN_RECVFROM = 45, LXN_SENDMSG = 46, LXN_RECVMSG = 47, LXN_SHUTDOWN = 48,
    LXN_BIND = 49, LXN_LISTEN = 50, LXN_GETSOCKNAME = 51, LXN_GETPEERNAME = 52,
    LXN_SOCKETPAIR = 53, LXN_SETSOCKOPT = 54, LXN_GETSOCKOPT = 55, LXN_ACCEPT4 = 288,
};

#define E_PERM 1
#define E_BADF 9
#define E_AGAIN 11
#define E_NOMEM 12
#define E_ACCES 13
#define E_FAULT 14
#define E_INVAL 22
#define E_MFILE 24
#define E_PIPE 32
#define E_NOTSOCK 88
#define E_PROTOTYPE 91
#define E_PROTONOSUPPORT 93
#define E_SOCKTNOSUPPORT 94
#define E_OPNOTSUPP 95
#define E_AFNOSUPPORT 97
#define E_ALREADY 114
#define E_INPROGRESS 115
#define NEG(e) ((uint64_t)(int64_t)-(e))
#define R(v) ((uint64_t)(int64_t)(v))

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_INET   2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3
#define SOCK_SEQPACKET 5
#define SOCK_TYPE_MASK 0xf
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC  0x80000
#define O_NONBLOCK_ 0x800

#define IO_MAX   65536          /* bytes moved per call */
#define NET_RECHECK 2           /* ticks between readiness checks of an AF_INET socket */

struct lx_sockaddr_in { uint16_t sin_family; uint16_t sin_port; uint32_t sin_addr; uint8_t zero[8]; };
struct lx_iovec { uint64_t base; uint64_t len; };
struct lx_msghdr {
    uint64_t name; uint32_t namelen; uint32_t pad0;
    uint64_t iov; uint64_t iovlen;
    uint64_t control; uint64_t controllen;
    int32_t  flags; uint32_t pad1;
};
struct lx_cmsghdr { uint64_t len; int32_t level; int32_t type; };
struct lx_sock_extended_err {
    uint32_t ee_errno; uint8_t ee_origin, ee_type, ee_code, ee_pad;
    uint32_t ee_info, ee_data;
};
union lx_sockaddr { uint16_t family; struct lx_sockaddr_in in; struct lx_sockaddr_un un; uint8_t raw[128]; };

static inline uint16_t bswap16(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static void bz(void* p, size_t n) { uint8_t* b = (uint8_t*)p; while (n--) *b++ = 0; }

/* ------------------------------------------------------------ fd lookup */
typedef struct { int inet; int sx; usock_t* u; int nonblock; } sockref_t;

static int sock_fd(process_t* c, int fd, sockref_t* s) {
    if (fd < 0 || fd >= 32 || !c->fds[fd].used) return -E_BADF;
    struct proc_fd_entry* e = &c->fds[fd];
    s->nonblock = (e->flags & O_NONBLOCK_) != 0;
    if (e->is_sock) { s->inet = 1; s->sx = e->is_sock - 1; s->u = NULL; return 0; }
    if (!e->is_pipe && e->inode && unix_is_sock(e->inode)) { s->inet = 0; s->sx = -1; s->u = unix_of(e->inode); return 0; }
    return -E_NOTSOCK;
}

/* Copy a sockaddr in (alen bytes, at most sizeof(union lx_sockaddr)). */
static int addr_in(uint64_t uaddr, uint32_t alen, union lx_sockaddr* a) {
    bz(a, sizeof(*a));
    if (!uaddr) return -E_FAULT;
    if (alen > sizeof(*a)) alen = sizeof(*a);
    if (alen < 2) return -E_INVAL;
    if (copy_from_user(a, (const void*)uaddr, alen) != 0) return -E_FAULT;
    return 0;
}
/* Copy a sockaddr out the Linux way: truncate to the caller's buffer, store
 * the real length. */
static int addr_out(uint64_t uaddr, uint64_t ulen, const void* a, uint32_t alen) {
    if (!uaddr || !ulen) return 0;
    uint32_t have;
    if (copy_from_user(&have, (const void*)ulen, sizeof(have)) != 0) return -E_FAULT;
    if ((int32_t)have < 0) return -E_INVAL;
    uint32_t n = have < alen ? have : alen;
    if (n && copy_to_user((void*)uaddr, a, n) != 0) return -E_FAULT;
    if (copy_to_user((void*)ulen, &alen, sizeof(alen)) != 0) return -E_FAULT;
    return 0;
}
static void in_make(struct lx_sockaddr_in* s, uint32_t ip, uint16_t port) {
    bz(s, sizeof(*s));
    s->sin_family = AF_INET; s->sin_addr = ip; s->sin_port = bswap16(port);
}

static int inet_ok(process_t* c) { return c->cap_net != 0; }

/* -------------------------------------------------------------- socket */
static uint64_t sys_socket(process_t* c, uint64_t num, int domain, int type, int proto) {
    int flags = type & ~SOCK_TYPE_MASK;
    int t = type & SOCK_TYPE_MASK;
    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) return NEG(E_INVAL);
    int cloexec = (flags & SOCK_CLOEXEC) != 0;
    if (domain == AF_UNIX) {
        if (!cap_check(c, CAP_IPC, num, "socket")) return NEG(E_PERM);
        if (proto != 0) return NEG(E_PROTONOSUPPORT);
        if (t == SOCK_SEQPACKET || t == SOCK_RAW) return NEG(E_SOCKTNOSUPPORT);
        if (t != SOCK_STREAM && t != SOCK_DGRAM) return NEG(E_INVAL);
        usock_t* u;
        int r = unix_create(c, t, &u);
        if (r) return R(r);
        int fd = anonfd_install(c, unix_anon(u), (uint32_t)flags, cloexec);
        if (fd < 0) { unix_drop(u); return NEG(E_MFILE); }
        return (uint64_t)fd;
    }
    if (domain != AF_INET) return NEG(E_AFNOSUPPORT);
    if (!inet_ok(c)) { cap_audit(c->pid, CAP_NET, num, "socket", 0); return NEG(E_ACCES); }
    int kind;
    if (t == SOCK_STREAM) {
        if (proto != 0 && proto != 6) return NEG(E_PROTONOSUPPORT);
        kind = SKIND_TCP; proto = 6;
    } else if (t == SOCK_DGRAM) {
        if (proto == 0 || proto == 17) { kind = SKIND_UDP; proto = 17; }
        else if (proto == 1) kind = SKIND_PING;           /* ICMP ping socket: any CAP_NET process */
        else return NEG(E_PROTONOSUPPORT);
    } else if (t == SOCK_RAW) {
        if (proto <= 0 || proto > 255) return NEG(E_PROTONOSUPPORT);
        /* Raw IP: root, or a manifest that declares CAP_NET_RAW. */
        if (!cred_is_root(c) && !cap_check_explicit(c, CAP_NET_RAW, num, "socket(raw)"))
            return NEG(E_PERM);
        kind = SKIND_RAW;
    } else {
        return NEG(E_SOCKTNOSUPPORT);
    }
    int sx = socket_create_ex(kind, proto);
    if (sx < 0) return R(sx);
    int fd = ksys_sock_install(c, sx, (uint32_t)flags, cloexec);
    if (fd < 0) { socket_unref(sx); return NEG(E_MFILE); }
    return (uint64_t)fd;
}

static uint64_t sys_socketpair(process_t* c, uint64_t num, int domain, int type, int proto, uint64_t usv) {
    int flags = type & ~SOCK_TYPE_MASK, t = type & SOCK_TYPE_MASK;
    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) return NEG(E_INVAL);
    if (domain == AF_INET) return NEG(E_OPNOTSUPP);
    if (domain != AF_UNIX) return NEG(E_AFNOSUPPORT);
    if (!cap_check(c, CAP_IPC, num, "socketpair")) return NEG(E_PERM);
    if (proto != 0) return NEG(E_PROTONOSUPPORT);
    if (t != SOCK_STREAM && t != SOCK_DGRAM) return NEG(t == SOCK_SEQPACKET ? E_SOCKTNOSUPPORT : E_INVAL);
    if (!user_range_valid((void*)usv, 2 * sizeof(int))) return NEG(E_FAULT);
    usock_t *a, *b;
    int r = unix_socketpair(c, t, &a, &b);
    if (r) return R(r);
    int cl = (flags & SOCK_CLOEXEC) != 0;
    int fa = anonfd_install(c, unix_anon(a), (uint32_t)flags, cl);
    if (fa < 0) { unix_drop(a); unix_drop(b); return NEG(E_MFILE); }
    int fb = anonfd_install(c, unix_anon(b), (uint32_t)flags, cl);
    if (fb < 0) { ksys_close(fa); unix_drop(b); return NEG(E_MFILE); }
    int sv[2] = { fa, fb };
    if (copy_to_user((void*)usv, sv, sizeof(sv)) != 0) { ksys_close(fa); ksys_close(fb); return NEG(E_FAULT); }
    return 0;
}

/* ---------------------------------------------------------- bind/listen */
static uint64_t sys_bind(process_t* c, int fd, uint64_t uaddr, uint32_t alen) {
    sockref_t s; int r = sock_fd(c, fd, &s); if (r) return R(r);
    union lx_sockaddr a;
    if ((r = addr_in(uaddr, alen, &a))) return R(r);
    if (!s.inet) {
        if (a.family != AF_UNIX) return NEG(E_INVAL);
        return R(unix_bind(c, s.u, &a.un, alen));
    }
    if (a.family != AF_INET) return NEG(E_AFNOSUPPORT);
    if (alen < sizeof(struct lx_sockaddr_in)) return NEG(E_INVAL);
    uint16_t port = bswap16(a.in.sin_port);
    int kind = socket_kind(s.sx, NULL);
    if ((kind == SKIND_TCP || kind == SKIND_UDP) && port && port < 1024 && !cred_is_root(c))
        return NEG(E_ACCES);                          /* [M43] privileged port */
    return R(socket_bind_ex(s.sx, a.in.sin_addr, port));
}

static uint64_t sys_listen(process_t* c, int fd, int backlog) {
    sockref_t s; int r = sock_fd(c, fd, &s); if (r) return R(r);
    if (backlog < 0 || backlog > 4096) backlog = 4096;
    if (!s.inet) return R(unix_listen(c, s.u, backlog));
    return R(socket_listen(s.sx, backlog ? backlog : 1));
}

/* -------------------------------------------------------------- connect */
static uint64_t sys_connect(process_t* c, trapframe_t* tf, int fd, uint64_t uaddr, uint32_t alen) {
    sockref_t s; int r = sock_fd(c, fd, &s); if (r) return R(r);
    union lx_sockaddr a;
    if ((r = addr_in(uaddr, alen, &a))) return R(r);
    if (!s.inet) {
        if (a.family == AF_UNSPEC) return R(unix_disconnect(s.u));
        if (a.family != AF_UNIX) return NEG(E_INVAL);
        void* w = NULL;
        r = unix_connect(c, s.u, &a.un, alen, &w);
        if (r == -E_AGAIN && !s.nonblock)
            return R(lx_wait(tf, w, 0, unix_timeo(s.u, 1), -E_AGAIN));
        return R(r);
    }
    if (a.family == AF_UNSPEC) return R(socket_connect_ex(s.sx, 0, 0, 1));
    if (a.family != AF_INET) return NEG(E_AFNOSUPPORT);
    if (alen < sizeof(struct lx_sockaddr_in)) return NEG(E_INVAL);
    r = socket_connect_ex(s.sx, a.in.sin_addr, bswap16(a.in.sin_port), 0);
    if ((r == -E_INPROGRESS || r == -E_ALREADY) && !s.nonblock)
        return R(lx_wait(tf, NULL, NET_RECHECK, socket_timeo(s.sx, 1), -E_INPROGRESS));
    return R(r);
}

/* --------------------------------------------------------------- accept */
static uint64_t sys_accept4(process_t* c, trapframe_t* tf, int fd, uint64_t uaddr, uint64_t ulen, int flags) {
    sockref_t s; int r = sock_fd(c, fd, &s); if (r) return R(r);
    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) return NEG(E_INVAL);
    int cl = (flags & SOCK_CLOEXEC) != 0;
    if (!s.inet) {
        usock_t* n = NULL;
        r = unix_accept(s.u, &n);
        if (r == -E_AGAIN) {
            if (s.nonblock) return NEG(E_AGAIN);
            return R(lx_wait(tf, unix_anon(s.u), 0, unix_timeo(s.u, 0), -E_AGAIN));
        }
        if (r) return R(r);
        struct lx_sockaddr_un pn; uint32_t pl = 2;
        unix_getname(n, 1, &pn, &pl);
        int nfd = anonfd_install(c, unix_anon(n), (uint32_t)flags, cl);
        if (nfd < 0) { unix_drop(n); return NEG(E_MFILE); }
        if ((r = addr_out(uaddr, ulen, &pn, pl))) { ksys_close(nfd); return R(r); }
        return (uint64_t)nfd;
    }
    uint32_t ip = 0; uint16_t port = 0;
    int nsx = socket_accept_ex(s.sx, &ip, &port);
    if (nsx == -E_AGAIN) {
        if (s.nonblock) return NEG(E_AGAIN);
        return R(lx_wait(tf, NULL, NET_RECHECK, socket_timeo(s.sx, 0), -E_AGAIN));
    }
    if (nsx < 0) return R(nsx);
    int nfd = ksys_sock_install(c, nsx, (uint32_t)flags, cl);
    if (nfd < 0) { socket_unref(nsx); return NEG(E_MFILE); }
    struct lx_sockaddr_in sa; in_make(&sa, ip, port);
    if ((r = addr_out(uaddr, ulen, &sa, sizeof(sa)))) { ksys_close(nfd); return R(r); }
    return (uint64_t)nfd;
}

/* ------------------------------------------------------- send / receive */
typedef struct {
    int has_dst; union lx_sockaddr dst; uint32_t dlen;
} dst_t;

/* One send of len bytes from the kernel buffer k. */
static int do_send(process_t* c, trapframe_t* tf, sockref_t* s, const void* k, int len, int flags,
                   const dst_t* d, int* blocked_ret) {
    int nb = s->nonblock || (flags & SK_MSG_DONTWAIT);
    int r;
    *blocked_ret = 0;
    if (!s->inet) {
        if (d->has_dst && d->dst.family != AF_UNIX) return -E_INVAL;
        void* w = NULL;
        r = unix_send(c, s->u, k, len, flags, d->has_dst ? &d->dst.un : NULL, d->dlen, &w);
        if (r == -E_AGAIN && !nb) { *blocked_ret = 1; return (int)lx_wait(tf, w, 0, unix_timeo(s->u, 1), -E_AGAIN); }
        return r;
    }
    uint32_t ip = 0; uint16_t port = 0;
    if (d->has_dst) {
        if (d->dst.family != AF_INET) return -E_AFNOSUPPORT;
        if (d->dlen < sizeof(struct lx_sockaddr_in)) return -E_INVAL;
        ip = d->dst.in.sin_addr; port = bswap16(d->dst.in.sin_port);
    }
    int kind = socket_kind(s->sx, NULL);
    if ((flags & SK_MSG_FASTOPEN) && kind == SKIND_TCP && d->has_dst) {
        /* No TCP Fast Open: start (or collect) the connect, then send. */
        r = socket_connect_ex(s->sx, ip, port, 0);
        if (r == -E_INPROGRESS || r == -E_ALREADY) {
            if (nb) return -E_INPROGRESS;
            *blocked_ret = 1;
            return (int)lx_wait(tf, NULL, NET_RECHECK, socket_timeo(s->sx, 1), -E_INPROGRESS);
        }
        if (r && r != -106 /* EISCONN */) return r;
        r = socket_sendmsg(s->sx, k, len, flags & ~SK_MSG_FASTOPEN, 0, 0, 0);
    } else {
        r = socket_sendmsg(s->sx, k, len, flags, kind == SKIND_TCP ? 0 : d->has_dst, ip, port);
    }
    if (r == -E_AGAIN && !nb) {
        *blocked_ret = 1;
        return (int)lx_wait(tf, NULL, NET_RECHECK, socket_timeo(s->sx, 1), -E_AGAIN);
    }
    return r;
}

static void maybe_sigpipe(process_t* c, sockref_t* s, int r, int flags) {
    if (r != -E_PIPE || (flags & SK_MSG_NOSIGNAL)) return;
    int stream = s->inet ? (socket_kind(s->sx, NULL) == SKIND_TCP) : (unix_type(s->u) == SOCK_STREAM);
    if (stream) signal_post_thread(c, SIGPIPE);   /* [U9] to the writing thread (Linux) */
}

static uint64_t sys_sendto(process_t* c, trapframe_t* tf, int fd, uint64_t ubuf, uint64_t len,
                           int flags, uint64_t uaddr, uint32_t alen) {
    sockref_t s; int r = sock_fd(c, fd, &s); if (r) return R(r);
    if ((int64_t)len < 0) return NEG(E_INVAL);
    dst_t d; d.has_dst = 0; d.dlen = 0;
    if (uaddr) {
        if ((r = addr_in(uaddr, alen, &d.dst))) return R(r);
        d.has_dst = 1; d.dlen = alen;
    }
    int n = len > IO_MAX ? IO_MAX : (int)len;
    if (!s.inet && unix_type(s.u) == SOCK_DGRAM && len > IO_MAX) return NEG(90);   /* EMSGSIZE */
    uint8_t* k = n ? (uint8_t*)kmalloc((size_t)n) : NULL;
    if (n && !k) return NEG(E_NOMEM);
    if (n && copy_from_user(k, (const void*)ubuf, (size_t)n) != 0) { kfree(k); return NEG(E_FAULT); }
    int blocked;
    r = do_send(c, tf, &s, k, n, flags, &d, &blocked);
    if (k) kfree(k);
    maybe_sigpipe(c, &s, r, flags);
    return R(r);
}

/* Receive into the kernel buffer; fills the source address (Linux shape). */
static int do_recv(process_t* c, trapframe_t* tf, sockref_t* s, void* k, int len, int flags,
                   union lx_sockaddr* from, uint32_t* fromlen, sock_rxmeta_t* m, uint32_t* full) {
    (void)c;
    int nb = s->nonblock || (flags & SK_MSG_DONTWAIT) || (flags & SK_MSG_ERRQUEUE);
    int r;
    *fromlen = 0; *full = 0;
    if (!s->inet) {
        if (flags & SK_MSG_ERRQUEUE) return -E_AGAIN;
        r = unix_recv(s->u, k, len, flags, &from->un, fromlen, full);
        if (r == -E_AGAIN && !nb) return (int)lx_wait(tf, unix_anon(s->u), 0, unix_timeo(s->u, 0), -E_AGAIN);
        return r;
    }
    r = socket_recvmsg(s->sx, k, len, flags, m);
    if (r == -E_AGAIN && !nb) return (int)lx_wait(tf, NULL, NET_RECHECK, socket_timeo(s->sx, 0), -E_AGAIN);
    if (r >= 0 && m->has_addr) { in_make(&from->in, m->ip, m->port); *fromlen = sizeof(struct lx_sockaddr_in); }
    *full = m->full_len ? m->full_len : (uint32_t)(r > 0 ? r : 0);
    return r;
}

static uint64_t sys_recvfrom(process_t* c, trapframe_t* tf, int fd, uint64_t ubuf, uint64_t len,
                             int flags, uint64_t uaddr, uint64_t ulen) {
    sockref_t s; int r = sock_fd(c, fd, &s); if (r) return R(r);
    if ((int64_t)len < 0) return NEG(E_INVAL);
    int n = len > IO_MAX ? IO_MAX : (int)len;
    uint8_t* k = (uint8_t*)kmalloc(n ? (size_t)n : 1);
    if (!k) return NEG(E_NOMEM);
    union lx_sockaddr from; uint32_t fl = 0, full = 0; sock_rxmeta_t m;
    bz(&m, sizeof(m));
    r = do_recv(c, tf, &s, k, n, flags, &from, &fl, &m, &full);
    if (r > 0 && copy_to_user((void*)ubuf, k, (size_t)r) != 0) r = -E_FAULT;
    kfree(k);
    if (r >= 0 && fl) { int e = addr_out(uaddr, ulen, &from, fl); if (e) return R(e); }
    if (r >= 0 && (flags & SK_MSG_TRUNC) && full > (uint32_t)r) r = (int)full;
    return R(r);
}

/* Gather/scatter: the iovec array of a msghdr, validated. */
#define IOV_MAX_ 1024
static int iov_load(const struct lx_msghdr* mh, struct lx_iovec** out, uint64_t* total) {
    *out = NULL; *total = 0;
    if (mh->iovlen > IOV_MAX_) return -E_INVAL;
    if (!mh->iovlen) return 0;
    struct lx_iovec* v = (struct lx_iovec*)kmalloc((size_t)mh->iovlen * sizeof(*v));
    if (!v) return -E_NOMEM;
    if (copy_from_user(v, (const void*)mh->iov, (size_t)mh->iovlen * sizeof(*v)) != 0) { kfree(v); return -E_FAULT; }
    uint64_t t = 0;
    for (uint64_t i = 0; i < mh->iovlen; i++) {
        if ((int64_t)v[i].len < 0) { kfree(v); return -E_INVAL; }
        t += v[i].len;
        if (t > 0x7FFFF000ull) { kfree(v); return -E_INVAL; }
    }
    *out = v; *total = t;
    return 0;
}

static uint64_t sys_sendmsg(process_t* c, trapframe_t* tf, int fd, uint64_t umsg, int flags) {
    sockref_t s; int r = sock_fd(c, fd, &s); if (r) return R(r);
    struct lx_msghdr mh;
    if (copy_from_user(&mh, (const void*)umsg, sizeof(mh)) != 0) return NEG(E_FAULT);
    if (mh.controllen) {
        /* Ancillary data: only refuse what would be silently wrong. */
        if (mh.controllen >= sizeof(struct lx_cmsghdr)) {
            struct lx_cmsghdr ch;
            if (copy_from_user(&ch, (const void*)mh.control, sizeof(ch)) != 0) return NEG(E_FAULT);
            if (ch.level == 1 /* SOL_SOCKET */ && (ch.type == 1 /* SCM_RIGHTS */ || ch.type == 2 /* SCM_CREDENTIALS */))
                return NEG(E_OPNOTSUPP);
        }
    }
    dst_t d; d.has_dst = 0; d.dlen = 0;
    if (mh.name && mh.namelen) {
        if ((r = addr_in(mh.name, mh.namelen, &d.dst))) return R(r);
        d.has_dst = 1; d.dlen = mh.namelen;
    }
    struct lx_iovec* v; uint64_t total;
    if ((r = iov_load(&mh, &v, &total))) return R(r);
    int n = total > IO_MAX ? IO_MAX : (int)total;
    if (!s.inet && unix_type(s.u) == SOCK_DGRAM && total > IO_MAX) { if (v) kfree(v); return NEG(90); }
    uint8_t* k = (uint8_t*)kmalloc(n ? (size_t)n : 1);
    if (!k) { if (v) kfree(v); return NEG(E_NOMEM); }
    int off = 0;
    for (uint64_t i = 0; v && i < mh.iovlen && off < n; i++) {
        uint64_t take = v[i].len; if (take > (uint64_t)(n - off)) take = (uint64_t)(n - off);
        if (take && copy_from_user(k + off, (const void*)v[i].base, (size_t)take) != 0) { kfree(k); kfree(v); return NEG(E_FAULT); }
        off += (int)take;
    }
    if (v) kfree(v);
    int blocked;
    r = do_send(c, tf, &s, k, n, flags, &d, &blocked);
    kfree(k);
    maybe_sigpipe(c, &s, r, flags);
    return R(r);
}

/* Append one cmsg into the kernel control buffer. */
static int cmsg_put(uint8_t* cb, uint64_t cap, uint64_t* used, int level, int type,
                    const void* data, uint32_t dlen, int* trunc) {
    uint64_t need = sizeof(struct lx_cmsghdr) + dlen;
    uint64_t space = (need + 7) & ~7ull;
    if (*used + need > cap) { *trunc = 1; return -1; }
    struct lx_cmsghdr h = { need, level, type };
    uint8_t* p = cb + *used;
    const uint8_t* hs = (const uint8_t*)&h;
    for (uint32_t i = 0; i < sizeof(h); i++) p[i] = hs[i];
    const uint8_t* ds = (const uint8_t*)data;
    for (uint32_t i = 0; i < dlen; i++) p[sizeof(h) + i] = ds[i];
    for (uint64_t i = need; i < space && *used + i < cap; i++) p[i] = 0;
    *used += (*used + space <= cap) ? space : need;
    return 0;
}

static uint64_t sys_recvmsg(process_t* c, trapframe_t* tf, int fd, uint64_t umsg, int flags) {
    sockref_t s; int r = sock_fd(c, fd, &s); if (r) return R(r);
    struct lx_msghdr mh;
    if (copy_from_user(&mh, (const void*)umsg, sizeof(mh)) != 0) return NEG(E_FAULT);
    struct lx_iovec* v; uint64_t total;
    if ((r = iov_load(&mh, &v, &total))) return R(r);
    int n = total > IO_MAX ? IO_MAX : (int)total;
    uint8_t* k = (uint8_t*)kmalloc(n ? (size_t)n : 1);
    if (!k) { if (v) kfree(v); return NEG(E_NOMEM); }
    union lx_sockaddr from; uint32_t fl = 0, full = 0; sock_rxmeta_t m;
    bz(&m, sizeof(m));
    r = do_recv(c, tf, &s, k, n, flags, &from, &fl, &m, &full);
    if (r < 0) { kfree(k); if (v) kfree(v); return R(r); }
    int off = 0;
    for (uint64_t i = 0; v && i < mh.iovlen && off < r; i++) {
        uint64_t put = v[i].len; if (put > (uint64_t)(r - off)) put = (uint64_t)(r - off);
        if (put && copy_to_user((void*)v[i].base, k + off, (size_t)put) != 0) { kfree(k); kfree(v); return NEG(E_FAULT); }
        off += (int)put;
    }
    kfree(k); if (v) kfree(v);
    int oflags = 0;
    if (full > (uint32_t)r && (!s.inet ? unix_type(s.u) == SOCK_DGRAM : socket_kind(s.sx, NULL) != SKIND_TCP))
        oflags |= SK_MSG_TRUNC;
    /* Ancillary data. */
    uint64_t used = 0; int ctrunc = 0;
    uint64_t ccap = mh.controllen > 512 ? 512 : mh.controllen;
    uint8_t cb[512];
    if (s.inet && mh.control && ccap) {
        if (flags & SK_MSG_ERRQUEUE) {
            struct { struct lx_sock_extended_err e; struct lx_sockaddr_in off; } ee;
            bz(&ee, sizeof(ee));
            ee.e.ee_errno = m.ee_errno; ee.e.ee_origin = m.ee_origin;
            ee.e.ee_type = m.ee_type; ee.e.ee_code = m.ee_code; ee.e.ee_info = m.ee_info;
            in_make(&ee.off, m.offender, 0);
            cmsg_put(cb, ccap, &used, 0 /* SOL_IP */, 11 /* IP_RECVERR */, &ee, sizeof(ee), &ctrunc);
        } else {
            if (m.want_ts) {
                int64_t tv[2] = { (int64_t)(m.ts_ns / 1000000000ull), (int64_t)((m.ts_ns % 1000000000ull) / 1000ull) };
                cmsg_put(cb, ccap, &used, 1, 29 /* SO_TIMESTAMP */, tv, sizeof(tv), &ctrunc);
            }
            if (m.want_tsns) {
                int64_t ts[2] = { (int64_t)(m.ts_ns / 1000000000ull), (int64_t)(m.ts_ns % 1000000000ull) };
                cmsg_put(cb, ccap, &used, 1, 35 /* SO_TIMESTAMPNS */, ts, sizeof(ts), &ctrunc);
            }
            if (m.want_ttl && m.has_addr) {
                int ttl = m.ttl;
                cmsg_put(cb, ccap, &used, 0, 2 /* IP_TTL */, &ttl, sizeof(ttl), &ctrunc);
            }
            if (m.want_tos && m.has_addr) {
                uint8_t tos = m.tos;
                cmsg_put(cb, ccap, &used, 0, 1 /* IP_TOS */, &tos, 1, &ctrunc);
            }
        }
    } else if (mh.controllen > 0 && (flags & SK_MSG_ERRQUEUE)) {
        ctrunc = 1;
    }
    if (ctrunc) oflags |= SK_MSG_CTRUNC;
    if (flags & SK_MSG_ERRQUEUE) oflags |= SK_MSG_ERRQUEUE;
    if (used && copy_to_user((void*)mh.control, cb, (size_t)used) != 0) return NEG(E_FAULT);
    /* Write back msg_namelen, msg_controllen, msg_flags. */
    uint32_t nl = 0;
    if (mh.name && fl) {
        uint32_t cpy = mh.namelen < fl ? mh.namelen : fl;
        if (cpy && copy_to_user((void*)mh.name, &from, cpy) != 0) return NEG(E_FAULT);
        nl = fl;
    }
    struct lx_msghdr* um = (struct lx_msghdr*)umsg;
    if (copy_to_user(&um->namelen, &nl, sizeof(nl)) != 0) return NEG(E_FAULT);
    if (copy_to_user(&um->controllen, &used, sizeof(used)) != 0) return NEG(E_FAULT);
    int32_t of = oflags;
    if (copy_to_user(&um->flags, &of, sizeof(of)) != 0) return NEG(E_FAULT);
    if ((flags & SK_MSG_TRUNC) && full > (uint32_t)r) r = (int)full;
    return R(r);
}

/* ------------------------------------------------------- the rest */
static uint64_t sys_shutdown(process_t* c, int fd, int how) {
    sockref_t s; int r = sock_fd(c, fd, &s); if (r) return R(r);
    return R(s.inet ? socket_shutdown(s.sx, how) : unix_shutdown(s.u, how));
}

static uint64_t sys_getname(process_t* c, int fd, uint64_t uaddr, uint64_t ulen, int peer) {
    sockref_t s; int r = sock_fd(c, fd, &s); if (r) return R(r);
    if (!s.inet) {
        struct lx_sockaddr_un un; uint32_t l;
        if ((r = unix_getname(s.u, peer, &un, &l))) return R(r);
        return R(addr_out(uaddr, ulen, &un, l));
    }
    uint32_t ip; uint16_t port;
    if ((r = socket_name(s.sx, peer, &ip, &port))) return R(r);
    struct lx_sockaddr_in sa; in_make(&sa, ip, port);
    return R(addr_out(uaddr, ulen, &sa, sizeof(sa)));
}

static uint64_t sys_setsockopt(process_t* c, int fd, int level, int name, uint64_t uval, uint32_t len) {
    sockref_t s; int r = sock_fd(c, fd, &s); if (r) return R(r);
    if ((int32_t)len < 0) return NEG(E_INVAL);
    uint8_t kv[64];
    uint32_t n = len > sizeof(kv) ? sizeof(kv) : len;
    bz(kv, sizeof(kv));
    if (n && copy_from_user(kv, (const void*)uval, n) != 0) return NEG(E_FAULT);
    if (!s.inet) return R(unix_setopt(s.u, level, name, kv, n));
    return R(socket_setopt(s.sx, level, name, kv, n, cred_is_root(c)));
}

static uint64_t sys_getsockopt(process_t* c, int fd, int level, int name, uint64_t uval, uint64_t ulen) {
    sockref_t s; int r = sock_fd(c, fd, &s); if (r) return R(r);
    int32_t have;
    if (copy_from_user(&have, (const void*)ulen, sizeof(have)) != 0) return NEG(E_FAULT);
    if (have < 0) return NEG(E_INVAL);
    uint8_t kv[64]; bz(kv, sizeof(kv));
    uint32_t n = (uint32_t)have > sizeof(kv) ? sizeof(kv) : (uint32_t)have;
    r = s.inet ? socket_getopt(s.sx, level, name, kv, &n) : unix_getopt(s.u, level, name, kv, &n);
    if (r) return R(r);
    if (n && copy_to_user((void*)uval, kv, n) != 0) return NEG(E_FAULT);
    int32_t out = (int32_t)n;
    if (copy_to_user((void*)ulen, &out, sizeof(out)) != 0) return NEG(E_FAULT);
    return 0;
}

/* [U10] FIONREAD / FIONBIO on an AF_INET socket fd (kernel/syscall.c ioctl). */
long lx_net_inet_ioctl(process_t* c, int fd, uint64_t cmd, uint64_t arg) {
    if (fd < 0 || fd >= 32 || !c->fds[fd].used || !c->fds[fd].is_sock) return -E_BADF;
    int v;
    switch (cmd) {
    case 0x5421:                                   /* FIONBIO */
        if (copy_from_user(&v, (const void*)arg, sizeof(v)) != 0) return -E_FAULT;
        if (v) c->fds[fd].flags |= O_NONBLOCK_; else c->fds[fd].flags &= ~(uint32_t)O_NONBLOCK_;
        return 0;
    case 0x541B:                                   /* FIONREAD */
        v = socket_nread(c->fds[fd].is_sock - 1);
        if (v < 0) return v;
        if (copy_to_user((void*)arg, &v, sizeof(v)) != 0) return -E_FAULT;
        return 0;
    }
    return -25;                                    /* ENOTTY */
}

int lx_net_syscall(trapframe_t* tf, uint64_t num, uint64_t* ret) {
    process_t* c = sched_get_current();
    uint64_t a0 = tf->rdi, a1 = tf->rsi, a2 = tf->rdx, a3 = tf->rcx, a4 = tf->r8, a5 = tf->r9;
    if (!c) return 0;
    switch (num) {
    case LX(LXN_SOCKET):      *ret = sys_socket(c, num, (int)a0, (int)a1, (int)a2); return 1;
    case LX(LXN_SOCKETPAIR):  *ret = sys_socketpair(c, num, (int)a0, (int)a1, (int)a2, a3); return 1;
    case LX(LXN_BIND):        *ret = sys_bind(c, (int)a0, a1, (uint32_t)a2); return 1;
    case LX(LXN_LISTEN):      *ret = sys_listen(c, (int)a0, (int)a1); return 1;
    case LX(LXN_CONNECT):     *ret = sys_connect(c, tf, (int)a0, a1, (uint32_t)a2); return 1;
    case LX(LXN_ACCEPT):      *ret = sys_accept4(c, tf, (int)a0, a1, a2, 0); return 1;
    case LX(LXN_ACCEPT4):     *ret = sys_accept4(c, tf, (int)a0, a1, a2, (int)a3); return 1;
    case LX(LXN_SENDTO):      *ret = sys_sendto(c, tf, (int)a0, a1, a2, (int)a3, a4, (uint32_t)a5); return 1;
    case LX(LXN_RECVFROM):    *ret = sys_recvfrom(c, tf, (int)a0, a1, a2, (int)a3, a4, a5); return 1;
    case LX(LXN_SENDMSG):     *ret = sys_sendmsg(c, tf, (int)a0, a1, (int)a2); return 1;
    case LX(LXN_RECVMSG):     *ret = sys_recvmsg(c, tf, (int)a0, a1, (int)a2); return 1;
    case LX(LXN_SHUTDOWN):    *ret = sys_shutdown(c, (int)a0, (int)a1); return 1;
    case LX(LXN_GETSOCKNAME): *ret = sys_getname(c, (int)a0, a1, a2, 0); return 1;
    case LX(LXN_GETPEERNAME): *ret = sys_getname(c, (int)a0, a1, a2, 1); return 1;
    case LX(LXN_SETSOCKOPT):  *ret = sys_setsockopt(c, (int)a0, (int)a1, (int)a2, a3, (uint32_t)a4); return 1;
    case LX(LXN_GETSOCKOPT):  *ret = sys_getsockopt(c, (int)a0, (int)a1, (int)a2, a3, a4); return 1;
    }
    return 0;
}
