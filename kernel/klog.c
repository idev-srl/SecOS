/*
 * klog.c — kernel log ring buffer. See klog.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "klog.h"

extern uint64_t ktime_ns(void);

static char     g_kbuf[KLOG_BUF_SIZE];
static uint64_t g_head;      /* absolute stream position of the next byte      */
static uint64_t g_clear;     /* CLEAR mark (dmesg -c)                          */
static uint64_t g_rd;        /* destructive reader cursor (SYSLOG_ACTION_READ) */
static char     g_line[480]; /* the line being assembled (not yet a record)    */
static int      g_llen;
static uint64_t g_line_ns;   /* when the line's first byte arrived             */
static volatile uint32_t g_klock;
static uint64_t g_nrec;      /* records committed since boot (kmsg seq)        */

/* The log lock is taken with interrupts off. It never waits forever: a CPU
 * that died holding it (or an exception path re-entering the log) must not
 * hang the kernel's last words, so after a bounded spin the byte is dropped
 * from the ring (it still reached debugcon). */
static int klock(uint64_t* fl) {
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(*fl) :: "memory");
    for (uint32_t i = 0; i < 2000000u; i++) {
        if (__sync_lock_test_and_set(&g_klock, 1) == 0) return 1;
        __asm__ volatile ("pause");
    }
    __asm__ volatile ("push %0; popfq" :: "r"(*fl) : "memory", "cc");
    return 0;
}
static void kunlock(uint64_t fl) {
    __sync_lock_release(&g_klock);
    __asm__ volatile ("push %0; popfq" :: "r"(fl) : "memory", "cc");
}

static void ring_put(char c) { g_kbuf[g_head % KLOG_BUF_SIZE] = c; g_head++; }

static int has(const char* s, int n, const char* pat) {
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (pat[k] && i + k < n && s[i + k] == pat[k]) k++;
        if (!pat[k]) return 1;
    }
    return 0;
}
static int line_level(const char* s, int n) {
    if (has(s, n, "[PANIC]") || has(s, n, "KERNEL PANIC")) return 0;
    if (has(s, n, "[EXC]")) return 2;
    if (has(s, n, "FAIL") || has(s, n, "[KILL]") || has(s, n, "ERROR")) return 3;
    if (has(s, n, "WARN")) return 4;
    return 6;
}

static void put_dec(uint64_t v, int width, char pad) {
    char b[24]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n < width) b[n++] = pad;
    while (n--) ring_put(b[n]);
}

/* Emit the assembled line as one record "<L>[sssss.uuuuuu] text\n". */
static void commit_line(void) {
    int n = g_llen;
    while (n > 0 && (g_line[n - 1] == '\r')) n--;
    ring_put('<'); ring_put((char)('0' + line_level(g_line, n))); ring_put('>');
    ring_put('[');
    put_dec(g_line_ns / 1000000000ull, 5, ' ');
    ring_put('.');
    put_dec((g_line_ns % 1000000000ull) / 1000ull, 6, '0');
    ring_put(']'); ring_put(' ');
    for (int i = 0; i < n; i++) ring_put(g_line[i]);
    ring_put('\n');
    g_llen = 0;
    g_nrec++;
}

void klog_putc(char c) {
    uint64_t fl;
    if (!klock(&fl)) return;
    if (c == '\n') {
        if (g_llen == 0) g_line_ns = ktime_ns();
        commit_line();
    } else if (c != 0) {
        if (g_llen == 0) g_line_ns = ktime_ns();
        g_line[g_llen++] = c;
        if (g_llen == (int)sizeof(g_line)) commit_line();   /* over-long: split */
    }
    kunlock(fl);
}

/* First record boundary at or after `pos` (pos inside the ring). */
static uint64_t align_record(uint64_t pos) {
    if (pos == 0) return 0;
    while (pos < g_head) {
        if (g_kbuf[(pos - 1) % KLOG_BUF_SIZE] == '\n') return pos;
        pos++;
    }
    return g_head;
}

void klog_window(int from_clear, uint64_t* start, uint64_t* end) {
    uint64_t fl;
    if (!klock(&fl)) { *start = *end = 0; return; }
    uint64_t oldest = g_head > KLOG_BUF_SIZE ? g_head - KLOG_BUF_SIZE : 0;
    uint64_t s = oldest;
    if (from_clear && g_clear > s) s = g_clear;
    *start = align_record(s);
    *end = g_head;
    kunlock(fl);
}

size_t klog_copy(uint64_t pos, char* dst, size_t len) {
    uint64_t fl;
    if (!klock(&fl)) return 0;
    uint64_t oldest = g_head > KLOG_BUF_SIZE ? g_head - KLOG_BUF_SIZE : 0;
    size_t n = 0;
    if (pos >= oldest) {
        while (n < len && pos + n < g_head) {
            dst[n] = g_kbuf[(pos + n) % KLOG_BUF_SIZE];
            n++;
        }
    }
    kunlock(fl);
    return n;
}

void klog_clear(void) {
    uint64_t fl;
    if (!klock(&fl)) return;
    g_clear = g_head;
    kunlock(fl);
}

uint64_t klog_read_pos(void) { return g_rd; }
void klog_set_read_pos(uint64_t pos) { g_rd = pos; }

/* /dev/kmsg: one Linux kmsg record "L,seq,usec,-;text\n" per read(). File
 * offset `off` is the byte position in the stream of converted records, counted
 * from the oldest record still in the ring (a reader that keeps up stays
 * aligned; one overtaken by the ring wrapping simply resumes at a later
 * record). Returns the record length, 0 at the end, -1 if `len` is too small. */
int klog_kmsg_read(uint64_t off, char* out, size_t len) {
    static char rec[600];
    uint64_t fl;
    if (!klock(&fl)) return 0;
    uint64_t oldest = g_head > KLOG_BUF_SIZE ? g_head - KLOG_BUF_SIZE : 0;
    uint64_t pos = align_record(oldest);
    uint64_t n = 0;                                   /* records in the window */
    for (uint64_t q = pos; q < g_head; q++) if (g_kbuf[q % KLOG_BUF_SIZE] == '\n') n++;
    uint64_t seq = g_nrec - n, cum = 0;
    int ret = 0;
    while (pos < g_head) {
        /* one ring record: "<L>[sssss.uuuuuu] text\n" */
        int lvl = 6; uint64_t sec = 0, usec = 0;
        uint64_t q = pos;
        char ch = g_kbuf[q % KLOG_BUF_SIZE];
        if (ch == '<' && q + 2 < g_head) { lvl = g_kbuf[(q + 1) % KLOG_BUF_SIZE] - '0'; q += 3; }
        if (q < g_head && g_kbuf[q % KLOG_BUF_SIZE] == '[') {
            q++;
            int frac = 0;
            while (q < g_head && (ch = g_kbuf[q % KLOG_BUF_SIZE]) != ']') {
                if (ch == '.') frac = 1;
                else if (ch >= '0' && ch <= '9') { if (frac) usec = usec * 10 + (uint64_t)(ch - '0'); else sec = sec * 10 + (uint64_t)(ch - '0'); }
                q++;
            }
            q++;                                      /* ']' */
            if (q < g_head && g_kbuf[q % KLOG_BUF_SIZE] == ' ') q++;
        }
        int o = 0;
        char num[24]; int nn;
        rec[o++] = (char)('0' + (lvl < 0 || lvl > 7 ? 6 : lvl)); rec[o++] = ',';
        nn = 0; { uint64_t v = seq; do { num[nn++] = (char)('0' + v % 10); v /= 10; } while (v); }
        while (nn) rec[o++] = num[--nn];
        rec[o++] = ',';
        nn = 0; { uint64_t v = sec * 1000000ull + usec; do { num[nn++] = (char)('0' + v % 10); v /= 10; } while (v); }
        while (nn) rec[o++] = num[--nn];
        rec[o++] = ','; rec[o++] = '-'; rec[o++] = ';';
        while (q < g_head && (ch = g_kbuf[q % KLOG_BUF_SIZE]) != '\n') { if (o < (int)sizeof(rec) - 1) rec[o++] = ch; q++; }
        rec[o++] = '\n';
        q++;                                          /* past the record's '\n' */
        if (cum >= off) {
            if ((size_t)o > len) { ret = -1; break; }
            for (int i = 0; i < o; i++) out[i] = rec[i];
            ret = o;
            break;
        }
        cum += (uint64_t)o; seq++; pos = q;
    }
    kunlock(fl);
    return ret;
}
