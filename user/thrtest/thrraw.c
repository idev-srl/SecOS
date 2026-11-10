/*
 * thrraw — U9 kernel thread tests with raw SecOS calls (no pthreads).
 * Built with tools/secos-cc; run on SecOS by tools/threads_test.sh.
 * SPDX-License-Identifier: MIT
 *
 *   thrraw all        every test: "TR PASS <name>" / "TR FAIL <name>: <why>",
 *                     then "TR DONE pass=N fail=M"
 *   thrraw <name>     one test
 *   thrraw spin N     N threads that sleep forever (for kill tests from the shell)
 *   thrraw execd CODE exec target: prints "TR EXECD pid=<pid> tid=<tid>", exits CODE
 *
 * The threads made here run on raw clone() (SecOS 512+56) with the parent's
 * TLS: they only use the raw calls below, never libc.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <dirent.h>

#define NR_CLONE   (512 + 56)
#define NR_EXIT    (512 + 60)
#define NR_GETTID  (512 + 186)
#define NR_TKILL   (512 + 200)
#define NR_FUTEX   (512 + 202)
#define NR_TGKILL  (512 + 234)
#define NR_EXITG   (512 + 231)
#define S_WRITE    2
#define S_GETPID   6
#define S_SLEEP    13
#define S_PIPE     31
#define S_YIELD    0
#define S_FORK     18
#define S_EXECVE   60

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define THR_FLAGS (CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SYSVSEM| \
                   CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID)

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE 128
#define FUTEX_CLOCK_RT 256

static long sc6(long n, long a, long b, long c, long d, long e, long f)
{
    long ret;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ __volatile__("int $0x80" : "=a"(ret)
                         : "0"(n), "D"(a), "S"(b), "d"(c), "c"(d), "r"(r8), "r"(r9)
                         : "memory", "cc");
    return ret;
}
#define sc0(n)          sc6(n,0,0,0,0,0,0)
#define sc1(n,a)        sc6(n,(long)(a),0,0,0,0,0)
#define sc2(n,a,b)      sc6(n,(long)(a),(long)(b),0,0,0,0)
#define sc3(n,a,b,c)    sc6(n,(long)(a),(long)(b),(long)(c),0,0,0)

/* raw_clone(flags, stack_top, ptid, ctid, fn, arg): the child runs fn(arg) on
 * the new stack and exits the thread with its return value. */
long raw_clone(long flags, void *stack_top, int *ptid, int *ctid, int (*fn)(void *), void *arg);
__asm__(
    ".text\n.global raw_clone\n.type raw_clone,@function\nraw_clone:\n"
    "  and $-16, %rsi\n"
    "  sub $16, %rsi\n"
    "  mov %r8, (%rsi)\n"          /* fn  */
    "  mov %r9, 8(%rsi)\n"         /* arg */
    "  xor %r8, %r8\n"             /* tls: none (shares the creator's) */
    "  mov $568, %eax\n"
    "  int $0x80\n"
    "  test %rax, %rax\n"
    "  jnz 1f\n"
    "  pop %rax\n"
    "  pop %rdi\n"
    "  xor %ebp, %ebp\n"
    "  call *%rax\n"
    "  mov %rax, %rdi\n"
    "  mov $572, %eax\n"
    "  int $0x80\n"
    "  hlt\n"
    "1: ret\n");

static int futex(volatile int *a, int op, int val, const struct timespec *ts, volatile int *a2, int v3)
{
    return (int)sc6(NR_FUTEX, (long)a, op, val, (long)ts, (long)a2, v3);
}
static int gettid_(void) { return (int)sc0(NR_GETTID); }
static int getpid_(void) { return (int)sc0(S_GETPID); }
static void tsleep(int ms) { sc1(S_SLEEP, ms); }


#define STK (64 * 1024)
static char *stack_new(void)
{
    char *s = mmap(0, STK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return s == MAP_FAILED ? NULL : s + STK;
}

typedef struct { volatile int tid; volatile int ctid; char *stk; } th_t;
static int th_start(th_t *t, int (*fn)(void *), void *arg)
{
    t->stk = stack_new();
    if (!t->stk) return -1;
    t->ctid = 1;
    long r = raw_clone(THR_FLAGS, t->stk, (int *)&t->tid, (int *)&t->ctid, fn, arg);
    if (r <= 0) { t->ctid = 0; return (int)r; }
    return 0;
}
/* join: CLONE_CHILD_CLEARTID zeroes ctid and wakes it at thread exit */
static void th_join(th_t *t)
{
    while (__atomic_load_n(&t->ctid, __ATOMIC_SEQ_CST))
        futex(&t->ctid, FUTEX_WAIT, 1, NULL, NULL, 0);
    if (t->stk) munmap(t->stk - STK, STK);
    t->stk = NULL;
}

static int g_pass, g_fail;
static void result(const char *name, int ok, const char *why)
{
    if (ok) { printf("TR PASS %s\n", name); g_pass++; }
    else    { printf("TR FAIL %s: %s\n", name, why ? why : "?"); g_fail++; }
    fflush(stdout);
}
#define CHECK(name, cond, ...) do { char _w[200]; snprintf(_w, sizeof _w, __VA_ARGS__); result(name, (cond), _w); } while (0)

static long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }

/* ---- step 1: thread groups ---- */
static volatile int g_shared, g_tpid, g_ttid;
static int fn_basic(void *a)
{
    g_tpid = getpid_(); g_ttid = gettid_();
    g_shared = (int)(long)a;
    return 0;
}
static void t_basic(void)
{
    th_t t;
    g_shared = 0;
    int r = th_start(&t, fn_basic, (void *)1234);
    if (r) { CHECK("clone-basic", 0, "clone failed %d", r); return; }
    int ptid = t.tid;
    th_join(&t);
    CHECK("clone-basic", g_shared == 1234 && g_tpid == getpid_() && g_ttid == ptid &&
          g_ttid != gettid_() && getpid_() == gettid_(),
          "shared=%d tpid=%d pid=%d ttid=%d ptid=%d", g_shared, g_tpid, getpid_(), g_ttid, ptid);
}

#define NMANY 40
static volatile int g_count;
static int fn_many(void *a)
{
    (void)a;
    for (int i = 0; i < 2000; i++) {
        __atomic_add_fetch(&g_count, 1, __ATOMIC_SEQ_CST);
        if ((i & 63) == 0) sc0(S_YIELD);
    }
    return 0;
}
static void t_many(void)
{
    static th_t t[NMANY];
    g_count = 0;
    int ok = 0;
    for (int i = 0; i < NMANY; i++) if (th_start(&t[i], fn_many, 0) == 0) ok++;
    for (int i = 0; i < NMANY; i++) if (t[i].stk) th_join(&t[i]);
    CHECK("many-threads", ok == NMANY && g_count == NMANY * 2000, "started=%d count=%d", ok, g_count);
}

static int fn_churn(void *a) { (void)a; return 0; }
static void t_churn(void)
{
    /* 2000 short threads, 8 at a time: slots and kernel stacks are recycled */
    th_t t[8]; int n = 0, bad = 0;
    for (int k = 0; k < 250; k++) {
        for (int i = 0; i < 8; i++) { if (th_start(&t[i], fn_churn, 0) == 0) n++; else { bad++; t[i].stk = NULL; } }
        for (int i = 0; i < 8; i++) if (t[i].stk) th_join(&t[i]);
    }
    CHECK("thread-churn-2000", n == 2000 && bad == 0, "ok=%d failed=%d", n, bad);
}

static int g_pfd[2];
static int fn_fds(void *a)
{
    (void)a;
    int p[2];
    if (sc1(S_PIPE, p) != 0) return 1;
    g_pfd[0] = p[0]; g_pfd[1] = p[1];
    return 0;
}
static void t_fds(void)
{
    th_t t;
    g_pfd[0] = g_pfd[1] = -1;
    th_start(&t, fn_fds, 0);
    th_join(&t);
    char b[8] = {0};
    int w = (int)write(g_pfd[1], "fdok", 4);
    int r = (int)read(g_pfd[0], b, 4);
    close(g_pfd[0]); close(g_pfd[1]);
    CHECK("shared-fd-table", w == 4 && r == 4 && !memcmp(b, "fdok", 4), "pipe from a thread: w=%d r=%d", w, r);
}

/* the thread runs exit_group(42): every thread of the child process goes */
static int fn_sleeper(void *a) { (void)a; for (;;) tsleep(1000); return 0; }
static int fn_exitg(void *a) { (void)a; tsleep(50); sc1(NR_EXITG, 42); return 0; }
static void t_exit_group(void)
{
    int pid = fork();
    if (pid == 0) {
        th_t t[4];
        for (int i = 0; i < 3; i++) th_start(&t[i], fn_sleeper, 0);
        th_start(&t[3], fn_exitg, 0);
        for (;;) tsleep(1000);
    }
    int st = 0; int w = waitpid(pid, &st, 0);
    CHECK("exit-group", w == pid && WIFEXITED(st) && WEXITSTATUS(st) == 42, "w=%d st=%#x", w, st);
}

/* the leader exits with exit(3) first; the last thread's exit(0) ends the
 * process with the leader's status */
static int fn_last(void *a) { (void)a; tsleep(100); return 0; }
static void t_leader_exit(void)
{
    int pid = fork();
    if (pid == 0) {
        th_t t;
        th_start(&t, fn_last, 0);
        sc1(NR_EXIT, 3);                     /* the main thread only */
        for (;;) ;
    }
    int st = 0; long t0 = now_ms();
    int w = waitpid(pid, &st, 0);
    long dt = now_ms() - t0;
    CHECK("leader-exit-first", w == pid && WIFEXITED(st) && WEXITSTATUS(st) == 3 && dt >= 80,
          "w=%d st=%#x after %ld ms", w, st, dt);
}

static volatile int g_never;
static int fn_block(void *a) { (void)a; for (;;) futex(&g_never, FUTEX_WAIT, 0, NULL, NULL, 0); return 0; }
static void t_kill9(void)
{
    int pid = fork();
    if (pid == 0) {
        th_t t[5];
        for (int i = 0; i < 5; i++) th_start(&t[i], fn_block, 0);
        for (;;) tsleep(1000);
    }
    tsleep(100);
    kill(pid, SIGKILL);
    int st = 0; int w = waitpid(pid, &st, 0);
    CHECK("kill9-multithreaded", w == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "w=%d st=%#x", w, st);
}

static volatile int g_fork_rc;
static int fn_forker(void *a)
{
    (void)a;
    long pid = sc0(S_FORK);
    if (pid == 0) {
        /* the child: one thread, pid == tid */
        sc1(NR_EXITG, getpid_() == gettid_() ? 5 : 6);
    }
    g_fork_rc = (int)pid;
    return 0;
}
static void t_fork_thread(void)
{
    th_t t;
    g_fork_rc = 0;
    th_start(&t, fn_forker, 0);
    th_join(&t);
    int st = 0; int w = g_fork_rc > 0 ? waitpid(g_fork_rc, &st, 0) : -1;
    CHECK("fork-from-thread", w == g_fork_rc && WIFEXITED(st) && WEXITSTATUS(st) == 5,
          "child=%d w=%d st=%#x", g_fork_rc, w, st);
}

static char g_self[256];
static int fn_execer(void *a)
{
    (void)a;
    tsleep(30);
    static char *av[] = { g_self, "execd", "9", NULL };
    static char *ev[] = { NULL };
    sc3(S_EXECVE, g_self, av, ev);
    sc1(NR_EXITG, 99);                       /* exec failed */
    return 0;
}
static void t_exec_thread(void)
{
    int pid = fork();
    if (pid == 0) {
        th_t t[3];
        th_start(&t[0], fn_sleeper, 0);
        th_start(&t[1], fn_block, 0);
        th_start(&t[2], fn_execer, 0);
        for (;;) tsleep(1000);
    }
    int st = 0; int w = waitpid(pid, &st, 0);
    CHECK("execve-from-thread", w == pid && WIFEXITED(st) && WEXITSTATUS(st) == 9, "w=%d st=%#x", w, st);
}

static void t_proc_task(void)
{
    th_t t[3];
    for (int i = 0; i < 3; i++) th_start(&t[i], fn_block, 0);
    tsleep(20);
    char p[64]; snprintf(p, sizeof p, "/proc/%d/status", getpid_());
    FILE *f = fopen(p, "r"); int threads = -1;
    char line[128];
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "Threads:", 8)) threads = atoi(line + 8);
    if (f) fclose(f);
    snprintf(p, sizeof p, "/proc/%d/task", getpid_());
    int ntask = 0;
    DIR *dd = opendir(p);
    struct dirent *de;
    while (dd && (de = readdir(dd))) if (de->d_name[0] >= '0' && de->d_name[0] <= '9') ntask++;
    if (dd) closedir(dd);
    CHECK("proc-threads", threads == 4 && ntask == 4, "Threads: %d, task entries %d", threads, ntask);
    /* the thread group goes with the process: kill it via a forked copy is
     * overkill; leave the blocked threads (they die with us at exit_group) */
}

/* ---- step 2: futex ---- */
static volatile int g_fw, g_fw2, g_fres[8];
static int fn_fwait(void *a)
{
    int i = (int)(long)a;
    g_fres[i] = futex(&g_fw, FUTEX_WAIT | FUTEX_PRIVATE, 0, NULL, NULL, 0);
    return 0;
}
static void t_futex(void)
{
    /* EAGAIN on a value mismatch */
    g_fw = 1;
    int r = futex(&g_fw, FUTEX_WAIT, 0, NULL, NULL, 0);
    CHECK("futex-eagain", r == -EAGAIN, "r=%d", r);
    /* relative timeout */
    struct timespec ts = { 0, 60 * 1000000 };
    long t0 = now_ms();
    r = futex(&g_fw, FUTEX_WAIT, 1, &ts, NULL, 0);
    long dt = now_ms() - t0;
    CHECK("futex-timeout", r == -ETIMEDOUT && dt >= 55 && dt < 1000, "r=%d dt=%ld", r, dt);
    /* absolute CLOCK_REALTIME bitset timeout */
    struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt);
    rt.tv_nsec += 50 * 1000000; if (rt.tv_nsec >= 1000000000) { rt.tv_sec++; rt.tv_nsec -= 1000000000; }
    t0 = now_ms();
    r = futex(&g_fw, FUTEX_WAIT_BITSET | FUTEX_CLOCK_RT, 1, &rt, NULL, -1);
    dt = now_ms() - t0;
    CHECK("futex-realtime-abs", r == -ETIMEDOUT && dt >= 40 && dt < 1000, "r=%d dt=%ld", r, dt);
    /* wake count: 4 waiters, wake 2, then the rest */
    g_fw = 0;
    th_t t[4];
    for (int i = 0; i < 4; i++) { g_fres[i] = 99; th_start(&t[i], fn_fwait, (void *)(long)i); }
    tsleep(50);
    int w1 = futex(&g_fw, FUTEX_WAKE | FUTEX_PRIVATE, 2, NULL, NULL, 0);
    tsleep(30);
    int done = 0; for (int i = 0; i < 4; i++) if (g_fres[i] == 0) done++;
    int w2 = futex(&g_fw, FUTEX_WAKE | FUTEX_PRIVATE, 100, NULL, NULL, 0);
    for (int i = 0; i < 4; i++) th_join(&t[i]);
    int all = 0; for (int i = 0; i < 4; i++) if (g_fres[i] == 0) all++;
    CHECK("futex-wake-count", w1 == 2 && done == 2 && w2 == 2 && all == 4, "w1=%d done=%d w2=%d all=%d", w1, done, w2, all);
    /* requeue: 3 waiters on g_fw, wake 1 and move 2 to g_fw2; wake g_fw2 */
    for (int i = 0; i < 3; i++) { g_fres[i] = 99; th_start(&t[i], fn_fwait, (void *)(long)i); }
    tsleep(50);
    int bad = futex(&g_fw, FUTEX_CMP_REQUEUE | FUTEX_PRIVATE, 1, (void *)2L, &g_fw2, 7);
    int rq = futex(&g_fw, FUTEX_CMP_REQUEUE | FUTEX_PRIVATE, 1, (void *)2L, &g_fw2, 0);
    tsleep(30);
    int none = futex(&g_fw, FUTEX_WAKE | FUTEX_PRIVATE, 100, NULL, NULL, 0);
    int moved = futex(&g_fw2, FUTEX_WAKE | FUTEX_PRIVATE, 100, NULL, NULL, 0);
    for (int i = 0; i < 3; i++) th_join(&t[i]);
    CHECK("futex-cmp-requeue", bad == -EAGAIN && rq == 3 && none == 0 && moved == 2,
          "bad=%d rq=%d none=%d moved=%d", bad, rq, none, moved);
    /* bitset: a waiter on bit 1 is not woken by a wake of bit 2 */
    g_fres[0] = 99;
    th_t b;
    g_fw = 0;
    th_start(&b, fn_fwait, (void *)0L);      /* waits with FUTEX_WAIT (all bits) */
    tsleep(30);
    int nb = futex(&g_fw, FUTEX_WAKE_BITSET | FUTEX_PRIVATE, 1, NULL, NULL, 2);
    th_join(&b);
    CHECK("futex-bitset", nb == 1 && g_fres[0] == 0, "nb=%d res=%d", nb, g_fres[0]);
    /* lost-wakeup stress: ping-pong 20000 times between two threads */
}

/* ping-pong over a futex word: no wake may be lost (else it hangs) */
static volatile int g_pp;
#define PP_N 20000
static int fn_pong(void *a)
{
    (void)a;
    for (int i = 0; i < PP_N; i++) {
        while (__atomic_load_n(&g_pp, __ATOMIC_SEQ_CST) != 1) futex(&g_pp, FUTEX_WAIT | FUTEX_PRIVATE, 0, NULL, NULL, 0);
        __atomic_store_n(&g_pp, 0, __ATOMIC_SEQ_CST);
        futex(&g_pp, FUTEX_WAKE | FUTEX_PRIVATE, 1, NULL, NULL, 0);
    }
    return 0;
}
static void t_pingpong(void)
{
    th_t t; g_pp = 0;
    th_start(&t, fn_pong, 0);
    long t0 = now_ms();
    for (int i = 0; i < PP_N; i++) {
        __atomic_store_n(&g_pp, 1, __ATOMIC_SEQ_CST);
        futex(&g_pp, FUTEX_WAKE | FUTEX_PRIVATE, 1, NULL, NULL, 0);
        while (__atomic_load_n(&g_pp, __ATOMIC_SEQ_CST) != 0) futex(&g_pp, FUTEX_WAIT | FUTEX_PRIVATE, 1, NULL, NULL, 0);
    }
    th_join(&t);
    CHECK("futex-pingpong-20000", 1, "%ld ms", now_ms() - t0);
}

/* ---- step 3: signals ---- */
static volatile int g_sig_tid, g_sig_n;
static void h_usr1(int s) { (void)s; g_sig_tid = gettid_(); __atomic_add_fetch(&g_sig_n, 1, __ATOMIC_SEQ_CST); }
static volatile int g_target_tid, g_go;
static int fn_sigwait(void *a)
{
    (void)a;
    g_target_tid = gettid_();
    while (!__atomic_load_n(&g_go, __ATOMIC_SEQ_CST)) tsleep(5);
    return 0;
}
static int fn_unblock_wait(void *a)
{
    (void)a;
    unsigned long set = 1UL << SIGUSR1;      /* SecOS mask: bit n = signal n */
    sc3(47 /* SYS_SIGPROCMASK */, 1 /* SIG_UNBLOCK */, set, 0);
    g_target_tid = gettid_();
    while (!__atomic_load_n(&g_go, __ATOMIC_SEQ_CST)) tsleep(5);
    return 0;
}
static void t_tgkill(void)
{
    signal(SIGUSR1, h_usr1);
    th_t t; g_go = 0; g_sig_tid = 0; g_sig_n = 0; g_target_tid = 0;
    th_start(&t, fn_sigwait, 0);
    while (!g_target_tid) tsleep(5);
    int r = (int)sc3(NR_TGKILL, getpid_(), g_target_tid, SIGUSR1);
    for (int i = 0; i < 100 && !g_sig_n; i++) tsleep(5);
    g_go = 1;
    th_join(&t);
    CHECK("tgkill-thread", r == 0 && g_sig_n == 1 && g_sig_tid == t.tid, "r=%d n=%d in tid %d (target %d)",
          r, g_sig_n, g_sig_tid, t.tid);
    /* a process-directed signal while the main thread blocks it: the thread
     * that does not block it runs the handler */
    sigset_t m; sigemptyset(&m); sigaddset(&m, SIGUSR1);
    sigprocmask(SIG_BLOCK, &m, NULL);
    g_go = 0; g_sig_tid = 0; g_sig_n = 0; g_target_tid = 0;
    th_start(&t, fn_unblock_wait, 0);        /* inherits the mask, then unblocks */
    while (!g_target_tid) tsleep(5);
    kill(getpid_(), SIGUSR1);
    for (int i = 0; i < 100 && !g_sig_n; i++) tsleep(5);
    g_go = 1;
    th_join(&t);
    CHECK("process-signal-to-unblocking-thread", g_sig_n == 1 && g_sig_tid == t.tid,
          "n=%d tid=%d thread=%d", g_sig_n, g_sig_tid, t.tid);
    /* every thread blocks it: it pends on the process until one unblocks */
    g_go = 0; g_sig_tid = 0; g_sig_n = 0; g_target_tid = 0;
    th_start(&t, fn_sigwait, 0);             /* keeps the inherited (blocking) mask */
    while (!g_target_tid) tsleep(5);
    kill(getpid_(), SIGUSR1);
    tsleep(30);
    int before = g_sig_n;
    sigprocmask(SIG_UNBLOCK, &m, NULL);       /* main unblocks: delivered to main */
    for (int i = 0; i < 100 && !g_sig_n; i++) tsleep(5);
    g_go = 1;
    th_join(&t);
    CHECK("process-signal-pends-until-unblocked", before == 0 && g_sig_n == 1 && g_sig_tid == gettid_(),
          "before=%d n=%d tid=%d main=%d", before, g_sig_n, g_sig_tid, gettid_());
    signal(SIGUSR1, SIG_DFL);
}

static int fn_count(void *a) { (void)a; for (;;) { __atomic_add_fetch(&g_count, 1, __ATOMIC_SEQ_CST); sc0(S_YIELD); } return 0; }
static void t_stopcont(void)
{
    int pid = fork();
    if (pid == 0) {
        th_t t[3];
        g_count = 0;
        for (int i = 0; i < 3; i++) th_start(&t[i], fn_count, 0);
        for (;;) tsleep(1000);
    }
    tsleep(100);
    kill(pid, SIGSTOP);
    int st = 0;
    int w = waitpid(pid, &st, WUNTRACED);
    int stopped = (w == pid && WIFSTOPPED(st));
    tsleep(50);
    /* every thread stopped: /proc/<pid>/task/<tid>/status State: T */
    int nT = 0, nall = 0;
    char p[96]; snprintf(p, sizeof p, "/proc/%d/task", pid);
    DIR *d = opendir(p); struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char q[400]; snprintf(q, sizeof q, "%s/%s/status", p, de->d_name);
        FILE *f = fopen(q, "r"); char line[128];
        while (f && fgets(line, sizeof line, f))
            if (!strncmp(line, "State:", 6)) { char *c = line + 6; while (*c == ' ' || *c == '\t') c++; nall++; if (*c == 'T') nT++; }
        if (f) fclose(f);
    }
    if (d) closedir(d);
    kill(pid, SIGCONT);
    w = waitpid(pid, &st, WCONTINUED);
    int cont = (w == pid && WIFCONTINUED(st));
    kill(pid, SIGINT);                       /* default action: the whole group ends */
    w = waitpid(pid, &st, 0);
    CHECK("group-stop-cont-sigint", stopped && nall == 4 && nT == 4 && cont && w == pid &&
          WIFSIGNALED(st) && WTERMSIG(st) == SIGINT,
          "stopped=%d T=%d/%d cont=%d w=%d st=%#x", stopped, nT, nall, cont, w, st);
}

static void spin_main(int n)
{
    static th_t t[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) th_start(&t[i], fn_sleeper, 0);
    printf("TR SPIN pid=%d threads=%d\n", getpid_(), n + 1);
    fflush(stdout);
    for (;;) tsleep(1000);
}

int main(int argc, char **argv)
{
    snprintf(g_self, sizeof g_self, "%s", argv[0]);
    if (argc >= 3 && !strcmp(argv[1], "execd")) {
        printf("TR EXECD pid=%d tid=%d\n", getpid_(), gettid_());
        fflush(stdout);
        return atoi(argv[2]);
    }
    if (argc >= 3 && !strcmp(argv[1], "spin")) { spin_main(atoi(argv[2])); return 0; }
    const char *w = argc >= 2 ? argv[1] : "all";
    struct { const char *n; void (*f)(void); } T[] = {
        { "basic", t_basic }, { "many", t_many }, { "churn", t_churn }, { "fds", t_fds },
        { "exitgroup", t_exit_group }, { "leader", t_leader_exit }, { "kill9", t_kill9 },
        { "fork", t_fork_thread }, { "exec", t_exec_thread }, { "futex", t_futex },
        { "pingpong", t_pingpong }, { "tgkill", t_tgkill }, { "stopcont", t_stopcont },
        { "proc", t_proc_task },
    };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(w, "all") || !strcmp(w, T[i].n)) T[i].f();
    printf("TR DONE pass=%d fail=%d\n", g_pass, g_fail);
    fflush(stdout);
    sc1(NR_EXITG, g_fail ? 1 : 0);           /* the proc test leaves blocked threads */
    return 0;
}
