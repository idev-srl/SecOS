/* sudo_ts.c - sudo credential timestamps, one per (user, terminal session).
 *
 * After a successful password check /bin/sudo (set-uid root) records "this
 * user authenticated in this terminal session at time T"; a later sudo in the
 * same session within the timeout skips the prompt. The record lives here, in
 * a small kernel table, instead of a file under /run:
 *   - the key is computed by the kernel from the caller itself, never from
 *     arguments or file contents: the real uid, the CONTROLLING terminal (the
 *     pty slot or the console, not whatever fd 0 happens to be), the session
 *     id and the session leader's creation time. A new login on a reused pty
 *     gets a new session (new leader pid and start time), so it never
 *     inherits the record; another terminal or another user has another key;
 *     a process without a controlling terminal gets no record at all;
 *   - the time is the monotonic clock (kclock_mono_ns), so setting the date
 *     cannot extend a record;
 *   - nothing to protect on disk (owner, mode, symlinks, a full /run) and it
 *     is gone at reboot;
 *   - only euid 0 may use it (sudo is set-uid root), so a user cannot create,
 *     refresh or even probe a record.
 * The timeout is the caller's (user/sudo.c owns the policy constant);
 * -DSECOS_SUDO_TS_TEST_TIMEOUT=<s> caps it in a test kernel so the selftest
 * can watch a record expire.
 *
 * Lock: g_ts_lock, a leaf (LK_S64_LEAF); taken with nothing else held, after
 * the key has been computed (pty lock / proc lock released).
 * SPDX-License-Identifier: MIT */
#include "sudo_ts.h"
#include "process.h"
#include "pty.h"
#include "kclock.h"
#include "spinlock.h"
#include "lock_order.h"

#define TS_MAX 32

typedef struct {
    int      used;
    uint32_t uid, tty, sid;
    uint64_t leader_start;     /* session leader's creation time (monotonic ns) */
    uint64_t when;             /* last successful authentication (monotonic ns) */
} sudo_ts_t;

static sudo_ts_t  g_ts[TS_MAX];
static spinlock_t g_ts_lock = SPINLOCK_INIT_CLASS(LK_S64_LEAF, "sudo_ts");

typedef struct { uint32_t uid, tty, sid; uint64_t leader_start; } ts_key_t;

/* The caller's key; -1 when it has no controlling terminal or its session
 * leader is gone (no record then: nothing ties the session to a login). */
static int ts_key(process_t* c, ts_key_t* k) {
    k->uid = c->cred.ruid;
    k->sid = c->sid;
    if (pty_ctty_id(c, &k->tty) != 0) return -1;
    if (k->sid == 0) return -1;
    process_t* l = process_get_by_pid(k->sid);
    if (!l) return -1;
    int ok = (l->sid == k->sid && l->state != PROC_ZOMBIE);
    k->leader_start = l->ki.start_ns;
    process_put(l);
    return ok ? 0 : -1;
}

static int ts_match(const sudo_ts_t* e, const ts_key_t* k) {
    return e->used && e->uid == k->uid && e->tty == k->tty && e->sid == k->sid &&
           e->leader_start == k->leader_start;
}

long sudo_ts_op(process_t* c, uint64_t op, uint64_t arg) {
    if (!c) return -22;
    if (c->cred.euid != 0) return -1;                    /* -EPERM */
    uint64_t now = kclock_mono_ns();
    if (op == SUDO_TS_REMOVE_ALL) {
        uint32_t uid = c->cred.ruid;
        long n = 0;
        uint64_t fl = spin_lock_irqsave(&g_ts_lock);
        for (int i = 0; i < TS_MAX; i++) if (g_ts[i].used && g_ts[i].uid == uid) { g_ts[i].used = 0; n++; }
        spin_unlock_irqrestore(&g_ts_lock, fl);
        return n;
    }
    if (op != SUDO_TS_CHECK && op != SUDO_TS_UPDATE && op != SUDO_TS_INVALIDATE) return -22;
    ts_key_t k;
    if (ts_key(c, &k) != 0) return op == SUDO_TS_CHECK ? 0 : -25;   /* -ENOTTY */

    long r = 0;
    uint64_t fl = spin_lock_irqsave(&g_ts_lock);
    if (op == SUDO_TS_CHECK) {
        uint64_t to = arg;
#ifdef SECOS_SUDO_TS_TEST_TIMEOUT
        if (to > (uint64_t)(SECOS_SUDO_TS_TEST_TIMEOUT)) to = (uint64_t)(SECOS_SUDO_TS_TEST_TIMEOUT);
#endif
        if (to > 86400ull * 365) to = 86400ull * 365;   /* keep the ns product in range */
        for (int i = 0; i < TS_MAX; i++) {
            if (!ts_match(&g_ts[i], &k)) continue;
            if (now >= g_ts[i].when && now - g_ts[i].when < to * 1000000000ull) r = 1;
            else g_ts[i].used = 0;                         /* expired: forget it */
            break;
        }
    } else if (op == SUDO_TS_INVALIDATE) {
        for (int i = 0; i < TS_MAX; i++) if (ts_match(&g_ts[i], &k)) g_ts[i].used = 0;
    } else {                                               /* UPDATE */
        int slot = -1, oldest = 0;
        for (int i = 0; i < TS_MAX; i++) {
            if (ts_match(&g_ts[i], &k)) { slot = i; break; }
            if (slot < 0 && !g_ts[i].used) slot = i;
            if (g_ts[i].when < g_ts[oldest].when) oldest = i;
        }
        if (slot < 0) slot = oldest;                       /* table full: the oldest goes */
        g_ts[slot].used = 1;
        g_ts[slot].uid = k.uid; g_ts[slot].tty = k.tty; g_ts[slot].sid = k.sid;
        g_ts[slot].leader_start = k.leader_start;
        g_ts[slot].when = now;
    }
    spin_unlock_irqrestore(&g_ts_lock, fl);
    return r;
}
