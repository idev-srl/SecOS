/* abi_test — ring-3 regression test for kernel/user ABI contracts.
 * SPDX-License-Identifier: MIT
 *
 *  - wait status: the kernel reports the POSIX/Linux encoding end to end, so an
 *    exit(142) is WIFEXITED/142 (not "killed by signal 14") and ring-3 CPU
 *    faults are reported as the matching signal (#PF -> SIGSEGV, #DE -> SIGFPE,
 *    #UD -> SIGILL).
 *  - DF on entry: ring 3 may enter the kernel with EFLAGS.DF=1 (`std`); every
 *    entry stub must `cld` (the kernel's compiler-generated rep movs/stos in
 *    fork/sigframe copies, memcpy, ... assume DF=0). Syscalls issued with DF=1
 *    must still copy correctly, the user's DF must survive the round trip, and
 *    a signal handler must start with DF=0.
 *  - PROT_NONE: a PROT_NONE page is reserved but inaccessible — a ring-3 read
 *    or write kills with SIGSEGV, a syscall copying from/to it gets -EFAULT,
 *    demand paging never materializes it, and mprotect back to RW restores
 *    access with the old contents (also for a page split out of a larger
 *    mapping, and for a parked page shared across fork()).
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

/* ---------------- DF=1 on kernel entry ---------------- */
#define SYS_READ_   3
#define SYS_WRITE_  2
#define SYS_GETTICKS_ 10
#define SYS_FORK_  18
#define SYS_STAT_  20
#define SYS_PIPE_  31
#define SYS_KILL_  46
#define EFL_DF 0x400UL

/* Issue a syscall with DF=1 (the asm block sets it right before `int 0x80`
 * and clears it right after, so no compiled C code runs with DF=1). *flags
 * gets RFLAGS as seen on return, before the cld. */
static long sc_df(long n, long a0, long a1, long a2, unsigned long* flags) {
    long ret; unsigned long fl;
    __asm__ volatile("std\n\tint $0x80\n\tpushfq\n\tpop %1\n\tcld"
                     : "=a"(ret), "=r"(fl)
                     : "0"(n), "D"(a0), "S"(a1), "d"(a2), "c"(0L)
                     : "memory", "cc");
    if (flags) *flags = fl;
    return ret;
}
static unsigned long read_flags(void) {
    unsigned long f; __asm__ volatile("pushfq\n\tpop %0" : "=r"(f)); return f;
}

static volatile unsigned long g_handler_flags;
static volatile int g_handler_ran;
static void on_usr1(int sig) { (void)sig; g_handler_flags = read_flags(); g_handler_ran = 1; }

static void test_df(void) {
    unsigned long fl = 0;
    int ok_df = 1;

    /* pipe + write + read with DF=1: the bytes must arrive intact, in order. */
    {
        int fds[2] = { -1, -1 };
        long r = sc_df(SYS_PIPE_, (long)fds, 0, 0, &fl); ok_df &= !!(fl & EFL_DF);
        static char out[200], in[200];
        for (int i = 0; i < 200; i++) { out[i] = (char)('A' + i % 26); in[i] = 0; }
        long w = r == 0 ? sc_df(SYS_WRITE_, fds[1], (long)out, 200, &fl) : -1; ok_df &= !!(fl & EFL_DF);
        long n = w == 200 ? sc_df(SYS_READ_, fds[0], (long)in, 200, &fl) : -1; ok_df &= !!(fl & EFL_DF);
        int same = n == 200;
        for (int i = 0; same && i < 200; i++) same = in[i] == out[i];
        check("df-pipe-write-read", same, n);
        if (fds[0] >= 0) close(fds[0]);
        if (fds[1] >= 0) close(fds[1]);
    }
    /* write to the console with DF=1 (copy_from_user + console path). */
    {
        static const char msg[] = "[ABI-TEST] df-console-write-text\n";
        long w = sc_df(SYS_WRITE_, 1, (long)msg, sizeof(msg) - 1, &fl); ok_df &= !!(fl & EFL_DF);
        check("df-console-write", w == (long)(sizeof(msg) - 1), w);
    }
    /* stat with DF=1 must fill the same struct as a normal stat. */
    {
        struct stat a, b;
        char* pa = (char*)&a; char* pb = (char*)&b;
        for (unsigned i = 0; i < sizeof(a); i++) { pa[i] = 0; pb[i] = (char)0x5a; }
        int r1 = stat("/bin/abi_test", &a);
        long r2 = sc_df(SYS_STAT_, (long)"/bin/abi_test", (long)&b, 0, &fl); ok_df &= !!(fl & EFL_DF);
        int same = r1 == 0 && r2 == 0 && a.st_size > 0 && a.st_size == b.st_size && a.st_mode == b.st_mode;
        check("df-stat", same, r2);
    }
    /* fork with DF=1: the kernel copies the PCB/trapframe (rep movsq); the
     * child must see the parent's memory and both must keep running. */
    {
        static char pat[4096];
        for (int i = 0; i < 4096; i++) pat[i] = (char)(i * 7);
        long pid = sc_df(SYS_FORK_, 0, 0, 0, &fl);
        if (pid == 0) {
            int good = (fl & EFL_DF) != 0;
            for (int i = 0; good && i < 4096; i++) good = pat[i] == (char)(i * 7);
            _exit(good ? 42 : 1);
        }
        ok_df &= !!(fl & EFL_DF);
        int st = pid > 0 ? waitpid((int)pid) : -1;
        expect_exit("df-fork-child-ok", st, 42);
    }
    /* signal raised with DF=1: the handler starts with DF=0 (ABI) and the
     * interrupted DF=1 is restored by sigreturn. */
    {
        signal(10 /*SIGUSR1*/, on_usr1);
        g_handler_ran = 0; g_handler_flags = EFL_DF;
        sc_df(SYS_KILL_, getpid(), 10, 0, &fl);
        check("df-signal-handler-entry-df0", g_handler_ran && !(g_handler_flags & EFL_DF), (long)g_handler_flags);
        check("df-restored-after-sigreturn", (fl & EFL_DF) != 0, (long)fl);
        signal(10, (sighandler_t)0);
    }
    /* Stay in ring 3 with DF=1 across many timer ticks (isr_timer + scheduler
     * run with the interrupted DF=1): the kernel must keep working. */
    {
        unsigned long t0 = getticks(), t = t0;
        long spins = 0;
        while (t - t0 < 20 && spins < 2000000) {
            t = (unsigned long)sc_df(SYS_GETTICKS_, 0, 0, 0, &fl);
            ok_df &= !!(fl & EFL_DF);
            __asm__ volatile("std\n\t"
                             "mov $20000, %%ecx\n\t1: dec %%ecx\n\tjnz 1b\n\t"
                             "cld" ::: "rcx", "cc");
            spins++;
        }
        check("df-timer-ticks-with-df1", t - t0 >= 20, (long)(t - t0));
    }
    check("df-user-df-preserved-across-syscalls", ok_df, 0);
}

/* ---------------- PROT_NONE ---------------- */
#define PG 0x1000UL
#define EFAULT_ 14
static volatile char* g_probe;
static void child_read(void)  { volatile char c = *g_probe; (void)c; _exit(0); }
static void child_write(void) { *g_probe = 'X'; _exit(0); }

static int probe(void (*fn)(void), volatile char* p) { g_probe = p; return child_status(fn, 0); }

static void test_prot_none(void) {
    /* (1) 3 RW pages, filled; middle one -> PROT_NONE after it was faulted in. */
    char* m = (char*)mmap(0, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (m == (char*)-1) { check("pn-mmap", 0, -1); return; }
    for (unsigned long i = 0; i < 3 * PG; i++) m[i] = (char)('a' + (i / PG));
    long r = mprotect(m + PG, PG, PROT_NONE);
    check("pn-mprotect-present-page", r == 0, r);
    expect_sig("pn-user-read-sigsegv", probe(child_read, m + PG), SIGSEGV_);
    expect_sig("pn-user-write-sigsegv", probe(child_write, m + PG + 100), SIGSEGV_);
    expect_exit("pn-neighbours-still-rw", probe(child_write, m), 0);
    expect_exit("pn-neighbour-after-still-rw", probe(child_write, m + 2 * PG), 0);
    r = secos_syscall(SYS_WRITE_, 1, (long)(m + PG), 16, 0, 0);
    check("pn-kernel-copy-from-efault", r == -EFAULT_, r);
    {
        int fd = open("/bin/abi_test", 0);
        r = fd >= 0 ? secos_syscall(SYS_READ_, fd, (long)(m + PG), 16, 0, 0) : -1;
        check("pn-kernel-copy-to-efault", r == -EFAULT_, r);
        if (fd >= 0) close(fd);
    }
    r = mprotect(m + PG, PG, PROT_READ | PROT_WRITE);
    int same = r == 0;
    for (unsigned long i = 0; same && i < PG; i++) same = m[PG + i] == 'b';
    check("pn-mprotect-back-keeps-data", same, r);
    m[PG + 5] = 'Z';
    check("pn-mprotect-back-writable", m[PG + 5] == 'Z' && m[0] == 'a' && m[2 * PG] == 'c', 0);

    /* (2) Fresh PROT_NONE mapping, never touched: demand paging must refuse. */
    char* n = (char*)mmap(0, 2 * PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS);
    expect_sig("pn-fresh-read-sigsegv", probe(child_read, n), SIGSEGV_);
    r = secos_syscall(SYS_WRITE_, 1, (long)n, 8, 0, 0);
    check("pn-fresh-kernel-copy-efault", r == -EFAULT_, r);
    r = mprotect(n, 2 * PG, PROT_READ | PROT_WRITE);
    int zero = r == 0 && n[0] == 0 && n[PG + 7] == 0;
    n[1] = 'q';
    check("pn-fresh-mprotect-rw-works", zero && n[1] == 'q', r);

    /* (3) Partial mprotect of an untouched mapping (VMA split): page 1 is a
     * guard, page 0 and 2 still demand-fault RW. */
    char* g = (char*)mmap(0, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    r = mprotect(g + PG, PG, PROT_NONE);
    g[0] = 1; g[2 * PG] = 2;
    check("pn-split-neighbours-demand-rw", r == 0 && g[0] == 1 && g[2 * PG] == 2, r);
    expect_sig("pn-split-guard-sigsegv", probe(child_read, g + PG), SIGSEGV_);
    r = mprotect(g + PG, PG, PROT_READ);
    check("pn-split-guard-to-ro-reads-zero", r == 0 && g[PG + 1] == 0, r);
    expect_sig("pn-split-guard-ro-write-sigsegv", probe(child_write, g + PG), SIGSEGV_);

    /* (4) A parked page shared across fork(): the child unparks and writes;
     * the parent's copy must stay intact (COW of the parked frame). */
    char* f = (char*)mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    for (unsigned long i = 0; i < PG; i++) f[i] = 'P';
    mprotect(f, PG, PROT_NONE);
    int pid = fork();
    if (pid == 0) {
        if (mprotect(f, PG, PROT_READ | PROT_WRITE) != 0) _exit(2);
        if (f[10] != 'P') _exit(3);
        f[10] = 'C';
        _exit(f[10] == 'C' ? 0 : 4);
    }
    int st = pid > 0 ? waitpid(pid) : -1;
    expect_exit("pn-fork-child-unparks-and-writes", st, 0);
    r = mprotect(f, PG, PROT_READ);
    check("pn-fork-parent-copy-isolated", r == 0 && f[10] == 'P', r);

    munmap(m, 3 * PG); munmap(n, 2 * PG); munmap(g, 3 * PG); munmap(f, PG);
}

/* umount must never detach the root filesystem, under any spelling (the
 * syscall fuzzer did it once and every later exec/stat in the system failed). */
static void test_umount_root(void) {
    static const char* spell[] = { "/", "", ".", "/tmp/..", "//", "/./" };
    int ok = 1;
    for (unsigned i = 0; i < sizeof(spell) / sizeof(spell[0]); i++)
        if (secos_syscall(39 /*SYS_UMOUNT*/, (long)spell[i], 0, 0, 0, 0) >= 0) ok = 0;
    struct stat st;
    check("umount-root-refused", ok && stat("/bin/abi_test", &st) == 0, 0);
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "segv") == 0) { die_segv(); return 1; }
    test_wait_status();
    test_df();
    test_prot_none();
    test_umount_root();
    put("[ABI-TEST] DONE pass="); putnum(g_pass);
    put(" fail="); putnum(g_fail); put("\n");
    return g_fail ? 1 : 0;
}
