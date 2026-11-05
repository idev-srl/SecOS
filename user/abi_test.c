/* abi_test — ring-3 regression test for kernel/user ABI contracts.
 * SPDX-License-Identifier: MIT
 *
 *  - wait status: the kernel reports the POSIX/Linux encoding end to end, so an
 *    exit(142) is WIFEXITED/142 (not "killed by signal 14") and ring-3 CPU
 *    faults are reported as the matching signal (#PF -> SIGSEGV, #DE -> SIGFPE,
 *    #UD -> SIGILL).
 *
 * Output: one "[ABI-TEST] <name> OK|FAIL" line per case, then
 * "[ABI-TEST] DONE pass=<n> fail=<m>". stdout is mirrored to debugcon.
 *
 * `abi_test segv` just does a wild NULL write (for shell $? = 139 checks).
 */
#include "libsecos.h"
#include <string.h>

#define SIGILL_  4
#define SIGFPE_  8
#define SIGKILL_ 9
#define SIGSEGV_ 11

static int g_pass, g_fail;

static void put(const char* s) { write(1, s, strlen(s)); }
static void putnum(long v) {
    char b[24]; int i = 23; int neg = v < 0; unsigned long u = neg ? -v : v;
    b[i] = 0;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    put(&b[i]);
}
static void check(const char* name, int ok, long got) {
    put("[ABI-TEST] "); put(name); put(ok ? " OK" : " FAIL");
    if (!ok) { put(" got="); putnum(got); }
    put("\n");
    if (ok) g_pass++; else g_fail++;
}

static void die_segv(void) { *(volatile int*)0 = 1; }
static void die_fpe(void) {
    volatile long z = 0;
    long r;
    __asm__ volatile("cqo; idivq %2" : "=a"(r) : "a"(1L), "r"(z) : "rdx");
    (void)r;
}
static void die_ill(void) { __asm__ volatile("ud2"); }

/* Fork a child that runs 'fn' (or exits 'code' when fn is NULL); return the
 * raw wait status. */
static int child_status(void (*fn)(void), int code) {
    int pid = fork();
    if (pid == 0) {
        if (fn) fn();
        _exit(code);
    }
    return pid > 0 ? waitpid(pid) : -1;
}
static void expect_exit(const char* name, int st, int code) {
    check(name, st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == code, st);
}
static void expect_sig(const char* name, int st, int sig) {
    check(name, st >= 0 && WIFSIGNALED(st) && !WIFEXITED(st) && WTERMSIG(st) == sig, st);
}

static void test_wait_status(void) {
    expect_exit("wait-exit-0", child_status(0, 0), 0);
    expect_exit("wait-exit-7", child_status(0, 7), 7);
    expect_exit("wait-exit-142-not-signal", child_status(0, 142), 142);
    expect_exit("wait-exit-255", child_status(0, 255), 255);
    expect_exit("wait-exit-256-truncated", child_status(0, 256), 0);
    expect_sig("wait-pf-is-sigsegv", child_status(die_segv, 0), SIGSEGV_);
    expect_sig("wait-de-is-sigfpe", child_status(die_fpe, 0), SIGFPE_);
    expect_sig("wait-ud-is-sigill", child_status(die_ill, 0), SIGILL_);
    {
        int pid = fork();
        if (pid == 0) { for (;;) sched_yield(); }
        if (pid > 0) kill(pid, SIGKILL_);
        int st = pid > 0 ? waitpid(pid) : -1;
        expect_sig("wait-sigkill", st, SIGKILL_);
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "segv") == 0) { die_segv(); return 1; }
    test_wait_status();
    put("[ABI-TEST] DONE pass="); putnum(g_pass);
    put(" fail="); putnum(g_fail); put("\n");
    return g_fail ? 1 : 0;
}
