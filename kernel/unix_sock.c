/*
 * unix_sock.c — [U10] AF_UNIX sockets: SOCK_STREAM and SOCK_DGRAM, named by a
 * filesystem path or an abstract name, socketpair(), SO_PEERCRED.
 * SPDX-License-Identifier: MIT
 *
 * Each socket is an anonfd (kernel/anonfd.h) of kind g_unix_ops. All of them,
 * their buffers, the accept queues and the name table are guarded by one
 * spinlock, g_unix_lock (S13); the syscall layer (kernel/lx_net.c) moves data
 * between user memory and kernel bounce buffers outside it, and every wake
 * (anonfd_wake) happens after the unlock.
 *
 * Names:
 *  - a path (sun_path[0] != 0) is a file: bind() creates it (mode 0777 &
 *    ~umask, owned by the caller, EADDRINUSE if the name exists) after the
 *    same checks as creat(); connect()/sendto() resolve it with search
 *    permission on every directory and need WRITE permission on the file
 *    (Linux). The binding remembers the file's inode, so a socket file that
 *    was removed and recreated by someone else never reaches the old socket.
 *    close() leaves the file in place (unlink it, as on Linux). The file is a
 *    regular file on disk: the SecOS filesystems cannot store S_IFSOCK yet;
 *  - an abstract name (sun_path[0] == 0) lives only in the kernel table; no
 *    permission check (Linux), released with the socket. bind() with only
 *    the family autobinds a 5-hex-digit abstract name.
 *
 * Lifetime: anonfd refs are fd references. The last one runs unix_release
 * ("close"): it unhooks the socket from its peer (the peer then reads EOF /
 * gets EPIPE or ECONNREFUSED), from every datagram socket connected to it and
 * from the name table, drops the not-yet-accepted connections of a listener,
 * and frees the memory. Peer pointers never outlive that unhook (both sides
 * are cleared under the lock), so they carry no reference.
 *
 * Not yet: SCM_RIGHTS / SCM_CREDENTIALS ancillary data, SOCK_SEQPACKET.
 */
#include <stdint.h>
#include <stddef.h>
#include "process.h"
#include "anonfd.h"
#include "cred.h"
#include "heap.h"
#include "signal.h"
#include "../fs/vfs.h"
#include "../arch/x86/spinlock.h"
#include "user_copy.h"
#include "unix_sock.h"

extern process_t* sched_get_current(void);
extern void proc_resolve_path(process_t* c, const char* in, char* out, int outsz);

#define U_EPERM 1
#define U_ENOENT 2
#define U_EAGAIN 11
#define U_ENOMEM 12
#define U_EACCES 13
#define U_EFAULT 14
#define U_EINVAL 22
#define U_EPIPE 32
#define U_ENOTSOCK 88
#define U_EDESTADDRREQ 89
#define U_EMSGSIZE 90
#define U_EPROTOTYPE 91
#define U_ENOPROTOOPT 92
#define U_EOPNOTSUPP 95
#define U_EADDRINUSE 98
#define U_ECONNRESET 104
#define U_EISCONN 106
#define U_ENOTCONN 107
#define U_ECONNREFUSED 111

#define U_BUF        65536          /* stream receive ring */
#define U_DGRAM_MAX  65536          /* largest datagram */
#define U_DGRAM_Q    212992         /* bytes queued per datagram socket */
#define U_BACKLOG    128

enum { US_UNCONN = 0, US_LISTEN, US_CONNECTED };

struct udg {                        /* a queued datagram */
    struct udg* next;
    uint32_t len;
    struct lx_sockaddr_un from; uint32_t fromlen;
    uint8_t data[];
};

typedef struct usock {
    anonfd_t a;
    struct usock* next_all;          /* every live socket (name + peer scans) */
    int type;                        /* 1 = SOCK_STREAM, 2 = SOCK_DGRAM */
    int state;
    struct usock* peer;              /* stream: the other end; dgram: connect() target */
    uint8_t peer_closed;             /* the peer went away (EOF / ECONNREFUSED) */
    uint8_t shut;                    /* 1 = SHUT_RD, 2 = SHUT_WR (ours) */
    uint8_t passcred;
    /* name */
    uint8_t named;
    struct lx_sockaddr_un name; uint32_t namelen;   /* as getsockname returns it */
    const void* name_ino;            /* path names: the socket file's inode, */
    uint64_t name_inonum;            /* its st_ino and its canonical path */
    char name_canon[256];
    /* stream data (received) */
    uint8_t* rbuf; uint32_t rhead, rlen;
    /* datagrams (received) */
    struct udg *dq_head, *dq_tail; uint32_t dq_bytes;
    /* listener */
    struct usock* aq[U_BACKLOG]; int aq_n, backlog;
    /* credentials: ours (when we listened / connected) and the peer's */
    struct lx_ucred cred, pcred; uint8_t has_pcred;
    uint64_t rcvtimeo_ns, sndtimeo_ns;
} usock_t;

static spinlock_t g_unix_lock = SPINLOCK_INIT_CLASS(LK_S13_UNIX, "unix");
static usock_t* g_all;                     /* g_unix_lock */
static uint32_t g_autobind = 0x10000;

static void unix_release(anonfd_t* f);
static int  unix_op_read(process_t* c, anonfd_t* f, void* ubuf, int len, int nb);
static int  unix_op_write(process_t* c, anonfd_t* f, const void* ubuf, int len, int nb);
static int  unix_op_poll(anonfd_t* f);
static long unix_op_ioctl(process_t* c, anonfd_t* f, uint64_t cmd, uint64_t arg);

static const anonfd_ops_t g_unix_ops = {
    .name = "socket:[unix]", .mode = 0140777,
    .read = unix_op_read, .write = unix_op_write, .poll = unix_op_poll,
    .ioctl = unix_op_ioctl, .release = unix_release,
};

static void bz(void* p, size_t n) { uint8_t* b = (uint8_t*)p; while (n--) *b++ = 0; }
static void bc(void* d, const void* s, size_t n) { uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while (n--) *a++ = *b++; }

usock_t* unix_of(const void* ino) { return (usock_t*)anonfd_of_kind(ino, &g_unix_ops); }
int unix_is_sock(const void* ino) { return unix_of(ino) != 0; }

static void cred_now(process_t* c, struct lx_ucred* u) {
    u->pid = c ? (int32_t)c->tgid : 0;              /* [U9] getpid(), not the tid */
    u->uid = c ? c->cred.euid : 0;
    u->gid = c ? c->cred.egid : 0;
}

/* A new socket object (refs = 1, owned by the caller). g_unix_lock NOT held. */
static usock_t* us_new(process_t* c, int type) {
    usock_t* u = (usock_t*)kmalloc(sizeof(usock_t));
    if (!u) return NULL;
    bz(u, sizeof(*u));
    anonfd_init(&u->a, &g_unix_ops);
    u->type = type;
    cred_now(c, &u->cred);
    if (type == 1) {
        u->rbuf = (uint8_t*)kmalloc(U_BUF);
        if (!u->rbuf) { kfree(u); return NULL; }
    }
    uint64_t fl = spin_lock_irqsave(&g_unix_lock);
    u->next_all = g_all; g_all = u;
    spin_unlock_irqrestore(&g_unix_lock, fl);
    return u;
}

/* g_unix_lock held: unlink from the global list. */
static void us_unlist(usock_t* u) {
    for (usock_t** pp = &g_all; *pp; pp = &(*pp)->next_all)
        if (*pp == u) { *pp = u->next_all; break; }
}

static void us_free(usock_t* u) {
    while (u->dq_head) { struct udg* d = u->dq_head; u->dq_head = d->next; kfree(d); }
    if (u->rbuf) kfree(u->rbuf);
    kfree(u);
}

/* Close (g_unix_lock held): unhook u from everything. Returns the sockets to
 * wake; the caller frees u and the embryos after the unlock. */
static int us_close_locked(usock_t* u, usock_t** wake, int nwake, usock_t** dead, int* ndead) {
    int n = 0;
    us_unlist(u);
    if (u->peer) {
        usock_t* p = u->peer;
        if (u->type == 1 || p->peer == u) { p->peer = NULL; p->peer_closed = 1; }
        if (n < nwake) wake[n++] = p;
        u->peer = NULL;
    }
    for (usock_t* o = g_all; o; o = o->next_all)          /* datagram sockets connected to u */
        if (o->peer == u) { o->peer = NULL; o->peer_closed = 1; if (n < nwake) wake[n++] = o; }
    for (int i = 0; i < u->aq_n; i++) {                    /* never-accepted connections */
        usock_t* e = u->aq[i];
        us_unlist(e);
        if (e->peer) { e->peer->peer = NULL; e->peer->peer_closed = 1; if (n < nwake) wake[n++] = e->peer; }
        e->peer = NULL;
        dead[(*ndead)++] = e;
    }
    u->aq_n = 0;
    return n;
}

static void unix_release(anonfd_t* f) {
    usock_t* u = (usock_t*)f;
    usock_t* wake[8]; usock_t* dead[U_BACKLOG]; int nd = 0;
    uint64_t fl = spin_lock_irqsave(&g_unix_lock);
    int nw = us_close_locked(u, wake, 8, dead, &nd);
    spin_unlock_irqrestore(&g_unix_lock, fl);
    for (int i = 0; i < nw; i++) anonfd_wake(&wake[i]->a);
    for (int i = 0; i < nd; i++) us_free(dead[i]);
    us_free(u);
}

/* ---------------------------------------------------------------- names */
/* Parse a sockaddr_un (kernel copy). Abstract: *abs = 1, key = bytes after
 * the leading NUL. Path: the NUL-terminated string. */
static int name_parse(const struct lx_sockaddr_un* a, uint32_t alen, int* abs, char* path, uint32_t* plen) {
    if (alen < 2 || alen > sizeof(*a) || a->sun_family != 1 /* AF_UNIX */) return -U_EINVAL;
    uint32_t n = alen - 2;
    if (n == 0) { *abs = 2; *plen = 0; return 0; }          /* autobind / unnamed */
    if (a->sun_path[0] == 0) { *abs = 1; *plen = n; bc(path, a->sun_path, n); return 0; }
    uint32_t k = 0;
    while (k < n && a->sun_path[k]) k++;
    if (k >= 108) return -U_EINVAL;
    *abs = 0; bc(path, a->sun_path, k); path[k] = 0; *plen = k;
    return 0;
}

/* g_unix_lock held: the socket bound to this name. */
static int canon_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static usock_t* name_find(int abs, const char* key, uint32_t klen, const void* ino, uint64_t inonum,
                          const char* canon) {
    for (usock_t* o = g_all; o; o = o->next_all) {
        if (!o->named) continue;
        if (abs) {
            if (o->name.sun_path[0] != 0 || o->namelen != 2 + klen) continue;
            int eq = 1;
            for (uint32_t i = 0; i < klen && eq; i++) if (o->name.sun_path[i] != key[i]) eq = 0;
            if (eq) return o;
        } else {
            if (o->name.sun_path[0] == 0 || !o->name_ino || o->name_ino != ino ||
                o->name_inonum != inonum || !canon_eq(o->name_canon, canon)) continue;
            return o;
        }
    }
    return NULL;
}

/* Resolve a path name for connect/sendto: the socket file's inode, write
 * permission required. */
static int path_target(process_t* c, const char* path, const void** ino, uint64_t* inonum, char* canon) {
    char ab[256];
    proc_resolve_path(c, path, ab, sizeof(ab));
    vfs_inode_t* v = NULL;
    int e = cred_check_path(c, ab, 1, MAY_WRITE, canon, 256, &v);
    if (e) return e;                        /* -ENOENT / -EACCES / -ENOTDIR ... */
    if (!v) return -U_ECONNREFUSED;
    if (v->type == VFS_NODE_DIR) return -U_ECONNREFUSED;
    *ino = v; *inonum = v->ino;
    return 0;
}

int unix_bind(process_t* c, usock_t* u, const struct lx_sockaddr_un* a, uint32_t alen) {
    int abs; char key[110]; uint32_t klen;
    int e = name_parse(a, alen, &abs, key, &klen);
    if (e) return e;
    const void* ino = NULL; uint64_t inonum = 0;
    struct lx_sockaddr_un nm; bz(&nm, sizeof(nm)); nm.sun_family = 1;
    uint32_t nmlen;
    char cp[256]; cp[0] = 0;
    if (__atomic_load_n(&u->named, __ATOMIC_ACQUIRE)) return -U_EINVAL;   /* before creating a file */
    if (abs == 0) {
        /* A file, created like creat(path, 0777) but failing if it exists. */
        char ab[256];
        proc_resolve_path(c, key, ab, sizeof(ab));
        if ((e = cred_resolve(c, ab, 0, cp, sizeof(cp)))) return e;
        if (vfs_lookup(cp)) return -U_EADDRINUSE;
        if ((e = cred_may_create(c, cp))) return e;
        vfs_attr_t na; unsigned nv = cred_new_attr(c, cp, 0777, 0, &na);
        if (vfs_create_attr(cp, &na, nv) != 0) return vfs_lookup(cp) ? -U_EADDRINUSE : -U_EACCES;
        vfs_inode_t* v = vfs_lookup(cp);
        if (!v) return -U_ENOENT;
        ino = v; inonum = v->ino;
        bc(nm.sun_path, key, klen + 1);
        nmlen = 2 + klen + 1;
    } else {
        nmlen = 2 + klen;
        if (abs == 1) bc(nm.sun_path, key, klen);
    }
    uint64_t fl = spin_lock_irqsave(&g_unix_lock);
    if (u->named) { spin_unlock_irqrestore(&g_unix_lock, fl); return -U_EINVAL; }
    if (abs == 2) {                                        /* autobind: "\0" + 5 hex digits */
        for (int tries = 0; tries < 0x100000; tries++) {
            uint32_t v = g_autobind++ & 0xFFFFF;
            char h[6]; h[0] = 0;
            for (int i = 0; i < 5; i++) { uint32_t d = (v >> (16 - 4 * i)) & 0xF; h[1 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); }
            if (!name_find(1, h, 6, NULL, 0, NULL)) { bc(nm.sun_path, h, 6); nmlen = 2 + 6; break; }
        }
    } else if (abs == 1 && name_find(1, key, klen, NULL, 0, NULL)) {
        spin_unlock_irqrestore(&g_unix_lock, fl);
        return -U_EADDRINUSE;
    }
    if (abs == 0) {
        /* An older binding to the same inode is stale (its file was removed
         * and the inode slot reused): it is no longer reachable by name. */
        for (usock_t* o = g_all; o; o = o->next_all)
            if (o != u && o->named && o->name_ino == ino && o->name_inonum == inonum) o->name_ino = NULL;
        int k = 0; for (; cp[k] && k < 255; k++) u->name_canon[k] = cp[k]; u->name_canon[k] = 0;
    }
    u->name = nm; u->namelen = nmlen; u->name_ino = ino; u->name_inonum = inonum; u->named = 1;
    spin_unlock_irqrestore(&g_unix_lock, fl);
    return 0;
}

int unix_listen(process_t* c, usock_t* u, int backlog) {
    if (u->type != 1) return -U_EOPNOTSUPP;
    uint64_t fl = spin_lock_irqsave(&g_unix_lock);
    int r = 0;
    if (u->state == US_CONNECTED) r = -U_EINVAL;
    else if (!u->named) r = -U_EINVAL;                     /* Linux: EINVAL when unbound */
    else {
        u->state = US_LISTEN;
        u->backlog = backlog < 1 ? 1 : backlog > U_BACKLOG ? U_BACKLOG : backlog;
        cred_now(c, &u->cred);
    }
    spin_unlock_irqrestore(&g_unix_lock, fl);
    return r;
}

/* connect(): stream = queue a new server-side socket on the listener; dgram =
 * set the default destination. -EAGAIN: the listener's backlog is full (the
 * caller blocks on *wait_on). */
int unix_connect(process_t* c, usock_t* u, const struct lx_sockaddr_un* a, uint32_t alen, void** wait_on) {
    int abs; char key[110]; uint32_t klen;
    int e = name_parse(a, alen, &abs, key, &klen);
    if (e) return e;
    if (abs == 2) return -U_EINVAL;
    const void* ino = NULL; uint64_t inonum = 0;
    char canon[256]; canon[0] = 0;
    if (abs == 0 && (e = path_target(c, key, &ino, &inonum, canon))) return e;
    usock_t* emb = NULL;
    if (u->type == 1) {
        emb = us_new(c, 1);                    /* the server side, before taking the lock */
        if (!emb) return -U_ENOMEM;
    }
    usock_t* wake = NULL;
    uint64_t fl = spin_lock_irqsave(&g_unix_lock);
    usock_t* t = name_find(abs, key, klen, ino, inonum, canon);
    int r = 0;
    if (!t) r = -U_ECONNREFUSED;
    else if (t->type != u->type) r = -U_EPROTOTYPE;
    else if (u->type == 2) {
        u->peer = t; u->peer_closed = 0;
        cred_now(c, &u->cred);
    } else if (u->state == US_CONNECTED) r = -U_EISCONN;
    else if (u->state == US_LISTEN) r = -U_EINVAL;
    else if (t->state != US_LISTEN) r = -U_ECONNREFUSED;
    else if (t->aq_n >= t->backlog) { r = -U_EAGAIN; *wait_on = &t->a; }
    else {
        cred_now(c, &u->cred);
        emb->state = US_CONNECTED; emb->peer = u; emb->cred = t->cred;
        emb->pcred = u->cred; emb->has_pcred = 1;
        emb->name = t->name; emb->namelen = t->namelen;          /* accept()ed socket's name */
        u->state = US_CONNECTED; u->peer = emb; u->peer_closed = 0;
        u->pcred = t->cred; u->has_pcred = 1;
        t->aq[t->aq_n++] = emb;
        emb = NULL;                                             /* now queued */
        wake = t;
    }
    if (emb) us_unlist(emb);
    spin_unlock_irqrestore(&g_unix_lock, fl);
    if (emb) us_free(emb);
    if (wake) anonfd_wake(&wake->a);
    return r;
}

/* Datagram disconnect (connect with AF_UNSPEC). */
int unix_disconnect(usock_t* u) {
    if (u->type != 2) return -U_EINVAL;
    uint64_t fl = spin_lock_irqsave(&g_unix_lock);
    u->peer = NULL; u->peer_closed = 0;
    spin_unlock_irqrestore(&g_unix_lock, fl);
    return 0;
}

/* accept(): the oldest queued connection as an anonfd the caller installs
 * (refs = 1). -EAGAIN when none. */
int unix_accept(usock_t* u, usock_t** out) {
    uint64_t fl = spin_lock_irqsave(&g_unix_lock);
    int r = 0;
    if (u->state != US_LISTEN) r = -U_EINVAL;
    else if (!u->aq_n) r = -U_EAGAIN;
    else {
        *out = u->aq[0];
        for (int i = 1; i < u->aq_n; i++) u->aq[i - 1] = u->aq[i];
        u->aq_n--;
    }
    spin_unlock_irqrestore(&g_unix_lock, fl);
    if (!r) anonfd_wake(&u->a);                 /* a connect() waiting for backlog room */
    return r;
}
anonfd_t* unix_anon(usock_t* u) { return &u->a; }

/* Give back an accepted socket that could not be installed (no fd). */
void unix_drop(usock_t* u) { anonfd_unref(&u->a.ino); }

int unix_socketpair(process_t* c, int type, usock_t** a, usock_t** b) {
    usock_t* x = us_new(c, type);
    usock_t* y = x ? us_new(c, type) : NULL;
    if (!x || !y) { if (x) anonfd_unref(&x->a.ino); return -U_ENOMEM; }
    uint64_t fl = spin_lock_irqsave(&g_unix_lock);
    x->peer = y; y->peer = x;
    x->state = y->state = US_CONNECTED;
    x->pcred = y->cred; y->pcred = x->cred; x->has_pcred = y->has_pcred = 1;
    spin_unlock_irqrestore(&g_unix_lock, fl);
    *a = x; *b = y;
    return 0;
}

int unix_create(process_t* c, int type, usock_t** out) {
    if (type != 1 && type != 2) return -94;     /* ESOCKTNOSUPPORT (SOCK_SEQPACKET: not yet) */
    usock_t* u = us_new(c, type);
    if (!u) return -U_ENOMEM;
    *out = u;
    return 0;
}

/* ------------------------------------------------------------- data path */
/* Send `len` bytes from the kernel buffer. Stream: as much as fits (-EAGAIN
 * if nothing does), dgram: all or nothing. dst = explicit destination (dgram
 * sendto), else the peer. *wait_on = the object whose wake means "retry". */
int unix_send(process_t* c, usock_t* u, const void* kbuf, int len, int flags,
              const struct lx_sockaddr_un* dst, uint32_t dlen, void** wait_on) {
    (void)flags;
    *wait_on = &u->a;
    if (u->type == 1) {
        if (dst && u->state == US_CONNECTED) return -U_EISCONN;
        uint64_t fl = spin_lock_irqsave(&g_unix_lock);
        usock_t* p = u->peer;
        int r;
        if (u->shut & 2) r = -U_EPIPE;
        else if (u->state != US_CONNECTED) r = -U_ENOTCONN;
        else if (!p) r = -U_EPIPE;                 /* the peer closed */
        else if (p->shut & 1) r = -U_EPIPE;
        else if (len == 0) r = 0;
        else {
            uint32_t room = U_BUF - p->rlen;
            uint32_t n = (uint32_t)len < room ? (uint32_t)len : room;
            if (n == 0) r = -U_EAGAIN;
            else {
                uint32_t tail = (p->rhead + p->rlen) % U_BUF;
                const uint8_t* s = (const uint8_t*)kbuf;
                for (uint32_t i = 0; i < n; i++) p->rbuf[(tail + i) % U_BUF] = s[i];
                p->rlen += n;
                r = (int)n;
            }
        }
        spin_unlock_irqrestore(&g_unix_lock, fl);
        if (r > 0) anonfd_wake(&p->a);
        return r;
    }
    /* SOCK_DGRAM */
    if (len > U_DGRAM_MAX) return -U_EMSGSIZE;
    int abs = 0; char key[110]; uint32_t klen = 0;
    const void* ino = NULL; uint64_t inonum = 0;
    char canon[256]; canon[0] = 0;
    if (dst) {
        int e = name_parse(dst, dlen, &abs, key, &klen);
        if (e) return e;
        if (abs == 2) return -U_EINVAL;
        if (abs == 0 && (e = path_target(c, key, &ino, &inonum, canon))) return e;
    }
    struct udg* d = (struct udg*)kmalloc(sizeof(struct udg) + (size_t)len);
    if (!d) return -U_ENOMEM;
    d->next = NULL; d->len = (uint32_t)len;
    bc(d->data, kbuf, (size_t)len);
    uint64_t fl = spin_lock_irqsave(&g_unix_lock);
    usock_t* t = dst ? name_find(abs, key, klen, ino, inonum, canon) : u->peer;
    int r;
    if (u->shut & 2) r = -U_EPIPE;
    else if (!t) r = dst ? -U_ECONNREFUSED : (u->peer_closed ? -U_ECONNREFUSED : -U_ENOTCONN);
    else if (t->type != 2) r = -U_EPROTOTYPE;
    else if (t->peer && t->peer != u) r = -U_EPERM;     /* t only talks to its own peer */
    else if (t->shut & 1) r = -U_EPIPE;
    else if (t->dq_bytes + (uint32_t)len > U_DGRAM_Q) { r = -U_EAGAIN; *wait_on = &t->a; }
    else {
        d->from = u->name; d->fromlen = u->named ? u->namelen : 2;
        d->from.sun_family = 1;
        if (t->dq_tail) t->dq_tail->next = d; else t->dq_head = d;
        t->dq_tail = d; t->dq_bytes += (uint32_t)len;
        d = NULL;
        r = len;
    }
    spin_unlock_irqrestore(&g_unix_lock, fl);
    if (d) kfree(d);
    if (r >= 0) anonfd_wake(&t->a);
    return r;
}

/* Receive into the kernel buffer. Stream: up to len bytes, 0 = EOF. Dgram: one
 * datagram (*full = its length), the sender's name in from/fromlen. */
int unix_recv(usock_t* u, void* kbuf, int len, int flags, struct lx_sockaddr_un* from,
              uint32_t* fromlen, uint32_t* full) {
    int peek = (flags & 0x2) != 0, waitall = (flags & 0x100) != 0;
    usock_t* wake = NULL;
    struct udg* gone = NULL;
    int r;
    if (full) *full = 0;
    uint64_t fl = spin_lock_irqsave(&g_unix_lock);
    if (u->type == 1) {
        if (u->state == US_LISTEN) r = -U_EINVAL;
        else if (u->state != US_CONNECTED) r = -U_ENOTCONN;
        else {
            int eof = !u->peer || (u->peer->shut & 2) || (u->shut & 1);
            if (u->rlen == 0) r = eof ? 0 : -U_EAGAIN;
            else if (waitall && !eof && u->rlen < (uint32_t)len && (uint32_t)len <= U_BUF) r = -U_EAGAIN;
            else {
                uint32_t n = (uint32_t)len < u->rlen ? (uint32_t)len : u->rlen;
                uint8_t* o = (uint8_t*)kbuf;
                for (uint32_t i = 0; i < n; i++) o[i] = u->rbuf[(u->rhead + i) % U_BUF];
                if (!peek) { u->rhead = (u->rhead + n) % U_BUF; u->rlen -= n; wake = u->peer; }
                r = (int)n;
            }
        }
        if (from) {                          /* the peer's name (unnamed: the family only) */
            bz(from, sizeof(*from)); from->sun_family = 1; *fromlen = 2;
            if (r >= 0 && u->peer && u->peer->namelen) { *from = u->peer->name; *fromlen = u->peer->namelen; }
        }
        if (full && r > 0) *full = (uint32_t)r;
    } else {
        struct udg* d = u->dq_head;
        if (!d) r = (u->shut & 1) ? 0 : -U_EAGAIN;
        else {
            uint32_t n = (uint32_t)len < d->len ? (uint32_t)len : d->len;
            bc(kbuf, d->data, n);
            if (from) { *from = d->from; *fromlen = d->fromlen; }
            if (full) *full = d->len;
            r = (int)n;
            if (!peek) {
                u->dq_head = d->next;
                if (!u->dq_head) u->dq_tail = NULL;
                u->dq_bytes -= d->len;
                gone = d;
                wake = u;                    /* senders blocked on our full queue */
            }
        }
    }
    spin_unlock_irqrestore(&g_unix_lock, fl);
    if (gone) kfree(gone);
    if (wake) anonfd_wake(&wake->a);
    return r;
}

int unix_shutdown(usock_t* u, int how) {
    if (how < 0 || how > 2) return -U_EINVAL;
    uint8_t bits = how == 0 ? 1 : how == 1 ? 2 : 3;
    uint64_t fl = spin_lock_irqsave(&g_unix_lock);
    int r = 0;
    if (u->type == 1 && u->state != US_CONNECTED) r = -U_ENOTCONN;
    else u->shut |= bits;
    usock_t* p = u->peer;
    spin_unlock_irqrestore(&g_unix_lock, fl);
    anonfd_wake(&u->a);
    if (p) anonfd_wake(&p->a);
    return r;
}

int unix_getname(usock_t* u, int peer, struct lx_sockaddr_un* out, uint32_t* len) {
    uint64_t fl = spin_lock_irqsave(&g_unix_lock);
    int r = 0;
    bz(out, sizeof(*out)); out->sun_family = 1; *len = 2;
    if (peer) {
        usock_t* p = u->peer;
        if (!p) r = -U_ENOTCONN;
        else if (p->namelen) { *out = p->name; *len = p->namelen; }
    } else if (u->namelen) {
        *out = u->name; *len = u->namelen;
    }
    spin_unlock_irqrestore(&g_unix_lock, fl);
    return r;
}

int unix_type(usock_t* u) { return u->type; }

int unix_poll(usock_t* u) {
    uint64_t fl = spin_lock_irqsave(&g_unix_lock);
    int ev = 0;
    if (u->type == 1) {
        if (u->state == US_LISTEN) ev = u->aq_n ? 0x001 : 0;
        else if (u->state != US_CONNECTED) ev = 0x004 | 0x010;          /* OUT|HUP, like Linux */
        else {
            usock_t* p = u->peer;
            if (u->rlen) ev |= 0x001;
            if (!p || (p->shut & 2) || (u->shut & 1)) ev |= 0x001 | 0x2000;   /* EOF readable, RDHUP */
            if (!p) ev |= 0x010;
            if (p && !(u->shut & 2) && p->rlen < U_BUF) ev |= 0x004;
            if (!p && !(u->shut & 2)) ev |= 0x004;                     /* a write reports EPIPE */
        }
    } else {
        if (u->dq_head || (u->shut & 1)) ev |= 0x001;
        usock_t* p = u->peer;
        if (!p || p->dq_bytes < U_DGRAM_Q) ev |= 0x004;
        if (u->peer_closed) ev |= 0x008;
    }
    spin_unlock_irqrestore(&g_unix_lock, fl);
    return ev;
}

int unix_nread(usock_t* u) {
    uint64_t fl = spin_lock_irqsave(&g_unix_lock);
    int n = u->type == 1 ? (int)u->rlen : (u->dq_head ? (int)u->dq_head->len : 0);
    spin_unlock_irqrestore(&g_unix_lock, fl);
    return n;
}

/* --------------------------------------------------------------- options */
int unix_setopt(usock_t* u, int level, int name, const void* val, uint32_t len) {
    if (level != 1 /* SOL_SOCKET */) return -U_ENOPROTOOPT;
    if (name == 20 || name == 21) {                       /* SO_RCVTIMEO / SO_SNDTIMEO */
        if (len < 16) return -U_EINVAL;
        const int64_t* t = (const int64_t*)val;
        if (t[1] < 0 || t[1] >= 1000000) return -33;
        uint64_t ns = t[0] < 0 ? 0 : (uint64_t)t[0] * 1000000000ull + (uint64_t)t[1] * 1000ull;
        if (name == 20) u->rcvtimeo_ns = ns; else u->sndtimeo_ns = ns;
        return 0;
    }
    if (len < 4) return -U_EINVAL;
    int v = *(const int*)val;
    switch (name) {
    case 2: case 15: case 7: case 8: case 32: case 33: case 9: return 0;   /* REUSEADDR/PORT, BUF sizes, KEEPALIVE */
    case 16: u->passcred = v ? 1 : 0; return 0;                             /* SO_PASSCRED (no SCM yet) */
    }
    return -U_ENOPROTOOPT;
}

int unix_getopt(usock_t* u, int level, int name, void* val, uint32_t* len) {
    if (level != 1) return -U_ENOPROTOOPT;
    int v;
    switch (name) {
    case 17: {                                                             /* SO_PEERCRED */
        if (*len < sizeof(struct lx_ucred)) return -U_EINVAL;
        uint64_t fl = spin_lock_irqsave(&g_unix_lock);
        int ok = u->has_pcred;
        struct lx_ucred pc = u->pcred;
        spin_unlock_irqrestore(&g_unix_lock, fl);
        if (!ok) { pc.pid = 0; pc.uid = 0xFFFFFFFFu; pc.gid = 0xFFFFFFFFu; }
        bc(val, &pc, sizeof(pc)); *len = sizeof(pc);
        return 0;
    }
    case 20: case 21: {
        if (*len < 16) return -U_EINVAL;
        uint64_t ns = name == 20 ? u->rcvtimeo_ns : u->sndtimeo_ns;
        int64_t* t = (int64_t*)val; t[0] = (int64_t)(ns / 1000000000ull); t[1] = (int64_t)((ns % 1000000000ull) / 1000ull);
        *len = 16; return 0;
    }
    case 3: v = u->type; break;                                            /* SO_TYPE */
    case 39: v = 1; break;                                                 /* SO_DOMAIN */
    case 38: v = 0; break;                                                 /* SO_PROTOCOL */
    case 4: v = 0; break;                                                  /* SO_ERROR */
    case 30: v = u->state == US_LISTEN; break;                             /* SO_ACCEPTCONN */
    case 7: case 8: v = u->type == 1 ? U_BUF : U_DGRAM_Q; break;
    case 16: v = u->passcred; break;
    case 2: case 15: case 9: v = 0; break;
    default: return -U_ENOPROTOOPT;
    }
    if (*len < 4) return -U_EINVAL;
    *(int*)val = v; *len = 4;
    return 0;
}

uint64_t unix_timeo(usock_t* u, int send) { return send ? u->sndtimeo_ns : u->rcvtimeo_ns; }

/* ------------------------------------------------- read()/write() on the fd */
#define U_CHUNK 16384
static int unix_op_read(process_t* c, anonfd_t* f, void* ubuf, int len, int nb) {
    (void)nb;
    usock_t* u = (usock_t*)f;
    if (len <= 0) return len == 0 ? 0 : -U_EINVAL;
    int want = len < U_CHUNK ? len : U_CHUNK;
    uint8_t* k = (uint8_t*)kmalloc((size_t)want);
    if (!k) return -U_ENOMEM;
    int r = unix_recv(u, k, want, 0, NULL, NULL, NULL);
    if (r > 0 && copy_to_user(ubuf, k, (size_t)r) != 0) r = -U_EFAULT;
    kfree(k);
    (void)c;
    return r;
}
static int unix_op_write(process_t* c, anonfd_t* f, const void* ubuf, int len, int nb) {
    (void)nb;
    usock_t* u = (usock_t*)f;
    if (len <= 0) return len == 0 ? 0 : -U_EINVAL;
    int want = len < U_CHUNK ? len : U_CHUNK;
    if (u->type == 2) { if (len > U_DGRAM_MAX) return -U_EMSGSIZE; want = len; }
    uint8_t* k = (uint8_t*)kmalloc((size_t)want);
    if (!k) return -U_ENOMEM;
    int r;
    void* w;
    if (copy_from_user(k, ubuf, (size_t)want) != 0) r = -U_EFAULT;
    else r = unix_send(c, u, k, want, 0, NULL, 0, &w);
    kfree(k);
    if (r == -U_EPIPE && c && u->type == 1) signal_post_thread(c, SIGPIPE);   /* write(): no MSG_NOSIGNAL; [U9] to the writing thread */
    return r;
}
static int unix_op_poll(anonfd_t* f) { return unix_poll((usock_t*)f); }
#define FIONREAD_ 0x541B
static long unix_op_ioctl(process_t* c, anonfd_t* f, uint64_t cmd, uint64_t arg) {
    (void)c;
    if (cmd == FIONREAD_) {
        int n = unix_nread((usock_t*)f);
        if (copy_to_user((void*)arg, &n, sizeof(n)) != 0) return -U_EFAULT;
        return 0;
    }
    return -25;                                            /* ENOTTY */
}
