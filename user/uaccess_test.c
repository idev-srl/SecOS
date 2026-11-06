/* uaccess_test — ring-3 regression test for the kernel's user-pointer fault
 * handling (exception-table fixups in copy_from/to_user + strncpy_from_user).
 * SPDX-License-Identifier: MIT
 *
 * Every case passes a syscall a pointer that is in the user range but unmapped
 * (an munmap'd hole, a wild address) or not writable (a PROT_NONE page), and
 * asserts the syscall returns an error (-EFAULT where the ABI reports it)
 * instead of the kernel faulting in ring 0. It also checks that kernel-mode
 * accesses to never-touched demand pages and to copy-on-write pages after
 * fork() still succeed (M14 / M19 must keep working from kernel mode).
 *
 * Output: one "[UA-TEST] <name> OK|FAIL" line per case, then
 * "[UA-TEST] DONE pass=<n> fail=<m>". stdout is mirrored to debugcon.
 */
#include "libsecos.h"

#define SYS_WRITE 2
#define SYS_READ 3
#define SYS_OPEN 4
#define SYS_SPAWN 8
#define SYS_MSG_SEND 11
#define SYS_STAT 20
#define SYS_PIPE 31
#define SYS_READLINK 35
#define SYS_LSTAT 37
#define SYS_GETDENTS 40
#define SYS_SIGPROCMASK 47
#define SYS_CHDIR 51
#define SYS_GETCWD 52
#define SYS_IOCTL 53
#define SYS_FSTAT 56

#define EFAULT_ 14
#define PG 0x1000UL

static int g_pass, g_fail;

static void put(const char* s) { write(1, s, strlen(s)); }
static void putnum(long v) {
    char b[24]; int i = 23; int neg = v < 0; unsigned long u = neg ? -v : v;
    b[i] = 0;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    put(&b[i]);
}
static long sc(long n, long a0, long a1, long a2, long a3) {
    return secos_syscall(n, a0, a1, a2, a3, 0);
}
static void check(const char* name, int ok, long got) {
    put("[UA-TEST] "); put(name); put(ok ? " OK" : " FAIL");
    if (!ok) { put(" got="); putnum(got); }
    put("\n");
    if (ok) g_pass++; else g_fail++;
}
/* A syscall reporting -EFAULT through the raw ABI. */
static void expect_efault(const char* name, long r) { check(name, r == -EFAULT_, r); }
/* A syscall that only promises a negative error code. */
static void expect_err(const char* name, long r) { check(name, r < 0, r); }


/* --- signal frame pushed from the #PF return path (nested kernel fault) ---
 * A signal that becomes pending while we run in ring 3 is delivered on the
 * next kernel exit. If that exit is a handled demand-paging #PF, the kernel
 * writes the sigframe onto the user stack from inside the #PF handler; when
 * the sigframe lands on a not-yet-faulted stack page that write faults AGAIN,
 * nested inside the first #PF. This case arranges exactly that: rsp sits just
 * above a fresh (never touched) stack page and a loop takes a #PF on every
 * iteration while a child exits and posts SIGCHLD. */
static volatile int g_chld;
static void on_chld(int sig) { (void)sig; g_chld = 1; }
static __attribute__((noinline)) long pf_loop(volatile char* mem, long pages) {
    long i = 0;
    for (; i < pages && !g_chld; i++) mem[i * PG] = 1;     /* one #PF per iteration */
    return i;
}
static __attribute__((noinline)) long pf_loop_low_stack(volatile char* mem, long pages) {
    /* Drop rsp to 96 bytes above a page boundary, 3 pages deeper than anything
     * touched so far, so the sigframe (below rsp-128) is on a fresh page. */
    unsigned long sp; __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    unsigned long target = ((sp - 0x3000) & ~(PG - 1)) + 96; /* 8-page user stack */
    volatile char* pad = (volatile char*)__builtin_alloca(sp - target);
    __asm__ volatile("" :: "r"(pad) : "memory");   /* keep the alloca */
    return pf_loop(mem, pages);
}

int main(void) {
    /* Fixtures: a RW page followed by an unmapped hole, and a PROT_NONE page. */
    char* rw = (char*)mmap(0, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    char* hole = rw + PG;
    munmap(hole, PG);
    char* none = (char*)mmap(0, PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS);
    const long wild = 0xDEADBEEFL;
    for (unsigned long i = 0; i < PG; i++) rw[i] = 'A';      /* no NUL in the page */

    /* --- the identity-map hole: low user pointers that alias the kernel-only
     * low identity map (physical 0..512 MB). Before the fix a syscall would
     * read/write real physical RAM (the IVT at 0x10, the kernel image at ~1 MB,
     * page tables) on the caller's behalf. Every one must now be refused by the
     * range check, and the kernel .text must be unchanged (the selftest gate
     * also checks /proc/version is intact after this run). --- */
    {
        const unsigned long lo[] = { 0x10, 0x1000, 0x100000, 0x200000, 0x1FFFFFFF };
        const char* nm[] = { "ident-read-0x10", "ident-read-0x1000",
                             "ident-read-0x100000", "ident-read-0x200000",
                             "ident-read-0x1FFFFFFF" };
        for (int i = 0; i < 5; i++) {
            /* read side: write(fd,ptr,n) copies FROM the pointer. */
            expect_efault(nm[i], sc(SYS_WRITE, 1, (long)lo[i], 16, 0));
        }
        /* write side: read(pipe, ptr, n) copies INTO the pointer (the dangerous
         * direction — a kernel-memory overwrite). Must be refused, not torn. */
        int wp[2];
        if (pipe(wp) == 0) {
            write(wp[1], "PWNED", 5);
            expect_efault("ident-write-0x100000", sc(SYS_READ, wp[0], 0x100000, 5, 0));
            expect_efault("ident-write-0x10", sc(SYS_READ, wp[0], 0x10, 5, 0));
            close(wp[0]); close(wp[1]);
        }
        /* output-buffer syscalls into the identity window. */
        expect_efault("ident-stat-buf-0x100000", sc(SYS_STAT, (long)"/", 0x100000, 0, 0));
        expect_err("ident-getcwd-0x100000", sc(SYS_GETCWD, 0x100000, 64, 0, 0));
        /* path/string argument in the identity window. */
        expect_efault("ident-open-path-0x100000", sc(SYS_OPEN, 0x100000, 0, 0, 0));
        expect_efault("ident-spawn-path-0x1000", sc(SYS_SPAWN, 0x1000, 0, 0, 0));
        /* a range straddling the window top (ends just inside user space) must
         * still be refused as a whole. */
        expect_efault("ident-straddle-min", sc(SYS_WRITE, 1, 0xFFFFF000UL, 0x4000, 0));
    }

    /* --- [M44] no low identity map in user address spaces, SMAP ---
     * The identity map is gone from every user CR3 (and from the kernel's own
     * tables after early boot); kernel-half pointers stay refused; a signal
     * frame aimed at the old identity window is never written by the kernel;
     * and entering the kernel with RFLAGS.AC=1 (which would disarm SMAP if the
     * entry stubs did not clac) changes nothing for a correct syscall. */
    {
        /* mmap ignores address hints below the user window. */
        char* m = (char*)mmap((void*)0x100000, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        check("noident-mmap-hint-low-ignored",
              m != (char*)-1 && (unsigned long)m >= 0x100000000UL, (long)m);
        if (m != (char*)-1) munmap(m, PG);
        /* Kernel-half pointers: the physmap (phys 1 MB) and the kernel image. */
        expect_efault("noident-read-physmap-ptr", sc(SYS_WRITE, 1, (long)0xFFFF888000100000UL, 16, 0));
        expect_efault("noident-read-kimage-ptr", sc(SYS_WRITE, 1, (long)0xFFFFFFFF80100000UL, 16, 0));
        int kp[2];
        if (pipe(kp) == 0) {
            write(kp[1], "PWNED", 5);
            expect_efault("noident-write-physmap-ptr",
                          sc(SYS_READ, kp[0], (long)0xFFFF888000100000UL, 5, 0));
            expect_efault("noident-write-kimage-ptr",
                          sc(SYS_READ, kp[0], (long)0xFFFFFFFF80100000UL, 5, 0));
            close(kp[0]); close(kp[1]);
        }
        /* A ring-3 load from the old identity window is a plain user fault. */
        int pid = fork();
        if (pid == 0) {
            volatile char* lo = (volatile char*)0x100000;
            char c = *lo;
            _exit(c ? 3 : 4);
        }
        int st = pid > 0 ? waitpid(pid) : -1;
        check("noident-ring3-load-0x100000-sigsegv",
              st >= 0 && WIFSIGNALED(st) && WTERMSIG(st) == 11, st);
        /* Signal delivery with rsp in the old identity window: the kernel must
         * refuse to build the frame there (copy_to_user) and kill the child
         * with the signal, never write physical memory. SIGCHLD (default:
         * ignore) with a handler installed: being killed BY it proves the frame
         * was refused rather than silently written. */
        pid = fork();
        if (pid == 0) {
            signal(17 /*SIGCHLD*/, on_chld);
            long me = getpid(), r;
            __asm__ volatile("mov %%rsp, %%r12\n\t"
                             "mov $0x100800, %%rsp\n\t"
                             "int $0x80\n\t"
                             "mov %%r12, %%rsp"
                             : "=a"(r) : "0"(46L /*SYS_KILL*/), "D"(me), "S"(17L)
                             : "r12", "memory");
            _exit(5);                        /* reached only if no frame was due */
        }
        st = pid > 0 ? waitpid(pid) : -1;
        check("noident-sigframe-low-rsp-killed",
              st >= 0 && WIFSIGNALED(st) && WTERMSIG(st) == 17, st);
        /* RFLAGS.AC=1 on entry: the kernel clears it (SMAP_CLAC) and the
         * uaccess primitives still copy correctly. */
        char sb[256];
        unsigned long fl;
        __asm__ volatile("pushfq; orq $0x40000, (%%rsp); popfq; pushfq; popq %0"
                         : "=r"(fl) :: "cc", "memory");
        long r = sc(SYS_STAT, (long)"/", (long)sb, 0, 0);
        __asm__ volatile("pushfq; andq $~0x40000, (%%rsp); popfq" ::: "cc", "memory");
        check("smap-entry-with-ac-set", r == 0 && (fl & 0x40000), r);
    }

    /* --- the three triggers found by the syscall fuzzer --- */
    expect_efault("spawn-wild-path", sc(SYS_SPAWN, wild, 0, 0, 0));
    expect_efault("lstat-protnone-statbuf", sc(SYS_LSTAT, (long)"/", (long)none, 0, 0));
    {
        long r = sc(SYS_WRITE, 1, (long)rw, 0x100000, 0);    /* runs off the mapping */
        check("write-console-overrun", r == -EFAULT_ || (r > 0 && r <= (long)PG), r);
    }

    /* --- strings --- */
    expect_efault("open-wild-path", sc(SYS_OPEN, wild, 0, 0, 0));
    expect_efault("open-hole-path", sc(SYS_OPEN, (long)hole, 0, 0, 0));
    expect_efault("open-unterminated-into-hole", sc(SYS_OPEN, (long)(hole - 16), 0, 0, 0));
    expect_efault("stat-wild-path", sc(SYS_STAT, wild, (long)rw, 0, 0));
    expect_err("chdir-hole-path", sc(SYS_CHDIR, (long)hole, 0, 0, 0));
    {
        char* argv[3]; argv[0] = "/bin/echo"; argv[1] = (char*)wild; argv[2] = 0;
        expect_efault("spawn-wild-argv-string", sc(SYS_SPAWN, (long)"/bin/echo", (long)argv, 0, 0));
        expect_efault("spawn-hole-argv-array", sc(SYS_SPAWN, (long)"/bin/echo", (long)hole, 0, 0));
    }

    /* --- output buffers (kernel writes into user memory) --- */
    expect_efault("stat-hole-statbuf", sc(SYS_STAT, (long)"/", (long)hole, 0, 0));
    expect_efault("stat-protnone-statbuf", sc(SYS_STAT, (long)"/", (long)none, 0, 0));
    expect_efault("fstat-hole-statbuf", sc(SYS_FSTAT, 1, (long)hole, 0, 0));
    expect_efault("pipe-hole-fds", sc(SYS_PIPE, (long)hole, 0, 0, 0));
    expect_err("getcwd-hole", sc(SYS_GETCWD, (long)hole, 64, 0, 0));
    expect_err("getdents-protnone", sc(SYS_GETDENTS, (long)"/", (long)none, 4096, 0));
    expect_err("sigprocmask-hole-oldset", sc(SYS_SIGPROCMASK, 0, 0, (long)hole, 0));
    expect_err("ioctl-tcgets-hole", sc(SYS_IOCTL, 0, 0x5401, (long)hole, 0));

    int fd = open("/bin/echo", 0);
    if (fd >= 0) {
        expect_efault("read-file-into-hole", sc(SYS_READ, fd, (long)hole, 512, 0));
        expect_efault("read-file-into-protnone", sc(SYS_READ, fd, (long)none, 512, 0));
        lseek(fd, 0, 0);
        /* Straddle: the first 64 bytes fit before the hole, the rest do not.
         * Either a short count or -EFAULT is acceptable; never a panic. */
        long r = sc(SYS_READ, fd, (long)(hole - 64), 4096, 0);
        check("read-file-straddle", r == -EFAULT_ || (r > 0 && r <= 64), r);
        close(fd);
    } else {
        check("open-/bin/echo", 0, fd);
    }

    /* --- input buffers (kernel reads user memory) --- */
    expect_efault("msgsend-hole", sc(SYS_MSG_SEND, 3, (long)hole, 16, 0));
    {
        int p[2];
        if (pipe(p) == 0) {
            expect_efault("pipe-write-from-hole", sc(SYS_WRITE, p[1], (long)hole, 16, 0));
            write(p[1], "xyz", 3);
            expect_efault("pipe-read-into-hole", sc(SYS_READ, p[0], (long)hole, 16, 0));
            close(p[0]); close(p[1]);
        } else {
            check("pipe-create", 0, -1);
        }
    }

    /* --- legitimate kernel-mode faults must still be serviced --- */
    {
        /* Never-touched demand page as a syscall output buffer (M14). */
        char* fresh = (char*)mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        long r = sc(SYS_STAT, (long)"/", (long)fresh, 0, 0);
        check("stat-into-demand-page", r == 0, r);
        int f2 = open("/bin/echo", 0);
        char* fresh2 = (char*)mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        r = f2 >= 0 ? sc(SYS_READ, f2, (long)fresh2, 4, 0) : -1;
        check("read-into-demand-page", r == 4 && fresh2[1] == 'E' && fresh2[2] == 'L', r);
        if (f2 >= 0) close(f2);
    }
    {
        /* Copy-on-write page written by the kernel in the child (M19). */
        static char cowbuf[64] = "parent";
        cowbuf[6] = '!';                     /* fault it in RW before fork -> COW */
        int pid = fork();
        if (pid == 0) {
            int f3 = open("/bin/echo", 0);
            long r = f3 >= 0 ? sc(SYS_READ, f3, (long)cowbuf, 4, 0) : -1;
            _exit(r == 4 && cowbuf[1] == 'E' ? 42 : 1);
        }
        int st = pid > 0 ? waitpid(pid) : -1;
        check("kernel-write-to-cow-page", st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 42 && cowbuf[0] == 'p', st);
    }


    {
        long pages = 16384;                  /* 64 MB of demand-zero memory */
        volatile char* mem = (volatile char*)mmap(0, pages * PG, PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_ANONYMOUS);
        signal(17 /*SIGCHLD*/, on_chld);
        int pid = fork();
        if (pid == 0) _exit(0);
        long n = pf_loop_low_stack(mem, pages);
        if (pid > 0) waitpid(pid);
        check("sigframe-from-pf-path", g_chld == 1 && n < pages, n);
        signal(17, (sighandler_t)0);
        munmap((void*)mem, pages * PG);
    }

    put("[UA-TEST] DONE pass="); putnum(g_pass);
    put(" fail="); putnum(g_fail); put("\n");
    return g_fail ? 1 : 0;
}
