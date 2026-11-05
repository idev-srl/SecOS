/* kitest — ring-3 test of the kernel interfaces for standard userland tools:
 * per-pid procfs + /proc/{stat,loadavg,meminfo,mounts,...}, the kernel log
 * (klogctl, /dev/kmsg), clocks + nanosleep + strftime, uname/sysinfo/umask/
 * access, statfs, rename/link/truncate/rmdir, poll/select and symlinks in the
 * middle of a path. SPDX-License-Identifier: MIT
 *
 *   kitest                 everything that needs no disk (root = RAMFS)
 *   kitest disk <dir>      filesystem cases on a mounted disk (<dir> = /mnt)
 *   kitest persist1 <dir>  stage an atomic rename-over-existing, then sync
 *   kitest persist2 <dir>  (next boot) verify it survived, report statfs
 *
 * Output: "[KI-TEST] <case> OK|FAIL ..." lines and a final
 * "[KI-TEST] DONE pass=<n> fail=<m>" (stdout is mirrored to debugcon).
 */
#include "libsecos.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/klog.h>
#include <signal.h>
#include <sys/stat.h>

static int g_pass, g_fail;
static char g_argv0[64];

static void say(const char* fmt, ...) {
    char b[512];
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    extern int vsnprintf(char*, unsigned long, const char*, __builtin_va_list);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    __builtin_va_end(ap);
    if (n > (int)sizeof b - 1) n = sizeof b - 1;
    if (n > 0) write(1, b, (size_t)n);
}
static void check(const char* name, int ok, long got) {
    if (ok) { say("[KI-TEST] %s OK\n", name); g_pass++; }
    else    { say("[KI-TEST] %s FAIL got=%ld errno=%d\n", name, got, errno); g_fail++; }
}
static int slurp(const char* path, char* buf, int cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = 0;
    for (;;) {
        long r = read(fd, buf + n, (size_t)(cap - 1 - n));
        if (r <= 0) break;
        n += (int)r;
        if (n >= cap - 1) break;
    }
    close(fd);
    buf[n] = 0;
    return n;
}
static int spit(const char* path, const char* s) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    long n = write(fd, s, strlen(s));
    close(fd);
    return n == (long)strlen(s) ? 0 : -1;
}
static long ms_now(int clk) { struct timespec t; clock_gettime(clk, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000L; }
static long us_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000L + t.tv_nsec / 1000L; }
static void msleep(long ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, 0); }
static int dents_has(const char* dir, const char* name) {
    static char buf[16384];
    long n = getdents(dir, buf, sizeof buf);
    for (long o = 0; o + 256 <= n; o += 256) if (strcmp(buf + o + 1, name) == 0) return 1;
    return 0;
}

/* ------------------------------------------------------------------ procfs */
static void t_procfs(void) {
    char b[4096], p[64], e[32];
    int pid = getpid();
    snprintf(e, sizeof e, "%d", pid);
    long n = readlink("/proc/self", b, sizeof b);
    if (n > 0) b[n] = 0;
    check("proc-self-symlink", n > 0 && strcmp(b, e) == 0, n);

    n = slurp("/proc/self/stat", b, sizeof b);
    int spid = -1, sppid = -1; char st = 0; char comm[32] = "";
    if (n > 0) {
        spid = atoi(b);
        char* lp = strchr(b, '('); char* rp = strrchr(b, ')');
        if (lp && rp && rp > lp) { int k = (int)(rp - lp - 1); if (k > 31) k = 31; memcpy(comm, lp + 1, (size_t)k); comm[k] = 0;
            st = rp[2]; sppid = atoi(rp + 4); }
    }
    check("proc-pid-stat-parses", spid == pid && st == 'R' && sppid == getppid() && strcmp(comm, "kitest") == 0, spid);
    { int fields = 0; for (char* q = strrchr(b, ')'); q && *q; q++) if (*q == ' ') fields++;
      check("proc-pid-stat-52-fields", fields >= 50, fields); }

    snprintf(p, sizeof p, "/proc/%d/status", pid);
    n = slurp(p, b, sizeof b);
    char want[48]; snprintf(want, sizeof want, "\nPid:\t%d\n", pid);
    check("proc-pid-status", n > 0 && strstr(b, "Name:\tkitest\n") && strstr(b, want) && strstr(b, "VmSize:"), n);

    n = slurp("/proc/self/cmdline", b, sizeof b);
    check("proc-pid-cmdline", n > 0 && strcmp(b, g_argv0) == 0, n);
    check("proc-root-lists-pid", dents_has("/proc", e) && dents_has("/proc", "self") && dents_has("/proc", "meminfo"), 0);

    int pf[2]; pipe(pf);
    snprintf(p, sizeof p, "/proc/self/fd/%d", pf[0]);
    n = readlink(p, b, sizeof b); if (n > 0) b[n] = 0;
    check("proc-fd-pipe-link", n > 0 && strncmp(b, "pipe:[", 6) == 0, n);
    check("proc-fd-dir-lists", dents_has("/proc/self/fd", "0") && dents_has("/proc/self/fd", "1"), 0);
    close(pf[0]); close(pf[1]);

    n = slurp("/proc/self/maps", b, sizeof b);
    check("proc-pid-maps-stack", n > 0 && strstr(b, "[stack]") && strstr(b, "r-xp"), n);
    chdir("/bin");
    n = readlink("/proc/self/cwd", b, sizeof b); if (n > 0) b[n] = 0;
    check("proc-pid-cwd-link", n > 0 && strcmp(b, "/bin") == 0, n);
    struct stat sb;
    check("midpath-symlink-via-proc-cwd", stat("/proc/self/cwd/kitest", &sb) == 0 && S_ISREG(sb.st_mode), 0);
    chdir("/");

    /* a child appears in /proc while alive and disappears once reaped */
    int c = fork();
    if (c == 0) { msleep(300); _exit(3); }
    snprintf(p, sizeof p, "/proc/%d", c);
    snprintf(e, sizeof e, "%d", c);
    check("proc-child-appears", stat(p, &sb) == 0 && S_ISDIR(sb.st_mode) && dents_has("/proc", e), c);
    msleep(60);                                    /* let the child reach its nanosleep */
    snprintf(p, sizeof p, "/proc/%d/stat", c);
    n = slurp(p, b, sizeof b);
    check("proc-child-state-sleeping", n > 0 && strstr(b, ") S "), n);
    int ws = waitpid(c);
    int gone = 0;
    snprintf(p, sizeof p, "/proc/%d", c);
    for (int i = 0; i < 40 && !gone; i++) { if (stat(p, &sb) != 0) gone = 1; else msleep(50); }
    check("proc-child-disappears", WIFEXITED(ws) && WEXITSTATUS(ws) == 3 && gone, ws);

    n = slurp("/proc/stat", b, sizeof b);
    check("proc-stat", n > 0 && strncmp(b, "cpu  ", 5) == 0 && strstr(b, "\ncpu0 ") && strstr(b, "\nctxt ") &&
          strstr(b, "\nprocesses ") && strstr(b, "\nprocs_running ") && strstr(b, "\nbtime "), n);
    n = slurp("/proc/loadavg", b, sizeof b);
    { int ok = n > 0; char* q = b;
      for (int i = 0; ok && i < 3; i++) { if (!(q[0] >= '0' && q[0] <= '9')) ok = 0; q = strchr(q, ' '); if (!q) ok = 0; else q++; }
      check("proc-loadavg", ok && strchr(b, '/'), n); }
    n = slurp("/proc/meminfo", b, sizeof b);
    check("proc-meminfo", n > 0 && strncmp(b, "MemTotal:", 9) == 0 && strstr(b, "\nMemFree:") &&
          strstr(b, "\nMemAvailable:") && strstr(b, "\nBuffers:") && strstr(b, "\nCached:") && strstr(b, " kB\n"), n);
    n = slurp("/proc/uptime", b, sizeof b);
    check("proc-uptime", n > 0 && strchr(b, '.') && strchr(b, ' ') && b[n - 1] == '\n', n);
    n = slurp("/proc/mounts", b, sizeof b);
    check("proc-mounts", n > 0 && strstr(b, " / ") && strstr(b, "proc /proc proc rw") && strstr(b, " 0 0\n"), n);
    say("[KI-TEST] mounts:\n%s", b);
    n = slurp("/proc/filesystems", b, sizeof b);
    check("proc-filesystems", n > 0 && strstr(b, "\text4\n") && strstr(b, "nodev\tproc\n"), n);
}

/* ------------------------------------------------------------------ klog */
static void t_klog(void) {
    static char b[131072];
    int sz = klogctl(SYSLOG_ACTION_SIZE_BUFFER, 0, 0);
    check("syslog-size-buffer", sz >= 65536, sz);
    int n = klogctl(SYSLOG_ACTION_READ_ALL, b, sizeof b - 1);
    if (n > 0) b[n] = 0;
    check("dmesg-has-boot-line", n > 0 && strstr(b, "SECoS build") && strstr(b, "[CLOCK] RTC"), n);
    check("dmesg-record-format", n > 0 && b[0] == '<' && b[2] == '>' && b[3] == '[' && strstr(b, "] "), n);
    check("dmesg-no-user-output", n > 0 && !strstr(b, "[KI-TEST] syslog-size-buffer"), n);
    int r = klogctl(SYSLOG_ACTION_READ_ALL, 0, 10);
    check("syslog-null-buf-einval", r < 0 && errno == EINVAL, r);
    int fd = open("/dev/kmsg", O_RDONLY);
    char rec[700]; long rl = fd >= 0 ? read(fd, rec, sizeof rec - 1) : -1;
    if (rl > 0) rec[rl] = 0;
    check("dev-kmsg-record", rl > 0 && rec[1] == ',' && strstr(rec, ",-;") && rec[rl - 1] == '\n', rl);
    long rl2 = fd >= 0 ? read(fd, rec, sizeof rec - 1) : -1;
    check("dev-kmsg-next-record", rl2 > 0 && rec[1] == ',', rl2);
    if (fd >= 0) close(fd);
    /* dmesg -c: clear, then only new lines are returned */
    r = klogctl(SYSLOG_ACTION_READ_CLEAR, b, sizeof b - 1);
    fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) { write(fd, "kitest-kmsg-marker\n", 19); close(fd); }
    n = klogctl(SYSLOG_ACTION_READ_ALL, b, sizeof b - 1);
    if (n > 0) b[n] = 0;
    check("dmesg-clear", r > 0 && n > 0 && !strstr(b, "SECoS build") && strstr(b, "kitest-kmsg-marker"), n);
}

/* ------------------------------------------------------------------ time */
static volatile int g_sig;
static void on_usr1(int s) { g_sig = s; }
static void t_time(void) {
    time_t t = time(0);
    say("[KI-TEST] time=%ld\n", (long)t);
    check("time-is-unix-epoch", t > 1704067200L && t < 4102444800L, t);
    struct timeval tv; gettimeofday(&tv, 0);
    check("gettimeofday-matches-time", tv.tv_sec >= t && tv.tv_sec <= t + 2 && tv.tv_usec >= 0 && tv.tv_usec < 1000000, tv.tv_sec);
    long a = ms_now(CLOCK_MONOTONIC); msleep(20); long b2 = ms_now(CLOCK_MONOTONIC);
    check("monotonic-advances-nanosleep-20ms", b2 - a >= 20 && b2 - a < 200, b2 - a);
    long u0 = us_now(); struct timespec q = { 0, 300000 }; nanosleep(&q, 0); long u1 = us_now();
    check("nanosleep-sub-tick-300us", u1 - u0 >= 300 && u1 - u0 < 3000, u1 - u0);
    struct timespec bt; clock_gettime(CLOCK_BOOTTIME, &bt);
    check("clock-boottime", bt.tv_sec >= 0 && bt.tv_nsec >= 0 && bt.tv_nsec < 1000000000, bt.tv_sec);
    struct timespec bad;
    check("clock-gettime-bad-id-einval", clock_gettime(99, &bad) == -1 && errno == EINVAL, 0);
    struct timespec inv = { 0, 2000000000L };
    check("nanosleep-bad-nsec-einval", nanosleep(&inv, 0) == -1 && errno == EINVAL, 0);
    /* EINTR with the time left in *rem */
    signal(SIGUSR1, on_usr1);
    int me = getpid(), c = fork();
    if (c == 0) {                                   /* signal once the parent sleeps */
        char sp[48], sb2[512]; snprintf(sp, sizeof sp, "/proc/%d/stat", me);
        msleep(100);
        long k0 = ms_now(CLOCK_MONOTONIC); int it = 0;
        for (; it < 100; it++) { if (slurp(sp, sb2, sizeof sb2) > 0 && strstr(sb2, ") S ")) break; msleep(10); }
        say("[KI-TEST] eintr-child: parent sleeping after %d polls (%ld ms), killing\n", it, ms_now(CLOCK_MONOTONIC) - k0);
        kill(me, SIGUSR1); _exit(0);
    }
    long p0 = ms_now(CLOCK_MONOTONIC);
    struct timespec two = { 2, 0 }, rem = { 0, 0 };
    int r = nanosleep(&two, &rem);
    int e = errno;
    waitpid(c);
    say("[KI-TEST] nanosleep-eintr r=%d e=%d sig=%d rem=%ld.%09ld after %ld ms\n", r, e, g_sig, rem.tv_sec, rem.tv_nsec, ms_now(CLOCK_MONOTONIC) - p0);
    check("nanosleep-eintr-rem", r == -1 && e == EINTR && g_sig == SIGUSR1 && rem.tv_sec >= 1 && rem.tv_sec <= 2, rem.tv_sec);
    /* settime moves REALTIME only */
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    long m0 = ms_now(CLOCK_MONOTONIC);
    struct timespec fut = { now.tv_sec + 100000, now.tv_nsec };
    int sr = clock_settime(CLOCK_REALTIME, &fut);
    time_t t2 = time(0);
    long m1 = ms_now(CLOCK_MONOTONIC);
    struct timeval back = { now.tv_sec, now.tv_nsec / 1000 };
    settimeofday(&back, 0);
    check("clock-settime-realtime", sr == 0 && t2 >= now.tv_sec + 100000 && t2 <= now.tv_sec + 100002 && m1 - m0 < 1000 && time(0) < now.tv_sec + 10, t2);
    check("clock-settime-monotonic-einval", clock_settime(CLOCK_MONOTONIC, &now) == -1 && errno == EINVAL, 0);
    char s[96]; time_t z = 0, k = 1700000000L; struct tm tm;
    strftime(s, sizeof s, "%Y-%m-%d %H:%M:%S %a %j", gmtime_r(&z, &tm));
    check("strftime-epoch", strcmp(s, "1970-01-01 00:00:00 Thu 001") == 0, 0);
    strftime(s, sizeof s, "%F %T %A %B %V %u %s %Z", gmtime_r(&k, &tm));
    check("strftime-1700000000", strcmp(s, "2023-11-14 22:13:20 Tuesday November 46 2 1700000000 UTC") == 0, 0);
    say("[KI-TEST] strftime=%s\n", s);
    struct tm m = { 20, 13, 22, 14, 10, 123, 0, 0, 0, 0, 0 };
    check("mktime-utc", mktime(&m) == 1700000000L && m.tm_wday == 2 && m.tm_yday == 317, (long)mktime(&m));
    check("ctime-format", strcmp(ctime(&k), "Tue Nov 14 22:13:20 2023\n") == 0, 0);
}

/* ------------------------------------------------------------------ misc */
static void t_misc(void) {
    struct utsname u;
    check("uname", uname(&u) == 0 && strcmp(u.sysname, "SecOS") == 0 && strcmp(u.machine, "x86_64") == 0 && u.release[0], 0);
    struct sysinfo si;
    check("sysinfo", sysinfo(&si) == 0 && si.totalram > 1000000 && si.freeram > 0 && si.freeram <= si.totalram &&
          si.procs >= 1 && si.mem_unit == 1 && si.uptime >= 0, (long)si.procs);
    unsigned old = umask(077);
    unsigned now = umask(old);
    check("umask-roundtrip", old == 022 && now == 077, (long)old);
    check("access-x-ok", access("/bin/kitest", X_OK) == 0, 0);
    check("access-enoent", access("/no/such/file", F_OK) == -1 && errno == ENOENT, 0);
    check("access-einval", access("/bin/kitest", 0x10) == -1 && errno == EINVAL, 0);
    struct statfs sf;
    check("statfs-proc-magic", statfs("/proc", &sf) == 0 && sf.f_type == PROC_SUPER_MAGIC, sf.f_type);
    check("statfs-root-ramfs", statfs("/", &sf) == 0 && sf.f_type == RAMFS_MAGIC, sf.f_type);
    int pf[2]; pipe(pf);
    check("fstatfs-pipe", fstatfs(pf[0], &sf) == 0 && sf.f_type == 0x50495045, sf.f_type);
    close(pf[0]); close(pf[1]);
    check("statfs-enoent", statfs("/no/such", &sf) == -1 && errno == ENOENT, 0);
    check("fsync-pipe-einval", (pipe(pf), fsync(pf[0]) == -1 && errno == EINVAL), 0);
    close(pf[0]); close(pf[1]);
    sync();
    check("sync-returns", 1, 0);
}

/* ------------------------------------------------------------------ poll */
static void t_poll(void) {
    int pf[2]; pipe(pf);
    struct pollfd p[2] = { { pf[0], POLLIN, 0 }, { pf[1], POLLOUT, 0 } };
    int r = poll(p, 2, 0);
    check("poll-pipe-write-ready", r == 1 && !(p[0].revents) && (p[1].revents & POLLOUT), r);
    long t0 = ms_now(CLOCK_MONOTONIC);
    r = poll(p, 1, 60);
    long dt = ms_now(CLOCK_MONOTONIC) - t0;
    check("poll-timeout-60ms", r == 0 && dt >= 55 && dt < 400, dt);
    int c = fork();
    if (c == 0) { msleep(100); write(pf[1], "x", 1); _exit(0); }
    t0 = ms_now(CLOCK_MONOTONIC);
    r = poll(p, 1, -1);
    dt = ms_now(CLOCK_MONOTONIC) - t0;
    check("poll-blocks-until-pipe-data", r == 1 && (p[0].revents & POLLIN) && dt >= 80 && dt < 2000, dt);
    char ch; read(pf[0], &ch, 1);
    waitpid(c);
    fd_set rs; FD_ZERO(&rs); FD_SET(pf[0], &rs);
    struct timeval tv = { 0, 50000 };
    r = select(pf[0] + 1, &rs, 0, 0, &tv);
    check("select-timeout", r == 0 && !FD_ISSET(pf[0], &rs), r);
    write(pf[1], "y", 1);
    FD_ZERO(&rs); FD_SET(pf[0], &rs);
    tv.tv_sec = 1; tv.tv_usec = 0;
    r = select(pf[0] + 1, &rs, 0, 0, &tv);
    check("select-readable", r == 1 && FD_ISSET(pf[0], &rs), r);
    read(pf[0], &ch, 1);
    close(pf[1]);
    p[0].events = POLLIN;
    r = poll(p, 1, 1000);
    check("poll-pipe-hup-on-eof", r == 1 && (p[0].revents & POLLHUP), p[0].revents);
    close(pf[0]);
    struct pollfd bad = { 29, POLLIN, 0 };
    r = poll(&bad, 1, 0);
    check("poll-closed-fd-nval", r == 1 && (bad.revents & POLLNVAL), bad.revents);
    struct pollfd neg = { -1, POLLIN, 77 };
    check("poll-negative-fd-ignored", poll(&neg, 1, 0) == 0 && neg.revents == 0, neg.revents);
    check("poll-efault", poll((struct pollfd*)0xdead0000UL, 1, 0) == -1 && errno == EFAULT, 0);
    struct pollfd many[65];
    check("poll-too-many-einval", poll(many, 65, 0) == -1 && errno == EINVAL, 0);
    struct pollfd tty = { 1, POLLOUT, 0 };
    check("poll-tty-writable", poll(&tty, 1, 0) == 1 && (tty.revents & POLLOUT), tty.revents);
    struct pollfd tin = { 0, POLLIN, 0 };
    t0 = ms_now(CLOCK_MONOTONIC);
    r = poll(&tin, 1, 150);
    dt = ms_now(CLOCK_MONOTONIC) - t0;
    check("poll-tty-no-input-timeout", r == 0 && dt >= 140 && dt < 1000, dt);
    /* a signal interrupts an infinite poll */
    signal(SIGUSR1, on_usr1); g_sig = 0;
    pipe(pf);
    int me = getpid(); c = fork();
    if (c == 0) {
        char sp[48], sb2[512]; snprintf(sp, sizeof sp, "/proc/%d/stat", me);
        msleep(80);
        for (int i = 0; i < 100; i++) { if (slurp(sp, sb2, sizeof sb2) > 0 && strstr(sb2, ") S ")) break; msleep(10); }
        kill(me, SIGUSR1); _exit(0);
    }
    struct pollfd w = { pf[0], POLLIN, 0 };
    r = poll(&w, 1, -1);
    int e = errno;
    waitpid(c);
    check("poll-eintr", r == -1 && e == EINTR && g_sig == SIGUSR1, r);
    close(pf[0]); close(pf[1]);
}

/* ------------------------------------------------------------------ namespace ops */
/* Shared by the RAMFS root and disk runs: dir = "" (root) or "/mnt". */
static void t_names(const char* dir, int hardlinks, int symlinks) {
    char a[128], b[128], d[128], buf[256];
    struct stat st;
    snprintf(a, sizeof a, "%s/ki_a.txt", dir); snprintf(b, sizeof b, "%s/ki_b.txt", dir);
    snprintf(d, sizeof d, "%s/ki_dir", dir);
    unlink(a); unlink(b);
    spit(a, "alpha\n"); spit(b, "bravo-old\n");
    int r = rename(a, b);
    slurp(b, buf, sizeof buf);
    check("rename-replaces-target", r == 0 && strcmp(buf, "alpha\n") == 0 && stat(a, &st) != 0, r);
    check("rename-enoent", rename(a, b) == -1 && errno == ENOENT, 0);
    check("rename-exdev", rename(b, "/proc/ki_x") == -1 && errno == EXDEV, errno);
    rename(b, a);
    mkdir(d, 0755);
    char sub[160]; snprintf(sub, sizeof sub, "%s/inner", d);
    check("rename-dir-into-itself-einval", rename(d, sub) == -1 && errno == EINVAL, errno);
    check("rename-dir-over-file-enotdir", rename(d, a) == -1 && errno == ENOTDIR, errno);
    check("rename-file-over-dir-eisdir", rename(a, d) == -1 && errno == EISDIR, errno);
    char inside[160]; snprintf(inside, sizeof inside, "%s/f", d);
    spit(inside, "in\n");
    check("rmdir-enotempty", rmdir(d) == -1 && errno == ENOTEMPTY, errno);
    check("rmdir-enotdir", rmdir(a) == -1 && errno == ENOTDIR, errno);
    char d2[128]; snprintf(d2, sizeof d2, "%s/ki_dir2", dir);
    r = rename(d, d2);
    snprintf(inside, sizeof inside, "%s/f", d2);
    check("rename-dir-moves-children", r == 0 && slurp(inside, buf, sizeof buf) == 3, r);
    unlink(inside);
    check("rmdir-empty", rmdir(d2) == 0 && stat(d2, &st) != 0, 0);
    /* truncate / ftruncate */
    spit(a, "0123456789ABCDEF");
    r = truncate(a, 4);
    int n = slurp(a, buf, sizeof buf);
    check("truncate-shrink", r == 0 && n == 4 && memcmp(buf, "0123", 4) == 0, n);
    int fd = open(a, O_RDWR);
    r = ftruncate(fd, 10);
    close(fd);
    n = slurp(a, buf, sizeof buf);
    check("ftruncate-grow-zero-fill", r == 0 && n == 10 && memcmp(buf, "0123\0\0\0\0\0\0", 10) == 0, n);
    fd = open(a, O_RDONLY);
    check("ftruncate-rdonly-einval", ftruncate(fd, 2) == -1 && errno == EINVAL, 0);
    close(fd);
    check("truncate-dir-eisdir", truncate(dir[0] ? dir : "/bin", 0) == -1 && errno == EISDIR, errno);
    /* hard links */
    if (hardlinks) {
        r = link(a, b);
        struct stat s1, s2; stat(a, &s1); stat(b, &s2);
        check("link-nlink-2", r == 0 && s1.st_nlink == 2 && s2.st_nlink == 2 && s1.st_size == s2.st_size, (long)s1.st_nlink);
        spit(b, "via-b\n");
        slurp(a, buf, sizeof buf);
        check("link-shares-data", strcmp(buf, "via-b\n") == 0, 0);
        check("link-eexist", link(a, b) == -1 && errno == EEXIST, errno);
        unlink(b);
        stat(a, &s1);
        check("unlink-one-link-keeps-file", s1.st_nlink == 1 && stat(b, &s2) != 0, (long)s1.st_nlink);
        check("link-exdev", link(a, "/ki_cross") == -1 && errno == EXDEV, errno);
        check("link-dir-eperm", link(dir, b) == -1 && errno == EPERM, errno);
    } else {
        check("link-unsupported-eperm", link(a, b) == -1 && errno == EPERM, errno);
    }
    /* timestamps from the wall clock */
    spit(a, "stamp\n");
    stat(a, &st);
    long t = (long)time(0);
    if (dir[0]) check("mtime-is-wall-clock", (long)st.st_mtime >= t - 5 && (long)st.st_mtime <= t + 1, (long)st.st_mtime);
    /* umask applies to created files (on FSes that keep modes) */
    if (hardlinks) {
        unsigned om = umask(027);
        char c[128]; snprintf(c, sizeof c, "%s/ki_mode", dir); unlink(c);
        fd = open(c, O_WRONLY | O_CREAT, 0666); close(fd);
        stat(c, &st);
        umask(om);
        check("umask-applied-on-create", (st.st_mode & 0777) == 0640, (long)(st.st_mode & 0777));
        unlink(c);
        char e2[128]; snprintf(e2, sizeof e2, "%s/ki_mdir", dir);
        mkdir(e2, 0700); stat(e2, &st);
        check("mkdir-mode", (st.st_mode & 0777) == 0700 && S_ISDIR(st.st_mode), (long)(st.st_mode & 0777));
        rmdir(e2);
        fd = open(a, O_WRONLY | O_CREAT | O_EXCL, 0644);
        check("open-excl-eexist", fd == -1 && errno == EEXIST, fd);
    }
    /* symlinks in the middle of a path */
    if (symlinks) {
        char s[128], l[160], f[200], lp[160];
        snprintf(s, sizeof s, "%s/ki_real", dir); mkdir(s, 0755);
        snprintf(f, sizeof f, "%s/file", s); spit(f, "through-link\n");
        snprintf(l, sizeof l, "%s/ki_rel", dir); unlink(l);
        r = symlink("ki_real", l);                      /* relative target */
        snprintf(lp, sizeof lp, "%s/ki_rel/file", dir);
        n = slurp(lp, buf, sizeof buf);
        check("midpath-relative-symlink", r == 0 && n > 0 && strcmp(buf, "through-link\n") == 0, n);
        char ab[160]; snprintf(ab, sizeof ab, "%s/ki_abs", dir); unlink(ab);
        symlink(s, ab);                                 /* absolute target */
        snprintf(lp, sizeof lp, "%s/ki_abs/file", dir);
        check("midpath-absolute-symlink", stat(lp, &st) == 0 && st.st_size == 13, (long)st.st_size);
        snprintf(lp, sizeof lp, "%s/ki_rel/../ki_real/file", dir);
        check("midpath-symlink-dotdot", stat(lp, &st) == 0, 0);
        snprintf(lp, sizeof lp, "%s/ki_rel/new.txt", dir);
        spit(lp, "made-through-link\n");
        snprintf(lp, sizeof lp, "%s/ki_real/new.txt", dir);
        check("create-through-midpath-symlink", slurp(lp, buf, sizeof buf) > 0 && strcmp(buf, "made-through-link\n") == 0, 0);
        unlink(lp);
        check("lstat-does-not-follow", lstat(l, &st) == 0 && S_ISLNK(st.st_mode) && stat(l, &st) == 0 && S_ISDIR(st.st_mode), 0);
        char x[128], y[128];
        snprintf(x, sizeof x, "%s/ki_loopa", dir); snprintf(y, sizeof y, "%s/ki_loopb", dir);
        unlink(x); unlink(y);
        symlink("ki_loopb", x); symlink("ki_loopa", y);
        check("symlink-loop-eloop", stat(x, &st) == -1 && errno == ELOOP, errno);
        snprintf(lp, sizeof lp, "%s/ki_loopa/z", dir);
        check("midpath-loop-eloop", stat(lp, &st) == -1 && errno == ELOOP, errno);
        check("open-loop-eloop", open(x, O_RDONLY) == -1 && errno == ELOOP, errno);
        snprintf(lp, sizeof lp, "%s/ki_real/file/x", dir);
        check("midpath-file-enotdir", stat(lp, &st) == -1 && errno == ENOTDIR, errno);
        unlink(x); unlink(y); unlink(ab); unlink(l); unlink(f); rmdir(s);
    }
    unlink(a); unlink(b);
}

static void t_statfs_disk(const char* dir) {
    struct statfs sf;
    int r = statfs(dir, &sf);
    say("[KI-TEST] statfs %s type=0x%lx bsize=%ld blocks=%lu bfree=%lu bavail=%lu files=%lu ffree=%lu namelen=%ld\n",
        dir, sf.f_type, sf.f_bsize, sf.f_blocks, sf.f_bfree, sf.f_bavail, sf.f_files, sf.f_ffree, sf.f_namelen);
    check("statfs-disk", r == 0 && sf.f_blocks > 0 && sf.f_bfree <= sf.f_blocks && sf.f_bavail <= sf.f_bfree && sf.f_bsize >= 512, r);
    char f[128]; snprintf(f, sizeof f, "%s/ki_big", dir);
    int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    static char blk[65536]; memset(blk, 'z', sizeof blk);
    write(fd, blk, sizeof blk);
    struct statfs s2; fstatfs(fd, &s2);
    close(fd);
    unsigned long used = (sf.f_bfree - s2.f_bfree) * (unsigned long)sf.f_bsize;
    check("statfs-tracks-allocation", s2.f_bfree < sf.f_bfree && used >= 65536 && used <= 65536 + 4 * (unsigned long)sf.f_bsize, (long)used);
    unlink(f);
    statfs(dir, &s2);
    check("statfs-frees-on-unlink", s2.f_bfree == sf.f_bfree, (long)(sf.f_bfree - s2.f_bfree));
}

int main(int argc, char** argv) {
    snprintf(g_argv0, sizeof g_argv0, "%s", argc > 0 ? argv[0] : "kitest");
    const char* mode = argc > 1 ? argv[1] : "";
    const char* dir = argc > 2 ? argv[2] : "/mnt";
    char buf[256];
    if (strcmp(mode, "disk") == 0) {
        struct statfs sf; statfs(dir, &sf);
        int ext = (sf.f_type == EXT2_SUPER_MAGIC);
        t_names(dir, ext, ext);
        t_statfs_disk(dir);
    } else if (strcmp(mode, "persist1") == 0) {
        char t[128], d[128]; snprintf(t, sizeof t, "%s/persist.tmp", dir); snprintf(d, sizeof d, "%s/persist.dat", dir);
        check("persist1-old", spit(d, "persist-OLD\n") == 0, 0);
        check("persist1-new", spit(t, "persist-NEW\n") == 0, 0);
        check("persist1-rename", rename(t, d) == 0, 0);
        sync();
    } else if (strcmp(mode, "persist2") == 0) {
        char t[128], d[128]; struct stat st; snprintf(t, sizeof t, "%s/persist.tmp", dir); snprintf(d, sizeof d, "%s/persist.dat", dir);
        int n = slurp(d, buf, sizeof buf);
        check("persist2-rename-survived-reboot", n > 0 && strcmp(buf, "persist-NEW\n") == 0 && stat(t, &st) != 0, n);
        struct statfs sf; statfs(dir, &sf);
        say("[KI-TEST] statfs-final blocks=%lu bfree=%lu files=%lu ffree=%lu\n", sf.f_blocks, sf.f_bfree, sf.f_files, sf.f_ffree);
    } else {
        t_procfs();
        t_klog();
        t_time();
        t_misc();
        t_poll();
        t_names("", 0, 0);
    }
    say("[KI-TEST] DONE pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
