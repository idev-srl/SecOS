/*
 * kiface.c — kernel interfaces for standard Linux userland tools.
 *
 * Syscalls 65..79 (numbers in kernel/syscall.h; arguments follow Linux and
 * use the Linux struct layouts below): POSIX clocks + nanosleep, the kernel log (klogctl),
 * statfs/fstatfs, sync/fsync, rename, link, truncate/ftruncate, rmdir, access,
 * umask, uname, poll and sysinfo. Plus the accounting /proc reports: per-task
 * name/argv/CPU times, per-CPU user/system/idle ticks, context switches, forks
 * and load averages.
 *
 * Every user pointer goes through copy_from_user/copy_to_user/
 * strncpy_from_user. Errors are returned as -errno (Linux values) so libc can
 * set errno. Confined (CAP_ENFORCE) programs are gated like the rest of the
 * syscall surface: FS_READ for lookups, FS_WRITE for namespace changes,
 * TIME for setting the clock.
 *
 * SPDX-License-Identifier: MIT
 */
#include "udrv.h"          /* [M57] claim fds in poll() */
#include "anonfd.h"        /* [U10] eventfd/timerfd/epoll/AF_UNIX in poll() */
#include "kiface.h"
#include "klock.h"
#include "cred.h"
#include "process.h"
#include "sched.h"
#include "signal.h"
#include "vfs.h"
#include "cap.h"
#include "klog.h"
#include "kclock.h"
#include "pipe.h"
#include "percpu.h"
#include "pmm.h"
#include "user_copy.h"
#include "../mm/pagecache.h"
#include <stddef.h>

extern uint64_t timer_get_ticks(void);
extern uint64_t tsc_hz(void);
extern void proc_resolve_path(process_t* c, const char* in, char* out, int outsz);
extern int  socket_poll(uint32_t pid, int fd);
#include "pty.h"     /* pty_poll / tty_console_poll / PTY_WAIT_ANY */
#include "input.h"   /* [M40] evdev clients */
extern int  devfs_is_console(void* inode);
extern int  keyboard_has_char(void);
extern void process_foreach(void (*cb)(process_t*, void*), void* user);

#define E_PERM    1
#define E_NOENT   2
#define E_INTR    4
#define E_BADF    9
#define E_ACCES  13
#define E_FAULT  14
#define E_INVAL  22
#define NEG(e)   ((uint64_t)(int64_t)-(e))

/* ---------------------------------------------------------------- accounting */
/* [M51-C6] Concurrency: g_cpu_* are per-CPU, each written only by its own CPU's
 * tick; g_load only by CPU 0's tick; readers take aligned 64-bit loads (never
 * torn on x86-64, a stat may be one tick old). g_ctxt / g_forks are bumped by
 * every CPU (switches, process creation): atomic adds, or concurrent updates
 * are lost. A process's ki.cmdline/comm are written by its own exec (or before
 * it is published) while /proc readers copy them unlocked: a reader can see a
 * mix of the old and new name, never past the array (cmdline_len is bounded by
 * sizeof(cmdline)) -- acceptable for /proc, like Linux's racy comm reads. */
static uint64_t g_cpu_user[SMP_MAX_CPUS], g_cpu_sys[SMP_MAX_CPUS], g_cpu_idle[SMP_MAX_CPUS];
static volatile uint64_t g_ctxt, g_forks;
static volatile uint32_t g_last_pid;
static uint64_t g_load[3];                 /* FSHIFT=11 fixed point */
#define FSHIFT   11
#define FIXED_1  (1u << FSHIFT)
#define LOAD_FREQ 5000                     /* ticks (1 kHz) = 5 s, as Linux */

static void ki_copy_name(char* d, const char* s, int cap) {
    int i = 0; for (; s && s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0;
}
static const char* ki_basename(const char* p) {
    const char* b = p; for (; p && *p; p++) if (*p == '/' && p[1]) b = p + 1;
    return b;
}

void kiface_proc_set_cmdline(process_t* p, int argc, const char* const* argv, const char* path) {
    if (!p) return;
    int o = 0;
    for (int i = 0; argv && i < argc && argv[i]; i++) {
        for (int k = 0; argv[i][k] && o < (int)sizeof(p->ki.cmdline) - 1; k++) p->ki.cmdline[o++] = argv[i][k];
        if (o < (int)sizeof(p->ki.cmdline)) p->ki.cmdline[o++] = 0;
    }
    if (o == 0 && path) {                       /* no argv: the path is argv[0] */
        for (int k = 0; path[k] && o < (int)sizeof(p->ki.cmdline) - 1; k++) p->ki.cmdline[o++] = path[k];
        p->ki.cmdline[o++] = 0;
    }
    p->ki.cmdline_len = (uint16_t)o;
    const char* name = (argv && argc > 0 && argv[0]) ? argv[0] : path;
    if (name) ki_copy_name(p->ki.comm, ki_basename(name), sizeof(p->ki.comm));
}

void kiface_proc_init(process_t* p, int argc, const char* const* argv) {
    if (!p) return;
    uint8_t* z = (uint8_t*)&p->ki;
    for (unsigned i = 0; i < sizeof(p->ki); i++) z[i] = 0;
    p->tg->umask = 022;
    p->ki.start_ns = kclock_mono_ns();
    ki_copy_name(p->ki.comm, "init", sizeof(p->ki.comm));
    kiface_proc_set_cmdline(p, argc, argv, 0);
    __atomic_add_fetch(&g_forks, 1, __ATOMIC_RELAXED);
    g_last_pid = p->pid;
}

void kiface_proc_fork(process_t* c) {
    if (!c) return;                             /* comm/cmdline/umask inherited */
    c->ki.start_ns = kclock_mono_ns();
    c->ki.utime_ticks = c->ki.stime_ticks = c->ki.nvcsw = 0;
    c->ki.nsleep_deadline = 0; c->ki.poll_deadline = 0;
    c->ki.poll_active = 0; c->ki.poll_wait = 0;
    __atomic_add_fetch(&g_forks, 1, __ATOMIC_RELAXED);
    g_last_pid = c->pid;
}

struct load_ctx { uint64_t n; };
static void load_cb(process_t* p, void* u) {
    if (p->pid == 0) return;                    /* idle tasks */
    if (p->state == PROC_READY || p->state == PROC_RUNNING || p->state == PROC_NEW)
        ((struct load_ctx*)u)->n++;
}

void kiface_account_tick(process_t* cur, int from_user, int is_idle) {
    uint32_t cpu = this_cpu()->index;
    if (cpu >= SMP_MAX_CPUS) cpu = 0;
    if (!cur || is_idle) g_cpu_idle[cpu]++;
    else if (from_user) { g_cpu_user[cpu]++; cur->ki.utime_ticks++; }
    else { g_cpu_sys[cpu]++; cur->ki.stime_ticks++; }
    if (cpu == 0 && (timer_get_ticks() % LOAD_FREQ) == 0) {
        static const uint64_t exp[3] = { 1884, 2014, 2037 };   /* 1/5/15 min */
        struct load_ctx c = { 0 };
        process_foreach(load_cb, &c);
        uint64_t active = c.n * FIXED_1;
        for (int i = 0; i < 3; i++)
            g_load[i] = (g_load[i] * exp[i] + active * (FIXED_1 - exp[i])) >> FSHIFT;
    }
}
void kiface_count_switch(void) { __atomic_add_fetch(&g_ctxt, 1, __ATOMIC_RELAXED); }

uint32_t kiface_cpu_count(void) { uint32_t n = smp_cpu_count(); return n ? (n > SMP_MAX_CPUS ? SMP_MAX_CPUS : n) : 1; }
void kiface_cpu_stat(uint32_t cpu, kiface_cpu_stat_t* o) {
    if (cpu >= SMP_MAX_CPUS) { o->user = o->sys = o->idle = 0; return; }
    o->user = g_cpu_user[cpu]; o->sys = g_cpu_sys[cpu]; o->idle = g_cpu_idle[cpu];
}
uint64_t kiface_ctxt(void) { return g_ctxt; }
uint64_t kiface_forks(void) { return g_forks; }
uint32_t kiface_last_pid(void) { return g_last_pid; }
void kiface_loadavg(uint64_t out[3]) { out[0] = g_load[0]; out[1] = g_load[1]; out[2] = g_load[2]; }

/* -------------------------------------------------------------- helpers */
/* Copy a user path in and make it absolute against the caller's cwd. */
static int ki_user_path(process_t* c, uint64_t up, char* abs, int cap) {
    char k[256];
    long n = strncpy_from_user(k, (const char*)up, sizeof(k));
    if (n < 0) return (int)n;
    if (n == 0) return -E_NOENT;
    proc_resolve_path(c, k, abs, cap);
    return 0;
}

/* Rename/unlink/truncate change which file a path names or what it holds:
 * drop the page cache of the inode the path resolves to (filesystems key their
 * inode slots by path, so the next open of this name must re-read). */
static void ki_pagecache_drop_path(const char* abs) {
    vfs_inode_t* ino = vfs_lookup(abs);
    if (ino) pagecache_invalidate(ino, 0, 1ULL << 62);
}

static int ki_fd_file(process_t* c, int fd, vfs_inode_t** out) {
    if (fd < 0 || fd >= 32 || !c->fds[fd].used) return -E_BADF;
    if (c->fds[fd].is_pipe || !c->fds[fd].inode) return -E_INVAL;   /* pipe / console */
    *out = (vfs_inode_t*)c->fds[fd].inode;
    return 0;
}

/* Mirrors syscall_trap.c: a pending default stop stops the caller here with
 * the syscall rewound (it restarts after SIGCONT); else report pending. */
static int ki_sig_interrupted(process_t* cur, trapframe_t* tf) {
    int ss = signal_take_stop(cur);
    if (ss) {
        cur->stop_sig = ss;
        tf->rip -= 2;
        sched_stop_current(tf);
    }
    return signal_pending(cur);
}

static void ki_block(trapframe_t* tf) {
    tf->rip -= 2;                               /* re-run `int 0x80` on wake */
    sched_block_current(tf);                    /* NORETURN for a user task */
    tf->rip += 2;                               /* (idle/kernel caller: no block) */
}

void kiface_apply_create_mode(const char* abs, uint32_t mode, int is_dir) {
    process_t* c = sched_get_current();
    uint32_t um = c ? c->tg->umask : 022;
    uint32_t want = ((mode ? mode : (is_dir ? 0777u : 0666u)) & 07777u) & ~um;
    uint32_t dflt = is_dir ? 0755u : 0644u;     /* what the filesystems create */
    if (want == dflt) return;
    vfs_attr_t a; a.mode = want; a.uid = a.gid = 0; a.atime = a.mtime = 0;
    vfs_setattr(abs, &a, VFS_ATTR_MODE);
}

/* ---------------------------------------------------------------- clocks */
struct k_timespec { int64_t tv_sec; int64_t tv_nsec; };

static uint64_t ki_clock_ns(process_t* c, int clk, int* ok) {
    *ok = 1;
    switch (clk) {
    case KCLOCK_REALTIME: case KCLOCK_REALTIME_COARSE: return kclock_real_ns();
    case KCLOCK_MONOTONIC: case KCLOCK_MONOTONIC_RAW:
    case KCLOCK_MONOTONIC_COARSE: case KCLOCK_BOOTTIME: return kclock_mono_ns();
    case KCLOCK_PROCESS_CPUTIME_ID: case KCLOCK_THREAD_CPUTIME_ID:
        return c ? (c->ki.utime_ticks + c->ki.stime_ticks) * 1000000ull : 0;
    }
    *ok = 0;
    return 0;
}

static uint64_t sys_clock_gettime(process_t* c, int clk, uint64_t uts) {
    int ok; uint64_t ns = ki_clock_ns(c, clk, &ok);
    if (!ok) return NEG(E_INVAL);
    if (!uts) return 0;
    struct k_timespec ts = { (int64_t)(ns / 1000000000ull), (int64_t)(ns % 1000000000ull) };
    return copy_to_user((void*)uts, &ts, sizeof(ts)) ? NEG(E_FAULT) : 0;
}

static uint64_t sys_clock_settime(process_t* c, uint64_t num, int clk, uint64_t uts) {
    if (!cap_check(c, CAP_TIME, num, "clock_settime")) return NEG(E_PERM);
    if (!cred_is_root(c)) return NEG(E_PERM);                 /* [M43] setting the clock: root */
    struct k_timespec ts;
    if (copy_from_user(&ts, (const void*)uts, sizeof(ts))) return NEG(E_FAULT);
    if (clk != KCLOCK_REALTIME) return NEG(E_INVAL);
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return NEG(E_INVAL);
    kclock_set_real_ns((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
    return 0;
}

/* nanosleep(req, rem): block on the tick for the whole milliseconds, then
 * finish the sub-millisecond rest on the TSC (spinning <= 1 tick). Interrupted
 * by a signal -> -EINTR with the remaining time in *rem. */
static uint64_t sys_nanosleep(process_t* c, trapframe_t* tf, uint64_t ureq, uint64_t urem) {
    if (!c) return NEG(E_INVAL);
    if (c->ki.nsleep_deadline == 0) {
        struct k_timespec rq;
        if (copy_from_user(&rq, (const void*)ureq, sizeof(rq))) return NEG(E_FAULT);
        if (rq.tv_sec < 0 || rq.tv_nsec < 0 || rq.tv_nsec >= 1000000000) return NEG(E_INVAL);
        uint64_t ns = (uint64_t)rq.tv_sec * 1000000000ull + (uint64_t)rq.tv_nsec;
        if ((uint64_t)rq.tv_sec > (1ull << 33)) ns = 1ull << 62;      /* clamp */
        if (ns == 0) return 0;
        c->ki.nsleep_deadline = kclock_mono_ns() + ns;
    }
    uint64_t dl = c->ki.nsleep_deadline;
    for (;;) {
        uint64_t now = kclock_mono_ns();
        if (now >= dl) { c->ki.nsleep_deadline = 0; c->sleep_until = 0; return 0; }
        uint64_t left = dl - now;
        if (ki_sig_interrupted(c, tf)) {
            c->ki.nsleep_deadline = 0; c->sleep_until = 0;
            if (urem) {
                struct k_timespec rm = { (int64_t)(left / 1000000000ull), (int64_t)(left % 1000000000ull) };
                if (copy_to_user((void*)urem, &rm, sizeof(rm))) return NEG(E_FAULT);
            }
            return NEG(E_INTR);
        }
        if (left < 1000000ull && tsc_hz()) {          /* sub-tick rest: spin on the TSC */
            while (kclock_mono_ns() < dl) __asm__ volatile ("pause");
            continue;
        }
        uint64_t ticks = left / 1000000ull;            /* wake a little early, finish on TSC */
        if (!tsc_hz() && ticks == 0) ticks = 1;
        c->sleep_until = timer_get_ticks() + (ticks ? ticks : 1);
        ki_block(tf);
        return NEG(E_INTR);                            /* not reached for a user task */
    }
}

/* ---------------------------------------------------------------- syslog */
static uint64_t ki_syslog_copy(uint64_t start, uint64_t end, uint64_t ubuf, uint64_t len) {
    char tmp[512];
    uint64_t done = 0;
    while (start + done < end && done < len) {
        size_t want = (size_t)((end - start - done) < sizeof(tmp) ? (end - start - done) : sizeof(tmp));
        if (want > len - done) want = (size_t)(len - done);
        size_t n = klog_copy(start + done, tmp, want);
        if (n == 0) break;
        if (copy_to_user((uint8_t*)ubuf + done, tmp, n)) return NEG(E_FAULT);
        done += n;
    }
    return done;
}

static uint64_t sys_syslog(process_t* c, uint64_t num, int type, uint64_t ubuf, int64_t len) {
    static int console_level = 7;
    int writes = (type == KLOG_ACTION_READ_CLEAR || type == KLOG_ACTION_CLEAR ||
                  type == KLOG_ACTION_CONSOLE_OFF || type == KLOG_ACTION_CONSOLE_ON ||
                  type == KLOG_ACTION_CONSOLE_LEVEL);
    if (!cap_check(c, writes ? CAP_FS_WRITE : CAP_FS_READ, num, "syslog")) return NEG(E_PERM);
    /* [M43] The kernel log is root's (dmesg_restrict=1): others may only ask
     * for the buffer sizes. */
    if (!cred_is_root(c) && type != KLOG_ACTION_CLOSE && type != KLOG_ACTION_OPEN &&
        type != 9 /*SIZE_UNREAD*/ && type != 10 /*SIZE_BUFFER*/) return NEG(E_PERM);
    switch (type) {
    case KLOG_ACTION_CLOSE: case KLOG_ACTION_OPEN: return 0;
    case KLOG_ACTION_READ: case KLOG_ACTION_READ_ALL: case KLOG_ACTION_READ_CLEAR: {
        if (len < 0 || (len && !ubuf)) return NEG(E_INVAL);
        if (len == 0) return 0;
        if (len > (1 << 20)) len = 1 << 20;
        if (!user_range_valid((void*)ubuf, (size_t)len)) return NEG(E_FAULT);
        uint64_t s, e;
        if (type == KLOG_ACTION_READ) {                /* destructive, from the cursor */
            klog_window(0, &s, &e);
            uint64_t rd = klog_read_pos();
            if (rd > s) s = rd;
            uint64_t r = ki_syslog_copy(s, e, ubuf, (uint64_t)len);
            if ((int64_t)r > 0) klog_set_read_pos(s + r);
            return r;
        }
        klog_window(1, &s, &e);
        if (e - s > (uint64_t)len) {                   /* the newest len bytes, whole records */
            uint64_t want = e - (uint64_t)len;
            char ch;
            while (want < e && klog_copy(want - 1, &ch, 1) == 1 && ch != '\n') want++;
            s = want;
        }
        uint64_t r = ki_syslog_copy(s, e, ubuf, (uint64_t)len);
        if (type == KLOG_ACTION_READ_CLEAR && (int64_t)r >= 0) klog_clear();
        return r;
    }
    case KLOG_ACTION_CLEAR: klog_clear(); return 0;
    case KLOG_ACTION_CONSOLE_OFF: console_level = 1; return 0;
    case KLOG_ACTION_CONSOLE_ON: console_level = 7; return 0;
    case KLOG_ACTION_CONSOLE_LEVEL:
        if (len < 1 || len > 8) return NEG(E_INVAL);
        console_level = (int)len; return 0;
    case KLOG_ACTION_SIZE_UNREAD: {
        uint64_t s, e; klog_window(0, &s, &e);
        uint64_t rd = klog_read_pos(); if (rd > s) s = rd;
        return e > s ? e - s : 0;
    }
    case KLOG_ACTION_SIZE_BUFFER: return KLOG_BUF_SIZE;
    }
    (void)console_level;
    return NEG(E_INVAL);
}

/* ---------------------------------------------------------------- statfs */
struct k_statfs {                 /* Linux x86-64 struct statfs (120 bytes) */
    int64_t  f_type, f_bsize;
    uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t  f_fsid[2];
    int64_t  f_namelen, f_frsize, f_flags, f_spare[4];
};

static uint64_t sys_statfs(process_t* c, uint64_t num, uint64_t upath, uint64_t ubuf, int fd) {
    if (!cap_check(c, CAP_FS_READ, num, "statfs")) return NEG(E_PERM);
    vfs_statfs_t v; int r;
    const void* key = 0;
    if (upath) {
        char ab0[256]; if ((r = ki_user_path(c, upath, ab0, sizeof(ab0)))) return NEG(-r);
        char abs[256];                                    /* [M43] search permission on the way */
        if ((r = cred_check_path(c, ab0, 1, 0, abs, sizeof(abs), 0))) return NEG(-r);
        r = vfs_statfs(abs, &v);
        vfs_inode_t* ino = vfs_lookup_follow(abs); key = ino ? ino->ops : 0;
    } else {
        if (fd < 0 || fd >= 32 || !c->fds[fd].used) return NEG(E_BADF);
        if (c->fds[fd].is_pipe || !c->fds[fd].inode) {        /* pipefs / devpts */
            for (unsigned i = 0; i < sizeof(v); i++) ((uint8_t*)&v)[i] = 0;
            v.type = c->fds[fd].is_pipe ? 0x50495045 : 0x1cd1;
            v.bsize = v.frsize = 4096; v.namelen = 255; r = 0;
        } else {
            vfs_inode_t* ino = (vfs_inode_t*)c->fds[fd].inode;
            key = ino->ops;
            r = vfs_statfs_ops(ino->ops, &v);
        }
    }
    if (r < 0) return NEG(-r);
    struct k_statfs ks;
    for (unsigned i = 0; i < sizeof(ks); i++) ((uint8_t*)&ks)[i] = 0;
    ks.f_type = (int64_t)v.type; ks.f_bsize = (int64_t)v.bsize;
    ks.f_blocks = v.blocks; ks.f_bfree = v.bfree; ks.f_bavail = v.bavail;
    ks.f_files = v.files; ks.f_ffree = v.ffree;
    uint64_t k = (uint64_t)(uintptr_t)key;
    ks.f_fsid[0] = (int32_t)(k >> 4); ks.f_fsid[1] = (int32_t)(k >> 36);
    ks.f_namelen = (int64_t)v.namelen; ks.f_frsize = (int64_t)v.frsize;
    ks.f_flags = 0x20 | (int64_t)v.flags;                      /* ST_VALID */
    return copy_to_user((void*)ubuf, &ks, sizeof(ks)) ? NEG(E_FAULT) : 0;
}

/* ---------------------------------------------------------------- uname / sysinfo */
struct k_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };

static void ut_set(char* d, const char* a, const char* b, const char* c2, const char* e) {
    int o = 0; const char* parts[4] = { a, b, c2, e };
    for (int p = 0; p < 4; p++) for (int i = 0; parts[p] && parts[p][i] && o < 64; i++) d[o++] = parts[p][i];
    d[o] = 0;
}
static uint64_t sys_uname(uint64_t ubuf) {
    struct k_utsname u;
    for (unsigned i = 0; i < sizeof(u); i++) ((char*)&u)[i] = 0;
    ut_set(u.sysname, "SecOS", 0, 0, 0);
    ut_set(u.nodename, "secos", 0, 0, 0);
    ut_set(u.release, "0.1.0-dev", 0, 0, 0);
    ut_set(u.version, "#1 SMP ", BUILD_TS, " git ", GIT_HASH);
    ut_set(u.machine, "x86_64", 0, 0, 0);
    ut_set(u.domainname, "(none)", 0, 0, 0);
    return copy_to_user((void*)ubuf, &u, sizeof(u)) ? NEG(E_FAULT) : 0;
}

struct k_sysinfo {                /* Linux x86-64 struct sysinfo (112 bytes) */
    int64_t  uptime;
    uint64_t loads[3];
    uint64_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    uint16_t procs, pad;
    uint32_t pad2;
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit;
    uint32_t pad3;
};
struct cnt_ctx { uint32_t n; };
static void cnt_cb(process_t* p, void* u) { if (p->pid) ((struct cnt_ctx*)u)->n++; }
static uint64_t sys_sysinfo(uint64_t ubuf) {
    struct k_sysinfo si;
    for (unsigned i = 0; i < sizeof(si); i++) ((uint8_t*)&si)[i] = 0;
    si.uptime = (int64_t)(kclock_mono_ns() / 1000000000ull);
    uint64_t l[3]; kiface_loadavg(l);
    for (int i = 0; i < 3; i++) si.loads[i] = l[i] << (16 - FSHIFT);   /* SI_LOAD_SHIFT 16 */
    si.totalram = pmm_get_total_memory();
    si.freeram = pmm_get_free_memory();
    struct cnt_ctx c = { 0 }; process_foreach(cnt_cb, &c);
    si.procs = (uint16_t)c.n;
    si.mem_unit = 1;
    return copy_to_user((void*)ubuf, &si, sizeof(si)) ? NEG(E_FAULT) : 0;
}

/* ---------------------------------------------------------------- poll */
#define POLLIN     0x001
#define POLLPRI    0x002
#define POLLOUT    0x004
#define POLLERR    0x008
#define POLLHUP    0x010
#define POLLNVAL   0x020
#define POLLRDNORM 0x040
#define POLLWRNORM 0x100
#define POLL_MAX   64
struct k_pollfd { int32_t fd; int16_t events, revents; };

/* Readiness of one fd. The console (fds 0-2 without a redirect, /dev/tty and
 * /dev/console) and pty ends ask the terminal layer (kernel/pty.c), which looks
 * at the line discipline's readable bytes (canonical: a whole line; raw: VMIN)
 * and the pty buffers/hangup state. *watch gets PW_CONSOLE / PW_PTY so the
 * caller knows which wake-up sources to arm before it blocks. */
#define PW_CONSOLE 1
#define PW_PTY     2
#define PW_INPUT   4    /* [M40] evdev: re-poll every 2 ms (USB HID is polled) */
static int ki_fd_revents(process_t* c, int fd, int events, int* watch) {
    if (fd < 0 || fd >= 32) return POLLNVAL;
    struct proc_fd_entry* e = &c->fds[fd];
    if (e->used && e->is_sock) return socket_poll(c->pid, e->is_sock - 1);
    if (e->used && e->is_pipe) return pipe_poll((pipe_t*)e->inode, e->pipe_w);
    if (e->used && e->inode && udrv_is_fd(e->inode))          /* [M57] claim fd: its IRQ wakes the poller */
        return udrv_fd_poll(e->inode);
    if (e->used && e->inode && anonfd_is(e->inode))           /* [U10] eventfd/timerfd/epoll/AF_UNIX */
        return anonfd_poll(e->inode);
    if (e->used && e->inode && input_is_client(e->inode)) {   /* [M40] evdev */
        *watch |= PW_INPUT;
        return input_poll(e->inode);
    }
    if (e->used && e->inode && pty_is_inode(e->inode)) {
        *watch |= PW_PTY;
        int r = pty_poll(e->inode, events | POLLIN | POLLOUT);
        return r < 0 ? POLLNVAL : r;
    }
    if ((e->used && e->inode && devfs_is_console(e->inode)) ||
        (e->used && !e->inode && !e->is_sock) || (!e->used && fd <= 2)) {          /* console TTY */
        *watch |= PW_CONSOLE;
        return tty_console_poll(events | POLLIN | POLLOUT);
    }
    if (e->used && e->inode) return POLLIN | POLLOUT;              /* regular file / device */
    return POLLNVAL;
}

/* [U10] The same readiness for epoll (kernel/lx_event.c), plus the wake-up
 * sources a blocked epoll_wait arms and clears like poll() does. */
int kiface_fd_revents(process_t* c, int fd, int events, int* watch) {
    return ki_fd_revents(c, fd, events, watch);
}
void kiface_arm_watch(process_t* c, int watch) {
    c->ki.poll_wait = 1;                          /* any pipe/anonfd activity */
    if (watch & PW_PTY) c->wait_pty = PTY_WAIT_ANY;
    if (watch & PW_CONSOLE) c->wait_tty = 1;
}
void kiface_clear_watch(process_t* c) {
    c->ki.poll_wait = 0;
    if (c->wait_pty == PTY_WAIT_ANY) c->wait_pty = NULL;
    c->wait_tty = 0;
}

static uint64_t sys_poll(process_t* c, trapframe_t* tf, uint64_t ufds, uint64_t nfds, int64_t timeout_ms) {
    if (!c) return NEG(E_INVAL);
    /* A wake from a previous block may have left these armed. */
    if (c->wait_pty == PTY_WAIT_ANY) c->wait_pty = NULL;
    c->wait_tty = 0;
    if (nfds > POLL_MAX) { c->ki.poll_active = 0; return NEG(E_INVAL); }
    struct k_pollfd pf[POLL_MAX];
    if (nfds && copy_from_user(pf, (const void*)ufds, nfds * sizeof(pf[0]))) {
        c->ki.poll_active = 0; c->ki.poll_wait = 0; return NEG(E_FAULT);
    }
    uint64_t now = kclock_mono_ns();
    if (!c->ki.poll_active) {
        c->ki.poll_active = 1;
        if (timeout_ms < 0) c->ki.poll_deadline = ~0ull;
        else c->ki.poll_deadline = now + (uint64_t)timeout_ms * 1000000ull;
    }
    int ready = 0, watch = 0;
    /* [U9] Another thread of the process may close a polled fd and drop the
     * last reference of its object meanwhile: the scan (no user copy, no
     * block) runs under the group's fd_lock, like close() does. */
    int mt = c->tg && __atomic_load_n(&c->tg->nr_live, __ATOMIC_RELAXED) > 1;
    if (mt) klock_lock(&c->tg->fd_lock);
    for (uint64_t i = 0; i < nfds; i++) {
        pf[i].revents = 0;
        if (pf[i].fd < 0) continue;
        int r = ki_fd_revents(c, pf[i].fd, pf[i].events, &watch);
        int want = pf[i].events | POLLERR | POLLHUP | POLLNVAL;
        if (pf[i].events & POLLRDNORM) want |= POLLIN;
        if (pf[i].events & POLLWRNORM) want |= POLLOUT;
        int got = r & want;
        if ((got & POLLIN) && (pf[i].events & POLLRDNORM)) got |= POLLRDNORM;
        if ((got & POLLOUT) && (pf[i].events & POLLWRNORM)) got |= POLLWRNORM;
        if (!(pf[i].events & POLLIN)) got &= ~POLLIN;
        if (!(pf[i].events & POLLOUT)) got &= ~POLLOUT;
        pf[i].revents = (int16_t)got;
        if (got) ready++;
    }
    if (mt) klock_unlock(&c->tg->fd_lock);
    if (ready || now >= c->ki.poll_deadline) {
        c->ki.poll_active = 0; c->ki.poll_wait = 0; c->sleep_until = 0;
        if (nfds && copy_to_user((void*)ufds, pf, nfds * sizeof(pf[0]))) return NEG(E_FAULT);
        return (uint64_t)ready;
    }
    if (ki_sig_interrupted(c, tf)) {
        c->ki.poll_active = 0; c->ki.poll_wait = 0; c->sleep_until = 0;
        return NEG(E_INTR);
    }
    /* Sleep until the deadline, a pipe event, or at most 10 ms (to re-check
     * the console and sockets, whose input arrives from interrupts). */
    uint64_t t = timer_get_ticks();
    uint64_t wake = t + ((watch & PW_INPUT) ? 2 : 10);
    if (c->ki.poll_deadline != ~0ull) {
        uint64_t left_ms = (c->ki.poll_deadline - now + 999999ull) / 1000000ull;
        if (t + left_ms < wake) wake = t + (left_ms ? left_ms : 1);
    }
    c->sleep_until = wake;
    c->ki.poll_wait = 1;
    /* Terminal input wakes the poller at once instead of on the 10 ms re-check:
     * any pty state change (sched_wake_pty), console input (tty_wake_readers). */
    if (watch & PW_PTY) c->wait_pty = PTY_WAIT_ANY;
    if (watch & PW_CONSOLE) c->wait_tty = 1;
    ki_block(tf);
    c->ki.poll_active = 0; c->ki.poll_wait = 0; c->sleep_until = 0;
    if (c->wait_pty == PTY_WAIT_ANY) c->wait_pty = NULL;
    c->wait_tty = 0;
    return 0;                                     /* only for a non-blockable caller */
}

/* [M43] rename(old, new) / link(old, new) permission rules (kernel/cred.c).
 * On success p0/p1 hold the canonical paths. rename: the source may be
 * removed from its directory (w+x, sticky), the target directory accepts a
 * new entry (w+x) and an existing target may be replaced (w+x, sticky); a
 * directory moved to another parent needs w on itself (its ".."). link: the
 * new name may be created, and a non-owner may only link a regular, non-set-id
 * file it can read and write (Linux fs.protected_hardlinks=1). */
static int ki_perm_rename_link(process_t* c, int is_rename, char* p0, char* p1) {
    char c0[256], c1[256]; int e;
    if ((e = cred_resolve(c, p0, 0, c0, sizeof(c0)))) return e;
    if ((e = cred_resolve(c, p1, 0, c1, sizeof(c1)))) return e;
    vfs_inode_t* src = vfs_lookup(c0);
    if (!src) return -E_NOENT;
    int src_dir = src->type == VFS_NODE_DIR;
    uint32_t smode = cred_inode_mode(src), suid = src->uid;
    int src_rw = cred_inode_perm(c, src, MAY_READ | MAY_WRITE) == 0;
    int src_w  = cred_inode_perm(c, src, MAY_WRITE) == 0;
    if (is_rename) {
        if ((e = cred_may_delete(c, c0))) return e;
        if (vfs_lookup(c1)) { if ((e = cred_may_delete(c, c1))) return e; }
        else if ((e = cred_may_create(c, c1))) return e;
        if (src_dir && !cred_is_root(c)) {
            size_t l0 = 0, l1 = 0;
            for (size_t i = 0; c0[i]; i++) if (c0[i] == '/') l0 = i;
            for (size_t i = 0; c1[i]; i++) if (c1[i] == '/') l1 = i;
            int same = (l0 == l1);
            for (size_t i = 0; same && i < l0; i++) if (c0[i] != c1[i]) same = 0;
            if (!same && !src_w) return -E_ACCES;
        }
    } else {
        if (vfs_lookup(c1)) return -17;                   /* EEXIST */
        if ((e = cred_may_create(c, c1))) return e;
        if (!cred_is_root(c) && suid != c->cred.euid) {
            if (src_dir || (smode & 0170000u) != 0100000u) return -E_PERM;
            if ((smode & 04000u) || ((smode & 02010u) == 02010u)) return -E_PERM;
            if (!src_rw) return -E_PERM;
        }
    }
    for (int i = 0; i < 256; i++) { p0[i] = c0[i]; if (!c0[i]) break; }
    for (int i = 0; i < 256; i++) { p1[i] = c1[i]; if (!c1[i]) break; }
    return 0;
}

/* ---------------------------------------------------------------- dispatcher */
int kiface_syscall(trapframe_t* tf, uint64_t num, uint64_t* ret) {
    if (num < 65 || num > 79) return 0;
    process_t* c = sched_get_current();
    uint64_t a0 = tf->rdi, a1 = tf->rsi, a2 = tf->rdx;
    uint64_t r;
    char p0[256], p1[256];
    int e;
    if (!c) { *ret = NEG(E_INVAL); return 1; }
    switch (num) {
    case 65: r = sys_clock_gettime(c, (int)a0, a1); break;              /* SYS_CLOCK_GETTIME */
    case 66: r = sys_clock_settime(c, num, (int)a0, a1); break;         /* SYS_CLOCK_SETTIME */
    case 67: r = sys_nanosleep(c, tf, a0, a1); break;                   /* SYS_NANOSLEEP */
    case 68: r = sys_syslog(c, num, (int)a0, a1, (int64_t)a2); break;   /* SYS_SYSLOG */
    case 69: r = sys_statfs(c, num, a0, a1, (int)a2); break;            /* SYS_STATFS */
    case 70: {                                                          /* SYS_SYNC(fd|-1) */
        int fd = (int)a0;
        if (fd < 0) { vfs_sync_all(); r = 0; break; }
        vfs_inode_t* ino; if ((e = ki_fd_file(c, fd, &ino))) { r = NEG(-e); break; }
        r = vfs_sync_ops(ino->ops) == 0 ? 0 : NEG(5);
        break;
    }
    case 71: case 72: {                                                 /* SYS_RENAME / SYS_LINK */
        if (!cap_check(c, CAP_FS_WRITE, num, num == 71 ? "rename" : "link")) { r = NEG(E_PERM); break; }
        if ((e = ki_user_path(c, a0, p0, sizeof(p0))) || (e = ki_user_path(c, a1, p1, sizeof(p1)))) { r = NEG(-e); break; }
        if ((e = ki_perm_rename_link(c, num == 71, p0, p1))) { r = NEG(-e); break; }   /* [M43] */
        if (num == 71) { ki_pagecache_drop_path(p0); ki_pagecache_drop_path(p1); e = vfs_rename(p0, p1); }
        else e = vfs_link(p0, p1);
        r = e < 0 ? NEG(-e) : 0;
        if (num == 71 && e == 0) ki_pagecache_drop_path(p1);
        break;
    }
    case 73: {                                                          /* SYS_TRUNCATE(path|0, len, fd) */
        if (!cap_check(c, CAP_FS_WRITE, num, "truncate")) { r = NEG(E_PERM); break; }
        int64_t len = (int64_t)a1;
        if (len < 0 || len > 0xFFFFFFFFll) { r = NEG(E_INVAL); break; }
        if (a0) {
            if ((e = ki_user_path(c, a0, p0, sizeof(p0)))) { r = NEG(-e); break; }
            /* [M43] write permission on the file. [M52] Then truncate the
             * inode that passed the check, pinned (vfs_ftruncate): a second
             * lookup by name could land on whatever a concurrent rename put
             * there (a symlink to a file the caller may not write). */
            char cp[256]; vfs_inode_t* ci = 0;
            if ((e = cred_check_path(c, p0, 1, MAY_WRITE, cp, sizeof(cp), &ci))) { r = NEG(-e); break; }
            vfs_pin_t pin = VFS_PIN_INIT;
            vfs_inode_t* ino = vfs_lookup_pin(cp, &pin);
            if (!ino) e = -2;                                         /* -ENOENT: gone meanwhile */
            else if (ino != ci && (e = cred_inode_perm(c, ino, MAY_WRITE))) { }
            else if (ino->type == VFS_NODE_DIR) e = -21;              /* -EISDIR */
            else if (!ino->ops || !ino->ops->truncate) e = -22;
            else {
                e = vfs_ftruncate(ino, (size_t)len);
                if (e == 0) pagecache_invalidate(ino, 0, 1ULL << 62);
            }
            vfs_unpin(&pin);
        } else {
            vfs_inode_t* ino;
            if ((e = ki_fd_file(c, (int)a2, &ino))) { r = NEG(-e); break; }
            if (!(c->fds[(int)a2].flags & 3)) { r = NEG(E_INVAL); break; }   /* not open for writing */
            if (ino->type != VFS_NODE_FILE || !ino->ops || !ino->ops->truncate) { r = NEG(E_INVAL); break; }
            e = vfs_ftruncate(ino, (size_t)len);          /* [M51 C1] vs O_APPEND */
            pagecache_invalidate(ino, 0, 1ULL << 62);
        }
        r = e < 0 ? (e == -1 ? NEG(5) : NEG(-e)) : 0;
        break;
    }
    case 74:                                                            /* SYS_RMDIR */
        if (!cap_check(c, CAP_FS_WRITE, num, "rmdir")) { r = NEG(E_PERM); break; }
        if ((e = ki_user_path(c, a0, p0, sizeof(p0)))) { r = NEG(-e); break; }
        { char cp[256];                                   /* [M43] w+x on the parent, sticky */
          if ((e = cred_resolve(c, p0, 0, cp, sizeof(cp)))) { r = NEG(-e); break; }
          if (!vfs_lookup(cp)) { r = NEG(E_NOENT); break; }
          if ((e = cred_may_delete(c, cp))) { r = NEG(-e); break; }
          for (int i = 0; i < 256; i++) { p0[i] = cp[i]; if (!cp[i]) break; } }
        e = vfs_rmdir(p0);
        r = e < 0 ? NEG(-e) : 0;
        break;
    case 75: {                                                          /* SYS_ACCESS(path, mode) */
        if (!cap_check(c, CAP_FS_READ, num, "access")) { r = NEG(E_PERM); break; }
        if ((e = ki_user_path(c, a0, p0, sizeof(p0)))) { r = NEG(-e); break; }
        int mode = (int)a1;
        if (mode & ~7) { r = NEG(E_INVAL); break; }
        /* [M43] access() answers for the REAL uid/gid (a set-id program asks
         * "may my invoker do this?"): run the walk and the check with the
         * effective ids temporarily set to the real ones (no preemption in
         * a syscall, and the process runs on one CPU). */
        /* [M50-B1] Each swap is published under proc_lock (process_set_cred);
         * a cross-process reader may observe the real ids for the duration of
         * the check, as with Linux's access() override credentials. */
        struct proc_cred saved = c->cred, tmp = c->cred;
        tmp.euid = tmp.ruid; tmp.egid = tmp.rgid;
        process_set_cred_task(c, &tmp);          /* [U9] not the sibling threads */
        vfs_inode_t* ino = 0;
        e = cred_check_path(c, p0, 1, mode, 0, 0, &ino);
        process_set_cred_task(c, &saved);
        r = e ? NEG(-e) : 0;
        break;
    }
    case 76: {                                                          /* SYS_UMASK */
        uint32_t old = c->tg->umask;
        c->tg->umask = (uint32_t)a0 & 0777;
        r = old;
        break;
    }
    case 77: r = sys_uname(a0); break;                                  /* SYS_UNAME */
    case 78: r = sys_poll(c, tf, a0, a1, (int64_t)(int32_t)a2); break;  /* SYS_POLL */
    case 79: r = sys_sysinfo(a0); break;                                /* SYS_SYSINFO */
    default: r = NEG(38);                                               /* ENOSYS */
    }
    *ret = r;
    return 1;
}
