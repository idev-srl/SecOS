/*
 * SecOS Kernel - TTY / pty lock self-test (M51 C5, -DSECOS_TTY_TEST)
 *
 * The syscall path still takes the BKL, so a userland stress serializes and
 * cannot show a missing TTY lock. Here every CPU (the BSP from kernel_main,
 * each AP from its idle loop, tty_test_ap) drives the console line discipline
 * and pty pairs at the same time, outside the BKL:
 *
 *   A  console, canonical: half the CPUs type lines into the keyboard ring
 *      (whole lines, atomically), the other half read them with
 *      tty_read_nonblock(); every CPU also pokes the ldisc (FIONREAD, poll,
 *      TCGETS, TIOCGWINSZ, the foreground group). Each line must arrive
 *      exactly once, whole, and every reader must see each typist's lines in
 *      order.
 *   B  the same with ECHO on (echo runs under the console lock and takes
 *      con_lock inside it) while another CPU writes to the console.
 *   C  console, raw (VMIN=1): several typists, one reader, other CPUs pull
 *      the ring into the queue through FIONREAD. Bytes carry (typist, seq):
 *      every typist's stream must come out complete and in order. 0x1B is
 *      one of the bytes, so the ESC wait (lock dropped, re-check) runs.
 *   D  an emulator reply (DSR, ESC[6n) reaches the read queue through the
 *      lock-free reply ring.
 *   E  ptys, raw: two pairs, two writers per direction per pair on different
 *      CPUs, one reader per direction, plus a CPU that opens and closes ptys
 *      all the time. Every stream complete and in order.
 *   F  pty, canonical: one writer, three readers, a CPU polling.
 *   N  negative control: A again with the console lock disabled; the checks
 *      are expected to see damage (informational: a run that sees none only
 *      prints a warning, a race is not guaranteed to show).
 * Output on debugcon: "[TTYTEST] ..." lines, ending with
 * "[TTYTEST] RESULT PASS|FAIL".
 *
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 */
#ifdef SECOS_TTY_TEST
#include <stdint.h>
#include <stddef.h>
#include "tty.h"
#include "pty.h"
#include "signal.h"
#include "debugcon.h"
#include "percpu.h"
#include "keyboard.h"

extern uint64_t ktime_ms(void);
extern uint32_t smp_online_count(void);
extern volatile int g_tty_test_nolock;
extern volatile uint32_t g_tty_test_esc_waits;
extern int  pty_test_open(void** m, void** s, const struct tty_termios* tio);
extern void pty_test_close(void* m, void* s);

#define MAXCPU     8
#define LINE_LEN   8                       /* "L" f seq(5) "\n" */
#define A_LINES    2500
#define B_LINES    250
#define N_LINES    1500
#define C_BYTES    20000
#define E_BYTES    30000
#define F_LINES    3000
#define MAXW       4                       /* writers per stream */
#define PHASE_MS   60000u
#define NEG_MS     8000u                    /* phase N: damage may lose lines */

/* ---- small output helpers --------------------------------------------------- */
static void say(const char* s) { debugcon_writestring(s); }
static void dec(uint64_t v) {
    char b[24]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) debugcon_putchar(b[--n]);
}
static volatile uint32_t g_fail;
static void fail(const char* phase, const char* what, uint64_t a, uint64_t b) {
    __atomic_add_fetch(&g_fail, 1, __ATOMIC_SEQ_CST);
    say("[TTYTEST] FAIL "); say(phase); say(": "); say(what);
    say(" "); dec(a); say(" "); dec(b); say("\n");
}

/* ---- phase machinery -------------------------------------------------------- */
static volatile uint32_t g_phase;          /* 0 = idle */
static volatile uint32_t g_ncpu;
static volatile uint8_t  g_done[16][MAXCPU];
static volatile uint32_t g_ndone[16];
static volatile int      g_running;
static uint64_t          g_t0;
static uint64_t          g_phase_ms = PHASE_MS;

static int expired(void) { return ktime_ms() - g_t0 > g_phase_ms; }

/* ---- termios helpers -------------------------------------------------------- */
static void tio_set(struct tty_termios* t, int canon, int echo) {
    for (unsigned i = 0; i < sizeof(*t); i++) ((uint8_t*)t)[i] = 0;
    t->c_cflag = T_B38400 | T_CS8 | T_CREAD;
    t->c_lflag = (canon ? T_ICANON : 0) | (echo ? T_ECHO : 0);
    t->c_cc[T_VMIN] = 1; t->c_cc[T_VTIME] = 0;
    t->c_cc[T_VEOF] = 0x04; t->c_cc[T_VERASE] = 0x7F;
}
static void con_mode(int canon, int echo) {
    struct tty_termios t;
    tio_set(&t, canon, echo);
    t.c_oflag = T_OPOST | T_ONLCR;
    tty_ldisc_ioctl_kbuf(tty_console(), TTY_TCSETSF, &t);
}
static void drain_console(void) {
    char b[64];
    while (keyboard_getc_raw_nopoll() >= 0) { }
    while (tty_read_nonblock(b, sizeof(b)) > 0) { }
}
/* Ldisc pokes that touch the console state under its lock without consuming. */
static void con_noise(uint32_t i) {
    union { struct tty_termios tio; struct tty_winsize ws; int n; } kb;
    switch (i & 7) {
    case 0: tty_ldisc_ioctl_kbuf(tty_console(), TTY_FIONREAD, &kb); break;
    case 1: (void)tty_ldisc_readable(tty_console()); break;
    case 2: tty_ldisc_ioctl_kbuf(tty_console(), TTY_TCGETS, &kb); break;
    case 3: tty_ldisc_ioctl_kbuf(tty_console(), TTY_TIOCGWINSZ, &kb); break;
    case 4: signal_set_foreground_pgid(0); break;
    case 5: (void)signal_get_foreground_pgid(); break;
    case 6: (void)tty_console_poll(PTY_POLLIN); break;
    default: { struct tty_termios t; tty_get_termios(&t); } break;
    }
}

/* ---- line streams (phases A, B, F, N) ------------------------------------------
 * A line is "L" <typist digit> <5-digit seq> "\n". Readers mark seen[f][seq]
 * and keep, per reader, the last seq of each typist (lines of one typist must
 * come in order). */
#define MAXLINES 3000
static volatile uint8_t  g_seen[MAXW][MAXLINES];
static volatile uint32_t g_got, g_bad, g_dup, g_order;
static volatile uint32_t g_typed[MAXW];
static int  g_lines, g_typists, g_readers;

static void lines_reset(int lines, int typists, int readers) {
    for (int f = 0; f < MAXW; f++) {
        g_typed[f] = 0;
        for (int i = 0; i < MAXLINES; i++) g_seen[f][i] = 0;
    }
    g_got = g_bad = g_dup = g_order = 0;
    g_lines = lines; g_typists = typists; g_readers = readers;
}
static void mk_line(char* b, int f, int seq) {
    b[0] = 'L'; b[1] = (char)('0' + f);
    for (int i = 6; i >= 2; i--) { b[i] = (char)('0' + seq % 10); seq /= 10; }
    b[7] = '\n';
}
/* Check one line a reader got; last[] is that reader's per-typist position. */
static void got_line(const char* b, int n, int* last) {
    int ok = n == LINE_LEN && b[0] == 'L' && b[7] == '\n' && b[1] >= '0' && b[1] < '0' + g_typists;
    int seq = 0;
    for (int i = 2; ok && i <= 6; i++) {
        if (b[i] < '0' || b[i] > '9') ok = 0; else seq = seq * 10 + (b[i] - '0');
    }
    if (ok && seq >= g_lines) ok = 0;
    if (!ok) { __atomic_add_fetch(&g_bad, 1, __ATOMIC_SEQ_CST); return; }
    int f = b[1] - '0';
    if (__atomic_exchange_n(&g_seen[f][seq], 1, __ATOMIC_SEQ_CST))
        __atomic_add_fetch(&g_dup, 1, __ATOMIC_SEQ_CST);
    if (seq <= last[f]) __atomic_add_fetch(&g_order, 1, __ATOMIC_SEQ_CST);
    last[f] = seq;
    __atomic_add_fetch(&g_got, 1, __ATOMIC_SEQ_CST);
}
static int lines_total(void) { return g_lines * g_typists; }
static int lines_over(void) {
    return (int)(g_got + g_bad) >= lines_total() || expired();
}

/* Flow control for the typists: the console ldisc drops input once its read
 * queue (TQ_SIZE) is full, as n_tty does, so a typist faster than the readers
 * would lose lines for a reason that has nothing to do with locking. Type
 * only while the queue (FIONREAD, which also pulls the ring) has room for the
 * ring (256) and a partial line on top. */
static int con_room(void) {
    int n = 0;
    tty_ldisc_ioctl_kbuf(tty_console(), TTY_FIONREAD, &n);
    return n < TQ_SIZE / 2;
}
/* Console typist f: whole lines into the keyboard ring. */
static void con_typist(int f, int noise) {
    char b[LINE_LEN];
    for (int seq = 0; seq < g_lines && !expired(); ) {
        mk_line(b, f, seq);
        if (!con_room()) { __asm__ volatile("pause"); continue; }
        if (keyboard_inject_seq_try(b, LINE_LEN)) { seq++; g_typed[f]++; }
        else __asm__ volatile("pause");
        if (noise) con_noise((uint32_t)seq);
    }
}
static void con_reader(void) {
    int last[MAXW]; for (int i = 0; i < MAXW; i++) last[i] = -1;
    char b[64];
    uint32_t i = 0;
    while (!lines_over()) {
        int r = tty_read_nonblock(b, sizeof(b));
        if (r > 0) got_line(b, r, last);
        else { __asm__ volatile("pause"); if ((++i & 15) == 0) con_noise(i); }
    }
}
/* Console output from another CPU while lines are echoed (phase B). */
static void con_writer(void) {
    static const char msg[] = "[ttytest] concurrent console output\n";
    while (!lines_over()) {
        tty_write(msg, (int)sizeof(msg) - 1);
        for (int k = 0; k < 2000; k++) __asm__ volatile("pause");
    }
}

static int lines_check(const char* ph, int expect_clean) {
    int missing = 0;
    for (int f = 0; f < g_typists; f++)
        for (int s = 0; s < g_lines; s++) if (!g_seen[f][s]) missing++;
    say("[TTYTEST] "); say(ph); say(" lines="); dec(g_got); say("/"); dec((uint64_t)lines_total());
    say(" bad="); dec(g_bad); say(" dup="); dec(g_dup); say(" order="); dec(g_order);
    say(" missing="); dec((uint64_t)missing); say("\n");
    int anomalies = (int)(g_bad + g_dup + g_order) + missing;
    if (expect_clean && anomalies) fail(ph, "line stream damaged", (uint64_t)anomalies, 0);
    return anomalies;
}

/* ---- byte streams (phases C, E) ---------------------------------------------------
 * Writer w of a stream sends bytes (w << 6) | (seq & 63), seq = 0..n-1; the
 * reader demultiplexes by the top two bits and checks each sub-stream. */
typedef struct {
    volatile uint32_t sent[MAXW];
    uint32_t next[MAXW];                   /* reader side */
    volatile uint32_t got, bad;
    int nw, nbytes;
} bstream_t;
static void bs_reset(bstream_t* b, int nw, int nbytes) {
    for (int i = 0; i < MAXW; i++) { b->sent[i] = 0; b->next[i] = 0; }
    b->got = b->bad = 0; b->nw = nw; b->nbytes = nbytes;
}
static void bs_take(bstream_t* b, const uint8_t* d, int n) {
    for (int i = 0; i < n; i++) {
        int w = d[i] >> 6;
        if (w >= b->nw || (d[i] & 63) != (b->next[w] & 63) || b->next[w] >= (uint32_t)b->nbytes) b->bad++;
        else b->next[w]++;
        b->got++;
    }
}
static int bs_done(bstream_t* b) { return (int)b->got >= b->nw * b->nbytes || expired(); }
static void bs_fill(uint8_t* out, int w, uint32_t seq, int n) {
    for (int i = 0; i < n; i++) out[i] = (uint8_t)((w << 6) | ((seq + (uint32_t)i) & 63));
}
static void bs_check(const char* ph, bstream_t* b) {
    say("[TTYTEST] "); say(ph); say(" bytes="); dec(b->got); say("/");
    dec((uint64_t)(b->nw * b->nbytes)); say(" bad="); dec(b->bad); say("\n");
    if (b->bad || (int)b->got != b->nw * b->nbytes) fail(ph, "byte stream damaged", b->got, b->bad);
}

/* ---- phase C: console raw ---------------------------------------------------- */
static bstream_t g_cs;
static void c_typist(int w) {
    uint8_t buf[3];
    uint32_t seq = 0;
    while (seq < (uint32_t)g_cs.nbytes && !expired()) {
        int n = 1 + (int)(seq % 3);
        if (seq + (uint32_t)n > (uint32_t)g_cs.nbytes) n = (int)((uint32_t)g_cs.nbytes - seq);
        bs_fill(buf, w, seq, n);
        if (!con_room()) { __asm__ volatile("pause"); continue; }
        if (keyboard_inject_seq_try((const char*)buf, n)) seq += (uint32_t)n;
        else __asm__ volatile("pause");
    }
    g_cs.sent[w] = seq;
}
static void c_reader(void) {
    uint8_t b[64];
    while (!bs_done(&g_cs)) {
        int r = tty_read_nonblock(b, sizeof(b));
        if (r > 0) bs_take(&g_cs, b, r); else __asm__ volatile("pause");
    }
}
static void c_noise(void) {
    uint32_t i = 0;
    while (!bs_done(&g_cs)) { con_noise(i); i = (i + 1) & 7; __asm__ volatile("pause"); }
}

/* ---- phase E/F: ptys -------------------------------------------------------- */
#define NPTY 2
static void* g_pm[NPTY];
static void* g_ps[NPTY];
static bstream_t g_ms[NPTY], g_sm[NPTY];   /* master->slave, slave->master */
static volatile uint32_t g_churn, g_churn_fail;

typedef struct { int kind, pty, w; uint32_t seq; int done; } role_t;
enum { R_MW = 1, R_SR, R_SW, R_MR, R_CH };
#define NROLES (NPTY * 6 + 1)
static role_t g_roles[NROLES];

static void e_setup_roles(void) {
    int k = 0;
    for (int p = 0; p < NPTY; p++) {
        role_t r[6] = { { R_MW, p, 0, 0, 0 }, { R_MW, p, 1, 0, 0 }, { R_SR, p, 0, 0, 0 },
                        { R_SW, p, 0, 0, 0 }, { R_SW, p, 1, 0, 0 }, { R_MR, p, 0, 0, 0 } };
        for (int i = 0; i < 6; i++) g_roles[k++] = r[i];
    }
    g_roles[k].kind = R_CH; g_roles[k].pty = 0; g_roles[k].w = 0; g_roles[k].seq = 0; g_roles[k].done = 0;
}
/* One step of a role; returns 1 when the role is finished. */
static int e_step(role_t* r) {
    uint8_t b[64];
    if (r->done) return 1;
    if (expired()) { r->done = 1; return 1; }
    switch (r->kind) {
    case R_MW: case R_SW: {
        bstream_t* s = r->kind == R_MW ? &g_ms[r->pty] : &g_sm[r->pty];
        void* ino = r->kind == R_MW ? g_pm[r->pty] : g_ps[r->pty];
        int n = 1 + (int)(r->seq % 37);
        if (r->seq + (uint32_t)n > (uint32_t)s->nbytes) n = (int)((uint32_t)s->nbytes - r->seq);
        bs_fill(b, r->w, r->seq, n);
        int got = pty_write(ino, b, n, 1);
        if (got > 0) r->seq += (uint32_t)got;
        if (r->seq >= (uint32_t)s->nbytes) { s->sent[r->w] = r->seq; r->done = 1; }
        return r->done;
    }
    case R_SR: case R_MR: {
        bstream_t* s = r->kind == R_SR ? &g_ms[r->pty] : &g_sm[r->pty];
        void* ino = r->kind == R_SR ? g_ps[r->pty] : g_pm[r->pty];
        int got = pty_read(ino, b, (int)sizeof(b), 1);
        if (got > 0) bs_take(s, b, got);
        if (bs_done(s)) r->done = 1;
        return r->done;
    }
    case R_CH: {
        void *m, *sl;
        struct tty_termios t; tio_set(&t, 1, 1);
        if (pty_test_open(&m, &sl, &t) == 0) {
            (void)pty_write(m, "x\n", 2, 1);
            (void)pty_poll(sl, PTY_POLLIN);
            (void)pty_read(sl, b, sizeof(b), 1);
            pty_test_close(m, sl);
            g_churn++;
        } else {
            g_churn_fail++;
        }
        if (g_churn >= 300) r->done = 1;
        return r->done;
    }
    }
    return 1;
}
static void e_run(uint32_t cpu) {
    for (;;) {
        int left = 0;
        for (int i = 0; i < NROLES; i++)
            if ((uint32_t)i % g_ncpu == cpu && !e_step(&g_roles[i])) left++;
        if (!left) return;
    }
}

/* phase F: canonical pty, one writer (role on CPU 0), readers elsewhere. */
static void f_writer(void) {
    char b[LINE_LEN];
    int seq = 0, off = 0;
    while (seq < g_lines && !expired()) {
        mk_line(b, 0, seq);
        int r = pty_write(g_pm[0], b + off, LINE_LEN - off, 1);
        if (r > 0) { off += r; if (off == LINE_LEN) { off = 0; seq++; g_typed[0]++; } }
        else __asm__ volatile("pause");
    }
}
static void f_reader(void) {
    int last[MAXW]; for (int i = 0; i < MAXW; i++) last[i] = -1;
    char b[64];
    while (!lines_over()) {
        int r = pty_read(g_ps[0], b, (int)sizeof(b), 1);
        if (r > 0) got_line(b, r, last);
        else { (void)pty_poll(g_ps[0], PTY_POLLIN); __asm__ volatile("pause"); }
    }
}

/* ---- roles per phase ------------------------------------------------------- */
enum { PH_A = 1, PH_B, PH_C, PH_E, PH_F, PH_N, PH_MAX };
static void run_role(uint32_t ph, uint32_t cpu) {
    uint32_t n = g_ncpu;
    switch (ph) {
    case PH_A: case PH_N:
        if (cpu < (uint32_t)g_typists) con_typist((int)cpu, 1); else con_reader();
        if (cpu < (uint32_t)g_typists) con_reader();     /* then help reading */
        break;
    case PH_B:
        if (cpu == 0) con_typist(0, 0);
        else if (cpu == n - 1) con_writer();
        else con_reader();
        if (cpu == 0) con_reader();
        break;
    case PH_C:
        if (cpu == 1) c_reader();
        else if (cpu == 0 || cpu == 2) c_typist(cpu == 0 ? 0 : 1);
        else c_noise();
        break;
    case PH_E:
        e_run(cpu);
        break;
    case PH_F:
        if (cpu == 0) { f_writer(); f_reader(); }
        else f_reader();
        break;
    }
}

void tty_test_ap(cpu_t* c) {
    if (!g_running) return;
    uint32_t ph = g_phase;
    if (!ph || ph >= PH_MAX || c->index >= MAXCPU || c->index >= g_ncpu) return;
    if (g_done[ph][c->index]) return;
    run_role(ph, c->index);
    g_done[ph][c->index] = 1;
    __atomic_add_fetch(&g_ndone[ph], 1, __ATOMIC_SEQ_CST);
}

static void run_phase(uint32_t ph) {
    g_t0 = ktime_ms();
    g_ndone[ph] = 0;
    for (int i = 0; i < MAXCPU; i++) g_done[ph][i] = 0;
    __atomic_store_n(&g_phase, ph, __ATOMIC_SEQ_CST);
    run_role(ph, 0);
    g_done[ph][0] = 1;
    __atomic_add_fetch(&g_ndone[ph], 1, __ATOMIC_SEQ_CST);
    while (g_ndone[ph] < g_ncpu && ktime_ms() - g_t0 < g_phase_ms + 5000) __asm__ volatile("pause");
    __atomic_store_n(&g_phase, 0, __ATOMIC_SEQ_CST);
    if (g_ndone[ph] < g_ncpu) fail("phase", "a CPU did not finish", ph, g_ndone[ph]);
    if (ktime_ms() - g_t0 > g_phase_ms && ph != PH_N) fail("phase", "timed out", ph, ktime_ms() - g_t0);
}

void tty_selftest(void) {
    uint32_t n = smp_online_count();
    if (n > MAXCPU) n = MAXCPU;
    say("[TTYTEST] start cpus="); dec(n); say("\n");
    if (n < 4) { say("[TTYTEST] SKIP: needs -smp 4 or more\n"); say("[TTYTEST] RESULT FAIL\n"); return; }
    g_ncpu = n;
    g_running = 1;
    uint64_t t_all = ktime_ms();

    /* A: console canonical, typists = half the CPUs. */
    drain_console(); con_mode(1, 0); signal_set_foreground_pgid(0);
    lines_reset(A_LINES, (int)(n / 2), (int)(n - n / 2));
    run_phase(PH_A);
    lines_check("A console canon", 1);

    /* B: echo on, a CPU writing to the console meanwhile. */
    drain_console(); con_mode(1, 1);
    lines_reset(B_LINES, 1, (int)n - 2);
    run_phase(PH_B);
    lines_check("B console echo", 1);

    /* C: console raw, 2 typists, 1 reader, the rest pulls through FIONREAD. */
    drain_console(); con_mode(0, 0);
    bs_reset(&g_cs, 2, C_BYTES);
    g_tty_test_esc_waits = 0;
    run_phase(PH_C);
    bs_check("C console raw", &g_cs);
    say("[TTYTEST] C ESC waits (lock dropped, re-checked): "); dec(g_tty_test_esc_waits); say("\n");

    /* D: an emulator reply through the reply ring (raw mode still on). */
    {
        drain_console();
        tty_write("\033[6n", 4);
        char b[32]; int r = 0;
        uint64_t t0 = ktime_ms();
        while (r <= 0 && ktime_ms() - t0 < 500) r = tty_read_nonblock(b, sizeof(b));
        if (r >= 6 && b[0] == 0x1B && b[1] == '[' && b[r - 1] == 'R') {
            say("[TTYTEST] D reply ring: cursor report "); dec((uint64_t)r); say(" bytes OK\n");
        } else {
            say("[TTYTEST] D reply ring: no cursor report (r="); dec((uint64_t)(r < 0 ? 0 : r));
            say(")\n");
            fail("D reply ring", "no DSR reply", (uint64_t)(r < 0 ? 0 : r), 0);
        }
    }

    /* E: ptys raw, two pairs, churn. */
    {
        struct tty_termios t; tio_set(&t, 0, 0);
        int ok = 1;
        for (int p = 0; p < NPTY; p++) {
            if (pty_test_open(&g_pm[p], &g_ps[p], &t) != 0) { ok = 0; fail("E", "pty_test_open", (uint64_t)p, 0); }
            bs_reset(&g_ms[p], 2, E_BYTES);
            bs_reset(&g_sm[p], 2, E_BYTES);
        }
        if (ok) {
            g_churn = g_churn_fail = 0;
            e_setup_roles();
            run_phase(PH_E);
            for (int p = 0; p < NPTY; p++) { bs_check("E pty master->slave", &g_ms[p]); bs_check("E pty slave->master", &g_sm[p]); }
            say("[TTYTEST] E pty churn opens="); dec(g_churn); say(" failed="); dec(g_churn_fail); say("\n");
            if (g_churn < 300 || g_churn_fail) fail("E", "pty churn", g_churn, g_churn_fail);
        }
        for (int p = 0; p < NPTY; p++) if (g_pm[p]) { pty_test_close(g_pm[p], g_ps[p]); g_pm[p] = g_ps[p] = 0; }
    }

    /* F: pty canonical, one writer, readers on every CPU. */
    {
        struct tty_termios t; tio_set(&t, 1, 0);
        if (pty_test_open(&g_pm[0], &g_ps[0], &t) == 0) {
            lines_reset(F_LINES, 1, (int)n);
            run_phase(PH_F);
            lines_check("F pty canon", 1);
            pty_test_close(g_pm[0], g_ps[0]); g_pm[0] = g_ps[0] = 0;
        } else fail("F", "pty_test_open", 0, 0);
    }

    /* N: negative control, A without the console lock (informational). */
    {
        drain_console(); con_mode(1, 0);
        lines_reset(N_LINES, (int)(n / 2), (int)(n - n / 2));
        g_phase_ms = NEG_MS;
        g_tty_test_nolock = 1;
        run_phase(PH_N);
        g_tty_test_nolock = 0;
        g_phase_ms = PHASE_MS;
        int an = lines_check("N negative control (no lock)", 0);
        if (an) { say("[TTYTEST] N negative control: damage seen without the lock ("); dec((uint64_t)an); say(")\n"); }
        else say("[TTYTEST] N negative control WARN: no damage seen without the lock this run\n");
    }

    tty_reset();
    drain_console();
    g_running = 0;
    say("[TTYTEST] ms="); dec(ktime_ms() - t_all); say("\n");
    say(g_fail ? "[TTYTEST] RESULT FAIL\n" : "[TTYTEST] RESULT PASS\n");
}
#endif /* SECOS_TTY_TEST */
