/*
 * thrstress — U9 gate program: POSIX threads on SecOS (musl pthreads).
 * Built with tools/secos-cc; installed as /bin/thrstress for the smptest-style
 * runs and used by tools/threads_test.sh. SPDX-License-Identifier: MIT
 *
 *   thrstress all [N]     every test with N worker threads (default 8):
 *                         "TS PASS <name>" / "TS FAIL <name>: <why>", then
 *                         "TS DONE pass=P fail=F"
 *   thrstress <name> [N]  one test (mutex condvar rwlock barrier sem cancel
 *                         join10k detach tls tsd once malloc stdio signal
 *                         kill sigint exec errno)
 *   thrstress spin N      N threads that sleep (for kill/SIGINT from a shell)
 *   thrstress execd CODE  exec target: exits with CODE
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/mman.h>

static int g_pass, g_fail, N = 8;
static char g_self[256];
static void result(const char *name, int ok, const char *why)
{
    if (ok) printf("TS PASS %s\n", name), g_pass++;
    else    printf("TS FAIL %s: %s\n", name, why ? why : "?"), g_fail++;
    fflush(stdout);
}
#define CHECK(name, cond, ...) do { char _w[200]; snprintf(_w, sizeof _w, __VA_ARGS__); result(name, (cond), _w); } while (0)
static long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void msleep(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, 0); }
static pid_t gettid_(void) { return (pid_t)syscall(SYS_gettid); }

/* ---- mutex ---- */
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static long g_counter;
#define MX_ITERS 20000
static void *f_mutex(void *a)
{
    (void)a;
    for (int i = 0; i < MX_ITERS; i++) {
        pthread_mutex_lock(&g_mx);
        long v = g_counter; if ((i & 255) == 0) sched_yield(); g_counter = v + 1;
        pthread_mutex_unlock(&g_mx);
    }
    return 0;
}
static void t_mutex(void)
{
    pthread_t t[64]; g_counter = 0; int ok = 0;
    for (int i = 0; i < N; i++) if (!pthread_create(&t[i], 0, f_mutex, 0)) ok++;
    for (int i = 0; i < ok; i++) pthread_join(t[i], 0);
    CHECK("mutex", ok == N && g_counter == (long)N * MX_ITERS, "threads=%d counter=%ld want %ld", ok, g_counter, (long)N * MX_ITERS);
}

/* ---- condvar: bounded queue, N/2 producers, N/2 consumers ---- */
#define QCAP 16
#define PER_PROD 5000
static int g_q[QCAP], g_qh, g_qt, g_qn, g_qdone;
static long g_sum_in, g_sum_out;
static pthread_mutex_t g_qm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_qne = PTHREAD_COND_INITIALIZER, g_qnf = PTHREAD_COND_INITIALIZER;
static void *f_prod(void *a)
{
    int id = (int)(long)a;
    for (int i = 1; i <= PER_PROD; i++) {
        pthread_mutex_lock(&g_qm);
        while (g_qn == QCAP) pthread_cond_wait(&g_qnf, &g_qm);
        g_q[g_qt] = id * 100000 + i; g_qt = (g_qt + 1) % QCAP; g_qn++;
        g_sum_in += id * 100000 + i;
        pthread_cond_signal(&g_qne);
        pthread_mutex_unlock(&g_qm);
    }
    return 0;
}
static void *f_cons(void *a)
{
    (void)a;
    for (;;) {
        pthread_mutex_lock(&g_qm);
        while (g_qn == 0 && !g_qdone) pthread_cond_wait(&g_qne, &g_qm);
        if (g_qn == 0 && g_qdone) { pthread_mutex_unlock(&g_qm); return 0; }
        int v = g_q[g_qh]; g_qh = (g_qh + 1) % QCAP; g_qn--;
        g_sum_out += v;
        pthread_cond_signal(&g_qnf);
        pthread_mutex_unlock(&g_qm);
    }
}
static void t_condvar(void)
{
    int np = N / 2 > 0 ? N / 2 : 1, nc = N - np > 0 ? N - np : 1;
    pthread_t p[64], c[64];
    g_qh = g_qt = g_qn = g_qdone = 0; g_sum_in = g_sum_out = 0;
    for (int i = 0; i < nc; i++) pthread_create(&c[i], 0, f_cons, 0);
    for (int i = 0; i < np; i++) pthread_create(&p[i], 0, f_prod, (void *)(long)(i + 1));
    for (int i = 0; i < np; i++) pthread_join(p[i], 0);
    pthread_mutex_lock(&g_qm); g_qdone = 1; pthread_cond_broadcast(&g_qne); pthread_mutex_unlock(&g_qm);
    for (int i = 0; i < nc; i++) pthread_join(c[i], 0);
    CHECK("condvar", g_sum_in == g_sum_out && g_qn == 0, "in=%ld out=%ld left=%d (%d producers, %d consumers)",
          g_sum_in, g_sum_out, g_qn, np, nc);
}

/* ---- rwlock: readers see a consistent pair ---- */
static pthread_rwlock_t g_rw = PTHREAD_RWLOCK_INITIALIZER;
static volatile long g_ra, g_rb; static volatile int g_rbad, g_rstop;
static void *f_reader(void *a)
{
    (void)a; long n = 0;
    while (!g_rstop) {
        pthread_rwlock_rdlock(&g_rw);
        if (g_ra != g_rb) g_rbad = 1;
        pthread_rwlock_unlock(&g_rw);
        if ((++n & 63) == 0) sched_yield();
    }
    return (void *)n;
}
static void *f_writer(void *a)
{
    (void)a;
    for (int i = 0; i < 3000; i++) {
        pthread_rwlock_wrlock(&g_rw);
        g_ra++; if ((i & 31) == 0) sched_yield(); g_rb++;
        pthread_rwlock_unlock(&g_rw);
    }
    return 0;
}
static void t_rwlock(void)
{
    pthread_t r[64], w[4]; int nr = N > 2 ? N - 2 : 1;
    g_ra = g_rb = 0; g_rbad = 0; g_rstop = 0;
    for (int i = 0; i < nr; i++) pthread_create(&r[i], 0, f_reader, 0);
    for (int i = 0; i < 2; i++) pthread_create(&w[i], 0, f_writer, 0);
    for (int i = 0; i < 2; i++) pthread_join(w[i], 0);
    g_rstop = 1;
    long reads = 0;
    for (int i = 0; i < nr; i++) { void *v; pthread_join(r[i], &v); reads += (long)v; }
    CHECK("rwlock", !g_rbad && g_ra == 6000 && g_rb == 6000, "bad=%d a=%ld b=%ld reads=%ld", g_rbad, g_ra, g_rb, reads);
}

/* ---- barrier ---- */
#define BROUNDS 200
static pthread_barrier_t g_bar;
static volatile int g_phase[64]; static volatile int g_bbad;
static void *f_barrier(void *a)
{
    int id = (int)(long)a;
    for (int r = 0; r < BROUNDS; r++) {
        g_phase[id] = r;
        pthread_barrier_wait(&g_bar);
        for (int j = 0; j < N; j++) if (g_phase[j] != r) g_bbad = 1;
        pthread_barrier_wait(&g_bar);
    }
    return 0;
}
static void t_barrier(void)
{
    pthread_t t[64]; g_bbad = 0;
    pthread_barrier_init(&g_bar, 0, (unsigned)N);
    for (int i = 0; i < N; i++) pthread_create(&t[i], 0, f_barrier, (void *)(long)i);
    for (int i = 0; i < N; i++) pthread_join(t[i], 0);
    pthread_barrier_destroy(&g_bar);
    CHECK("barrier", !g_bbad, "%d threads x %d rounds, phase mismatch=%d", N, BROUNDS, g_bbad);
}

/* ---- semaphore: N workers, a pool of 3 ---- */
static sem_t g_sem; static volatile int g_inside, g_maxin;
static void *f_sem(void *a)
{
    (void)a;
    for (int i = 0; i < 300; i++) {
        sem_wait(&g_sem);
        int v = __atomic_add_fetch(&g_inside, 1, __ATOMIC_SEQ_CST);
        int m = g_maxin; while (v > m && !__atomic_compare_exchange_n(&g_maxin, &m, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) ;
        if ((i & 7) == 0) sched_yield();
        __atomic_sub_fetch(&g_inside, 1, __ATOMIC_SEQ_CST);
        sem_post(&g_sem);
    }
    return 0;
}
static void t_sem(void)
{
    pthread_t t[64]; g_inside = 0; g_maxin = 0;
    sem_init(&g_sem, 0, 3);
    for (int i = 0; i < N; i++) pthread_create(&t[i], 0, f_sem, 0);
    for (int i = 0; i < N; i++) pthread_join(t[i], 0);
    int v = -1; sem_getvalue(&g_sem, &v);
    sem_destroy(&g_sem);
    CHECK("semaphore", g_maxin <= 3 && g_maxin >= 1 && v == 3, "max inside=%d final=%d", g_maxin, v);
}

/* ---- cancellation ---- */
static int g_cpipe[2]; static volatile int g_cleanup;
static void cleanup(void *a) { (void)a; __atomic_add_fetch(&g_cleanup, 1, __ATOMIC_SEQ_CST); }
static void *f_cancel_read(void *a)
{
    (void)a; char b;
    pthread_cleanup_push(cleanup, 0);
    read(g_cpipe[0], &b, 1);                 /* blocks: a cancellation point */
    pthread_cleanup_pop(0);
    return (void *)1;
}
static pthread_mutex_t g_cm = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cc = PTHREAD_COND_INITIALIZER;
static void unlock_m(void *a) { pthread_mutex_unlock((pthread_mutex_t *)a); cleanup(0); }
static void *f_cancel_cond(void *a)
{
    (void)a;
    pthread_mutex_lock(&g_cm);
    pthread_cleanup_push(unlock_m, &g_cm);
    for (;;) pthread_cond_wait(&g_cc, &g_cm);
    pthread_cleanup_pop(1);
    return 0;
}
static void *f_cancel_sleep(void *a) { (void)a; pthread_cleanup_push(cleanup, 0); for (;;) sleep(100); pthread_cleanup_pop(0); return 0; }
static volatile long g_spin;
static void *f_cancel_async(void *a)
{
    (void)a; pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, 0);
    pthread_cleanup_push(cleanup, 0);
    for (;;) g_spin++;
    pthread_cleanup_pop(0);
    return 0;
}
static void *f_cancel_disabled(void *a)
{
    (void)a; int old;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
    msleep(100);                             /* not cancelled here */
    pthread_setcancelstate(old, 0);
    pthread_testcancel();                    /* acted on here */
    return (void *)2;
}
static void t_cancel(void)
{
    pipe(g_cpipe); g_cleanup = 0;
    pthread_t a, b, c, d, e; void *ra = 0, *rb = 0, *rc = 0, *rd = 0, *re = 0;
    pthread_create(&a, 0, f_cancel_read, 0);
    pthread_create(&b, 0, f_cancel_cond, 0);
    pthread_create(&c, 0, f_cancel_sleep, 0);
    pthread_create(&d, 0, f_cancel_async, 0);
    pthread_create(&e, 0, f_cancel_disabled, 0);
    msleep(80);
    long t0 = now_ms();
    pthread_cancel(a); pthread_cancel(b); pthread_cancel(c); pthread_cancel(d); pthread_cancel(e);
    pthread_join(a, &ra); pthread_join(b, &rb); pthread_join(c, &rc); pthread_join(d, &rd); pthread_join(e, &re);
    long dt = now_ms() - t0;
    close(g_cpipe[0]); close(g_cpipe[1]);
    int ok = ra == PTHREAD_CANCELED && rb == PTHREAD_CANCELED && rc == PTHREAD_CANCELED &&
             rd == PTHREAD_CANCELED && re == PTHREAD_CANCELED && g_cleanup == 4;
    CHECK("pthread_cancel", ok, "read=%p cond=%p sleep=%p async=%p disabled=%p cleanups=%d in %ld ms",
          ra, rb, rc, rd, re, g_cleanup, dt);
    /* the mutex the cond waiter held was released by its cleanup handler */
    CHECK("cancel-cond-mutex-released", pthread_mutex_trylock(&g_cm) == 0 && pthread_mutex_unlock(&g_cm) == 0, "trylock");
}

/* ---- join 10000 short threads ---- */
static void *f_short(void *a) { return (void *)((long)a + 1); }
static void t_join10k(void)
{
    long t0 = now_ms(); int bad = 0, made = 0;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 16384);
    for (int i = 0; i < 10000; i += 4) {
        pthread_t t[4];
        for (int k = 0; k < 4; k++) { if (pthread_create(&t[k], &at, f_short, (void *)(long)(i + k))) { bad++; t[k] = 0; } else made++; }
        for (int k = 0; k < 4; k++) { void *r; if (t[k] && (pthread_join(t[k], &r) || (long)r != i + k + 1)) bad++; }
    }
    void *probe = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK("join-10000", made == 10000 && bad == 0, "made=%d bad=%d in %ld ms (next mmap %p)", made, bad, now_ms() - t0, probe);
    if (probe != MAP_FAILED) munmap(probe, 4096);
}

/* ---- detached threads unmap their own stacks ---- */
static volatile int g_det;
static void *f_det(void *a) { (void)a; __atomic_add_fetch(&g_det, 1, __ATOMIC_SEQ_CST); return 0; }
static int maps_lines(void)
{
    FILE *f = fopen("/proc/self/maps", "r"); int n = 0, c;
    if (!f) return -1;
    while ((c = fgetc(f)) != EOF) if (c == '\n') n++;
    fclose(f);
    return n;
}
static void t_detach(void)
{
    g_det = 0;
    int before = maps_lines();
    for (int i = 0; i < 500; i++) {
        pthread_t t; pthread_attr_t at; pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, f_det, 0)) break;
        if ((i & 15) == 15) msleep(5);
    }
    for (int i = 0; i < 200 && g_det < 500; i++) msleep(10);
    msleep(50);
    int after = maps_lines();
    CHECK("detached-500", g_det == 500 && after <= before + 4, "ran=%d maps %d -> %d", g_det, before, after);
}

/* ---- __thread ---- */
static __thread long t_tls = 7;
static void *f_tls(void *a)
{
    long id = (long)a;
    if (t_tls != 7) return (void *)1;
    t_tls = id * 1000;
    for (int i = 0; i < 2000; i++) { if (t_tls != id * 1000) return (void *)1; if ((i & 63) == 0) sched_yield(); }
    return 0;
}
static void t_tls_test(void)
{
    pthread_t t[64]; int bad = 0;
    for (long i = 0; i < N; i++) pthread_create(&t[i], 0, f_tls, (void *)(i + 1));
    for (int i = 0; i < N; i++) { void *r; pthread_join(t[i], &r); if (r) bad++; }
    CHECK("thread-local", bad == 0 && t_tls == 7, "bad=%d main=%ld", bad, t_tls);
}

/* ---- pthread keys (destructors) + once ---- */
static pthread_key_t g_key; static volatile int g_dtor;
static void dtor(void *v) { if (v) __atomic_add_fetch(&g_dtor, 1, __ATOMIC_SEQ_CST); }
static void *f_key(void *a) { pthread_setspecific(g_key, a); return pthread_getspecific(g_key) == a ? 0 : (void *)1; }
static pthread_once_t g_once = PTHREAD_ONCE_INIT; static volatile int g_once_n;
static void once_fn(void) { __atomic_add_fetch(&g_once_n, 1, __ATOMIC_SEQ_CST); msleep(20); }
static void *f_once(void *a) { (void)a; pthread_once(&g_once, once_fn); return g_once_n == 1 ? 0 : (void *)1; }
static void t_tsd(void)
{
    pthread_key_create(&g_key, dtor); g_dtor = 0;
    pthread_t t[64]; int bad = 0;
    for (long i = 0; i < N; i++) pthread_create(&t[i], 0, f_key, (void *)(i + 1));
    for (int i = 0; i < N; i++) { void *r; pthread_join(t[i], &r); if (r) bad++; }
    CHECK("tsd-destructors", bad == 0 && g_dtor == N, "bad=%d dtors=%d", bad, g_dtor);
    for (int i = 0; i < N; i++) pthread_create(&t[i], 0, f_once, 0);
    bad = 0; for (int i = 0; i < N; i++) { void *r; pthread_join(t[i], &r); if (r) bad++; }
    CHECK("pthread_once", bad == 0 && g_once_n == 1, "bad=%d calls=%d", bad, g_once_n);
}

/* ---- malloc + stdio from many threads ---- */
static void *f_malloc(void *a)
{
    unsigned s = (unsigned)(long)a * 2654435761u;
    void *p[64] = {0};
    for (int i = 0; i < 20000; i++) {
        s = s * 1103515245 + 12345;
        int k = (s >> 8) & 63;
        if (p[k]) { if (((unsigned char *)p[k])[0] != (unsigned char)k) return (void *)1; free(p[k]); p[k] = 0; }
        else { size_t n = 1 + ((s >> 16) % 3000); p[k] = malloc(n); if (!p[k]) return (void *)2; memset(p[k], k, n); }
    }
    for (int k = 0; k < 64; k++) free(p[k]);
    return 0;
}
static void t_malloc(void)
{
    pthread_t t[64]; int bad = 0;
    for (long i = 0; i < N; i++) pthread_create(&t[i], 0, f_malloc, (void *)(i + 1));
    for (int i = 0; i < N; i++) { void *r; pthread_join(t[i], &r); if (r) bad++; }
    CHECK("malloc-threads", bad == 0, "bad=%d", bad);
}
static void *f_stdio(void *a)
{
    FILE *f = (FILE *)a;
    for (int i = 0; i < 500; i++) fprintf(f, "line-%04d-%s\n", i, "abcdefghijklmnopqrstuvwxyz");
    return 0;
}
static void t_stdio(void)
{
    char path[] = "/tmp/thrstress.XXXXXX"; int fd = mkstemp(path);
    FILE *f = fdopen(fd, "w+");
    pthread_t t[64];
    for (int i = 0; i < N; i++) pthread_create(&t[i], 0, f_stdio, f);
    for (int i = 0; i < N; i++) pthread_join(t[i], 0);
    fflush(f); rewind(f);
    char line[128]; int good = 0, badl = 0;
    while (fgets(line, sizeof line, f)) { if (strlen(line) == 37 && !strncmp(line, "line-", 5)) good++; else badl++; }
    fclose(f); unlink(path);
    CHECK("stdio-locking", good == N * 500 && badl == 0, "good=%d torn=%d", good, badl);
}

/* ---- signals ---- */
static volatile pid_t g_sigtid; static volatile int g_sigs;
static void h_usr1(int s) { (void)s; g_sigtid = gettid_(); __atomic_add_fetch(&g_sigs, 1, __ATOMIC_SEQ_CST); }
static volatile pid_t g_wtid; static volatile int g_wgo;
static void *f_sigwait(void *a) { (void)a; g_wtid = gettid_(); while (!g_wgo) msleep(5); return 0; }
static void t_signal(void)
{
    signal(SIGUSR1, h_usr1);
    pthread_t t; g_wgo = 0; g_wtid = 0; g_sigs = 0;
    pthread_create(&t, 0, f_sigwait, 0);
    while (!g_wtid) msleep(2);
    pthread_kill(t, SIGUSR1);
    for (int i = 0; i < 100 && !g_sigs; i++) msleep(5);
    g_wgo = 1; pthread_join(t, 0);
    CHECK("pthread_kill", g_sigs == 1 && g_sigtid == g_wtid, "n=%d in %d (target %d)", g_sigs, g_sigtid, g_wtid);
    /* pthread_sigmask: main blocks, a thread does not -> the thread runs it */
    sigset_t m; sigemptyset(&m); sigaddset(&m, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &m, 0);
    g_wgo = 0; g_wtid = 0; g_sigs = 0;
    pthread_create(&t, 0, f_sigwait, 0);     /* inherits the mask */
    while (!g_wtid) msleep(2);
    kill(getpid(), SIGUSR1);
    msleep(30);
    int pend = g_sigs;
    pthread_sigmask(SIG_UNBLOCK, &m, 0);
    for (int i = 0; i < 100 && !g_sigs; i++) msleep(5);
    g_wgo = 1; pthread_join(t, 0);
    CHECK("process-signal-routing", pend == 0 && g_sigs == 1 && g_sigtid == gettid_(), "pending=%d n=%d tid=%d", pend, g_sigs, g_sigtid);
    signal(SIGUSR1, SIG_DFL);
}

/* ---- kill / SIGINT / exec of a multi-threaded child ---- */
static void *f_forever(void *a) { (void)a; for (;;) pause(); return 0; }
static void *f_busy(void *a) { (void)a; for (;;) g_spin++; return 0; }
static pid_t spawn_threaded(int n)
{
    pid_t pid = fork();
    if (pid == 0) {
        pthread_t t;
        for (int i = 0; i < n; i++) pthread_create(&t, 0, (i & 1) ? f_busy : f_forever, 0);
        for (;;) pause();
    }
    return pid;
}
static void t_kill(void)
{
    pid_t pid = spawn_threaded(N);
    msleep(150);
    kill(pid, SIGKILL);
    int st = 0; pid_t w = waitpid(pid, &st, 0);
    CHECK("kill-multithreaded", w == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "w=%d st=%#x", w, st);
}
static void t_sigint(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        pthread_t t;
        for (int i = 0; i < N; i++) pthread_create(&t, 0, (i & 1) ? f_busy : f_forever, 0);
        for (;;) pause();
    }
    setpgid(pid, pid);
    msleep(150);
    kill(-pid, SIGINT);                      /* to the process group */
    int st = 0; pid_t w = waitpid(pid, &st, 0);
    CHECK("sigint-to-group", w == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGINT, "w=%d st=%#x", w, st);
}
static void *f_exec(void *a)
{
    (void)a; msleep(50);
    char *av[] = { g_self, "execd", "23", 0 };
    execv(g_self, av);
    _exit(99);
}
static void t_exec(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        pthread_t t;
        for (int i = 0; i < N; i++) pthread_create(&t, 0, (i & 1) ? f_busy : f_forever, 0);
        pthread_create(&t, 0, f_exec, 0);
        for (;;) pause();
    }
    int st = 0; pid_t w = waitpid(pid, &st, 0);
    CHECK("execve-from-thread", w == pid && WIFEXITED(st) && WEXITSTATUS(st) == 23, "w=%d st=%#x", w, st);
}

/* ---- errno is per thread ---- */
static void *f_errno(void *a)
{
    long id = (long)a;
    for (int i = 0; i < 1000; i++) {
        errno = (int)id;
        if ((i & 31) == 0) sched_yield();
        if (errno != (int)id) return (void *)1;
    }
    close(-1);
    return errno == EBADF ? 0 : (void *)2;
}
static void t_errno(void)
{
    pthread_t t[64]; int bad = 0;
    for (long i = 0; i < N; i++) pthread_create(&t[i], 0, f_errno, (void *)(i + 100));
    for (int i = 0; i < N; i++) { void *r; pthread_join(t[i], &r); if (r) bad++; }
    CHECK("errno-per-thread", bad == 0, "bad=%d", bad);
}

static void spin_main(int n)
{
    pthread_t t;
    for (int i = 0; i < n; i++) pthread_create(&t, 0, (i & 1) ? f_busy : f_forever, 0);
    printf("TS SPIN pid=%d threads=%d\n", getpid(), n + 1); fflush(stdout);
    for (;;) pause();
}

int main(int argc, char **argv)
{
    snprintf(g_self, sizeof g_self, "%s", argv[0]);
    if (argc >= 3 && !strcmp(argv[1], "execd")) return atoi(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "spin")) { spin_main(atoi(argv[2])); return 0; }
    const char *w = argc >= 2 ? argv[1] : "all";
    if (argc >= 3) N = atoi(argv[2]);
    if (N < 2) N = 2;
    if (N > 64) N = 64;
    struct { const char *n; void (*f)(void); } T[] = {
        { "mutex", t_mutex }, { "condvar", t_condvar }, { "rwlock", t_rwlock }, { "barrier", t_barrier },
        { "sem", t_sem }, { "cancel", t_cancel }, { "join10k", t_join10k }, { "detach", t_detach },
        { "tls", t_tls_test }, { "tsd", t_tsd }, { "malloc", t_malloc }, { "stdio", t_stdio },
        { "signal", t_signal }, { "kill", t_kill }, { "sigint", t_sigint }, { "exec", t_exec },
        { "errno", t_errno },
    };
    long t0 = now_ms();
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(w, "all") || !strcmp(w, T[i].n)) T[i].f();
    printf("TS DONE pass=%d fail=%d N=%d ms=%ld\n", g_pass, g_fail, N, now_ms() - t0);
    fflush(stdout);
    return g_fail ? 1 : 0;
}
