/*
 * mtest — the MUSL-BASE gate program (U1, tools/musl_base_test.sh).
 * Built with tools/secos-cc against the musl sysroot: one static-PIE binary,
 * signed, run on SecOS from a test disk. SPDX-License-Identifier: MIT
 *
 *   mtest all [DIR]      every self-checking test (files in DIR, default
 *                        /tmp/mt; symlinks need an ext2/ext4 DIR); "MT PASS <name>" or
 *                        "MT FAIL <name>: <why>", then "MT DONE pass=N fail=M"
 *   mtest addr           prints "MT ADDR <&main> <&tls>" (ASLR check)
 *   mtest tls N          N yields with a __thread value checked each time
 *                        (FS base across context switches; run several at once)
 *   mtest enosys         calls an unsupported Linux syscall (inotify_init1)
 *   mtest spawn          starts "mtest child 3 noenv" with SYS_SPAWN (libsecos):
 *                        the kernel-made stack + auxv path, no environment
 *   mtest child CODE [noenv]  exec target: checks argv/envp, exits with CODE
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <setjmp.h>
#include <signal.h>
#include <locale.h>
#include <wchar.h>
#include <iconv.h>
#include <time.h>
#include <sched.h>
#include <spawn.h>
#include <math.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <secos/secos.h>

static int g_pass, g_fail;
static char g_self[256];
static const char *g_work = "/tmp/mt";   /* t_files directory (needs symlinks: ext2/ext4) */
static char *wp(const char *rel) { static char b[8][256]; static int k; k = (k + 1) & 7; snprintf(b[k], 256, "%s%s", g_work, rel); return b[k]; }

static void result(const char *name, int ok, const char *why)
{
    if (ok) { printf("MT PASS %s\n", name); g_pass++; }
    else    { printf("MT FAIL %s: %s\n", name, why ? why : "?"); g_fail++; }
    fflush(stdout);
}
#define CHECK(name, cond, ...) do { char _w[160]; snprintf(_w, sizeof _w, __VA_ARGS__); result(name, (cond), _w); } while (0)

/* ---- stdio formatting ---- */
static void t_printf(void)
{
    char b[256];
    snprintf(b, sizeof b, "%.3f|%e|%g|%g|%x|%s|%5d|%-5d|%+.2e|%a",
             3.14159265, 12345.678, 0.0001, 1e20, 0xbeef, "str", 42, 7, -0.000123, 1.0);
    const char *want = "3.142|1.234568e+04|0.0001|1e+20|beef|str|   42|7    |-1.23e-04|0x1p+0";
    CHECK("printf", strcmp(b, want) == 0, "got '%.140s'", b);
    snprintf(b, sizeof b, "%.17g %.17g %.0f", 0.1, 1.0 / 3.0, 1e22);
    CHECK("printf-exact", strcmp(b, "0.10000000000000001 0.33333333333333331 10000000000000000000000") == 0, "got '%.140s'", b);
    double d = 0; int i = 0; char s[16] = { 0 }; float f = 0;
    int n = sscanf("3.25 -17 abc 1e-3", "%lf %d %15s %f", &d, &i, s, &f);
    CHECK("scanf", n == 4 && d == 3.25 && i == -17 && !strcmp(s, "abc") && fabsf(f - 1e-3f) < 1e-9f,
          "n=%d d=%g i=%d s=%s f=%g", n, d, i, s, f);
    CHECK("strtod", strtod("0x1.8p1", 0) == 3.0 && strtod("2.5e-3", 0) == 0.0025 && isinf(strtod("inf", 0)),
          "strtod mismatch");
    CHECK("math", fabs(sqrt(2.0) - 1.4142135623730951) < 1e-15 && fabs(sin(M_PI / 6) - 0.5) < 1e-15 &&
          fabs(log(exp(3.0)) - 3.0) < 1e-15, "libm mismatch");
}

/* ---- TLS, errno, auxv ---- */
static __thread int t_tls = 1234;
static __thread char t_tlsbuf[64] = "tls-initialised";
static void t_tls_errno(void)
{
    CHECK("tls", t_tls == 1234 && !strcmp(t_tlsbuf, "tls-initialised"), "tls=%d buf=%s", t_tls, t_tlsbuf);
    errno = 0;
    int fd = open("/nonexistent/file", O_RDONLY);
    int e1 = errno;
    close(-1);
    int e2 = errno;
    CHECK("errno", fd == -1 && e1 == ENOENT && e2 == EBADF && !strcmp(strerror(ENOENT), "No such file or directory"),
          "fd=%d e1=%d e2=%d", fd, e1, e2);
    unsigned long rnd = getauxval(AT_RANDOM), pg = getauxval(AT_PAGESZ), ph = getauxval(AT_PHDR);
    const char *efn = (const char *)getauxval(AT_EXECFN);
    CHECK("auxv", rnd && pg == 4096 && ph && !getauxval(AT_SECURE) && efn && strstr(efn, "mtest") &&
          getauxval(AT_UID) == getuid() && getauxval(AT_EUID) == geteuid() && sysconf(_SC_PAGESIZE) == 4096,
          "rnd=%lx pg=%lu ph=%lx efn=%s", rnd, pg, ph, efn ? efn : "(null)");
}

/* ---- locale, wide chars, iconv ---- */
static void t_locale(void)
{
    const char *l = setlocale(LC_ALL, "C.UTF-8");
    wchar_t w[8];
    size_t n = mbstowcs(w, "\xc3\xa0\xc3\xa8\xe2\x82\xac", 8);           /* à è € */
    CHECK("locale-utf8", l && n == 3 && w[0] == 0xe0 && w[1] == 0xe8 && w[2] == 0x20ac &&
          wcwidth(0xe0) == 1 && wcwidth(0x4e00) == 2 && wcwidth(0x301) == 0,
          "l=%s n=%zu", l ? l : "(null)", n);
    iconv_t cd = iconv_open("ISO-8859-1", "UTF-8");
    char in[] = "a\xc3\xa0\xc3\xa9z", out[16] = { 0 };
    char *ip = in, *op = out; size_t il = strlen(in), ol = sizeof out;
    size_t r = cd == (iconv_t)-1 ? (size_t)-1 : iconv(cd, &ip, &il, &op, &ol);
    if (cd != (iconv_t)-1) iconv_close(cd);
    int ok1 = r != (size_t)-1 && op - out == 4 && !memcmp(out, "a\xe0\xe9z", 4);
    cd = iconv_open("UTF-8", "ISO-8859-1");
    char back[16] = { 0 }; ip = out; il = 4; op = back; ol = sizeof back;
    r = cd == (iconv_t)-1 ? (size_t)-1 : iconv(cd, &ip, &il, &op, &ol);
    if (cd != (iconv_t)-1) iconv_close(cd);
    CHECK("iconv", ok1 && r != (size_t)-1 && !strcmp(back, in), "utf8->latin1=%d back=%s", ok1, back);
    setlocale(LC_ALL, "C");
}

/* ---- qsort / bsearch ---- */
static int cmpint(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
static void t_sort(void)
{
    enum { N = 20000 };
    int *v = malloc(N * sizeof *v);
    if (!v) { result("qsort", 0, "malloc"); return; }
    srand(12345);
    for (int i = 0; i < N; i++) v[i] = rand() % 100000;
    v[N / 2] = 777777;
    qsort(v, N, sizeof *v, cmpint);
    int sorted = 1;
    for (int i = 1; i < N; i++) if (v[i - 1] > v[i]) sorted = 0;
    int key = 777777;
    int *hit = bsearch(&key, v, N, sizeof *v, cmpint);
    key = 100001;
    int *miss = bsearch(&key, v, N, sizeof *v, cmpint);
    CHECK("qsort-bsearch", sorted && hit && *hit == 777777 && !miss, "sorted=%d hit=%p", sorted, (void *)hit);
    free(v);
}

/* ---- malloc stress (mallocng) ---- */
static void t_malloc(void)
{
    enum { SLOTS = 512, OPS = 100000 };
    static unsigned char *p[SLOTS];
    static size_t sz[SLOTS];
    unsigned seed = 42, bad = 0, nulls = 0, big = 0;
    for (int op = 0; op < OPS; op++) {
        seed = seed * 1103515245u + 12345u;
        int i = (int)((seed >> 8) % SLOTS);
        if (p[i]) {
            for (size_t k = 0; k < sz[i]; k += 97) if (p[i][k] != (unsigned char)(i + k)) bad++;
            free(p[i]); p[i] = 0;
            continue;
        }
        unsigned r = (seed >> 16) & 1023;
        size_t n = r < 900 ? 1 + (seed % 256)            /* mostly small */
                 : r < 1015 ? 256 + (seed % 8192)        /* medium */
                 : 65536 + (seed % 200000);              /* a few large (mmap) */
        if (n >= 65536) {                                /* keep few large blocks live */
            if (big >= 4) n = 64;
            else big++;
        }
        p[i] = malloc(n);
        if (!p[i]) { nulls++; continue; }
        sz[i] = n;
        for (size_t k = 0; k < n; k += 97) p[i][k] = (unsigned char)(i + k);
        if (op % 50 == 0) {                              /* realloc a live block */
            unsigned char *q = realloc(p[i], n * 2);
            if (q) {
                p[i] = q;
                for (size_t k = (n + 96) / 97 * 97; k < 2 * n; k += 97) q[k] = (unsigned char)(i + k);
                sz[i] = 2 * n;
            }
        }
    }
    for (int i = 0; i < SLOTS; i++) if (p[i]) {
        if (sz[i] >= 65536) big--;
        free(p[i]);
    }
    void *c = calloc(1000, 1000);
    int zero = c != 0;
    for (int k = 0; c && k < 1000000; k += 4093) if (((unsigned char *)c)[k]) zero = 0;
    free(c);
    CHECK("malloc-stress", !bad && !nulls && zero, "%d ops: corrupt=%u null=%u calloc-zero=%d", OPS, bad, nulls, zero);
}

/* ---- setjmp ---- */
static jmp_buf g_jb;
static void jump(int depth) { if (depth == 0) longjmp(g_jb, 7); jump(depth - 1); }
static void t_setjmp(void)
{
    volatile int count = 0;
    int r = setjmp(g_jb);
    if (r == 0) { count++; jump(10); }
    CHECK("setjmp", r == 7 && count == 1, "r=%d count=%d", r, count);
}

/* ---- signals ---- */
static volatile sig_atomic_t g_sig, g_sig_tls;
static void on_usr1(int s) { g_sig = s; g_sig_tls = t_tls; }
static void t_signal(void)
{
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    int r1 = sigaction(SIGUSR1, &sa, 0);
    int r2 = sigaction(SIGUSR1, 0, &old);
    raise(SIGUSR1);
    int got = g_sig;
    /* blocked while masked, delivered on unblock */
    sigset_t set, prev;
    sigemptyset(&set); sigaddset(&set, SIGUSR1);
    g_sig = 0;
    sigprocmask(SIG_BLOCK, &set, &prev);
    kill(getpid(), SIGUSR1);
    int while_blocked = g_sig;
    sigprocmask(SIG_SETMASK, &prev, 0);
    int after = g_sig;
    double x = 1.5; x = x * x;                           /* FPU state survives the handler */
    signal(SIGUSR1, SIG_DFL);
    CHECK("signal", !r1 && !r2 && old.sa_handler == on_usr1 && got == SIGUSR1 && g_sig_tls == 1234 &&
          while_blocked == 0 && after == SIGUSR1 && x == 2.25,
          "r=%d/%d got=%d blocked=%d after=%d tls=%d", r1, r2, got, while_blocked, after, (int)g_sig_tls);
    CHECK("sigset-api", sigismember(&set, SIGUSR1) == 1 && sigismember(&set, SIGUSR2) == 0, "sigset");
}

/* ---- fork / exec / wait, posix_spawn, pipes ---- */
static void t_process(void)
{
    int fds[2];
    if (pipe(fds) != 0) { result("fork-pipe", 0, strerror(errno)); return; }
    t_tls = 99;                                           /* the child inherits the FS base + TLS */
    pid_t c = fork();
    if (c == 0) {
        close(fds[0]);
        char m[32]; int n = snprintf(m, sizeof m, "child tls=%d", t_tls);
        write(fds[1], m, n);
        _exit(5);
    }
    t_tls = 1234;
    close(fds[1]);
    char buf[64] = { 0 }; ssize_t n = read(fds[0], buf, sizeof buf - 1);
    close(fds[0]);
    int st = 0; pid_t w = waitpid(c, &st, 0);
    CHECK("fork-pipe-wait", c > 0 && w == c && WIFEXITED(st) && WEXITSTATUS(st) == 5 && n > 0 &&
          !strcmp(buf, "child tls=99") && t_tls == 1234,
          "c=%d w=%d st=%x buf='%s'", (int)c, (int)w, st, buf);

    c = fork();
    if (c == 0) {
        char *av[] = { g_self, "child", "7", 0 };
        char *ev[] = { "MTEST_ENV=from-exec", 0 };
        execve(g_self, av, ev);
        _exit(100 + errno);
    }
    w = waitpid(c, &st, 0);
    CHECK("fork-exec-wait", w == c && WIFEXITED(st) && WEXITSTATUS(st) == 7, "w=%d st=%x", (int)w, st);

    pid_t sp = 0;
    char *av[] = { g_self, "child", "9", 0 };
    char *ev[] = { "MTEST_ENV=from-exec", 0 };
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 3, "/dev/null", O_RDONLY, 0);
    int e = posix_spawn(&sp, g_self, &fa, 0, av, ev);
    posix_spawn_file_actions_destroy(&fa);
    st = 0; w = e ? -1 : waitpid(sp, &st, 0);
    CHECK("posix_spawn", !e && w == sp && WIFEXITED(st) && WEXITSTATUS(st) == 9, "e=%d w=%d st=%x", e, (int)w, st);
    e = posix_spawn(&sp, "/nonexistent/prog", 0, 0, av, ev);
    CHECK("posix_spawn-enoent", e == ENOENT, "e=%d", e);

    c = fork();
    if (c == 0) { raise(SIGTERM); _exit(1); }
    w = waitpid(c, &st, 0);
    CHECK("wait-signaled", w == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "st=%x", st);
    CHECK("wait-echild", waitpid(-1, &st, WNOHANG) == -1 && errno == ECHILD, "errno=%d", errno);
}

/* ---- files, directories ---- */
static int name_in(char names[][32], int n, const char *s)
{
    for (int i = 0; i < n; i++) if (!strcmp(names[i], s)) return 1;
    return 0;
}
static void t_files(void)
{
    mkdir(wp(""), 0755);
    FILE *f = fopen(wp("/f.txt"), "w");
    int ok = f != 0;
    for (int i = 0; f && i < 1000; i++) fprintf(f, "line %d %.2f\n", i, i / 4.0);
    if (f) ok &= fclose(f) == 0;
    struct stat st;
    ok &= stat(wp("/f.txt"), &st) == 0 && S_ISREG(st.st_mode);
    long size = (long)st.st_size;
    f = fopen(wp("/f.txt"), "r");
    char line[64] = { 0 }; int lines = 0, last = -1;
    while (f && fgets(line, sizeof line, f)) { lines++; sscanf(line, "line %d", &last); }
    if (f) { fseek(f, 0, SEEK_END); ok &= ftell(f) == size; fseek(f, 5, SEEK_SET); ok &= fgetc(f) == '0'; fclose(f); }
    CHECK("stdio-files", ok && lines == 1000 && last == 999, "ok=%d lines=%d last=%d size=%ld", ok, lines, last, size);

    int fd = open(wp("/g"), O_CREAT | O_EXCL | O_WRONLY, 0640);
    int ex = open(wp("/g"), O_CREAT | O_EXCL | O_WRONLY, 0640) == -1 && errno == EEXIST;
    write(fd, "abcdef", 6);
    close(fd);
    fd = open(wp("/g"), O_RDONLY);
    char pb[4] = { 0 };
    ssize_t pr = pread(fd, pb, 3, 2);
    off_t pos = lseek(fd, 0, SEEK_CUR);
    close(fd);
    int ren = rename(wp("/g"), wp("/h")) == 0 && access(wp("/h"), R_OK) == 0 && access(wp("/g"), F_OK) == -1;
    int lnk = symlink("h", wp("/l")) == 0;
    char lb[16] = { 0 }; ssize_t ll = readlink(wp("/l"), lb, sizeof lb - 1);
    struct stat ls; int lst = lstat(wp("/l"), &ls) == 0 && S_ISLNK(ls.st_mode);
    CHECK("posix-files", fd >= 0 && ex && pr == 3 && !strcmp(pb, "cde") && pos == 0 && ren &&
          lnk && ll == 1 && lb[0] == 'h' && lst,
          "ex=%d pr=%zd pb=%s pos=%ld ren=%d lnk=%d ll=%zd lst=%d", ex, pr, pb, (long)pos, ren, lnk, ll, lst);

    mkdir(wp("/sub"), 0755);
    DIR *d = opendir(wp(""));
    char names[16][32]; int n = 0, dirs = 0, links = 0;
    struct dirent *de;
    while (d && (de = readdir(d)) && n < 16) {
        snprintf(names[n++], 32, "%.31s", de->d_name);
        if (de->d_type == DT_DIR) dirs++;
        if (de->d_type == DT_LNK) links++;
    }
    int again = 0;
    if (d) { rewinddir(d); while (readdir(d)) again++; closedir(d); }
    CHECK("opendir", d && n == 6 && name_in(names, n, ".") && name_in(names, n, "..") &&
          name_in(names, n, "f.txt") && name_in(names, n, "h") && name_in(names, n, "l") &&
          name_in(names, n, "sub") && dirs == 3 && links == 1 && again == 6,
          "n=%d dirs=%d links=%d again=%d", n, dirs, links, again);
    int bin = 0;
    d = opendir("/bin");
    while (d && (de = readdir(d))) if (!strcmp(de->d_name, "bash")) bin = 1;
    if (d) closedir(d);
    CHECK("opendir-bin", bin, "bash not listed in /bin");

    char cwd[256] = { 0 };
    int cd = chdir(wp("/sub")) == 0 && getcwd(cwd, sizeof cwd) && !strcmp(cwd, wp("/sub"));
    int rel = cd && chdir("..") == 0 && access("f.txt", F_OK) == 0;
    CHECK("chdir-getcwd", cd && rel, "cwd=%s rel=%d", cwd, rel);
    unlink(wp("/l")); unlink(wp("/h")); unlink(wp("/f.txt")); rmdir(wp("/sub"));
    int gone = rmdir(wp("")) == 0 && access(wp(""), F_OK) == -1 && errno == ENOENT;
    chdir("/");
    CHECK("unlink-rmdir", gone, "errno=%d", errno);
}

/* ---- time, misc ---- */
static void t_misc(void)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    struct timespec req = { 0, 20 * 1000 * 1000 };
    int ns = nanosleep(&req, 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long el = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    CHECK("time", ns == 0 && el >= 19 && time(0) > 1600000000, "nanosleep=%d elapsed=%ldms time=%ld", ns, el, (long)time(0));
    struct utsname u;
    CHECK("uname", uname(&u) == 0 && u.sysname[0], "uname");
    unsigned char r[32] = { 0 }; int nz = 0;
    getentropy(r, sizeof r);
    for (int i = 0; i < 32; i++) nz |= r[i];
    CHECK("getentropy", nz, "all zero");
    void *m = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int mo = m != MAP_FAILED;
    if (mo) { memset(m, 0x5a, 3 * 4096); mo = mprotect(m, 4096, PROT_READ) == 0 && munmap(m, 3 * 4096) == 0; }
    void *wx = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK("mmap", mo && wx == MAP_FAILED, "mmap ok=%d wx=%p", mo, wx);
}

/* 8 MiB stack from PT_GNU_STACK: 2 MiB of frames */
static int deep(int n) { volatile char pad[1024]; pad[0] = (char)n; return n ? deep(n - 1) + pad[0] - (char)n : 0; }
static void t_stack(void) { CHECK("big-stack", deep(2048) == 0, "recursion"); }

static int run_all(const char *work)
{
    if (work) g_work = work;
    t_printf(); t_tls_errno(); t_locale(); t_sort(); t_malloc(); t_setjmp();
    t_signal(); t_process(); t_files(); t_misc(); t_stack();
    printf("MT DONE pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    snprintf(g_self, sizeof g_self, "%s", argv[0]);
    if (argc > 1 && !strcmp(argv[1], "addr")) {
        printf("MT ADDR %p %p\n", (void *)main, (void *)&t_tls);
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "tls")) {
        int n = atoi(argv[2]), pid = getpid(), bad = 0;
        t_tls = pid;
        int *where = &t_tls;
        for (int i = 0; i < n; i++) {
            sched_yield();
            if (t_tls != pid || &t_tls != where) bad++;
            t_tls = pid;
        }
        printf("MT TLS pid=%d bad=%d\n", pid, bad);
        return bad ? 1 : 0;
    }
    if (argc > 1 && !strcmp(argv[1], "enosys")) {
        long r = syscall(SYS_inotify_init1, 0);
        printf("MT ENOSYS r=%ld errno=%d\n", r, errno);
        return (r == -1 && errno == ENOSYS) ? 0 : 1;
    }
    if (argc > 1 && !strcmp(argv[1], "spawn")) {
        char *av[] = { g_self, "child", "3", "noenv", 0 };
        int pid = secos_spawn(g_self, av);
        int st = pid > 0 ? secos_wait(pid) : -1;
        printf("MT SPAWN pid=%d status=%x\n", pid, st);
        return (pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 3) ? 0 : 1;
    }
    if (argc > 3 && !strcmp(argv[1], "child") && !strcmp(argv[3], "noenv")) {
        /* spawned by the kernel: argv only, and musl initialised from the auxv */
        if (getenv("MTEST_ENV") || t_tls != 1234 || getauxval(AT_PAGESZ) != 4096 || !getauxval(AT_RANDOM)) return 60;
        return atoi(argv[2]);
    }
    if (argc > 2 && !strcmp(argv[1], "child")) {
        const char *e = getenv("MTEST_ENV");
        if (!e || strcmp(e, "from-exec")) return 50;
        return atoi(argv[2]);
    }
    return run_all(argc > 2 && !strcmp(argv[1], "all") ? argv[2] : 0);
}
