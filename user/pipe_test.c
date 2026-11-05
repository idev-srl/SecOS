/* pipe_test — ring-3 regression test for blocking-pipe data integrity.
 * SPDX-License-Identifier: MIT
 *
 * Pipes used to lose data once more than the 4 KB ring was in flight: a write
 * into a nearly-full pipe stored only what fitted and returned a short count,
 * and callers (libc fflush, coreutils) dropped the rest. POSIX requires:
 *  - a write of n <= PIPE_BUF (4096) bytes is atomic (never split/interleaved);
 *  - a blocking write returns only once all n bytes are queued — or, if a
 *    signal interrupts it, the bytes written so far (EINTR if none);
 *  - read returns what is available; EOF only when empty and no writer left;
 *  - write with no reader -> EPIPE + SIGPIPE.
 *
 * Every transfer runs across fork() with a position-dependent byte pattern so a
 * dropped, duplicated or reordered byte is caught at the exact offset, plus a
 * running checksum. Writers check that every blocking write() returned the full
 * count. Output: one "[PIPE-TEST] <name> OK|FAIL" line per case, then
 * "[PIPE-TEST] DONE pass=<n> fail=<m>". stdout is mirrored to debugcon.
 */
#include "libsecos.h"
#include <string.h>
#include <errno.h>
#include <stdio.h>

#define SIGPIPE_ 13
#define SIGUSR1_ 10
#define SIG_IGN_ ((sighandler_t)1)

static int g_pass, g_fail;

static void put(const char* s) { write(2, s, strlen(s)); }
static void putnum(long v) {
    char b[24]; int i = 23; int neg = v < 0; unsigned long u = neg ? -v : v;
    b[i] = 0;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    put(&b[i]);
}
static void check(const char* name, int ok, long got) {
    put("[PIPE-TEST] "); put(name); put(ok ? " OK" : " FAIL");
    if (!ok) { put(" got="); putnum(got); }
    put("\n");
    if (ok) g_pass++; else g_fail++;
}

/* Byte at stream offset i: varies within and across 256-byte spans. */
static unsigned char pat(unsigned long i) { return (unsigned char)(i * 31u + (i >> 8) * 7u + 3u); }

static unsigned char g_buf[65536];

/* Writer child: send 'total' pattern bytes in 'wchunk' writes; sleep every
 * 'pause_every' writes (slow writer). Exit 0 iff every write was complete. */
static void writer(int fd, unsigned long total, unsigned long wchunk, int pause_every) {
    unsigned long off = 0; int nw = 0, bad = 0;
    while (off < total) {
        unsigned long n = total - off < wchunk ? total - off : wchunk;
        for (unsigned long k = 0; k < n; k++) g_buf[k] = pat(off + k);
        long r = write(fd, g_buf, n);
        if (r != (long)n) { bad = 1; if (r <= 0) break; }
        off += r > 0 ? (unsigned long)r : 0;
        if (pause_every && ++nw % pause_every == 0) sleep_ticks(2);
    }
    _exit(bad ? 1 : 0);
}

/* Reader: drain to EOF in 'rchunk' reads, verifying every byte's position.
 * Returns bytes read; *mism = first mismatch offset + 1 (0 = none). */
static unsigned long reader(int fd, unsigned long rchunk, int pause_every, unsigned long* mism,
                            unsigned long* sum) {
    unsigned long off = 0; int nr = 0; *mism = 0; *sum = 0;
    for (;;) {
        long r = read(fd, g_buf, rchunk);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        for (long k = 0; k < r; k++) {
            if (!*mism && g_buf[k] != pat(off + k)) *mism = off + k + 1;
            *sum = *sum * 33u + g_buf[k];
        }
        off += (unsigned long)r;
        if (pause_every && ++nr % pause_every == 0) sleep_ticks(2);
    }
    return off;
}

static unsigned long expect_sum(unsigned long total) {
    unsigned long s = 0;
    for (unsigned long i = 0; i < total; i++) s = s * 33u + pat(i);
    return s;
}

static void xfer(const char* name, unsigned long total, unsigned long wchunk, unsigned long rchunk,
                 int wpause, int rpause) {
    int p[2];
    if (pipe(p) != 0) { check(name, 0, -1); return; }
    int pid = fork();
    if (pid == 0) { close(p[0]); writer(p[1], total, wchunk, wpause); }
    close(p[1]);
    unsigned long mism, sum;
    unsigned long got = reader(p[0], rchunk, rpause, &mism, &sum);
    close(p[0]);
    int st = pid > 0 ? waitpid(pid) : -1;
    int ok = got == total && mism == 0 && sum == expect_sum(total) &&
             st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    long why = got != total ? (long)got : mism ? -(long)mism : st;
    check(name, ok, why);
}

/* PIPE_BUF atomicity: two writers each send 'nrec' records of 'rec' bytes,
 * every record filled with its writer's tag. The stream must be whole records. */
static void atomic_case(const char* name, unsigned long rec, int nrec) {
    int p[2];
    if (pipe(p) != 0) { check(name, 0, -1); return; }
    int pids[2];
    for (int w = 0; w < 2; w++) {
        pids[w] = fork();
        if (pids[w] == 0) {
            close(p[0]);
            memset(g_buf, 'A' + w, rec);
            int bad = 0;
            for (int i = 0; i < nrec; i++) if (write(p[1], g_buf, rec) != (long)rec) bad = 1;
            _exit(bad);
        }
    }
    close(p[1]);
    static unsigned char recbuf[4096];
    unsigned long fill = 0, total = 0; int torn = 0, cnt[2] = {0, 0};
    for (;;) {
        long r = read(p[0], g_buf, 4096);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        for (long k = 0; k < r; k++) {
            recbuf[fill++] = g_buf[k];
            if (fill == rec) {
                unsigned char t = recbuf[0];
                for (unsigned long j = 1; j < rec; j++) if (recbuf[j] != t) { torn = 1; break; }
                if (t == 'A') cnt[0]++; else if (t == 'B') cnt[1]++; else torn = 1;
                fill = 0;
            }
        }
        total += (unsigned long)r;
    }
    close(p[0]);
    int s0 = waitpid(pids[0]), s1 = waitpid(pids[1]);
    int ok = !torn && fill == 0 && cnt[0] == nrec && cnt[1] == nrec &&
             total == rec * 2 * (unsigned long)nrec &&
             WIFEXITED(s0) && WEXITSTATUS(s0) == 0 && WIFEXITED(s1) && WEXITSTATUS(s1) == 0;
    check(name, ok, torn ? -1 : (long)total);
}

static volatile int g_got_usr1;
static void on_usr1(int s) { (void)s; g_got_usr1 = 1; }

/* A signal interrupting a blocked writer: with bytes already queued the write
 * returns that short count; with none queued it fails with EINTR. */
static void signal_case(void) {
    int p[2], rp[2];
    if (pipe(p) != 0 || pipe(rp) != 0) { check("signal-setup", 0, -1); return; }
    int pid = fork();
    if (pid == 0) {
        close(p[0]); close(rp[0]);
        signal(SIGUSR1_, on_usr1);
        for (unsigned long k = 0; k < 65536; k++) g_buf[k] = pat(k);
        long res[3];
        res[0] = write(p[1], g_buf, 65536);       /* fills 4096, blocks, interrupted */
        g_got_usr1 = 0;
        res[1] = write(p[1], g_buf, 16);           /* pipe full: blocks, nothing queued */
        res[2] = errno;
        write(rp[1], res, sizeof(res));
        _exit(0);
    }
    close(p[1]); close(rp[1]);
    sleep_ticks(200);
    kill(pid, SIGUSR1_);                          /* interrupts write #1 (partial) */
    sleep_ticks(200);
    kill(pid, SIGUSR1_);                          /* interrupts write #2 (empty) */
    long res[3] = {0, 0, 0};
    long rr = read(rp[0], res, sizeof(res));
    unsigned long mism, sum;
    unsigned long got = reader(p[0], 4096, 0, &mism, &sum);
    close(p[0]); close(rp[0]);
    int st = waitpid(pid);
    check("signal-partial-write-returns-count", rr == sizeof(res) && res[0] == 4096, res[0]);
    check("signal-empty-write-returns-eintr", res[1] == -1 && res[2] == EINTR, res[1] * 1000 + res[2]);
    check("signal-partial-bytes-intact", got == 4096 && mism == 0, (long)got);
    check("signal-writer-exit-0", WIFEXITED(st) && WEXITSTATUS(st) == 0, st);
}

/* No reader left: SIGPIPE kills by default; with SIG_IGN write -> EPIPE. */
static void epipe_case(void) {
    int p[2];
    pipe(p); close(p[0]);
    int pid = fork();
    if (pid == 0) { write(p[1], "x", 1); _exit(0); }
    int st = waitpid(pid);
    check("epipe-default-sigpipe-kills", WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE_, st);
    pid = fork();
    if (pid == 0) {
        signal(SIGPIPE_, SIG_IGN_);
        long r = write(p[1], "x", 1);
        _exit(r == -1 && errno == EPIPE ? 0 : 1);
    }
    st = waitpid(pid);
    check("epipe-ignored-returns-epipe", WIFEXITED(st) && WEXITSTATUS(st) == 0, st);
    close(p[1]);
    /* A writer blocked on a full pipe whose reader then exits gets EPIPE too. */
    pipe(p);
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        signal(SIGPIPE_, SIG_IGN_);
        static char big[8192];
        long r = write(p[1], big, sizeof(big));   /* 4096 queued, then blocks */
        _exit(r == 4096 ? 0 : r == -1 ? 2 : 1);
    }
    close(p[1]);
    sleep_ticks(100);
    close(p[0]);
    st = waitpid(pid);
    check("epipe-blocked-writer-returns-partial", WIFEXITED(st) && WEXITSTATUS(st) == 0, st);
}

/* stdio through a pipe: the child's line-buffered printf output (the original
 * `seq | wc` failure pattern) must arrive complete. */
static void stdio_case(void) {
    int p[2];
    pipe(p);
    int pid = fork();
    if (pid == 0) {
        close(p[0]); dup2(p[1], 1); close(p[1]);
        for (int i = 1; i <= 20000; i++) printf("%d\n", i);
        fflush(stdout);
        _exit(0);
    }
    close(p[1]);
    long lines = 0, sum = 0, cur = 0, bytes = 0, bad = 0;
    for (;;) {
        long r = read(p[0], g_buf, 1000);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        bytes += r;
        for (long k = 0; k < r; k++) {
            if (g_buf[k] == '\n') { lines++; if (cur != lines) bad++; sum += cur; cur = 0; }
            else cur = cur * 10 + (g_buf[k] - '0');
        }
    }
    close(p[0]);
    int st = waitpid(pid);
    check("stdio-printf-20000-lines", lines == 20000 && bad == 0 && bytes == 108894 &&
          sum == 200010000L && WIFEXITED(st) && WEXITSTATUS(st) == 0, lines * 100 + bad);
}

int main(void) {
    xfer("xfer-1B", 1, 1, 1, 0, 0);
    xfer("xfer-4095", 4095, 4095, 4096, 0, 0);
    xfer("xfer-4096", 4096, 4096, 4096, 0, 0);
    xfer("xfer-4097", 4097, 4097, 4096, 0, 0);
    xfer("xfer-64K-one-write", 65536, 65536, 4096, 0, 0);
    xfer("xfer-64K-small-writes-5B", 65536, 5, 4096, 0, 0);
    xfer("xfer-64K-small-reads-7B", 65536, 4096, 7, 0, 0);
    xfer("xfer-1M-64K-writes", 1UL << 20, 65536, 4096, 0, 0);
    xfer("xfer-1M-1000B-writes", 1UL << 20, 1000, 3000, 0, 0);
    xfer("xfer-256K-slow-reader", 262144, 6000, 4096, 0, 8);
    xfer("xfer-256K-slow-writer", 262144, 3000, 4096, 8, 0);
    atomic_case("atomic-1000B-records-2-writers", 1000, 300);
    atomic_case("atomic-4096B-records-2-writers", 4096, 64);
    signal_case();
    epipe_case();
    stdio_case();
    put("[PIPE-TEST] DONE pass="); putnum(g_pass); put(" fail="); putnum(g_fail); put("\n");
    return g_fail ? 1 : 0;
}
