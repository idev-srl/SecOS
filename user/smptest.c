/* smptest — SMP regression checks run from user space (M48).
 * SPDX-License-Identifier: MIT
 *
 *   smptest fpu [children] [iters]
 *     Each child runs the same floating-point recurrence twice: pass A with
 *     SIGUSR1 ignored, pass B with a SIGUSR1 handler that clobbers every XMM
 *     register and switches MXCSR to round-toward-zero. The parent keeps
 *     signalling the children and there are more children than CPUs, so both
 *     passes are preempted, migrated between CPUs and (pass B) interrupted by
 *     handlers many times. Pass A must equal pass B bit for bit: the kernel
 *     must carry the x87/SSE state across a migration and restore the
 *     interrupted state after a handler (sigreturn). Exit 0 when every child
 *     matched; prints "[smptest] fpu PASS" / "FAIL".
 *
 *   smptest burst [rounds] [width]
 *     Fork/exit bursts: each round forks `width` children that exit with a
 *     distinct status, and reaps them with waitpid; checks every status.
 *     Prints "[smptest] burst PASS".
 *
 *   smptest cpus
 *     Every userland view of the CPU count must agree: sysconf
 *     (_SC_NPROCESSORS_ONLN/_CONF), get_nprocs{,_conf} and the bits set by the
 *     sched_getaffinity syscall. Prints "[smptest] cpus=<n> onln=.. conf=..
 *     nprocs=.. conf_nprocs=.. affinity=.." and "[smptest] cpus PASS" / "FAIL".
 *
 *   smptest procrace [secs] [readers] [churners]
 *     Process-lifetime race (M50 B1). `readers` children loop on random pids
 *     around the live range: kill(pid, 0) and reads of /proc/<pid>/status,
 *     /proc/<pid>/maps and /proc/<pid>/stat. `churners` children keep forking
 *     short-lived workers that mmap/touch/mprotect/munmap (with splits),
 *     grow and shrink the heap, sometimes leave an orphan grandchild for init
 *     and sometimes are SIGKILLed mid-run; every worker's wait status is
 *     checked. On a kernel without process references or the per-process
 *     mm_lock this is a use-after-free of the process_t / its page tables
 *     (heap poisoning turns that into a fault). Prints "[smptest] procrace
 *     PASS ..." / "FAIL ...".
 */
#include "libsecos.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <stdio.h>
#include <fcntl.h>

#define P(s) write(1, s, strlen(s))

static void put_dec(unsigned long v) {
    char b[24]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    char o[24]; int k = 0; while (n) o[k++] = b[--n];
    write(1, o, (unsigned long)k);
}

static volatile unsigned long g_handled;

/* Trash every XMM register and set MXCSR.RC = round toward zero: if the
 * kernel did not restore the interrupted state, pass B diverges. */
static void on_usr1(int sig) {
    (void)sig;
    unsigned int rz = 0x7F80;                     /* masked exceptions, RC=11 */
    __asm__ volatile(
        "ldmxcsr %0\n\t"
        "pcmpeqd %%xmm0, %%xmm0\n\t"  "pcmpeqd %%xmm1, %%xmm1\n\t"
        "pcmpeqd %%xmm2, %%xmm2\n\t"  "pcmpeqd %%xmm3, %%xmm3\n\t"
        "pcmpeqd %%xmm4, %%xmm4\n\t"  "pcmpeqd %%xmm5, %%xmm5\n\t"
        "pcmpeqd %%xmm6, %%xmm6\n\t"  "pcmpeqd %%xmm7, %%xmm7\n\t"
        "pcmpeqd %%xmm8, %%xmm8\n\t"  "pcmpeqd %%xmm9, %%xmm9\n\t"
        "pcmpeqd %%xmm10, %%xmm10\n\t" "pcmpeqd %%xmm11, %%xmm11\n\t"
        "pcmpeqd %%xmm12, %%xmm12\n\t" "pcmpeqd %%xmm13, %%xmm13\n\t"
        "pcmpeqd %%xmm14, %%xmm14\n\t" "pcmpeqd %%xmm15, %%xmm15\n\t"
        :: "m"(rz) : "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7",
                     "xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15");
    g_handled++;
}

/* Values stay in XMM registers across the whole loop (no calls inside). */
/* noipa: two calls with the same arguments must both run (no CSE).
 * k1/k2 are read once and live in XMM registers for the whole loop; their
 * product converted to an integer is compared with an immediate, so a lost
 * register file (e.g. reloaded with zeros) is caught even when it would make
 * both passes collapse to the same wrong value. */
static volatile double g_k1 = 3.0, g_k2 = 3.0;
static volatile unsigned long g_reg_lost;
__attribute__((noipa)) static unsigned long long fp_pass(unsigned long iters, double seed) {
    double a = seed, b = 1.0 / seed, c = 0.0;
    double k1 = g_k1, k2 = g_k2;
    unsigned long lost = 0;
    for (unsigned long i = 0; i < iters; i++) {
        a = a * 1.0000001192092896 + 0.25;
        b = b * 0.9999998807907104 + a * 1e-9;
        c = c + (a - b) * 0.5;
        if (a > 1e6) a = a * 1e-6;
        if ((long)(k1 * k2) != 9) lost++;
    }
    g_reg_lost += lost;
    union { double d; unsigned long long u; } r;
    r.d = a + b * 3.0 + c * 7.0;
    return r.u;
}

static int fpu_child(unsigned long iters, int idx) {
    double seed = 1.5 + idx * 0.125;
    signal(SIGUSR1, SIG_IGN);
    unsigned long long ra = fp_pass(iters, seed);
    signal(SIGUSR1, on_usr1);
    unsigned long long rb = fp_pass(iters, seed);
    signal(SIGUSR1, SIG_IGN);
    if (ra != rb || ra == 0 || g_reg_lost) {
        P("[smptest] fpu child "); put_dec((unsigned long)idx);
        P(" MISMATCH handled="); put_dec(g_handled); P(" lost="); put_dec(g_reg_lost); P("\n");
        return 1;
    }
    if (idx == 0) {
        P("[smptest] fpu child 0 handlers="); put_dec(g_handled);
        P(" result="); put_dec((unsigned long)(ra >> 32)); P(":"); put_dec((unsigned long)(ra & 0xFFFFFFFFu));
        P("\n");
    }
    return 0;
}

static int cmd_fpu(int n, unsigned long iters) {
    int pids[32];
    if (n > 24) n = 24;
    signal(SIGUSR1, SIG_IGN);       /* inherited: a child signalled before its own setup survives */
    for (int i = 0; i < n; i++) {
        int pid = fork();
        if (pid == 0) exit(fpu_child(iters, i));
        pids[i] = pid;
    }
    /* Keep the handlers firing while the children compute. */
    for (int round = 0; round < 400; round++) {
        int alive = 0;
        for (int i = 0; i < n; i++)
            if (pids[i] > 0 && kill(pids[i], SIGUSR1) == 0) alive++;
        if (!alive) break;
        sleep_ticks(5);
    }
    int bad = 0;
    for (int i = 0; i < n; i++) {
        int st = waitpid(pids[i]);
        if (st != 0) bad++;
    }
    P(bad ? "[smptest] fpu FAIL children=" : "[smptest] fpu PASS children=");
    put_dec((unsigned long)n); P(" bad="); put_dec((unsigned long)bad); P("\n");
    return bad ? 1 : 0;
}

static int cmd_burst(int rounds, int width) {
    if (width > 24) width = 24;
    int bad = 0;
    for (int r = 0; r < rounds; r++) {
        int pids[32];
        for (int i = 0; i < width; i++) {
            int pid = fork();
            if (pid == 0) exit((r * 7 + i) & 0x7F);
            pids[i] = pid;
            if (pid < 0) bad++;
        }
        for (int i = 0; i < width; i++) {
            if (pids[i] <= 0) continue;
            int st = waitpid(pids[i]);
            if (st != (((r * 7 + i) & 0x7F) << 8)) bad++;
        }
    }
    P(bad ? "[smptest] burst FAIL bad=" : "[smptest] burst PASS bad=");
    put_dec((unsigned long)bad); P("\n");
    return bad ? 1 : 0;
}

static unsigned long arg_num(const char* s, unsigned long dflt) {
    if (!s) return dflt;
    unsigned long v = 0; int any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned long)(*s++ - '0'); any = 1; }
    return any ? v : dflt;
}

static int cmd_cpus(void) {
    long onln = sysconf(_SC_NPROCESSORS_ONLN), conf = sysconf(_SC_NPROCESSORS_CONF);
    long np = get_nprocs(), npc = get_nprocs_conf();
    unsigned long mask[16];                       /* 1024 CPUs, like glibc's cpu_set_t */
    for (int i = 0; i < 16; i++) mask[i] = 0;
    long r = secos_sched_getaffinity(0, sizeof(mask), mask);
    long aff = 0;
    for (long w = 0; r > 0 && w < r / 8; w++)
        for (unsigned long m = mask[w]; m; m &= m - 1) aff++;
    long small = secos_sched_getaffinity(0, 4, mask);   /* too small: must be EINVAL */
    P("[smptest] cpus="); put_dec((unsigned long)np);
    P(" onln="); put_dec((unsigned long)onln); P(" conf="); put_dec((unsigned long)conf);
    P(" nprocs="); put_dec((unsigned long)np); P(" conf_nprocs="); put_dec((unsigned long)npc);
    P(" affinity="); put_dec((unsigned long)aff); P(" bytes="); put_dec((unsigned long)(r < 0 ? 0 : r));
    P("\n");
    int ok = np >= 1 && onln == np && conf == npc && npc >= np && aff == np && r >= 8 && small < 0;
    P(ok ? "[smptest] cpus PASS\n" : "[smptest] cpus FAIL\n");
    return ok ? 0 : 1;
}

/* ---- procrace ------------------------------------------------------------ */
static unsigned long g_rng;
static unsigned long rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}

/* Read a whole (small) file; bytes read or -1. */
static int read_file(const char* path, char* buf, int cap) {
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

/* Highest pid handed out so far: the 5th field of /proc/loadavg. */
static long last_pid(void) {
    char b[128];
    if (read_file("/proc/loadavg", b, sizeof(b)) <= 0) return -1;
    int f = 0; const char* q = b;
    while (*q && f < 4) { if (*q == ' ') f++; q++; }
    return (long)arg_num(q, 0);
}

static int race_reader(int idx, unsigned long deadline, int lo) {
    g_rng = 0x9E3779B97F4A7C15ul ^ ((unsigned long)getpid() << 20) ^ (unsigned long)idx;
    static char buf[8192];
    char path[64];
    int bad = 0, shown = 0;
    unsigned long ops = 0;
    while (getticks() < deadline) {
        long hi = last_pid();
        if (hi < lo) hi = lo;
        int pid = lo + (int)(rnd() % (unsigned long)(hi - lo + 4));
        kill(pid, 0);                                  /* ESRCH or 0, never a crash */
        snprintf(path, sizeof(path), "/proc/%d/status", pid);
        int n = read_file(path, buf, sizeof(buf));
        if (n > 0 && strncmp(buf, "Name:", 5) != 0) {
            bad++;
            if (!shown++) { buf[40] = 0; P("[procrace] status mismatch: "); P(path); P(" -> "); P(buf); P("\n"); }
        }
        snprintf(path, sizeof(path), "/proc/%d/maps", pid);
        read_file(path, buf, sizeof(buf));
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        n = read_file(path, buf, sizeof(buf));
        if (n > 0 && (buf[0] < '0' || buf[0] > '9')) {
            bad++;
            if (!shown++) { buf[40] = 0; P("[procrace] stat mismatch: "); P(path); P(" -> "); P(buf); P("\n"); }
        }
        ops++;
    }
    return bad ? 1 : (ops ? 0 : 2);
}

/* One short-lived worker: address-space churn, then exit(code). */
static void race_worker(int code) {
    const unsigned long PG = 4096;
    char* a = (char*)mmap(0, 16 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (a && a != (char*)MAP_FAILED) {
        for (int i = 0; i < 16; i++) a[i * PG] = (char)i;
        mprotect(a + 4 * PG, 4 * PG, PROT_READ);       /* split in three */
        munmap(a + 8 * PG, 4 * PG);                    /* hole: another split */
        munmap(a, 16 * PG);
    }
    char* h = (char*)sbrk(64 * 1024);
    if (h && h != (char*)-1) {
        for (int i = 0; i < 16; i++) h[i * PG] = 1;
        sbrk(-64 * 1024);
    }
    if ((code & 7) == 3) {                             /* an orphan for init to reap */
        if (fork() == 0) _exit(0);
    }
    _exit(code);
}

static int race_churner(int idx, unsigned long deadline) {
    g_rng = 0xD1B54A32D192ED03ul ^ ((unsigned long)getpid() << 24) ^ (unsigned long)idx;
    int bad = 0; unsigned long rounds = 0;
    while (getticks() < deadline) {
        int code = (int)(rnd() & 0x3F);
        int pid = fork();
        if (pid == 0) race_worker(code);
        if (pid < 0) { sleep_ticks(2); continue; }     /* process table full: retry */
        int killed = (rnd() & 3) == 0;
        if (killed) kill(pid, SIGKILL);
        int st = waitpid(pid);
        if (st != (code << 8) && !(killed && st == SIGKILL)) bad++;
        rounds++;
    }
    return bad ? 1 : (rounds ? 0 : 2);
}

static int cmd_procrace(int secs, int nread, int nchurn) {
    if (nread > 12) nread = 12;
    if (nchurn > 6) nchurn = 6;
    unsigned long deadline = getticks() + (unsigned long)secs * 1000ul;   /* 1 kHz tick */
    int lo = getpid() > 8 ? getpid() - 8 : 1;
    int pids[20], kind[20], n = 0, bad = 0, spawn_fail = 0;
    for (int i = 0; i < nread + nchurn; i++) {
        int pid = fork();
        if (pid == 0) _exit(i < nread ? race_reader(i, deadline, lo) : race_churner(i, deadline));
        if (pid < 0) { spawn_fail++; continue; }
        pids[n] = pid; kind[n] = i < nread; n++;
    }
    int rbad = 0, cbad = 0;
    for (int i = 0; i < n; i++) {
        int st = waitpid(pids[i]);
        if (st != 0) { bad++; if (kind[i]) rbad++; else cbad++; }
    }
    P(bad || spawn_fail ? "[smptest] procrace FAIL" : "[smptest] procrace PASS");
    P(" readers="); put_dec((unsigned long)nread); P(" churners="); put_dec((unsigned long)nchurn);
    P(" bad_readers="); put_dec((unsigned long)rbad); P(" bad_churners="); put_dec((unsigned long)cbad);
    P(" spawn_fail="); put_dec((unsigned long)spawn_fail); P("\n");
    return bad || spawn_fail ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "fpu") == 0)
        return cmd_fpu((int)arg_num(argc > 2 ? argv[2] : 0, 8), arg_num(argc > 3 ? argv[3] : 0, 3000000));
    if (argc >= 2 && strcmp(argv[1], "burst") == 0)
        return cmd_burst((int)arg_num(argc > 2 ? argv[2] : 0, 20), (int)arg_num(argc > 3 ? argv[3] : 0, 12));
    if (argc >= 2 && strcmp(argv[1], "cpus") == 0)
        return cmd_cpus();
    if (argc >= 2 && strcmp(argv[1], "procrace") == 0)
        return cmd_procrace((int)arg_num(argc > 2 ? argv[2] : 0, 20), (int)arg_num(argc > 3 ? argv[3] : 0, 8),
                            (int)arg_num(argc > 4 ? argv[4] : 0, 4));
    P("usage: smptest fpu [children] [iters] | burst [rounds] [width] | cpus"
      " | procrace [secs] [readers] [churners]\n");
    return 2;
}
