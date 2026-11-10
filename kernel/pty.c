/*
 * SecOS Kernel - Pseudo-terminals (/dev/ptmx, /dev/pts/N)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * See pty.h for the model. Implementation notes:
 *  - One global spinlock (g_pty_lock) guards the pty table and every pty's
 *    state, including its struct tty. Nothing that takes the process-table lock
 *    (signal posting, sched_wake_pty) runs under it: signals raised by the line
 *    discipline are queued in the tty and posted after unlocking.
 *  - Blocking follows the pipe/tty pattern: an op that cannot progress returns
 *    -2 with current->wait_pty = the pty; syscall_trap.c blocks the caller and
 *    re-runs the syscall once sched_wake_pty() (the other side) or the periodic
 *    tty tick makes it READY.
 *  - The slave's output goes into a 4 KB ring the master reads; echo of master
 *    input goes there too (dropped when the ring is full, as a flooded n_tty
 *    echo would be).
 */
#include "cred.h"
#include <stdint.h>
#include <stddef.h>
#include "pty.h"
#include "thread.h"   /* [U9] */
#include "tty.h"
#include "process.h"
#include "signal.h"
#include "sched.h"
#include "spinlock.h"
#include "heap.h"
#include "../mm/user_copy.h"

extern process_t* sched_get_current(void);
extern void process_foreach(void (*cb)(process_t*, void*), void* user);
extern int devfs_is_ptmx(void* inode);
extern int devfs_is_devtty(void* inode);

#define PTY_OBUF 4096

#define E_PERM   (-1)
#define E_NXIO   (-6)
#define E_IO     (-5)
#define E_AGAIN  (-11)
#define E_INVAL  (-22)
#define E_NOTTY  (-25)
#define E_NOSPC  (-28)
#define E_SRCH   (-3)
#define E_INTR   (-4)
#define WOULDBLOCK (-2)

#define K_O_NOCTTY 0x100

typedef struct pty {
    int         idx;
    struct tty  t;                  /* the slave's line discipline */
    uint8_t     obuf[PTY_OBUF];     /* slave output -> master read */
    uint32_t    ohead, olen;
    int         mrefs, srefs, crefs;/* master fds, slave fds, ctty holders */
    int         slave_opened;       /* a slave was opened once (master EIO after close) */
    int         locked;             /* TIOCSPTLCK: slave open refused while set */
    vfs_inode_t mino, sino;         /* what pty fds point at */
} pty_t;

static pty_t* g_pty[PTY_MAX];
/* [M52] Pseudo-terminals live in a static pool and are never freed, only
 * detached (g_pty[idx] = NULL) and reused. A /dev/pts/N lookup hands out
 * &p->sino after dropping g_pty_lock, and the VFS writes ino->ops into it
 * and the open path reads its mode/uid; with kmalloc'd ptys a concurrent
 * hangup could free it in between (a write into freed heap). A slot reused
 * by another pty in that window is caught by re-checking under the lock
 * (pty_open_hook, pty_devfs_setattr). */
static pty_t g_pty_mem[PTY_MAX];
static spinlock_t g_pty_lock = SPINLOCK_INIT_CLASS(LK_S10_TTY, "pty");
vfs_inode_t g_pty_dir_inode;

/* ---- helpers ----------------------------------------------------------------- */
static pty_t* pty_of(const void* inode, int* kind) {
    if (!inode || inode == (void*)1) return NULL;
    for (int i = 0; i < PTY_MAX; i++) {
        pty_t* p = g_pty[i];
        if (!p) continue;
        if (inode == &p->mino) { if (kind) *kind = PTY_KIND_MASTER; return p; }
        if (inode == &p->sino) { if (kind) *kind = PTY_KIND_SLAVE;  return p; }
    }
    return NULL;
}

int pty_kind(const void* inode) {
    int k = PTY_KIND_NONE;
    uint64_t fl = spin_lock_irqsave(&g_pty_lock);
    pty_of(inode, &k);
    spin_unlock_irqrestore(&g_pty_lock, fl);
    return k;
}
int pty_is_inode(const void* inode) { return pty_kind(inode) != PTY_KIND_NONE; }

static void name_slave(char* out, int idx) {
    const char* pre = "/pts/";
    int k = 0;
    while (pre[k]) { out[k] = pre[k]; k++; }
    char d[4]; int n = 0;
    do { d[n++] = (char)('0' + idx % 10); idx /= 10; } while (idx && n < 3);
    while (n) out[k++] = d[--n];
    out[k] = 0;
}

/* Slave output sink used by the ldisc's echo: best effort. */
static void obuf_put(pty_t* p, char c) {
    if (p->olen >= PTY_OBUF) return;
    p->obuf[(p->ohead + p->olen) % PTY_OBUF] = (uint8_t)c;
    p->olen++;
}
static void pty_echo_out(struct tty* t, const char* s, int n, int onlcr) {
    pty_t* p = (pty_t*)t->owner;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\n' && onlcr) {
            if (PTY_OBUF - p->olen < 2) return;
            obuf_put(p, '\r');
        }
        obuf_put(p, s[i]);
    }
}

/* Drop the pty object if nothing refers to it any more. Lock held; returns the
 * object to kfree once the lock is dropped (or NULL). */
static pty_t* maybe_detach(pty_t* p) {
    if (p->mrefs > 0 || p->srefs > 0 || p->crefs > 0) return NULL;
    if (g_pty[p->idx] == p) g_pty[p->idx] = NULL;
    return p;
}

/* Hang the terminal's session up: SIGHUP + SIGCONT to the foreground group and
 * the session leader, then the tty controls no session. Lock held. */
static void hangup_session(struct tty* t) {
    if (t->fg_pgrp) { tty_emit_sig(t, t->fg_pgrp, SIGHUP, 0); tty_emit_sig(t, t->fg_pgrp, SIGCONT, 0); }
    if (t->session) { tty_emit_sig(t, t->session, SIGHUP, 1); tty_emit_sig(t, t->session, SIGCONT, 1); }
    t->session = 0;
    t->fg_pgrp = 0;
}

/* Common tail: post queued signals, wake waiters, free if released. */
static void finish(pty_t* p, struct tty_sig* sigs, int nsig, int wake, pty_t* to_free) {
    (void)to_free;                  /* [M52] detached under the lock; pool slots are not freed */
    tty_post_sigs(sigs, nsig);
    if (wake) sched_wake_pty(p);
}

/* ---- references -------------------------------------------------------------- */
void pty_ref(void* inode) {
    uint64_t fl = spin_lock_irqsave(&g_pty_lock);
    int k = 0; pty_t* p = pty_of(inode, &k);
    if (p) { if (k == PTY_KIND_MASTER) p->mrefs++; else p->srefs++; }
    spin_unlock_irqrestore(&g_pty_lock, fl);
}

void pty_unref(void* inode) {
    struct tty_sig sigs[TTY_PSIG_MAX]; int ns = 0;
    pty_t* dead = NULL;
    uint64_t fl = spin_lock_irqsave(&g_pty_lock);
    int k = 0; pty_t* p = pty_of(inode, &k);
    if (!p) { spin_unlock_irqrestore(&g_pty_lock, fl); return; }
    if (k == PTY_KIND_MASTER) {
        if (p->mrefs > 0 && --p->mrefs == 0) {      /* last master close: hangup */
            p->t.hungup = 1;
            hangup_session(&p->t);
        }
    } else {
        if (p->srefs > 0) p->srefs--;
    }
    ns = tty_take_sigs(&p->t, sigs);
    dead = maybe_detach(p);
    spin_unlock_irqrestore(&g_pty_lock, fl);
    finish(p, sigs, ns, 1, dead);
}

void pty_ctty_ref(void* tty) {
    if (!tty) return;
    uint64_t fl = spin_lock_irqsave(&g_pty_lock);
    pty_t* p = (pty_t*)((struct tty*)tty)->owner;
    if (p) p->crefs++;
    spin_unlock_irqrestore(&g_pty_lock, fl);
}

/* Drop p's controlling terminal. A session leader that still controls it
 * (exit, TIOCNOTTY) hangs its session up. Lock NOT held. */
static void ctty_drop(process_t* c, int leader_hangup) {
    struct tty* t = (struct tty*)c->ctty;
    if (!t) return;
    struct tty_sig sigs[TTY_PSIG_MAX]; int ns = 0;
    pty_t* dead = NULL;
    uint64_t fl = spin_lock_irqsave(&g_pty_lock);
    pty_t* p = (pty_t*)t->owner;
    c->ctty = NULL;
    if (leader_hangup && c->tgid == c->sid && t->session == c->sid) hangup_session(t);   // [U9] tgid
    if (p->crefs > 0) p->crefs--;
    ns = tty_take_sigs(t, sigs);
    dead = maybe_detach(p);
    spin_unlock_irqrestore(&g_pty_lock, fl);
    finish(p, sigs, ns, 1, dead);
}

void pty_proc_exit(process_t* p) {
    if (!p) return;
    ctty_drop(p, 1);
}

// [U9] A thread that is not the last of its process: drop its own controlling
// terminal reference only (the session lives on).
void pty_thread_exit(process_t* p) {
    if (!p) return;
    ctty_drop(p, 0);
}

void pty_inherit_ctty(process_t* child, const process_t* parent) {
    if (!child || !parent) return;
    child->sid = parent->sid;
    child->ctty_detached = parent->ctty_detached;
    child->ctty = parent->ctty;
    pty_ctty_ref(child->ctty);
}

struct tty* pty_proc_ctty(process_t* p, int* is_console) {
    if (is_console) *is_console = 0;
    if (!p) return NULL;
    struct tty* t = (struct tty*)p->ctty;
    if (t) return (t->session && t->session == p->sid) ? t : NULL;
    if (p->ctty_detached) return NULL;
    if (is_console) *is_console = 1;
    return NULL;
}

/* [U-sudo] A stable id for p's controlling terminal (kernel/sudo_ts.c): the
 * pty pool index + 1, or PTY_CTTY_CONSOLE. -1 when p has none. */
int pty_ctty_id(process_t* p, uint32_t* id) {
    if (!p || !id) return -1;
    int con = 0, r = -1;
    uint64_t fl = spin_lock_irqsave(&g_pty_lock);
    struct tty* t = pty_proc_ctty(p, &con);
    if (con) { *id = PTY_CTTY_CONSOLE; r = 0; }
    else if (t && t->owner && !t->hungup) {
        pty_t* o = (pty_t*)t->owner;
        if (o >= g_pty_mem && o < g_pty_mem + PTY_MAX) { *id = (uint32_t)(o - g_pty_mem) + 1; r = 0; }
    }
    spin_unlock_irqrestore(&g_pty_lock, fl);
    return r;
}

/* Make t the controlling terminal of c's session. Lock held. Old (stale)
 * ctty references are dropped by the caller before. */
static void acquire_ctty(process_t* c, pty_t* p) {
    p->t.session = c->sid;
    p->t.fg_pgrp = c->pgid;
    c->ctty = &p->t;
    c->ctty_detached = 0;
    p->crefs++;
}

/* ---- open ---------------------------------------------------------------------- */
static int pty_alloc(vfs_inode_t* ptmx, vfs_inode_t** out) {
    uint64_t fl = spin_lock_irqsave(&g_pty_lock);
    int idx = -1;
    for (int i = 0; i < PTY_MAX; i++) if (!g_pty[i]) { idx = i; break; }
    if (idx < 0) { spin_unlock_irqrestore(&g_pty_lock, fl); return E_NOSPC; }
    pty_t* np = &g_pty_mem[idx];                    /* [M52] static pool slot */
    for (size_t i = 0; i < sizeof(*np); i++) ((uint8_t*)np)[i] = 0;
    np->idx = idx;
    tty_instance_init(&np->t, pty_echo_out, np);
    np->mrefs = 1;
    np->locked = 1;                                  /* until unlockpt() */
    /* master: anonymous char device; slave: /pts/N in devfs */
    np->mino.path[0] = '/'; np->mino.path[1] = 'p'; np->mino.path[2] = 't';
    np->mino.path[3] = 'm'; np->mino.path[4] = 'x'; np->mino.path[5] = 0;
    name_slave(np->sino.path, idx);
    np->mino.type = np->sino.type = VFS_NODE_FILE;
    np->mino.mode = 0020666; np->sino.mode = 0020620;   /* S_IFCHR */
    /* [M43] devpts: the slave belongs to whoever opened /dev/ptmx (the session
     * user: secwm's terminals, or sshd, which chowns it to the login user),
     * group tty, crw--w----; the master is only reachable through its fd. */
    {   extern process_t* sched_get_current(void);
        process_t* op = sched_get_current();
        np->sino.uid = cred_is_kernel(op) ? 0 : op->cred.euid; np->sino.gid = GID_TTY;
        np->mino.uid = np->sino.uid; np->mino.gid = 0; }
    np->mino.nlink = np->sino.nlink = 1;
    np->mino.ops = np->sino.ops = ptmx->ops;
    np->mino.fs_data = np->sino.fs_data = NULL;
    g_pty[idx] = np;
    spin_unlock_irqrestore(&g_pty_lock, fl);
    *out = &np->mino;
    return 0;
}

int pty_open_hook(process_t* c, vfs_inode_t** ino, int flags) {
    if (!ino || !*ino) return 0;
    vfs_inode_t* in = *ino;
    if (devfs_is_ptmx(in)) return pty_alloc(in, ino);
    if (devfs_is_devtty(in)) {
        int con = 0;
        struct tty* t = pty_proc_ctty(c, &con);
        if (con) return 0;                           /* the console node itself */
        if (!t) return E_NXIO;                       /* no controlling terminal */
        pty_t* p = (pty_t*)t->owner;
        uint64_t fl = spin_lock_irqsave(&g_pty_lock);
        p->srefs++; p->slave_opened = 1;
        spin_unlock_irqrestore(&g_pty_lock, fl);
        *ino = &p->sino;
        return 0;
    }
    uint64_t fl = spin_lock_irqsave(&g_pty_lock);
    int k = 0; pty_t* p = pty_of(in, &k);
    if (!p) { spin_unlock_irqrestore(&g_pty_lock, fl); return 0; }   /* not a pty node */
    if (k != PTY_KIND_SLAVE) { spin_unlock_irqrestore(&g_pty_lock, fl); return E_IO; }
    if (p->locked || p->mrefs == 0) { spin_unlock_irqrestore(&g_pty_lock, fl); return E_IO; }
    /* [M52] The open path checked the permission on a lookup made without
     * this lock; the slot may since have been reused by another user's pty.
     * Check again against what is there now (cred_inode_perm takes no lock). */
    {   int acc = flags & 3, mask = 0;
        if (acc != 1) mask |= MAY_READ;
        if (acc != 0) mask |= MAY_WRITE;
        int pe = cred_inode_perm(c, &p->sino, mask);
        if (pe) { spin_unlock_irqrestore(&g_pty_lock, fl); return pe; } }
    p->srefs++; p->slave_opened = 1;
    /* A session leader without a controlling terminal acquires a free one. */
    int stale = 0;
    if (!(flags & K_O_NOCTTY) && c && c->pid != 0 && c->sid == c->pid && p->t.session == 0) {
        struct tty* ct = (struct tty*)c->ctty;
        int has = ct ? (ct->session && ct->session == c->sid) : !c->ctty_detached;
        if (!has) {
            stale = ct != NULL;
            if (!stale) acquire_ctty(c, p);
        }
    }
    spin_unlock_irqrestore(&g_pty_lock, fl);
    if (stale) {                                     /* drop the dead one, then acquire */
        ctty_drop(c, 0);
        fl = spin_lock_irqsave(&g_pty_lock);
        if (p->t.session == 0 && !c->ctty) acquire_ctty(c, p);
        spin_unlock_irqrestore(&g_pty_lock, fl);
    }
    return 0;
}

/* ---- I/O ------------------------------------------------------------------------- */
int pty_read(void* inode, void* buf, int len, int nonblock) {
    if (len <= 0) return E_INVAL;
    process_t* cur = sched_get_current();
    struct tty_sig sigs[TTY_PSIG_MAX]; int ns = 0;
    uint64_t fl = spin_lock_irqsave(&g_pty_lock);
    int k = 0; pty_t* p = pty_of(inode, &k);
    if (!p) { spin_unlock_irqrestore(&g_pty_lock, fl); return E_IO; }
    int r;
    if (k == PTY_KIND_MASTER) {
        if (p->olen > 0) {
            uint32_t n = (uint32_t)len < p->olen ? (uint32_t)len : p->olen;
            uint8_t* d = (uint8_t*)buf;
            for (uint32_t i = 0; i < n; i++) d[i] = p->obuf[(p->ohead + i) % PTY_OBUF];
            p->ohead = (p->ohead + n) % PTY_OBUF; p->olen -= n;
            r = (int)n;
        } else if (p->slave_opened && p->srefs == 0) {
            r = E_IO;                                /* every slave closed (Linux) */
        } else if (cur && signal_pending(cur)) {
            r = E_INTR;
        } else if (nonblock || !cur || cur->pid == 0) {
            r = E_AGAIN;
        } else {
            cur->wait_pty = p;
            r = WOULDBLOCK;
        }
    } else {
        r = tty_ldisc_read(&p->t, buf, len, nonblock ? TTY_RD_NONBLOCK : TTY_RD_BLOCK);
        ns = tty_take_sigs(&p->t, sigs);
    }
    spin_unlock_irqrestore(&g_pty_lock, fl);
    finish(p, sigs, ns, r > 0, NULL);               /* room freed: wake the writer */
    return r;
}

int pty_write(void* inode, const void* buf, int len, int nonblock) {
    if (len <= 0) return E_INVAL;
    process_t* cur = sched_get_current();
    const uint8_t* s = (const uint8_t*)buf;
    struct tty_sig sigs[TTY_PSIG_MAX]; int ns = 0;
    uint64_t fl = spin_lock_irqsave(&g_pty_lock);
    int k = 0; pty_t* p = pty_of(inode, &k);
    if (!p) { spin_unlock_irqrestore(&g_pty_lock, fl); return E_IO; }
    int r = 0;
    if (k == PTY_KIND_MASTER) {
        int room = tty_input_room(&p->t);
        if (room <= 0) {
            r = (cur && signal_pending(cur)) ? E_INTR
              : (nonblock || !cur || cur->pid == 0) ? E_AGAIN : WOULDBLOCK;
            if (r == WOULDBLOCK) cur->wait_pty = p;
        } else {
            int n = len < room ? len : room;
            for (int i = 0; i < n; i++) tty_input_byte(&p->t, s[i]);
            r = n;
        }
    } else if (p->t.hungup) {
        r = E_IO;
    } else {
        if (p->t.tio.c_lflag & T_TOSTOP) r = tty_bg_check(&p->t, cur, SIGTTOU);
        if (r == 0) {
            int onlcr = tty_onlcr(&p->t);
            int n = 0;
            while (n < len) {
                int need = (s[n] == '\n' && onlcr) ? 2 : 1;
                if ((int)(PTY_OBUF - p->olen) < need) break;
                if (need == 2) obuf_put(p, '\r');
                obuf_put(p, (char)s[n]);
                n++;
            }
            if (n > 0) r = n;
            else if (cur && signal_pending(cur)) r = E_INTR;
            else if (nonblock || !cur || cur->pid == 0) r = E_AGAIN;
            else { cur->wait_pty = p; r = WOULDBLOCK; }
        }
    }
    ns = tty_take_sigs(&p->t, sigs);
    spin_unlock_irqrestore(&g_pty_lock, fl);
    finish(p, sigs, ns, r > 0, NULL);
    return r;
}

int pty_poll(void* inode, int events) {
    uint64_t fl = spin_lock_irqsave(&g_pty_lock);
    int k = 0; pty_t* p = pty_of(inode, &k);
    if (!p) { spin_unlock_irqrestore(&g_pty_lock, fl); return -1; }
    int rev = 0;
    if (k == PTY_KIND_MASTER) {
        if (p->olen > 0) rev |= PTY_POLLIN;
        if (tty_input_room(&p->t) > 0) rev |= PTY_POLLOUT;
        if (p->slave_opened && p->srefs == 0) rev |= PTY_POLLHUP | PTY_POLLIN;
    } else {
        if (tty_ldisc_readable(&p->t) > 0) rev |= PTY_POLLIN;
        if (PTY_OBUF - p->olen >= 2) rev |= PTY_POLLOUT;
        if (p->t.hungup) rev = PTY_POLLHUP | PTY_POLLIN | PTY_POLLERR;
    }
    spin_unlock_irqrestore(&g_pty_lock, fl);
    return rev & (events | PTY_POLLHUP | PTY_POLLERR);
}

int tty_console_poll(int events) {
    int rev = PTY_POLLOUT;
    if (tty_ldisc_readable(tty_console()) > 0) rev |= PTY_POLLIN;
    return rev & (events | PTY_POLLHUP | PTY_POLLERR);
}

/* ---- ioctl ------------------------------------------------------------------------ */
struct pg_find { uint32_t pgid, sid; int found; };
static void pg_find_cb(process_t* p, void* u) {
    struct pg_find* f = (struct pg_find*)u;
    if (p->state != PROC_ZOMBIE && p->pgid == f->pgid && (f->sid == 0 || p->sid == f->sid)) f->found = 1;
}

/* Termios, window size, queue, job-control and session requests on a pty fd.
 * Every user payload is copied in before taking the pty lock and copied out
 * after it: [M50] no user copy runs under a spinlock (a fault there would page
 * in, maybe from disk, with interrupts off), so the line-discipline requests
 * work on a kernel buffer (tty_ldisc_ioctl_kbuf). */
static int ldisc_locked(struct tty* t, uint64_t cmd, uint64_t arg, unsigned plen,
                        int kb_err, void* kb) {
    if (!plen) return tty_ldisc_ioctl(t, cmd, arg);     /* no payload: no copy */
    if (kb_err) return -14;                              /* where the copy-in would fail */
    return tty_ldisc_ioctl_kbuf(t, cmd, kb);
}

int pty_ioctl(process_t* c, void* inode, uint64_t cmd, uint64_t arg) {
    int inv = 0;
    int pg_ok = 0;
    union { struct tty_termios tio; struct tty_winsize ws; int i; } kb;
    int p_in = 0, kb_err = 0;
    unsigned plen = tty_ldisc_ioctl_payload(cmd, &p_in);
    if (plen > sizeof(kb)) plen = 0;                     /* cannot happen; stay safe */
    if (plen && p_in && copy_from_user(&kb, (void*)arg, plen)) kb_err = 1;
    if (cmd == TTY_TIOCSPTLCK || cmd == TTY_TIOCSPGRP) {
        if (copy_from_user(&inv, (void*)arg, sizeof(inv))) return -EFAULT;
        if (cmd == TTY_TIOCSPGRP && c && inv > 0) {   /* the group must be in our session */
            struct pg_find f = { (uint32_t)inv, c->sid, 0 };
            process_foreach(pg_find_cb, &f);
            pg_ok = f.found;
        }
    }
    struct tty_sig sigs[TTY_PSIG_MAX]; int ns = 0;
    int r = 0, outv = 0, have_out = 0, stale = 0, notty = 0;
    int k = 0;
    uint64_t fl = spin_lock_irqsave(&g_pty_lock);
    pty_t* p = pty_of(inode, &k);
    if (!p) { spin_unlock_irqrestore(&g_pty_lock, fl); return E_NOTTY; }
    struct tty* t = &p->t;
    int master = (k == PTY_KIND_MASTER);
    int is_ctty = c && (struct tty*)c->ctty == t && t->session && t->session == c->sid;
    switch (cmd) {
    case TTY_TIOCGPTN:                               /* ptsname() */
        if (!master) r = E_NOTTY; else { outv = p->idx; have_out = 1; }
        break;
    case TTY_TIOCGPTLCK:
        if (!master) r = E_NOTTY; else { outv = p->locked; have_out = 1; }
        break;
    case TTY_TIOCSPTLCK:                             /* unlockpt() = TIOCSPTLCK(0) */
        if (!master) r = E_NOTTY; else p->locked = (inv != 0);
        break;
    case TTY_TIOCGPGRP:                              /* tcgetpgrp(); slave: own ctty only */
        if (!master && !is_ctty) r = E_NOTTY; else { outv = (int)t->fg_pgrp; have_out = 1; }
        break;
    case TTY_TIOCGSID:                               /* tcgetsid() */
        if (t->session == 0) r = E_NOTTY; else { outv = (int)t->session; have_out = 1; }
        break;
    case TTY_TIOCSPGRP:                              /* tcsetpgrp(): our ctty, our session */
        if (master || !is_ctty) { r = E_NOTTY; break; }
        r = tty_bg_check(t, c, SIGTTOU);
        if (r) break;
        if (inv <= 0) r = E_INVAL;
        else if (!pg_ok) r = E_PERM;
        else t->fg_pgrp = (uint32_t)inv;
        break;
    case TTY_TIOCSCTTY: {                            /* become our controlling terminal */
        if (master) { r = E_NOTTY; break; }
        if (!c || c->pid == 0 || c->sid != c->pid) { r = E_PERM; break; }   /* not a leader */
        if (is_ctty) break;
        struct tty* ct = (struct tty*)c->ctty;
        int has = ct ? (ct->session && ct->session == c->sid) : !c->ctty_detached;
        if (has) { r = E_PERM; break; }             /* already has one */
        if (t->session && t->session != c->sid && (arg != 1 || !cred_is_root(c))) { r = E_PERM; break; } /* arg 1: steal (root) */
        if (ct) { stale = 1; break; }                /* drop the dead ctty first */
        acquire_ctty(c, p);
        break;
    }
    case TTY_TIOCNOTTY:                              /* give up the controlling terminal */
        if (!is_ctty) r = E_NOTTY; else notty = 1;
        break;
    case TTY_TCSETS: case TTY_TCSETSW: case TTY_TCSETSF:
    case TTY_TCFLSH: case TTY_TCXONC:
        if (!master && is_ctty) { r = tty_bg_check(t, c, SIGTTOU); if (r) break; }
        r = ldisc_locked(t, cmd, arg, plen, kb_err, &kb);
        break;
    case TTY_FIONREAD:                               /* master: output waiting to be read */
        if (master) { outv = (int)p->olen; have_out = 1; plen = 0; }
        else r = ldisc_locked(t, cmd, arg, plen, kb_err, &kb);
        break;
    case TTY_TIOCOUTQ:                               /* slave: output not yet read */
        outv = master ? 0 : (int)p->olen; have_out = 1;
        break;
    default:                                         /* termios get, winsize, flush... */
        r = ldisc_locked(t, cmd, arg, plen, kb_err, &kb);
        break;
    }
    ns = tty_take_sigs(t, sigs);
    spin_unlock_irqrestore(&g_pty_lock, fl);
    finish(p, sigs, ns, 1, NULL);

    if (stale) {
        ctty_drop(c, 0);
        fl = spin_lock_irqsave(&g_pty_lock);
        if (pty_of(inode, &k) == p && !c->ctty && (t->session == 0 || t->session == c->sid || arg == 1))
            acquire_ctty(c, p);
        spin_unlock_irqrestore(&g_pty_lock, fl);
    }
    if (notty) {
        ctty_drop(c, 1);                             /* a session leader hangs the session up */
        c->ctty_detached = 1;
    }
    if (have_out && r == 0 && copy_to_user((void*)arg, &outv, sizeof(outv))) return -EFAULT;
    if (!have_out && r == 0 && plen && !p_in && copy_to_user((void*)arg, &kb, plen)) return -14;
    return r;
}

/* The console as a controlling terminal: TIOCSCTTY gives a session leader
 * without one the console back; TIOCNOTTY detaches the caller from it. */
int pty_console_sctty(process_t* c) {
    if (!c || c->pid == 0) return 0;
    int con = 0;
    struct tty* t = pty_proc_ctty(c, &con);
    if (con) return 0;
    if (t || c->sid != c->tgid) return E_PERM;
    if (c->ctty) ctty_drop(c, 0);
    c->ctty_detached = 0;
    return 0;
}
int pty_console_notty(process_t* c) {
    if (!c || c->pid == 0) return 0;
    int con = 0;
    pty_proc_ctty(c, &con);
    if (!con) return E_NOTTY;
    c->ctty_detached = 1;
    return 0;
}

/* ---- sessions ---------------------------------------------------------------------- */
long ksys_setsid(void) {
    process_t* c = sched_get_current();
    if (!c || c->pid == 0) return E_PERM;
    if (thread_group_live(c) > 1) return E_PERM;     /* [U9] see ksys_ioctl (TIOCSCTTY) */
    struct pg_find f = { c->tgid, 0, 0 };           /* sid 0: any session */
    /* [M52] The check and the sid/pgid stores in one proc_lock section: a
     * parent's setpgid() on another CPU checks our sid and stores our pgid
     * under the same lock. ctty_drop (g_pty_lock, S10) runs after it: it
     * does not look at the session (no leader hangup here). */
    uint64_t fl = process_lock_irqsave();
    process_foreach_locked(pg_find_cb, &f);
    if (!f.found) { c->sid = c->tgid; c->pgid = c->tgid; }
    process_unlock_irqrestore(fl);
    if (f.found) return E_PERM;                       /* already a group leader (POSIX) */
    ctty_drop(c, 0);
    c->ctty = NULL;
    c->ctty_detached = 1;                             /* a new session has no terminal */
    return (long)c->sid;
}
long ksys_getsid(int pid) {
    process_t* c = sched_get_current();
    if (pid == 0) return c ? (long)c->sid : E_SRCH;
    process_t* t = process_get_by_pid((uint32_t)pid);   // [M50-B1]
    if (!t) return E_SRCH;
    long sid = (long)t->sid;
    process_put(t);
    return sid;
}

/* ---- devfs glue --------------------------------------------------------------------- */
static int atoi_dec(const char* s, int* out) {
    int v = 0, n = 0;
    while (*s >= '0' && *s <= '9' && n < 4) { v = v * 10 + (*s - '0'); s++; n++; }
    if (*s || n == 0) return -1;
    *out = v;
    return 0;
}

vfs_inode_t* pty_devfs_lookup(const char* rel) {
    /* rel: "/pts" or "/pts/N" */
    if (!(rel[0] == '/' && rel[1] == 'p' && rel[2] == 't' && rel[3] == 's')) return NULL;
    if (rel[4] == 0) {
        vfs_inode_t* d = &g_pty_dir_inode;
        d->path[0] = '/'; d->path[1] = 'p'; d->path[2] = 't'; d->path[3] = 's'; d->path[4] = 0;
        d->type = VFS_NODE_DIR; d->size = 0; d->fs_data = NULL; d->mode = 0040755; d->nlink = 2;
        return d;
    }
    if (rel[4] != '/') return NULL;
    int idx;
    if (atoi_dec(rel + 5, &idx) != 0 || idx < 0 || idx >= PTY_MAX) return NULL;
    uint64_t fl = spin_lock_irqsave(&g_pty_lock);
    pty_t* p = g_pty[idx];
    vfs_inode_t* r = (p && p->mrefs > 0) ? &p->sino : NULL;
    spin_unlock_irqrestore(&g_pty_lock, fl);
    return r;
}

/* [M52] chmod/chown/utimes of /dev/pts/N (devfs routes them here): applied to
 * the live slave under g_pty_lock. The caller's permission was checked on an
 * earlier lookup; unless it is root it must still own the slave now (the
 * slot may have been reused by another user's pty meanwhile). 1 = not a
 * /pts/N path, else 0 / -errno. */
int pty_devfs_setattr(const char* rel, const vfs_attr_t* a, unsigned valid) {
    if (!(rel[0] == '/' && rel[1] == 'p' && rel[2] == 't' && rel[3] == 's' && rel[4] == '/')) return 1;
    int idx;
    if (atoi_dec(rel + 5, &idx) != 0 || idx < 0 || idx >= PTY_MAX) return -2;
    extern process_t* sched_get_current(void);
    process_t* c = sched_get_current();
    uint64_t fl = spin_lock_irqsave(&g_pty_lock);
    pty_t* p = g_pty[idx];
    int r = 0;
    if (!p || p->mrefs <= 0) r = -2;                               /* -ENOENT */
    else if (!cred_is_root(c) && c->cred.euid != p->sino.uid &&
             (valid & (VFS_ATTR_MODE | VFS_ATTR_UID | VFS_ATTR_GID))) r = -1;   /* -EPERM */
    else {
        vfs_inode_t* ino = &p->sino;
        if (valid & VFS_ATTR_MODE)  ino->mode = (ino->mode & 0170000u) | (a->mode & 07777u);
        if (valid & VFS_ATTR_UID)   ino->uid = a->uid;
        if (valid & VFS_ATTR_GID)   ino->gid = a->gid;
        if (valid & VFS_ATTR_ATIME) ino->atime = a->atime;
        if (valid & VFS_ATTR_MTIME) ino->mtime = a->mtime;
    }
    spin_unlock_irqrestore(&g_pty_lock, fl);
    return r;
}

int pty_devfs_readdir(vfs_iter_cb cb, void* user) {
    vfs_inode_t* list[PTY_MAX]; int n = 0;
    uint64_t fl = spin_lock_irqsave(&g_pty_lock);
    for (int i = 0; i < PTY_MAX; i++) if (g_pty[i] && g_pty[i]->mrefs > 0) list[n++] = &g_pty[i]->sino;
    spin_unlock_irqrestore(&g_pty_lock, fl);
    for (int i = 0; i < n; i++) cb(list[i], user);
    return 0;
}

#ifdef SECOS_TTY_TEST
/* [M51] kernel/tty_test.c: a pty pair opened from kernel context (no fds): the
 * master as /dev/ptmx would give it, the slave unlocked and referenced once,
 * with the given termios. pty_test_close drops both references. */
int pty_test_open(void** m, void** s, const struct tty_termios* tio) {
    extern vfs_inode_t* vfs_lookup(const char*);
    vfs_inode_t* ptmx = vfs_lookup("/dev/ptmx");
    vfs_inode_t* mi = NULL;
    if (!ptmx || !devfs_is_ptmx(ptmx) || pty_alloc(ptmx, &mi) != 0) return -1;
    uint64_t fl = spin_lock_irqsave(&g_pty_lock);
    pty_t* p = pty_of(mi, NULL);
    if (!p) { spin_unlock_irqrestore(&g_pty_lock, fl); return -1; }
    p->locked = 0; p->srefs++; p->slave_opened = 1;
    if (tio) p->t.tio = *tio;
    spin_unlock_irqrestore(&g_pty_lock, fl);
    *m = mi; *s = &p->sino;
    return 0;
}
void pty_test_close(void* m, void* s) { pty_unref(s); pty_unref(m); }
#endif
