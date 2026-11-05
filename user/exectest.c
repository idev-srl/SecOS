/* SecOS: in-place execve (SYS_EXECVE) regression test.
 *
 * Installed by the EXECVE_DEMO gate as /bin/exectest, next to a tampered copy
 * (/bin/exectest_tampered), an unsigned build (/bin/exectest_unsigned) and a
 * text script (/bin/exectest_script). Roles, picked by argv[1]:
 *   (none)  : failure cases (the caller must continue), then a mmap/heap-heavy,
 *             COW-sharing image execs itself as "child" with argv + envp.
 *   child   : checks what must survive exec (pid, parent link, argv, envp, plain
 *             fds, signal mask, pending signals) and what must not (FD_CLOEXEC
 *             fds, caught handlers); then 200 fork+exec cycles with a stable PMM
 *             free count.
 *   leaf    : exits 0 at once (the fork+exec cycle body).
 * Markers: "[EXECVE] ... OK" / "... FAIL" on fd 1 (console + debugcon).
 * SPDX-License-Identifier: MIT */
#include "libsecos.h"
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>

#define SIGUSR1 10
#define SIGUSR2 12
#define SIGTERM 15
#define LEAK_CYCLES 200
#define LEAK_WARMUP 20

static void say(const char* s) { write(1, s, strlen(s)); }
/* One write per result line: kernel debugcon lines (preemption traces) can
 * only land between writes, so a split line would break the selftest grep. */
static void check(int ok, const char* what) {
    char line[256]; size_t n = 0;
    const char* parts[3] = { "[EXECVE] ", what, ok ? " OK\n" : " FAIL\n" };
    for (int k = 0; k < 3; k++)
        for (const char* c = parts[k]; *c && n < sizeof(line) - 1; c++) line[n++] = *c;
    write(1, line, n);
}
static void utoa(unsigned long v, char* out) {
    char t[24]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0;
}
static long atol_(const char* s) {
    long v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v;
}
static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; } return *a == *b;
}
static void say_num(const char* label, long v) {
    char b[24]; say(label);
    if (v < 0) { say("-"); v = -v; }
    utoa((unsigned long)v, b); say(b); say("\n");
}

/* MemFree from /proc/meminfo, in kB (-1 if unreadable). */
static long mem_free_kb(void) {
    char buf[256];
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return -1;
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    for (long i = 0; i + 8 < n; i++)
        if (buf[i] == 'M' && buf[i+1] == 'e' && buf[i+2] == 'm' && buf[i+3] == 'F') {
            const char* p = buf + i + 8;
            while (*p == ' ') p++;
            return atol_(p);
        }
    return -1;
}

static volatile int g_usr1_hits = 0;
static volatile int g_term_hits = 0;
static void on_usr1(int s) { (void)s; g_usr1_hits++; }
static void on_term(int s) { (void)s; g_term_hits++; }

static int main_role(char* self) {
    say("[EXECVE] --- in-place execve test ---\n");
    int pid0 = getpid();

    /* 1. failure cases: each returns -1 + errno, and we are still here. */
    char* av0[] = { self, 0 };
    char* ev0[] = { 0 };
    int r = execve("/bin/exectest_nonexistent", av0, ev0);
    check(r == -1 && errno == ENOENT, "nonexistent path -> ENOENT, caller continues");
    r = execve("/bin/exectest_tampered", av0, ev0);
    check(r == -1 && errno == EACCES, "tampered ELF refused (EACCES), caller continues");
    r = execve("/bin/exectest_unsigned", av0, ev0);
    check(r == -1 && errno == EACCES, "unsigned ELF refused (EACCES), caller continues");
    r = execve("/bin/exectest_script", av0, ev0);
    check(r == -1 && errno == ENOEXEC, "non-ELF file -> ENOEXEC, caller continues");
    r = execve("/bin", av0, ev0);
    check(r == -1 && errno == EACCES, "directory -> EACCES");
    r = execve((const char*)0x7ff000001000ULL, av0, ev0);   /* unmapped user VA */
    check(r == -1 && errno == EFAULT, "bad path pointer -> EFAULT");
    r = execve("/bin/exectest", (char* const*)0x7ff000000000ULL, ev0);
    check(r == -1 && errno == EFAULT, "bad argv pointer -> EFAULT");
    char* badv[] = { self, (char*)0xdead0000ULL, 0 };
    r = execve("/bin/exectest", badv, ev0);
    check(r == -1 && errno == EFAULT, "bad argv[1] string -> EFAULT");
    r = execve("/bin/exectest", av0, (char* const*)0xffff800000001000ULL);
    check(r == -1 && errno == EFAULT, "kernel envp pointer -> EFAULT");
    check(getpid() == pid0, "pid unchanged after failed execs");

    /* 2. state the successful exec must keep / drop. */
    signal(SIGUSR1, on_usr1);                 /* caught  -> must become SIG_DFL */
    signal(SIGUSR2, (sighandler_t)1);         /* SIG_IGN -> must stay ignored */
    kill(getpid(), SIGUSR1);
    check(g_usr1_hits == 1, "pre-exec SIGUSR1 handler runs");
    unsigned long blk = 1UL << SIGTERM, old = 0;
    sigprocmask(0 /*SIG_BLOCK*/, &blk, &old);
    kill(getpid(), SIGTERM);                  /* stays pending across exec */
    int keep = open("/proc/uptime", O_RDONLY);
    int cx = open("/proc/version", O_RDONLY | O_CLOEXEC);
    int cy = dup(keep);
    fcntl(cy, F_SETFD, FD_CLOEXEC);
    check(fcntl(cy, F_GETFD) == FD_CLOEXEC && fcntl(keep, F_GETFD) == 0, "fcntl F_SETFD/F_GETFD");
    int hi = fcntl(keep, F_DUPFD, 10);
    check(hi >= 10, "fcntl F_DUPFD honours the minimum fd");
    int pf[2] = { -1, -1 };
    check(pipe2(pf, O_CLOEXEC) == 0 && fcntl(pf[0], F_GETFD) == FD_CLOEXEC, "pipe2(O_CLOEXEC)");
    setenv("EXECTEST_VAR", "hello42", 1);

    /* 3. make the old image big + COW-shared: heap, mmap, and a forked kid. */
    char* heap = (char*)malloc(256 * 1024);
    for (int i = 0; heap && i < 256 * 1024; i += 4096) heap[i] = (char)i;
    char* mm = (char*)mmap(0, 1024 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    for (int i = 0; mm && mm != (char*)-1 && i < 1024 * 1024; i += 4096) mm[i] = 1;
    int kid = fork();
    if (kid == 0) { sleep_ticks(50); _exit(3); }   /* holds COW refs while we exec */

    char s_pid[24], s_keep[24], s_cx[24], s_cy[24], s_pf[24], s_kid[24];
    utoa((unsigned long)pid0, s_pid); utoa((unsigned long)keep, s_keep);
    utoa((unsigned long)cx, s_cx); utoa((unsigned long)cy, s_cy);
    utoa((unsigned long)pf[1], s_pf); utoa((unsigned long)kid, s_kid);
    char* av[] = { "exectest", "child", s_pid, s_keep, s_cx, s_cy, s_pf, s_kid, "arg with spaces", 0 };
    char* ev[] = { "EXECTEST_VAR=hello42", "PATH=/bin", "EMPTY=", 0 };
    say("[EXECVE] exec'ing self as child\n");
    execve("/bin/exectest", av, ev);
    say_num("[EXECVE] successful exec returned FAIL errno=", errno);
    return 1;
}

static int child_role(int argc, char** argv, char** envp) {
    say("[EXECVE] child image running\n");
    check(argc == 9 && streq(argv[0], "exectest") && streq(argv[8], "arg with spaces") && argv[9] == 0,
          "argv delivered to new image");
    check(getpid() == (int)atol_(argv[2]), "pid unchanged across exec");
    const char* v = getenv("EXECTEST_VAR");
    check(v && streq(v, "hello42"), "envp delivered (getenv)");
    check(envp && envp[0] && streq(envp[0], "EXECTEST_VAR=hello42") && envp[2] && streq(envp[2], "EMPTY=") && !envp[3],
          "envp delivered to main");
    int keep = (int)atol_(argv[3]), cx = (int)atol_(argv[4]), cy = (int)atol_(argv[5]), pw = (int)atol_(argv[6]);
    char b[8];
    check(fcntl(keep, F_GETFD) == 0 && read(keep, b, 1) == 1, "plain fd survives exec");
    errno = 0;
    check(fcntl(cx, F_GETFD) == -1 && errno == EBADF, "O_CLOEXEC fd closed by exec");
    errno = 0;
    check(fcntl(cy, F_GETFD) == -1 && errno == EBADF, "FD_CLOEXEC (F_SETFD) fd closed by exec");
    errno = 0;
    check(fcntl(pw, F_GETFD) == -1 && errno == EBADF, "pipe2(O_CLOEXEC) end closed by exec");

    /* parent link survives: the kid forked by the old image is still ours. */
    int st = waitpid((int)atol_(argv[7]));
    check(WIFEXITED(st) && WEXITSTATUS(st) == 3, "pre-exec child collected by the new image");

    /* caught handler -> SIG_DFL: SIGUSR1 now kills (checked in a fork). */
    int k = fork();
    if (k == 0) { kill(getpid(), SIGUSR1); _exit(0); }
    st = waitpid(k);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGUSR1, "caught handler reset to SIG_DFL");
    k = fork();
    if (k == 0) { kill(getpid(), SIGUSR2); _exit(5); }
    st = waitpid(k);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 5, "ignored signal stays ignored");

    unsigned long none = 0, cur = 0;
    sigprocmask(0 /*SIG_BLOCK*/, &none, &cur);
    check((cur >> SIGTERM) & 1, "signal mask preserved");
    signal(SIGTERM, on_term);
    unsigned long blk = 1UL << SIGTERM;
    sigprocmask(1 /*SIG_UNBLOCK*/, &blk, 0);
    check(g_term_hits == 1, "pending signal preserved across exec");

    /* 200 fork+exec cycles: the PMM free count must not drift. */
    long base = -1;
    int bad = 0;
    for (int i = 1; i <= LEAK_CYCLES; i++) {
        int c = fork();
        if (c == 0) {
            char* lav[] = { "exectest", "leaf", 0 };
            char* lev[] = { 0 };
            execve("/bin/exectest", lav, lev);
            _exit(99);
        }
        if (c < 0) { bad++; continue; }
        int s = waitpid(c);
        if (!WIFEXITED(s) || WEXITSTATUS(s) != 0) bad++;
        if (i == LEAK_WARMUP) { sleep_ticks(100); base = mem_free_kb(); }
        if (i == 110) { sleep_ticks(100); say_num("[EXECVE] leak: MemFree kB after 110 cycles=", mem_free_kb()); }
    }
    sleep_ticks(100);                    /* let every CPU reap its zombies (idle) */
    long end = mem_free_kb();
    say_num("[EXECVE] leak: MemFree kB after warm-up=", base);
    say_num("[EXECVE] leak: MemFree kB after 200 cycles=", end);
    check(bad == 0, "200 fork+exec cycles all exit 0");
    long d = base - end;
    check(base > 0 && end > 0 && d <= 64 && d >= -64, "no memory leak across 200 fork+exec (|delta| <= 64 kB)");
    say("[EXECVE] DONE-USER\n");
    return 0;
}

int main(int argc, char** argv, char** envp) {
    if (argc >= 2 && streq(argv[1], "leaf")) return 0;
    if (argc >= 2 && streq(argv[1], "child")) return child_role(argc, argv, envp);
    return main_role(argc >= 1 ? argv[0] : "exectest");
}
