/* fsstress — generic filesystem stress (M51 C3, plan §7 test 1).
 * SPDX-License-Identifier: MIT
 *
 *   fsstress <dir> [workers] [iters] [seed]     run, verify, clean up
 *   fsstress -k <dir> [workers] [iters] [seed]  same, keep the tree
 *   fsstress -c <base>                          check the log of an earlier
 *                                               (possibly crashed) run
 *
 * Makes <dir>/fss.<pid> with one directory per worker (w0, w1, ...), a
 * shared directory and a shared log, then forks `workers` processes (the
 * kernel places them round-robin on the CPUs). Worker w owns the slot files
 * w<w>/f0..f7 and keeps a model of each one: version, size and how much of
 * it is pattern (the rest reads as zeros after a truncate-grow). Per
 * iteration it does one or more of:
 *   - rewrite a slot with O_TRUNC in random chunk sizes (new version);
 *   - O_APPEND more pattern of the same version;
 *   - truncate shrink / grow (grown bytes must read as zero);
 *   - rename the slot into the shared directory and back, or onto another
 *     slot (over an existing target);
 *   - unlink a slot;
 *   - mkdir a two-level directory, create a file in it, stat, unlink, rmdir;
 *   - symlink + readlink, chmod + stat, link + unlink;
 *   - getdents of its own directory (must equal the model) and of the shared
 *     directory (must succeed while other workers change it);
 *   - stat (size must match) and a full read() compared with a MAP_PRIVATE
 *     mmap of the same file and with the model;
 * and appends one line (32..4000 bytes, "L w=.. i=.. n=.. " + payload) to the
 * shared log with O_APPEND in a single write(). At the end every worker
 * checks all its slots, and the parent checks the log: every line well
 * formed with its exact length and payload (no torn or interleaved lines),
 * and the lines of each worker numbered 0..iters-1 in order (none lost, none
 * doubled). -c applies the same log check to a run that was cut off (each
 * worker's lines must still be a gap-free prefix 0..k-1).
 *
 * Output: "[fsstress] <base> PASS ..." or "[fsstress] FAIL ..." lines; exit
 * 0 = pass. The target directory is a parameter so the same program covers
 * ext4, FAT32, exFAT and ramfs (a FS without symlinks/hard links/modes:
 * pass -n to skip those ops).
 */
#include "libsecos.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>

#define P(s) write(1, s, strlen(s))
#define NSLOT    8
#define MAXFILE  24576
#define MAXW     16
#define LOGMAX   4000

typedef struct { unsigned ver, size, valid, pid; int exists; } slot_t;   /* pid: pattern id (slot it was written in) */

static char g_base[160];
static int  g_noposix;                 /* -n: no symlink/link/chmod */
static unsigned char g_buf[MAXFILE + 4096];
static unsigned char g_rd[MAXFILE + 4096];
static char g_line[LOGMAX + 64];

/* ---- small helpers ------------------------------------------------------ */
static int dec(char* o, unsigned long v) {
    char b[24]; int n = 0, k = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) o[k++] = b[--n];
    o[k] = 0; return k;
}
static void put_dec(unsigned long v) { char b[24]; int n = dec(b, v); write(1, b, (unsigned long)n); }
static void cat(char* o, const char* s) { strcpy(o + strlen(o), s); }
static void catn(char* o, unsigned long v) { dec(o + strlen(o), v); }

static unsigned long g_rng;
static unsigned rnd(unsigned n) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return n ? (unsigned)(g_rng % n) : 0;
}

/* Pattern byte x of version v of slot s of worker w (never 0, so a zero
 * byte inside the pattern part is always a corruption). */
static unsigned char pat(unsigned w, unsigned s, unsigned v, unsigned x) {
    unsigned h = x * 2654435761u ^ (v * 40503u + w * 977u + s * 131u);
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    unsigned char c = (unsigned char)h;
    return c ? c : 0x5A;
}

static int g_me;
static unsigned long g_ops, g_fail;
static void fail(const char* what, unsigned s, unsigned long a, unsigned long b) {
    g_fail++;
    if (g_fail > 20) return;
    char m[256] = "[fsstress] FAIL w="; catn(m, (unsigned long)g_me);
    cat(m, " slot="); catn(m, s); cat(m, " "); cat(m, what);
    cat(m, " a="); catn(m, a); cat(m, " b="); catn(m, b); cat(m, "\n");
    P(m);
}

static void wdir(char* o, int w) { strcpy(o, g_base); cat(o, "/w"); catn(o, (unsigned long)w); }
static void spath(char* o, int w, unsigned s) { wdir(o, w); cat(o, "/f"); catn(o, s); }

static int write_all(int fd, const unsigned char* p, unsigned n) {
    unsigned done = 0;
    while (done < n) {
        long r = write(fd, p + done, n - done);
        if (r <= 0) return -1;
        done += (unsigned)r;
    }
    return 0;
}
static long read_all(int fd, unsigned char* p, unsigned max) {
    unsigned done = 0;
    while (done < max) {
        long r = read(fd, p + done, max - done);
        if (r < 0) return -1;
        if (r == 0) break;
        done += (unsigned)r;
    }
    return (long)done;
}

/* ---- the model ------------------------------------------------------------ */
static slot_t g_sl[NSLOT];

static int expect_byte(unsigned s, unsigned x) {
    return x < g_sl[s].valid ? pat((unsigned)g_me, g_sl[s].pid, g_sl[s].ver, x) : 0;
}

/* Check slot s through stat, read() and (sometimes) mmap. */
static void check_slot(unsigned s, int use_mmap) {
    char p[200]; spath(p, g_me, s);
    struct stat st;
    int sr = stat(p, &st);
    g_ops++;
    if (!g_sl[s].exists) {
        if (sr == 0) fail("exists but model says unlinked", s, st.st_size, 0);
        return;
    }
    if (sr != 0) { fail("stat failed", s, 0, 0); return; }
    if (st.st_size != g_sl[s].size) { fail("stat size", s, st.st_size, g_sl[s].size); return; }
    int fd = open(p, O_RDONLY);
    if (fd < 0) { fail("open for check", s, 0, 0); return; }
    long n = read_all(fd, g_rd, MAXFILE + 4096);
    if (n != (long)g_sl[s].size) { fail("read size", s, (unsigned long)n, g_sl[s].size); close(fd); return; }
    for (unsigned x = 0; x < (unsigned)n; x++)
        if (g_rd[x] != expect_byte(s, x)) { fail("read content", s, x, g_sl[s].ver); close(fd); return; }
    if (use_mmap && n > 0) {
        unsigned char* m = (unsigned char*)mmap_file(0, (unsigned long)n, PROT_READ, MAP_PRIVATE, fd);
        if ((long)m == -1L) fail("mmap", s, 0, 0);
        else {
            for (unsigned x = 0; x < (unsigned)n; x++)
                if (m[x] != g_rd[x]) { fail("mmap differs from read", s, x, g_sl[s].ver); break; }
            munmap(m, (unsigned long)n);
        }
        g_ops++;
    }
    close(fd);
}

static void op_rewrite(unsigned s, unsigned v) {
    char p[200]; spath(p, g_me, s);
    unsigned size = rnd(MAXFILE);
    if (rnd(4) == 0) size = rnd(64);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { fail("create", s, 0, 0); return; }
    for (unsigned x = 0; x < size; x++) g_buf[x] = pat((unsigned)g_me, s, v, x);
    unsigned done = 0;
    while (done < size) {
        unsigned c = 1 + rnd(6000); if (c > size - done) c = size - done;
        if (write_all(fd, g_buf + done, c) != 0) { fail("write", s, done, size); close(fd); g_sl[s].exists = 0; return; }
        done += c;
    }
    close(fd);
    g_sl[s].exists = 1; g_sl[s].ver = v; g_sl[s].size = size; g_sl[s].valid = size; g_sl[s].pid = s;
    g_ops++;
}

static void op_append(unsigned s) {
    slot_t* m = &g_sl[s];
    if (!m->exists || m->valid != m->size) return;
    unsigned n = 1 + rnd(5000);
    if (m->size + n > MAXFILE) return;
    char p[200]; spath(p, g_me, s);
    int fd = open(p, O_WRONLY | O_APPEND);
    if (fd < 0) { fail("open append", s, 0, 0); return; }
    for (unsigned i = 0; i < n; i++) g_buf[i] = pat((unsigned)g_me, m->pid, m->ver, m->size + i);
    if (write_all(fd, g_buf, n) != 0) fail("append write", s, n, m->size);
    close(fd);
    m->size += n; m->valid = m->size;
    g_ops++;
}

static void op_truncate(unsigned s) {
    slot_t* m = &g_sl[s];
    if (!m->exists) return;
    char p[200]; spath(p, g_me, s);
    unsigned n = rnd(MAXFILE);
    if (truncate(p, (long)n) != 0) { fail("truncate", s, n, m->size); return; }
    if (n < m->valid) m->valid = n;
    m->size = n;
    g_ops++;
}

static void op_rename_shared(unsigned s) {
    if (!g_sl[s].exists) return;
    char a[200], b[200]; spath(a, g_me, s);
    strcpy(b, g_base); cat(b, "/shared/w"); catn(b, (unsigned long)g_me); cat(b, ".f"); catn(b, s);
    if (rename(a, b) != 0) { fail("rename to shared", s, 0, 0); return; }
    struct stat st;
    if (stat(a, &st) == 0) fail("old name still there after rename", s, 0, 0);
    if (rename(b, a) != 0) { fail("rename back", s, 0, 0); g_sl[s].exists = 0; return; }
    g_ops += 2;
}

static void op_rename_slot(unsigned s, unsigned t) {
    if (!g_sl[s].exists || s == t) return;
    char a[200], b[200]; spath(a, g_me, s); spath(b, g_me, t);
    if (rename(a, b) != 0) { fail("rename over slot", s, t, 0); return; }
    g_sl[t] = g_sl[s]; g_sl[s].exists = 0;
    g_ops++;
}

static void op_unlink(unsigned s) {
    if (!g_sl[s].exists) return;
    char p[200]; spath(p, g_me, s);
    if (unlink(p) != 0) { fail("unlink", s, 0, 0); return; }
    g_sl[s].exists = 0;
    g_ops++;
}

static void op_dirs(unsigned i) {
    char d1[200], d2[200], f[220];
    wdir(d1, g_me); cat(d1, "/d"); catn(d1, i);
    strcpy(d2, d1); cat(d2, "/sub");
    strcpy(f, d2); cat(f, "/x");
    if (mkdir(d1, 0755) != 0) { fail("mkdir", 0, i, 0); return; }
    if (mkdir(d2, 0755) != 0) { fail("mkdir sub", 0, i, 0); rmdir(d1); return; }
    int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) fail("create in sub", 0, i, 0);
    else {
        unsigned n = 1 + rnd(3000);
        for (unsigned x = 0; x < n; x++) g_buf[x] = pat((unsigned)g_me, 99, i, x);
        if (write_all(fd, g_buf, n) != 0) fail("write in sub", 0, i, n);
        close(fd);
        struct stat st;
        if (stat(f, &st) != 0 || st.st_size != n) fail("stat in sub", 0, i, n);
        if (rmdir(d2) == 0) fail("rmdir of a non-empty dir succeeded", 0, i, 0);
        if (unlink(f) != 0) fail("unlink in sub", 0, i, 0);
    }
    if (rmdir(d2) != 0) fail("rmdir sub", 0, i, 0);
    if (rmdir(d1) != 0) fail("rmdir", 0, i, 0);
    g_ops += 6;
}

static void op_posix(unsigned s, unsigned i) {
    if (g_noposix || !g_sl[s].exists) return;
    char p[200], l[200], h[200], t[64];
    spath(p, g_me, s);
    wdir(l, g_me); cat(l, "/l"); catn(l, i);
    t[0] = 'f'; dec(t + 1, s);
    if (symlink(t, l) != 0) fail("symlink", s, i, 0);
    else {
        char got[64];
        long n = readlink(l, got, sizeof(got) - 1);
        if (n < 0 || n != (long)strlen(t) || memcmp(got, t, (unsigned long)n) != 0) fail("readlink", s, (unsigned long)n, 0);
        struct stat st;
        if (stat(l, &st) != 0 || st.st_size != g_sl[s].size) fail("stat through symlink", s, st.st_size, g_sl[s].size);
        if (unlink(l) != 0) fail("unlink symlink", s, i, 0);
    }
    if (chmod(p, 0600) != 0) fail("chmod", s, 0, 0);
    else {
        struct stat st;
        if (stat(p, &st) != 0 || (st.st_mode & 0777) != 0600) fail("mode after chmod", s, st.st_mode & 0777, 0600);
        chmod(p, 0644);
    }
    wdir(h, g_me); cat(h, "/h"); catn(h, i);
    if (link(p, h) != 0) fail("link", s, i, 0);
    else {
        struct stat st;
        if (stat(p, &st) != 0 || st.st_nlink != 2) fail("nlink after link", s, st.st_nlink, 2);
        if (unlink(h) != 0) fail("unlink hard link", s, i, 0);
        if (stat(p, &st) != 0 || st.st_nlink != 1) fail("nlink after unlink", s, st.st_nlink, 1);
    }
    g_ops += 8;
}

static void op_listing(void) {
    char d[200]; wdir(d, g_me);
    DIR* dp = opendir(d);
    if (!dp) { fail("opendir", 0, 0, 0); return; }
    int seen[NSLOT]; for (int s = 0; s < NSLOT; s++) seen[s] = 0;
    struct dirent* e;
    while ((e = readdir(dp)) != 0) {
        const char* n = e->d_name;
        if (n[0] == '.') continue;
        /* FAT32 lists short names in upper case (F0..F7). */
        if ((n[0] == 'f' || n[0] == 'F') && n[1] >= '0' && n[1] < '0' + NSLOT && n[2] == 0) seen[n[1] - '0']++;
        else fail("unexpected entry in own dir", 0, (unsigned long)(unsigned char)n[0], 0);
    }
    closedir(dp);
    for (int s = 0; s < NSLOT; s++)
        if (seen[s] != (g_sl[s].exists ? 1 : 0)) fail("listing differs from model", (unsigned)s, (unsigned long)seen[s], (unsigned long)g_sl[s].exists);
    char sh[200]; strcpy(sh, g_base); cat(sh, "/shared");
    dp = opendir(sh);
    if (!dp) fail("opendir shared", 0, 0, 0);
    else { while (readdir(dp) != 0) {} closedir(dp); }
    g_ops += 2;
}

/* One log line: "L w=WW i=IIIII n=NNNN " + payload + "\n", n bytes in all. */
static int make_line(unsigned w, unsigned i, unsigned n) {
    char* o = g_line;
    strcpy(o, "L w=");
    if (w < 10) cat(o, "0");
    catn(o, w); cat(o, " i=");
    char t[16]; int k = dec(t, i); for (int z = k; z < 5; z++) cat(o, "0"); cat(o, t);
    cat(o, " n=");
    k = dec(t, n); for (int z = k; z < 4; z++) cat(o, "0"); cat(o, t); cat(o, " ");
    unsigned h = (unsigned)strlen(o);
    for (unsigned j = h; j < n - 1; j++) o[j] = (char)('a' + (w * 7 + i * 3 + j) % 26);
    o[n - 1] = '\n';
    return (int)n;
}

static void log_append(int lfd, unsigned i) {
    unsigned n = 32 + rnd(LOGMAX - 32);
    make_line((unsigned)g_me, i, n);
    long r = write(lfd, g_line, n);
    if (r != (long)n) fail("log append", 0, (unsigned long)r, n);
    g_ops++;
}

static int worker(int w, unsigned iters, unsigned long seed) {
    g_me = w;
    g_rng = seed * 0x9E3779B97F4A7C15ul + (unsigned long)w * 0xBF58476D1CE4E5B9ul + 1;
    char lp[200]; strcpy(lp, g_base); cat(lp, "/log");
    int lfd = open(lp, O_WRONLY | O_APPEND);
    if (lfd < 0) { fail("open log", 0, 0, 0); return 1; }
    for (unsigned i = 0; i < iters && g_fail == 0; i++) {
        unsigned s = rnd(NSLOT), v = i + 1;
        switch (rnd(12)) {
        case 0: case 1: case 2: op_rewrite(s, v); break;
        case 3: op_append(s); break;
        case 4: op_truncate(s); break;
        case 5: op_rename_shared(s); break;
        case 6: op_rename_slot(s, rnd(NSLOT)); break;
        case 7: op_unlink(s); break;
        case 8: op_dirs(i); break;
        case 9: op_posix(s, i); break;
        case 10: op_listing(); break;
        default: check_slot(s, 1); break;
        }
        if (rnd(3) == 0) check_slot(rnd(NSLOT), rnd(2));
        log_append(lfd, i);
    }
    close(lfd);
    for (unsigned s = 0; s < NSLOT; s++) check_slot(s, 1);
    op_listing();
    P("[fsstress] worker "); put_dec((unsigned long)w); P(g_fail ? " FAIL" : " ok");
    P(" ops="); put_dec(g_ops); P("\n");
    return g_fail ? 1 : 0;
}

/* ---- log check -------------------------------------------------------------- */
static int num(const char* p, int digits, unsigned* out) {
    unsigned v = 0;
    for (int k = 0; k < digits; k++) { if (p[k] < '0' || p[k] > '9') return -1; v = v * 10 + (unsigned)(p[k] - '0'); }
    *out = v; return 0;
}
/* Returns the number of bad lines; next[w] = lines seen of worker w. */
static unsigned long check_log(const char* base, unsigned next[MAXW], unsigned long* lines) {
    char lp[200]; strcpy(lp, base); cat(lp, "/log");
    int fd = open(lp, O_RDONLY);
    if (fd < 0) { P("[fsstress] FAIL cannot open log\n"); return 1; }
    for (int w = 0; w < MAXW; w++) next[w] = 0;
    unsigned long bad = 0, off = 0; *lines = 0;
    static char lb[LOGMAX + 64];
    unsigned have = 0;
    for (;;) {
        long r = read(fd, lb + have, sizeof(lb) - have);
        if (r < 0) { bad++; break; }
        have += (unsigned)r;
        if (have == 0) break;
        /* one header needs 22 bytes */
        if (have < 22) {
            if (r == 0) { P("[fsstress] FAIL log ends with a partial line\n"); bad++; break; }
            continue;
        }
        unsigned w, i, n;
        if (lb[0] != 'L' || memcmp(lb + 1, " w=", 3) != 0 || num(lb + 4, 2, &w) || memcmp(lb + 6, " i=", 3) != 0 ||
            num(lb + 9, 5, &i) || memcmp(lb + 14, " n=", 3) != 0 || num(lb + 17, 4, &n) || lb[21] != ' ' ||
            w >= MAXW || n < 32 || n > LOGMAX) {
            P("[fsstress] FAIL malformed log line at byte "); put_dec(off); P("\n");
            bad++; break;
        }
        if (have < n) {
            if (r == 0) { P("[fsstress] FAIL log ends inside a line at byte "); put_dec(off); P("\n"); bad++; break; }
            continue;
        }
        make_line(w, i, n);
        if (memcmp(lb, g_line, n) != 0) {
            P("[fsstress] FAIL torn or interleaved log line at byte "); put_dec(off);
            P(" w="); put_dec(w); P(" i="); put_dec(i); P("\n");
            bad++; break;
        }
        if (i != next[w]) {
            P("[fsstress] FAIL log line out of order w="); put_dec(w); P(" i="); put_dec(i);
            P(" expected "); put_dec(next[w]); P("\n");
            bad++;
        }
        next[w] = i + 1;
        (*lines)++;
        off += n;
        memmove(lb, lb + n, have - n); have -= n;
    }
    close(fd);
    return bad;
}

static void rm_tree(const char* d) {
    DIR* dp = opendir(d);
    if (!dp) { unlink(d); return; }
    char names[64][32]; int nn = 0, more = 1;
    while (more) {
        nn = 0; more = 0;
        struct dirent* e;
        while ((e = readdir(dp)) != 0) {
            if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
            if (nn == 64) { more = 1; break; }
            strncpy(names[nn], e->d_name, 31); names[nn][31] = 0; nn++;
        }
        closedir(dp);
        for (int k = 0; k < nn; k++) {
            char p[256]; strcpy(p, d); cat(p, "/"); cat(p, names[k]);
            struct stat st;
            if (lstat(p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) rm_tree(p);
            else unlink(p);
        }
        if (more) { dp = opendir(d); if (!dp) break; }
    }
    rmdir(d);
}

static unsigned long uarg(const char* s, unsigned long dflt) {
    if (!s) return dflt;
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned long)(*s++ - '0');
    return v;
}

int main(int argc, char** argv) {
    int a = 1, keep = 0, checkonly = 0;
    while (a < argc && argv[a][0] == '-') {
        if (argv[a][1] == 'k') keep = 1;
        else if (argv[a][1] == 'c') checkonly = 1;
        else if (argv[a][1] == 'n') g_noposix = 1;
        else break;
        a++;
    }
    if (a >= argc) { P("usage: fsstress [-k] [-n] <dir> [workers] [iters] [seed] | fsstress -c <base>\n"); return 2; }
    if (checkonly) {
        unsigned next[MAXW]; unsigned long lines;
        unsigned long bad = check_log(argv[a], next, &lines);
        P("[fsstress] check "); P(argv[a]); P(bad ? " FAIL" : " PASS");
        P(" lines="); put_dec(lines); P(" per-worker=");
        for (int w = 0; w < MAXW; w++) { if (w) P(","); put_dec(next[w]); }
        P("\n");
        return bad ? 1 : 0;
    }
    unsigned nw = (unsigned)uarg(a + 1 < argc ? argv[a + 1] : 0, 6);
    unsigned iters = (unsigned)uarg(a + 2 < argc ? argv[a + 2] : 0, 200);
    unsigned long seed = uarg(a + 3 < argc ? argv[a + 3] : 0, 1);
    if (nw < 1) nw = 1;
    if (nw > MAXW) nw = MAXW;
    strcpy(g_base, argv[a]); cat(g_base, "/fss."); catn(g_base, (unsigned long)getpid());
    if (mkdir(g_base, 0755) != 0) { P("[fsstress] FAIL cannot mkdir "); P(g_base); P("\n"); return 1; }
    char p[200];
    strcpy(p, g_base); cat(p, "/shared"); mkdir(p, 0755);
    for (unsigned w = 0; w < nw; w++) { wdir(p, (int)w); if (mkdir(p, 0755) != 0) { P("[fsstress] FAIL mkdir worker dir\n"); return 1; } }
    strcpy(p, g_base); cat(p, "/log");
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { P("[fsstress] FAIL cannot create log\n"); return 1; }
    close(fd);
    P("[fsstress] start "); P(g_base); P(" workers="); put_dec(nw); P(" iters="); put_dec(iters);
    P(" seed="); put_dec(seed); P("\n");

    int pids[MAXW]; unsigned np = 0; unsigned long fails = 0;
    for (unsigned w = 0; w < nw; w++) {
        int pid = fork();
        if (pid == 0) _exit(worker((int)w, iters, seed));
        if (pid < 0) { P("[fsstress] FAIL fork\n"); fails++; break; }
        pids[np++] = pid;
    }
    for (unsigned k = 0; k < np; k++) {
        int s = waitpid(pids[k]);
        if (!WIFEXITED(s) || WEXITSTATUS(s) != 0) {
            P("[fsstress] FAIL worker "); put_dec(k); P(" status "); put_dec((unsigned long)s); P("\n");
            fails++;
        }
    }
    unsigned next[MAXW]; unsigned long lines;
    unsigned long bad = check_log(g_base, next, &lines);
    for (unsigned w = 0; w < np; w++)
        if (next[w] != iters) {
            P("[fsstress] FAIL worker "); put_dec(w); P(" logged "); put_dec(next[w]);
            P(" lines, expected "); put_dec(iters); P("\n");
            bad++;
        }
    if (lines != (unsigned long)np * iters) bad++;
    if (!keep && !fails && !bad) rm_tree(g_base);
    P("[fsstress] "); P(g_base); P((fails || bad) ? " FAIL" : " PASS");
    P(" workers="); put_dec(np); P(" iters="); put_dec(iters); P(" loglines="); put_dec(lines);
    P(" failed-workers="); put_dec(fails); P(" bad-log="); put_dec(bad); P("\n");
    return (fails || bad) ? 1 : 0;
}
