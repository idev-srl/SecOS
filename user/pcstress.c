/* pcstress — page-cache coherence stress (M50 B2).
 * SPDX-License-Identifier: MIT
 *
 *   pcstress <dir> [rounds] [checkers] [hammers]
 *
 * Two files <dir>/pcs.0 and <dir>/pcs.1, each owned by one writer process.
 * Round v of a writer changes its file with one operation picked by v
 * (overwrite in place, O_TRUNC + rewrite, O_APPEND one page, ftruncate
 * shrink, ftruncate grow) so the file always holds 1..16 pages; page p of
 * version v is the pattern word[i] = v<<32 | file<<24 | p<<16 | i, a page
 * grown by ftruncate is all zero (version 0).
 *
 * After each operation the writer checks the file itself through read() and
 * a fresh MAP_PRIVATE mmap, then sends the expected state (size + version of
 * every page) to its `checkers` (pipes), which check it the same way from
 * their own process and acknowledge; only then does the next round start.
 * Meanwhile `hammers` per file read the file in a loop through read() and
 * mmap without any synchronisation, so the page cache is full of the
 * previous version when each write lands: every page they see must be a
 * well-formed page of the right file and page index (any version), and
 * every lock-step check must see exactly the new version — a stale cached
 * page after a completed write, a torn page, or read()/mmap disagreeing is a
 * failure.
 *
 * Prints "[pcstress] <dir> PASS rounds=N checks=M" or "... FAIL ..." lines;
 * exit 0 = pass.
 */
#include "libsecos.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define P(s) write(1, s, strlen(s))
#define NFILES   2
#define MAXPG    16
#define MAXCHK   4
#define MAXHAM   6

typedef struct { unsigned v, npages, pver[MAXPG]; } state_t;

static char g_dir[128];
static char g_buf[(MAXPG + 1) * 4096];
static unsigned char g_wb[MAXPG * 4096];

static void put_dec(unsigned long v) {
    char b[24]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    char o[24]; int k = 0; while (n) o[k++] = b[--n];
    write(1, o, (unsigned long)k);
}
static void path_of(char* out, const char* name) {
    size_t n = strlen(g_dir);
    memcpy(out, g_dir, n); out[n] = '/';
    strcpy(out + n + 1, name);
}
static void file_path(char* out, int f) { char nm[8] = "pcs.0"; nm[4] = (char)('0' + f); path_of(out, nm); }

static void fill_page(unsigned char* pg, unsigned v, unsigned f, unsigned p) {
    unsigned long* w = (unsigned long*)pg;
    for (unsigned i = 0; i < 512; i++)
        w[i] = v ? (((unsigned long)v << 32) | ((unsigned long)f << 24) | ((unsigned long)p << 16) | i) : 0;
}
/* 1 if 'pg' is page p of file f at version v (v=0: all zero). */
static int page_is(const unsigned char* pg, unsigned v, unsigned f, unsigned p) {
    const unsigned long* w = (const unsigned long*)pg;
    for (unsigned i = 0; i < 512; i++) {
        unsigned long e = v ? (((unsigned long)v << 32) | ((unsigned long)f << 24) | ((unsigned long)p << 16) | i) : 0;
        if (w[i] != e) return 0;
    }
    return 1;
}
/* Version of a well-formed page p of file f, or -1 if it is not one. */
static long page_version(const unsigned char* pg, unsigned f, unsigned p) {
    const unsigned long* w = (const unsigned long*)pg;
    unsigned v = (unsigned)(w[0] >> 32);
    return page_is(pg, v, f, p) ? (long)v : -1;
}

static int read_all(int fd, char* buf, int cap) {
    int got = 0;
    while (got < cap) {
        int r = (int)read(fd, buf + got, (unsigned long)(cap - got));
        if (r <= 0) break;
        got += r;
    }
    return got;
}
static int read_exact(int fd, void* p, int n) {
    int got = 0;
    while (got < n) { int r = (int)read(fd, (char*)p + got, (unsigned long)(n - got)); if (r <= 0) return -1; got += r; }
    return 0;
}

static void fail_line(int f, const char* who, const char* what, unsigned v, unsigned pg) {
    P("[pcstress] "); P(g_dir); P(" FAIL file="); put_dec((unsigned long)f);
    P(" "); P(who); P(": "); P(what); P(" round="); put_dec(v); P(" page="); put_dec(pg); P("\n");
}

/* Check file f against state s through read() and through a fresh mmap. */
static int check_state(int f, const state_t* s, const char* who) {
    char path[160]; file_path(path, f);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fail_line(f, who, "open", s->v, 0); return 0; }
    long size = lseek(fd, 0, SEEK_END);
    if (size != (long)s->npages * 4096) { fail_line(f, who, "size", s->v, (unsigned)(size / 4096)); close(fd); return 0; }
    lseek(fd, 0, SEEK_SET);
    int n = read_all(fd, g_buf, (int)sizeof(g_buf));
    if (n != (int)size) { fail_line(f, who, "read() length", s->v, (unsigned)(n / 4096)); close(fd); return 0; }
    for (unsigned p = 0; p < s->npages; p++)
        if (!page_is((unsigned char*)g_buf + p * 4096, s->pver[p], (unsigned)f, p)) {
            fail_line(f, who, "read() stale or torn page", s->v, p); close(fd); return 0;
        }
    unsigned char* m = (unsigned char*)mmap_file(0, (unsigned long)size, PROT_READ, MAP_PRIVATE, fd);
    if ((long)m == -1L) { fail_line(f, who, "mmap", s->v, 0); close(fd); return 0; }
    int good = 1;
    for (unsigned p = 0; p < s->npages && good; p++)
        if (!page_is(m + p * 4096, s->pver[p], (unsigned)f, p)) { fail_line(f, who, "mmap stale or torn page", s->v, p); good = 0; }
    munmap(m, (unsigned long)size);
    close(fd);
    return good;
}

/* One writer round: apply operation v to file f, update the model. */
static int writer_op(int f, state_t* s, unsigned v) {
    char path[160]; file_path(path, f);
    unsigned op = v % 5;
    if (op == 2 && s->npages >= MAXPG) op = 1;
    if (op == 3 && s->npages <= 1) op = 4;
    if (op == 4 && s->npages >= MAXPG) op = 3;
    int fd, r = 0;
    switch (op) {
    case 0:                                   /* overwrite in place */
        fd = open(path, O_RDWR);
        if (fd < 0) return 0;
        for (unsigned p = 0; p < s->npages; p++) { fill_page(g_wb + p * 4096, v, (unsigned)f, p); s->pver[p] = v; }
        r = (int)write(fd, g_wb, (unsigned long)s->npages * 4096) == (int)(s->npages * 4096);
        close(fd);
        break;
    case 1: {                                 /* O_TRUNC + rewrite 1..6 pages */
        unsigned k = 1 + (v * 7) % 6;
        fd = open(path, O_RDWR | O_TRUNC);
        if (fd < 0) return 0;
        for (unsigned p = 0; p < k; p++) { fill_page(g_wb + p * 4096, v, (unsigned)f, p); s->pver[p] = v; }
        s->npages = k;
        r = (int)write(fd, g_wb, (unsigned long)k * 4096) == (int)(k * 4096);
        close(fd);
        break; }
    case 2:                                   /* O_APPEND one page */
        fd = open(path, O_WRONLY | O_APPEND);
        if (fd < 0) return 0;
        fill_page(g_wb, v, (unsigned)f, s->npages);
        r = (int)write(fd, g_wb, 4096) == 4096;
        s->pver[s->npages++] = v;
        close(fd);
        break;
    case 3:                                   /* ftruncate shrink by one page */
        fd = open(path, O_RDWR);
        if (fd < 0) return 0;
        s->npages--;
        r = ftruncate(fd, (long)s->npages * 4096) == 0;
        close(fd);
        break;
    case 4:                                   /* ftruncate grow by one zero page */
        fd = open(path, O_RDWR);
        if (fd < 0) return 0;
        s->pver[s->npages++] = 0;
        r = ftruncate(fd, (long)s->npages * 4096) == 0;
        close(fd);
        break;
    }
    s->v = v;
    if (!r) fail_line(f, "writer", "operation failed", v, op);
    return r;
}

static int stop_requested(void) {
    char p[160]; path_of(p, "pcs.stop");
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

/* Unsynchronised reader: every page seen must be well-formed. */
static int hammer(int f, int id) {
    char path[160]; file_path(path, f);
    unsigned long reads = 0;
    while (!stop_requested()) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        int n = read_all(fd, g_buf, (int)sizeof(g_buf));
        for (int p = 0; p * 4096 + 4096 <= n; p++)
            if (page_version((unsigned char*)g_buf + p * 4096, (unsigned)f, (unsigned)p) < 0) {
                fail_line(f, "hammer read()", "torn or foreign page", 0, (unsigned)p); close(fd); return 1;
            }
        long size = lseek(fd, 0, SEEK_END) & ~4095L;
        if (size > 0 && (id & 1)) {
            unsigned char* m = (unsigned char*)mmap_file(0, (unsigned long)size, PROT_READ, MAP_PRIVATE, fd);
            if ((long)m != -1L) {
                for (long p = 0; p * 4096 < size; p++) {
                    long ver = page_version(m + p * 4096, (unsigned)f, (unsigned)p);
                    /* A page past a concurrent shrink reads as zeros (version 0). */
                    if (ver < 0) { fail_line(f, "hammer mmap", "torn or foreign page", 0, (unsigned)p); munmap(m, (unsigned long)size); close(fd); return 1; }
                }
                munmap(m, (unsigned long)size);
            }
        }
        close(fd);
        reads++;
    }
    return reads ? 0 : 2;
}

int main(int argc, char** argv) {
    if (argc < 2) { P("usage: pcstress <dir> [rounds] [checkers] [hammers]\n"); return 2; }
    strncpy(g_dir, argv[1], sizeof(g_dir) - 1);
    unsigned rounds = argc > 2 ? (unsigned)atoi(argv[2]) : 100;
    int nchk = argc > 3 ? atoi(argv[3]) : 2;
    int nham = argc > 4 ? atoi(argv[4]) : 3;
    if (nchk < 1) nchk = 1;
    if (nchk > MAXCHK) nchk = MAXCHK;
    if (nham < 0) nham = 0;
    if (nham > MAXHAM) nham = MAXHAM;

    char sp[160]; path_of(sp, "pcs.stop"); unlink(sp);
    state_t init[NFILES];
    for (int f = 0; f < NFILES; f++) {             /* version 1: 2 pages */
        char path[160]; file_path(path, f);
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { P("[pcstress] "); P(g_dir); P(" FAIL cannot create test file\n"); return 1; }
        init[f].v = 1; init[f].npages = 2;
        for (unsigned p = 0; p < 2; p++) { fill_page(g_wb + p * 4096, 1, (unsigned)f, p); init[f].pver[p] = 1; }
        write(fd, g_wb, 8192);
        close(fd);
    }

    int st[NFILES][MAXCHK][2], ack[NFILES][2];
    for (int f = 0; f < NFILES; f++) {
        if (pipe(ack[f]) != 0) return 1;
        for (int c = 0; c < nchk; c++) if (pipe(st[f][c]) != 0) return 1;
    }
    int pids[NFILES * (1 + MAXCHK + MAXHAM)], np = 0, writers[NFILES], nw = 0;
    int forkfail = 0;
    for (int f = 0; f < NFILES && !forkfail; f++) {
        for (int c = 0; c < nchk; c++) {            /* checkers */
            int pid = fork();
            if (pid == 0) {
                state_t s;
                while (read_exact(st[f][c][0], &s, (int)sizeof(s)) == 0) {
                    if (s.v == 0) _exit(0);             /* end of run */
                    char a = check_state(f, &s, "checker") ? '1' : '0';
                    write(ack[f][1], &a, 1);
                }
                _exit(3);
            }
            if (pid < 0) { forkfail = 1; break; }
            pids[np++] = pid;
        }
        for (int h = 0; h < nham && !forkfail; h++) {   /* hammers */
            int pid = fork();
            if (pid == 0) _exit(hammer(f, h));
            if (pid < 0) { forkfail = 1; break; }
            pids[np++] = pid;
        }
    }
    for (int f = 0; f < NFILES && !forkfail; f++) {    /* writers */
        int pid = fork();
        if (pid == 0) {
            state_t s = init[f];
            unsigned long checks = 0; int bad = 0;
            for (unsigned v = 2; v < rounds + 2 && !bad; v++) {
                if (!writer_op(f, &s, v)) { bad = 1; break; }
                if (!check_state(f, &s, "writer")) { bad = 1; break; }
                checks++;
                for (int c = 0; c < nchk; c++) write(st[f][c][1], &s, sizeof(s));
                for (int c = 0; c < nchk; c++) {
                    char a = 0;
                    if (read_exact(ack[f][0], &a, 1) != 0 || a != '1') bad = 1;
                    else checks++;
                }
            }
            state_t end; memset(&end, 0, sizeof(end));
            for (int c = 0; c < nchk; c++) write(st[f][c][1], &end, sizeof(end));
            P("[pcstress] "); P(g_dir); P(" writer "); put_dec((unsigned long)f);
            P(bad ? " FAIL" : " ok"); P(" checks="); put_dec(checks); P("\n");
            _exit(bad);
        }
        if (pid < 0) { forkfail = 1; break; }
        writers[nw++] = pid;
    }
    int fails = 0;
    if (forkfail) {
        /* Out of process slots: release whoever already runs (checkers wait
         * for a state message, hammers for the stop file) and fail. */
        P("[pcstress] "); P(g_dir); P(" FAIL fork (process table full?)\n");
        fails++;
        state_t end; memset(&end, 0, sizeof(end));
        for (int f = nw; f < NFILES; f++)          /* files without a writer */
            for (int c = 0; c < nchk; c++) write(st[f][c][1], &end, sizeof(end));
    }
    for (int f = 0; f < nw; f++) {
        int s = waitpid(writers[f]);
        if (!WIFEXITED(s) || WEXITSTATUS(s) != 0) fails++;
    }
    int fd = open(sp, O_RDWR | O_CREAT, 0644); if (fd >= 0) close(fd);
    for (int i = 0; i < np; i++) {
        int s = waitpid(pids[i]);
        if (!WIFEXITED(s) || WEXITSTATUS(s) != 0) fails++;
    }
    unlink(sp);
    for (int f = 0; f < NFILES; f++) { char path[160]; file_path(path, f); unlink(path); }
    P("[pcstress] "); P(g_dir); P(fails ? " FAIL" : " PASS");
    P(" rounds="); put_dec(rounds); P(" procs="); put_dec((unsigned long)(np + NFILES));
    P(" failed="); put_dec((unsigned long)fails); P("\n");
    return fails ? 1 : 0;
}
