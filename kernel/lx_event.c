/*
 * lx_event.c — [U10] eventfd, timerfd, epoll and ITIMER_REAL, Linux-shaped
 * (syscall numbers 512 + the Linux x86-64 number, Linux arguments, -errno).
 * SPDX-License-Identifier: MIT
 *
 *   eventfd / eventfd2      a 64-bit counter fd (EFD_SEMAPHORE/NONBLOCK/CLOEXEC)
 *   timerfd_create/settime/gettime
 *                           CLOCK_REALTIME/MONOTONIC/BOOTTIME timers read as an
 *                           expiration count (TFD_TIMER_ABSTIME)
 *   epoll_create/create1/ctl/wait/pwait/pwait2
 *                           an interest list over the caller's fds, evaluated
 *                           with the same readiness function as poll()
 *   setitimer / getitimer   ITIMER_REAL only (SIGALRM from the BSP tick);
 *                           musl's alarm() is built on setitimer
 *
 * All three fd kinds are anonfds (kernel/anonfd.h). One spinlock, g_ev_lock
 * (S14), guards every counter, timer and interest list; user copies, wakes and
 * the readiness checks of watched fds all run with it dropped.
 *
 * epoll notes (differences from Linux, by design of this first version):
 *  - a registration is (fd number, fd generation) in the CALLER's fd table,
 *    not (open file, fd): an epoll fd shared across fork is evaluated against
 *    the fd table of whoever waits on it. A registered fd that was closed is
 *    dropped (its generation changed), like Linux's removal on last close;
 *  - EPOLLET reports a readiness bit when it appears, and again if it is
 *    still set EP_ET_REPEAT_MS later: an edge that arrives between two scans
 *    is seen at most that late, and an fd left ready is not reported in a
 *    busy loop;
 *  - blocking re-checks every 2 ms when sockets, evdev or timerfds are
 *    watched (the network RX path has no wake hook yet), else 10 ms, and any
 *    pipe/anonfd/pty/console activity wakes the waiter at once.
 */
#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "process.h"
#include "trapframe.h"
#include "user_copy.h"
#include "anonfd.h"
#include "kclock.h"
#include "signal.h"
#include "heap.h"
#include "../arch/x86/spinlock.h"

extern process_t* sched_get_current(void);
extern uint64_t timer_get_ticks(void);
extern long lx_wait(trapframe_t* tf, void* wake_obj, uint32_t recheck_ticks,
                    uint64_t timeout_ns, long timeout_ret);
extern int  kiface_fd_revents(process_t* c, int fd, int events, int* watch);
extern void kiface_arm_watch(process_t* c, int watch);
extern void kiface_clear_watch(process_t* c);

#define LX(n) (SYS_LX_BASE + (n))
enum {
    LXN_GETITIMER = 36, LXN_SETITIMER = 38, LXN_EPOLL_CREATE = 213,
    LXN_EPOLL_WAIT = 232, LXN_EPOLL_CTL = 233, LXN_EPOLL_PWAIT = 281,
    LXN_TIMERFD_CREATE = 283, LXN_EVENTFD = 284, LXN_TIMERFD_SETTIME = 286,
    LXN_TIMERFD_GETTIME = 287, LXN_EVENTFD2 = 290, LXN_EPOLL_CREATE1 = 291,
    LXN_EPOLL_PWAIT2 = 441,
};

#define E_PERM    1
#define E_NOENT   2
#define E_BADF    9
#define E_AGAIN   11
#define E_NOMEM   12
#define E_FAULT   14
#define E_EXIST   17
#define E_INVAL   22
#define E_MFILE   24
#define E_NOSPC   28
#define E_LOOP    40
#define NEG(e) ((uint64_t)(int64_t)-(e))

#define O_NONBLOCK_ 0x800
#define O_CLOEXEC_  0x80000

#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

static spinlock_t g_ev_lock = SPINLOCK_INIT_CLASS(LK_S14_LXEVENT, "lx-event");

static void bzero_(void* p, size_t n) { uint8_t* b = (uint8_t*)p; while (n--) *b++ = 0; }

/* ======================================================================
 * eventfd
 * ====================================================================== */
#define EFD_SEMAPHORE 1
#define EV_MAX 0xFFFFFFFFFFFFFFFEull

typedef struct { anonfd_t a; uint64_t count; int sem; } evfd_t;

static int ev_read(process_t* c, anonfd_t* f, void* ubuf, int len, int nb) {
    (void)c; (void)nb;
    evfd_t* e = (evfd_t*)f;
    if (len < 8) return -E_INVAL;
    uint64_t fl = spin_lock_irqsave(&g_ev_lock);
    if (e->count == 0) { spin_unlock_irqrestore(&g_ev_lock, fl); return -E_AGAIN; }
    uint64_t v = e->sem ? 1 : e->count;
    e->count -= v;
    spin_unlock_irqrestore(&g_ev_lock, fl);
    anonfd_wake(f);                                  /* a blocked writer has room */
    if (copy_to_user(ubuf, &v, 8) != 0) return -E_FAULT;
    return 8;
}

static int ev_write(process_t* c, anonfd_t* f, const void* ubuf, int len, int nb) {
    (void)c; (void)nb;
    evfd_t* e = (evfd_t*)f;
    if (len < 8) return -E_INVAL;
    uint64_t v;
    if (copy_from_user(&v, ubuf, 8) != 0) return -E_FAULT;
    if (v == 0xFFFFFFFFFFFFFFFFull) return -E_INVAL;
    uint64_t fl = spin_lock_irqsave(&g_ev_lock);
    if (EV_MAX - e->count < v) { spin_unlock_irqrestore(&g_ev_lock, fl); return -E_AGAIN; }
    e->count += v;
    spin_unlock_irqrestore(&g_ev_lock, fl);
    if (v) anonfd_wake(f);
    return 8;
}

static int ev_poll(anonfd_t* f) {
    evfd_t* e = (evfd_t*)f;
    uint64_t fl = spin_lock_irqsave(&g_ev_lock);
    int r = 0;
    if (e->count > 0) r |= POLLIN;
    if (e->count < EV_MAX) r |= POLLOUT;
    spin_unlock_irqrestore(&g_ev_lock, fl);
    return r;
}

static void ev_release(anonfd_t* f) { kfree(f); }

static const anonfd_ops_t g_evfd_ops = {
    .name = "anon_inode:[eventfd]", .mode = 0100600,
    .read = ev_read, .write = ev_write, .poll = ev_poll, .release = ev_release,
};

static uint64_t sys_eventfd2(process_t* c, uint64_t initval, uint64_t flags) {
    if (flags & ~(uint64_t)(EFD_SEMAPHORE | O_NONBLOCK_ | O_CLOEXEC_)) return NEG(E_INVAL);
    evfd_t* e = (evfd_t*)kmalloc(sizeof(*e));
    if (!e) return NEG(E_NOMEM);
    bzero_(e, sizeof(*e));
    anonfd_init(&e->a, &g_evfd_ops);
    e->count = (uint32_t)initval;
    e->sem = (flags & EFD_SEMAPHORE) ? 1 : 0;
    int fd = anonfd_install(c, &e->a, (uint32_t)flags, (flags & O_CLOEXEC_) != 0);
    if (fd < 0) { kfree(e); return NEG(E_MFILE); }
    return (uint64_t)fd;
}

/* ======================================================================
 * timerfd
 * ====================================================================== */
#define CLK_REALTIME  0
#define CLK_MONOTONIC 1
#define CLK_BOOTTIME  7
#define CLK_REALTIME_ALARM 8
#define CLK_BOOTTIME_ALARM 9
#define TFD_TIMER_ABSTIME 1
#define TFD_TIMER_CANCEL_ON_SET 2

struct k_ts { int64_t sec, nsec; };
struct k_its { struct k_ts interval, value; };

typedef struct {
    anonfd_t a;
    int      clock;
    uint64_t next;          /* monotonic ns of the next expiry, 0 = disarmed */
    uint64_t interval;      /* ns, 0 = one-shot */
    uint64_t ticks;         /* expirations not read yet */
} tfd_t;

static const anonfd_ops_t g_tfd_ops;

/* Fold elapsed expirations into ticks (g_ev_lock held). */
static void tfd_advance(tfd_t* t, uint64_t now) {
    if (!t->next || now < t->next) return;
    uint64_t n = 1;
    if (t->interval) {
        n += (now - t->next) / t->interval;
        t->next += n * t->interval;
    } else {
        t->next = 0;
    }
    t->ticks += n;
}

static int tfd_read(process_t* c, anonfd_t* f, void* ubuf, int len, int nb) {
    tfd_t* t = (tfd_t*)f;
    if (len < 8) return -E_INVAL;
    uint64_t now = kclock_mono_ns();
    uint64_t fl = spin_lock_irqsave(&g_ev_lock);
    tfd_advance(t, now);
    if (t->ticks == 0) {
        uint64_t next = t->next;
        spin_unlock_irqrestore(&g_ev_lock, fl);
        if (!nb && next) {
            /* Wake at the expiry (settime also wakes us through the anonfd). */
            uint64_t ms = (next - now + 999999ull) / 1000000ull;
            c->sleep_until = timer_get_ticks() + (ms ? ms : 1);
        }
        return -E_AGAIN;
    }
    uint64_t v = t->ticks;
    t->ticks = 0;
    spin_unlock_irqrestore(&g_ev_lock, fl);
    if (copy_to_user(ubuf, &v, 8) != 0) return -E_FAULT;
    return 8;
}

static int tfd_poll(anonfd_t* f) {
    tfd_t* t = (tfd_t*)f;
    uint64_t now = kclock_mono_ns();
    uint64_t fl = spin_lock_irqsave(&g_ev_lock);
    tfd_advance(t, now);
    int r = t->ticks ? POLLIN : 0;
    spin_unlock_irqrestore(&g_ev_lock, fl);
    return r;
}

static void tfd_release(anonfd_t* f) { kfree(f); }

static const anonfd_ops_t g_tfd_ops = {
    .name = "anon_inode:[timerfd]", .mode = 0100600,
    .read = tfd_read, .poll = tfd_poll, .release = tfd_release,
};

int lx_is_timerfd(const void* ino) { return anonfd_of_kind(ino, &g_tfd_ops) != 0; }

static int clock_ok(int clk) {
    return clk == CLK_REALTIME || clk == CLK_MONOTONIC || clk == CLK_BOOTTIME ||
           clk == CLK_REALTIME_ALARM || clk == CLK_BOOTTIME_ALARM;
}

static uint64_t sys_timerfd_create(process_t* c, int clk, uint64_t flags) {
    if (!clock_ok(clk)) return NEG(E_INVAL);
    if (flags & ~(uint64_t)(O_NONBLOCK_ | O_CLOEXEC_)) return NEG(E_INVAL);
    tfd_t* t = (tfd_t*)kmalloc(sizeof(*t));
    if (!t) return NEG(E_NOMEM);
    bzero_(t, sizeof(*t));
    anonfd_init(&t->a, &g_tfd_ops);
    t->clock = clk;
    int fd = anonfd_install(c, &t->a, (uint32_t)flags, (flags & O_CLOEXEC_) != 0);
    if (fd < 0) { kfree(t); return NEG(E_MFILE); }
    return (uint64_t)fd;
}

static tfd_t* fd_timerfd(process_t* c, int fd) {
    if (fd < 0 || fd >= 32 || !c->fds[fd].used || c->fds[fd].is_pipe || c->fds[fd].is_sock) return NULL;
    return (tfd_t*)anonfd_of_kind(c->fds[fd].inode, &g_tfd_ops);
}

static int ts_valid(const struct k_ts* t) { return t->sec >= 0 && t->nsec >= 0 && t->nsec < 1000000000; }
static uint64_t ts_ns(const struct k_ts* t) { return (uint64_t)t->sec * 1000000000ull + (uint64_t)t->nsec; }
static void ns_ts(uint64_t ns, struct k_ts* t) { t->sec = (int64_t)(ns / 1000000000ull); t->nsec = (int64_t)(ns % 1000000000ull); }

/* The current setting of t as an itimerspec (relative remaining time). */
static void tfd_get(tfd_t* t, uint64_t now, struct k_its* out) {
    ns_ts(t->interval, &out->interval);
    ns_ts(t->next && t->next > now ? t->next - now : (t->next ? 1 : 0), &out->value);
}

static uint64_t sys_timerfd_settime(process_t* c, int fd, uint64_t flags, uint64_t unew, uint64_t uold) {
    tfd_t* t = fd_timerfd(c, fd);
    if (!t) return NEG(E_BADF);
    if (flags & ~(uint64_t)(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET)) return NEG(E_INVAL);
    struct k_its nv, ov;
    if (copy_from_user(&nv, (const void*)unew, sizeof(nv)) != 0) return NEG(E_FAULT);
    if (!ts_valid(&nv.interval) || !ts_valid(&nv.value)) return NEG(E_INVAL);
    uint64_t now = kclock_mono_ns();
    uint64_t val = ts_ns(&nv.value), next = 0;
    if (val) {
        if (flags & TFD_TIMER_ABSTIME) {
            if (t->clock == CLK_REALTIME || t->clock == CLK_REALTIME_ALARM) {
                uint64_t real = kclock_real_ns();
                next = val > real ? now + (val - real) : now;
            } else {
                next = val > now ? val : now;
            }
        } else {
            next = now + val;
        }
        if (!next) next = 1;
    }
    uint64_t fl = spin_lock_irqsave(&g_ev_lock);
    tfd_advance(t, now);
    tfd_get(t, now, &ov);
    t->next = next;
    t->interval = ts_ns(&nv.interval);
    t->ticks = 0;
    spin_unlock_irqrestore(&g_ev_lock, fl);
    anonfd_wake(&t->a);                           /* a blocked reader re-reads its deadline */
    if (uold && copy_to_user((void*)uold, &ov, sizeof(ov)) != 0) return NEG(E_FAULT);
    return 0;
}

static uint64_t sys_timerfd_gettime(process_t* c, int fd, uint64_t ucur) {
    tfd_t* t = fd_timerfd(c, fd);
    if (!t) return NEG(E_BADF);
    struct k_its cur;
    uint64_t now = kclock_mono_ns();
    uint64_t fl = spin_lock_irqsave(&g_ev_lock);
    tfd_advance(t, now);
    tfd_get(t, now, &cur);
    spin_unlock_irqrestore(&g_ev_lock, fl);
    if (copy_to_user((void*)ucur, &cur, sizeof(cur)) != 0) return NEG(E_FAULT);
    return 0;
}

/* ======================================================================
 * epoll
 * ====================================================================== */
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLLIN      0x001
#define EPOLLPRI     0x002
#define EPOLLOUT     0x004
#define EPOLLERR     0x008
#define EPOLLHUP     0x010
#define EPOLLRDNORM  0x040
#define EPOLLRDBAND  0x080
#define EPOLLWRNORM  0x100
#define EPOLLWRBAND  0x200
#define EPOLLMSG     0x400
#define EPOLLRDHUP   0x2000
#define EPOLLEXCLUSIVE (1u << 28)
#define EPOLLWAKEUP  (1u << 29)
#define EPOLLONESHOT (1u << 30)
#define EPOLLET      (1u << 31)
#define EP_EVMASK    (EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDNORM | \
                      EPOLLRDBAND | EPOLLWRNORM | EPOLLWRBAND | EPOLLMSG | EPOLLRDHUP)
#define EP_MAX_ENTRIES 4096
#define EP_MAX_EVENTS  1024            /* per epoll_wait call */
#define EP_ET_REPEAT_MS 20

struct lx_epoll_event { uint32_t events; uint64_t data; } __attribute__((packed));

typedef struct {
    int      fd;
    uint32_t gen;            /* proc_fd_entry.gen at registration */
    uint32_t events;         /* EPOLL* requested (incl. ET/ONESHOT) */
    uint64_t data;
    uint32_t last;           /* EPOLLET: bits reported/seen at the last scan */
    uint64_t last_ms;        /* EPOLLET: tick of the last report */
    uint8_t  off;            /* EPOLLONESHOT: reported, disabled until MOD */
} ep_ent_t;

typedef struct {
    anonfd_t  a;
    ep_ent_t* e;
    int       n, cap;
} epoll_t;

static const anonfd_ops_t g_ep_ops;

static int ep_find(epoll_t* ep, int fd) {
    for (int i = 0; i < ep->n; i++) if (ep->e[i].fd == fd) return i;
    return -1;
}
static void ep_del_at(epoll_t* ep, int i) {
    ep->e[i] = ep->e[ep->n - 1];
    ep->n--;
}

/* Readiness without blocking: an epoll fd is readable when one of its own
 * entries is ready for the caller (evaluated on the current process). */
static int ep_scan(process_t* c, epoll_t* ep, struct lx_epoll_event* out, int max,
                   int* watch, int consume);

static int ep_poll(anonfd_t* f) {
    process_t* c = sched_get_current();
    int watch = 0;
    struct lx_epoll_event one;
    return (c && ep_scan(c, (epoll_t*)f, &one, 1, &watch, 0) > 0) ? POLLIN : 0;
}

static void ep_release(anonfd_t* f) {
    epoll_t* ep = (epoll_t*)f;
    if (ep->e) kfree(ep->e);
    kfree(ep);
}

static const anonfd_ops_t g_ep_ops = {
    .name = "anon_inode:[eventpoll]", .mode = 0100600,
    .poll = ep_poll, .release = ep_release,
};

static epoll_t* fd_epoll(process_t* c, int fd) {
    if (fd < 0 || fd >= 32 || !c->fds[fd].used || c->fds[fd].is_pipe || c->fds[fd].is_sock) return NULL;
    return (epoll_t*)anonfd_of_kind(c->fds[fd].inode, &g_ep_ops);
}

static uint64_t sys_epoll_create1(process_t* c, uint64_t flags) {
    if (flags & ~(uint64_t)O_CLOEXEC_) return NEG(E_INVAL);
    epoll_t* ep = (epoll_t*)kmalloc(sizeof(*ep));
    if (!ep) return NEG(E_NOMEM);
    bzero_(ep, sizeof(*ep));
    anonfd_init(&ep->a, &g_ep_ops);
    int fd = anonfd_install(c, &ep->a, 0, (flags & O_CLOEXEC_) != 0);
    if (fd < 0) { kfree(ep); return NEG(E_MFILE); }
    return (uint64_t)fd;
}

/* Can fd be watched? Linux refuses regular files and directories (EPERM). */
static int ep_target_ok(process_t* c, int fd) {
    struct proc_fd_entry* e = &c->fds[fd];
    if (!e->used) return fd <= 2 ? 0 : -E_BADF;      /* unredirected console stdio */
    if (e->is_pipe || e->is_sock || !e->inode) return 0;
    if (anonfd_is(e->inode)) return 0;
    extern int pty_is_inode(const void*);
    extern int input_is_client(const void*);
    extern int udrv_is_fd(const void*);
    extern int devfs_is_console(void*);
    if (pty_is_inode(e->inode) || input_is_client(e->inode) || udrv_is_fd(e->inode) ||
        devfs_is_console(e->inode)) return 0;
    return -E_PERM;
}

static uint64_t sys_epoll_ctl(process_t* c, int epfd, int op, int fd, uint64_t uev) {
    epoll_t* ep = fd_epoll(c, epfd);
    if (!ep) {
        if (epfd < 0 || epfd >= 32 || !c->fds[epfd].used) return NEG(E_BADF);
        return NEG(E_INVAL);
    }
    if (fd < 0 || fd >= 32) return NEG(E_BADF);
    if (fd == epfd) return NEG(E_INVAL);
    int e = ep_target_ok(c, fd);
    if (e) return NEG(-e);
    /* Nested epoll: allowed one level (no cycle check needed beyond self). */
    if (c->fds[fd].used && fd_epoll(c, fd) && fd_epoll(c, fd) == ep) return NEG(E_LOOP);
    struct lx_epoll_event ev = { 0, 0 };
    if (op != EPOLL_CTL_DEL) {
        if (copy_from_user(&ev, (const void*)uev, sizeof(ev)) != 0) return NEG(E_FAULT);
        if ((ev.events & EPOLLEXCLUSIVE) && op == EPOLL_CTL_MOD) return NEG(E_INVAL);
    }
    uint32_t gen = c->fds[fd].used ? c->fds[fd].gen : 0;
    /* Grow outside the lock if needed (the owner is the only writer of its
     * fd table, but other tasks sharing the epoll fd may add entries). */
    ep_ent_t* grown = NULL; int gcap = 0;
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&g_ev_lock);
        int i = ep_find(ep, fd);
        /* A registration whose fd was closed (generation changed) is gone. */
        if (i >= 0 && ep->e[i].gen != gen) { ep_del_at(ep, i); i = -1; }
        uint64_t r = 0;
        if (op == EPOLL_CTL_ADD) {
            if (i >= 0) r = NEG(E_EXIST);
            else if (ep->n >= EP_MAX_ENTRIES) r = NEG(E_NOSPC);
            else if (ep->n == ep->cap) {
                if (grown && gcap > ep->cap) {
                    for (int k = 0; k < ep->n; k++) grown[k] = ep->e[k];
                    ep_ent_t* old = ep->e;
                    ep->e = grown; ep->cap = gcap; grown = old;   /* free the old one below */
                } else {
                    int want = ep->cap ? ep->cap * 2 : 16;
                    spin_unlock_irqrestore(&g_ev_lock, fl);
                    if (grown) kfree(grown);
                    grown = (ep_ent_t*)kmalloc((size_t)want * sizeof(ep_ent_t));
                    gcap = want;
                    if (!grown) return NEG(E_NOMEM);
                    continue;
                }
            }
            if (!r) {
                ep_ent_t* n = &ep->e[ep->n++];
                bzero_(n, sizeof(*n));
                n->fd = fd; n->gen = gen; n->events = ev.events; n->data = ev.data;
            }
        } else if (op == EPOLL_CTL_MOD) {
            if (i < 0) r = NEG(E_NOENT);
            else {
                ep->e[i].events = ev.events; ep->e[i].data = ev.data;
                ep->e[i].off = 0; ep->e[i].last = 0;
            }
        } else if (op == EPOLL_CTL_DEL) {
            if (i < 0) r = NEG(E_NOENT);
            else ep_del_at(ep, i);
        } else {
            r = NEG(E_INVAL);
        }
        spin_unlock_irqrestore(&g_ev_lock, fl);
        if (grown && grown != ep->e) kfree(grown);
        return r;
    }
}

/* Evaluate ep's entries for c: up to `max` ready events into out[]. With
 * `consume`, EPOLLONESHOT entries are disabled and EPOLLET state advances. */
static int ep_scan(process_t* c, epoll_t* ep, struct lx_epoll_event* out, int max,
                   int* watch, int consume) {
    /* Snapshot the list (the readiness functions take other locks). */
    uint64_t fl = spin_lock_irqsave(&g_ev_lock);
    int n = ep->n;
    ep_ent_t* snap = NULL;
    if (n) {
        spin_unlock_irqrestore(&g_ev_lock, fl);
        snap = (ep_ent_t*)kmalloc((size_t)n * sizeof(ep_ent_t));
        if (!snap) return 0;
        fl = spin_lock_irqsave(&g_ev_lock);
        if (ep->n < n) n = ep->n;
        for (int i = 0; i < n; i++) snap[i] = ep->e[i];
    }
    spin_unlock_irqrestore(&g_ev_lock, fl);

    uint64_t now_ms = timer_get_ticks();
    int got = 0;
    uint8_t* rep = n ? (uint8_t*)kmalloc((size_t)n) : NULL;     /* 1 = report, 2 = stale */
    if (n && !rep) { kfree(snap); return 0; }
    uint32_t* seen = n ? (uint32_t*)kmalloc((size_t)n * sizeof(uint32_t)) : NULL;
    if (n && !seen) { kfree(snap); kfree(rep); return 0; }
    for (int i = 0; i < n; i++) {
        ep_ent_t* e = &snap[i];
        rep[i] = 0; seen[i] = 0;
        struct proc_fd_entry* fe = (e->fd >= 0 && e->fd < 32) ? &c->fds[e->fd] : NULL;
        int alive = fe && (fe->used ? fe->gen == e->gen : (e->fd <= 2 && e->gen == 0));
        if (!alive) { rep[i] = 2; continue; }
        if (e->off) continue;
        if (fe->used && fe->is_sock) *watch |= 8;                     /* PW_SOCK: re-check often */
        if (fe->used && fe->inode && lx_is_timerfd(fe->inode)) *watch |= 8;
        int r = kiface_fd_revents(c, e->fd, (int)(e->events & EP_EVMASK), watch);
        if (r & POLLNVAL) { rep[i] = 2; continue; }
        uint32_t want = (e->events & EP_EVMASK) | EPOLLERR | EPOLLHUP;
        uint32_t ready = (uint32_t)r & want;
        if (ready & EPOLLIN) ready |= (e->events & EPOLLRDNORM);
        if (ready & EPOLLOUT) ready |= (e->events & EPOLLWRNORM);
        seen[i] = ready;
        if (!ready) continue;
        if ((e->events & EPOLLET) && !(ready & ~e->last) &&
            now_ms < e->last_ms + EP_ET_REPEAT_MS) continue;          /* no new edge */
        if (got < max) {
            out[got].events = ready;
            out[got].data = e->data;
            got++;
            rep[i] = 1;
        }
    }
    /* Apply: stale entries go, ONESHOT disables, ET remembers. */
    fl = spin_lock_irqsave(&g_ev_lock);
    for (int i = 0; i < n; i++) {
        int k = ep_find(ep, snap[i].fd);
        if (k < 0 || ep->e[k].gen != snap[i].gen) continue;
        if (rep[i] == 2) { ep_del_at(ep, k); continue; }
        if (!consume) continue;
        if (ep->e[k].events & EPOLLET) {
            ep->e[k].last = seen[i];
            if (rep[i] == 1) ep->e[k].last_ms = now_ms;
        }
        if (rep[i] == 1 && (ep->e[k].events & EPOLLONESHOT)) ep->e[k].off = 1;
    }
    spin_unlock_irqrestore(&g_ev_lock, fl);
    kfree(snap); kfree(rep); kfree(seen);
    return got;
}

static uint64_t sys_epoll_wait(process_t* c, trapframe_t* tf, int epfd, uint64_t uevs,
                               int maxev, int64_t timeout_ns) {
    epoll_t* ep = fd_epoll(c, epfd);
    if (!ep) {
        if (epfd < 0 || epfd >= 32 || !c->fds[epfd].used) return NEG(E_BADF);
        return NEG(E_INVAL);
    }
    if (maxev <= 0) return NEG(E_INVAL);
    if (maxev > EP_MAX_EVENTS) maxev = EP_MAX_EVENTS;
    if (!user_range_valid((void*)uevs, (size_t)maxev * sizeof(struct lx_epoll_event))) return NEG(E_FAULT);
    kiface_clear_watch(c);
    struct lx_epoll_event* out = (struct lx_epoll_event*)kmalloc((size_t)maxev * sizeof(*out));
    if (!out) return NEG(E_NOMEM);
    int watch = 0;
    int got = ep_scan(c, ep, out, maxev, &watch, 1);
    if (got > 0) {
        int bad = copy_to_user((void*)uevs, out, (size_t)got * sizeof(*out)) != 0;
        kfree(out);
        return bad ? NEG(E_FAULT) : (uint64_t)got;
    }
    kfree(out);
    if (timeout_ns == 0) return 0;
    kiface_arm_watch(c, watch);                    /* pipes/ptys/console/anonfds wake us */
    long r = lx_wait(tf, NULL, (watch & (4 | 8)) ? 2 : 10,
                     timeout_ns < 0 ? 0 : (uint64_t)timeout_ns, 0);
    kiface_clear_watch(c);
    return (uint64_t)r;                            /* 0 (timeout) or -EINTR */
}

/* ======================================================================
 * ITIMER_REAL
 * ====================================================================== */
#define ITIMER_REAL 0
struct k_tv { int64_t sec, usec; };
struct k_itv { struct k_tv interval, value; };
static volatile uint32_t g_itimers;            /* armed timers (tick fast path) */

static uint64_t tv_ns(const struct k_tv* t) { return (uint64_t)t->sec * 1000000000ull + (uint64_t)t->usec * 1000ull; }
static void ns_tv(uint64_t ns, struct k_tv* t) { t->sec = (int64_t)(ns / 1000000000ull); t->usec = (int64_t)((ns % 1000000000ull) / 1000ull); }

static void itimer_get(process_t* c, uint64_t now, struct k_itv* o) {
    uint64_t next = c->tg->itimer_next_ns;
    ns_tv(c->tg->itimer_int_ns, &o->interval);
    ns_tv(next ? (next > now ? next - now : 1000) : 0, &o->value);
}

static uint64_t sys_setitimer(process_t* c, int which, uint64_t unew, uint64_t uold) {
    if (which != ITIMER_REAL) return NEG(E_INVAL);   /* VIRTUAL/PROF: not yet */
    struct k_itv nv, ov;
    if (unew) {
        if (copy_from_user(&nv, (const void*)unew, sizeof(nv)) != 0) return NEG(E_FAULT);
        if (nv.value.sec < 0 || nv.value.usec < 0 || nv.value.usec >= 1000000 ||
            nv.interval.sec < 0 || nv.interval.usec < 0 || nv.interval.usec >= 1000000)
            return NEG(E_INVAL);
    } else {
        bzero_(&nv, sizeof(nv));                       /* Linux: NULL = disarm */
    }
    uint64_t now = kclock_mono_ns();
    itimer_get(c, now, &ov);
    uint64_t v = tv_ns(&nv.value);
    thread_group_t* tg = c->tg;                    /* [U9] the timer is per process */
    uint64_t was = __atomic_exchange_n(&tg->itimer_next_ns, 0, __ATOMIC_SEQ_CST);
    if (was) __atomic_sub_fetch(&g_itimers, 1, __ATOMIC_SEQ_CST);
    tg->itimer_int_ns = tv_ns(&nv.interval);
    if (v) {
        __atomic_add_fetch(&g_itimers, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&tg->itimer_next_ns, now + v, __ATOMIC_SEQ_CST);
    }
    if (uold && copy_to_user((void*)uold, &ov, sizeof(ov)) != 0) return NEG(E_FAULT);
    return 0;
}

static uint64_t sys_getitimer(process_t* c, int which, uint64_t ucur) {
    if (which != ITIMER_REAL) return NEG(E_INVAL);
    struct k_itv cur;
    itimer_get(c, kclock_mono_ns(), &cur);
    if (copy_to_user((void*)ucur, &cur, sizeof(cur)) != 0) return NEG(E_FAULT);
    return 0;
}

/* BSP tick: post SIGALRM to every process whose ITIMER_REAL expired. The
 * scan runs under proc_lock (process_foreach); the posts after it. [U9] The
 * timer lives in the thread group and every thread of it is visited: the
 * compare-exchange below lets exactly one of them claim each expiry (the
 * others then see the new value), and signal_post() is process-directed. */
#define ITIMER_BATCH 32
struct it_ctx { uint64_t now; int n; process_t* p[ITIMER_BATCH]; };
static void it_cb(process_t* p, void* u) {
    struct it_ctx* x = (struct it_ctx*)u;
    thread_group_t* tg = p->tg;
    if (!tg || tg->dead) return;
    uint64_t next = __atomic_load_n(&tg->itimer_next_ns, __ATOMIC_ACQUIRE);
    if (!next || x->now < next || p->state == PROC_ZOMBIE || x->n >= ITIMER_BATCH) return;
    uint64_t iv = tg->itimer_int_ns, nn = 0;
    if (iv) { nn = next + iv; if (nn <= x->now) nn = x->now + iv; }
    if (!__atomic_compare_exchange_n(&tg->itimer_next_ns, &next, nn, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return;                                      /* setitimer raced us: its value wins */
    if (!nn) __atomic_sub_fetch(&g_itimers, 1, __ATOMIC_SEQ_CST);
    x->p[x->n++] = process_get(p);
}
void lx_itimer_tick(void) {
    if (!__atomic_load_n(&g_itimers, __ATOMIC_ACQUIRE)) return;
    struct it_ctx x; x.now = kclock_mono_ns(); x.n = 0;
    process_foreach(it_cb, &x);
    for (int i = 0; i < x.n; i++) {
        if (x.p[i]) { signal_post(x.p[i], SIGALRM); process_put(x.p[i]); }
    }
}
/* A process that goes away with its timer armed. */
void lx_itimer_exit(process_t* p) {
    if (!p->tg) return;
    uint64_t was = __atomic_exchange_n(&p->tg->itimer_next_ns, 0, __ATOMIC_SEQ_CST);
    if (was) __atomic_sub_fetch(&g_itimers, 1, __ATOMIC_SEQ_CST);
}

/* ======================================================================
 * dispatcher
 * ====================================================================== */
int lx_event_syscall(trapframe_t* tf, uint64_t num, uint64_t* ret) {
    process_t* c = sched_get_current();
    uint64_t a0 = tf->rdi, a1 = tf->rsi, a2 = tf->rdx, a3 = tf->rcx, a4 = tf->r8;
    if (!c) return 0;
    switch (num) {
    case LX(LXN_EVENTFD):  *ret = sys_eventfd2(c, a0, 0); return 1;
    case LX(LXN_EVENTFD2): *ret = sys_eventfd2(c, a0, a1); return 1;
    case LX(LXN_TIMERFD_CREATE):  *ret = sys_timerfd_create(c, (int)a0, a1); return 1;
    case LX(LXN_TIMERFD_SETTIME): *ret = sys_timerfd_settime(c, (int)a0, a1, a2, a3); return 1;
    case LX(LXN_TIMERFD_GETTIME): *ret = sys_timerfd_gettime(c, (int)a0, a1); return 1;
    case LX(LXN_EPOLL_CREATE):
        *ret = (int)a0 <= 0 ? NEG(E_INVAL) : sys_epoll_create1(c, 0); return 1;
    case LX(LXN_EPOLL_CREATE1): *ret = sys_epoll_create1(c, a0); return 1;
    case LX(LXN_EPOLL_CTL): *ret = sys_epoll_ctl(c, (int)a0, (int)a1, (int)a2, a3); return 1;
    case LX(LXN_EPOLL_WAIT):
    case LX(LXN_EPOLL_PWAIT): {
        /* epoll_pwait's sigmask (a4) is not applied atomically: like the
         * musl layer's ppoll, the mask is left as it is (musl passes NULL). */
        (void)a4;
        int ms = (int)a3;
        int64_t tns = ms < 0 ? -1 : (int64_t)ms * 1000000ll;
        *ret = sys_epoll_wait(c, tf, (int)a0, a1, (int)a2, tns);
        return 1;
    }
    case LX(LXN_EPOLL_PWAIT2): {
        int64_t tns = -1;
        if (a3) {
            struct k_ts ts;
            if (copy_from_user(&ts, (const void*)a3, sizeof(ts)) != 0) { *ret = NEG(E_FAULT); return 1; }
            if (!ts_valid(&ts)) { *ret = NEG(E_INVAL); return 1; }
            tns = (int64_t)ts_ns(&ts);
        }
        *ret = sys_epoll_wait(c, tf, (int)a0, a1, (int)a2, tns);
        return 1;
    }
    case LX(LXN_SETITIMER): *ret = sys_setitimer(c, (int)a0, a1, a2); return 1;
    case LX(LXN_GETITIMER): *ret = sys_getitimer(c, (int)a0, a1); return 1;
    }
    return 0;
}
