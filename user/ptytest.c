/* SecOS - pseudo-terminal regression test and `script`-style relay.
 * SPDX-License-Identifier: MIT
 *
 * `ptytest` runs the pty checks and prints one PT-<name>-OK/FAIL line each,
 * then PT-DONE <pass>/<total>:
 *   OPEN     posix_openpt/grantpt/unlockpt/ptsname; a locked slave is refused
 *   SESSION  forkpty child: new session, the slave is its controlling tty,
 *            ttyname(0) == ptsname, /dev/tty opens, tcgetpgrp == its group
 *   CANON    master write "hello\r" -> child reads "hello\n" (ICRNL), echo
 *            "hello\r\n" and the reply "GOT:hello\r\n" (ONLCR) on the master
 *   SIGINT   master writes ^C -> the child's foreground group gets SIGINT
 *   WINCH    TIOCSWINSZ on the master -> SIGWINCH once; the same size again
 *            raises none; the child sees the new size
 *   RAW      cfmakeraw on the slave: bytes (incl. ^C) arrive unechoed
 *   HUP      master close -> child SIGHUP, slave read -> EIO
 *   EOF      the child exits -> master read returns EIO once drained
 *   CYCLE    100 openpty/close cycles, slot reused (no leak), plus 20 forkpty
 *   BASH     bash -c 'tty; echo ...' on a pty: prints /dev/pts/N
 *   IBASH    interactive bash on a pty: runs a line, ^C stops `sleep 100`,
 *            `exit` ends it
 *
 * `ptytest script` (or the binary installed as /bin/script) runs an
 * interactive bash on a new pty and relays the console to it until it exits,
 * like script(1) without the typescript file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <pty.h>
#include <utmp.h>

static int pass, total;
static void ok(const char* what, int cond) {
    total++;
    if (cond) pass++;
    printf("PT-%s-%s\n", what, cond ? "OK" : "FAIL");
    fflush(stdout);
}

static void set_nonblock(int fd) { int one = 1; ioctl(fd, FIONBIO, &one); }

/* Accumulate master output into buf until `want` shows up (or EIO/timeout).
 * Returns 1 when found. *eio is set when the master read EIO. */
static char g_out[8192];
static int  g_outn;
static int collect(int m, const char* want, int ms, int* eio) {
    unsigned long t0 = getticks();
    if (eio) *eio = 0;
    for (;;) {
        if (want && g_outn > 0 && strstr(g_out, want)) return 1;
        char b[512];
        long n = read(m, b, sizeof(b));
        if (n > 0) {
            if (g_outn + n >= (long)sizeof(g_out) - 1) {     /* keep the tail */
                int keep = 2048;
                memmove(g_out, g_out + g_outn - keep, keep); g_outn = keep;
            }
            memcpy(g_out + g_outn, b, (size_t)n); g_outn += (int)n; g_out[g_outn] = 0;
            continue;
        }
        if (n < 0 && errno == EIO) { if (eio) *eio = 1; return want ? (strstr(g_out, want) != 0) : 1; }
        if (getticks() - t0 > (unsigned long)ms) return 0;
        sleep_ticks(5);
    }
}
static void out_reset(void) { g_outn = 0; g_out[0] = 0; }
static void mwrite(int m, const char* s) { write(m, s, strlen(s)); }

/* ---- child side ---------------------------------------------------------------- */
static volatile int g_sigint, g_winch, g_hup;
static void on_int(int s) { (void)s; g_sigint++; }
static void on_winch(int s) { (void)s; g_winch++; }
static void on_hup(int s) { (void)s; g_hup++; }

static void say(const char* s) { write(1, s, strlen(s)); }

static int child_main(const char* name) {
    char buf[256];
    signal(SIGINT, on_int);
    signal(SIGWINCH, on_winch);
    signal(SIGHUP, on_hup);
    /* SESSION */
    char* tn = ttyname(0);
    int tfd = open("/dev/tty", O_RDWR);
    int sess = getsid(0) == getpid() && tcgetsid(0) == getpid();
    int fg = tcgetpgrp(0) == getpgrp();
    snprintf(buf, sizeof(buf), "C-SESSION %s tty=%d fg=%d name=%s\n",
             (sess && tn && strcmp(tn, name) == 0 && tfd >= 0 && fg) ? "ok" : "bad",
             tfd, fg, tn ? tn : "-");
    say(buf);
    if (tfd >= 0) close(tfd);
    /* CANON */
    long n = read(0, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = 0;
        char rep[300]; snprintf(rep, sizeof(rep), "GOT:%s\n", buf); say(rep);
    }
    /* SIGINT: block in read until ^C interrupts it */
    n = read(0, buf, sizeof(buf));
    snprintf(buf, sizeof(buf), "C-INT %s\n", (n < 0 && errno == EINTR && g_sigint == 1) ? "ok" : "bad");
    say(buf);
    /* WINCH: wait for the resize (the parent sends two, same size) */
    for (int i = 0; i < 200 && g_winch == 0; i++) sleep_ticks(5);
    sleep_ticks(200);
    struct winsize ws; ioctl(0, TIOCGWINSZ, &ws);
    snprintf(buf, sizeof(buf), "C-WINCH %s %dx%d n=%d\n",
             (g_winch == 1 && ws.ws_row == 40 && ws.ws_col == 100) ? "ok" : "bad",
             ws.ws_row, ws.ws_col, g_winch);
    say(buf);
    /* RAW */
    struct termios t, raw;
    tcgetattr(0, &t); raw = t; cfmakeraw(&raw);
    tcsetattr(0, TCSAFLUSH, &raw);
    say("C-RAW-READY\n");
    unsigned char c[4]; int got = 0;
    while (got < 3) { long r = read(0, c + got, 3 - got); if (r <= 0) break; got += (int)r; }
    tcsetattr(0, TCSANOW, &t);
    snprintf(buf, sizeof(buf), "C-RAW %s %02x%02x%02x\n",
             (got == 3 && c[0] == 'x' && c[1] == 3 && c[2] == 'y' && g_sigint == 1) ? "ok" : "bad",
             c[0], c[1], c[2]);
    say(buf);
    /* HUP: wait for the master to go away */
    say("C-HUP-READY\n");
    do { n = read(0, buf, sizeof(buf)); } while (n < 0 && errno == EINTR);  /* SIGHUP wakes it first */
    int e = errno;
    _exit(40 + (g_hup ? 1 : 0) + ((n < 0 && e == EIO) ? 2 : 0));
}

/* ---- tests ---------------------------------------------------------------------- */
static void t_open(void) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    char* nm = m >= 0 ? ptsname(m) : 0;
    char name[32] = "";
    if (nm) strcpy(name, nm);
    int s_locked = name[0] ? open(name, O_RDWR | O_NOCTTY) : -1;
    int lockerr = errno;
    int g = grantpt(m), u = unlockpt(m);
    int s = name[0] ? open(name, O_RDWR | O_NOCTTY) : -1;
    unsigned ptn = 999; ioctl(m, TIOCGPTN, &ptn);
    struct stat st; int st_ok = s >= 0 && stat(name, &st) == 0 && (st.st_mode & S_IFMT) == S_IFCHR;
    printf("PT-open m=%d name=%s locked=%d/%d s=%d ptn=%u\n", m, name, s_locked, lockerr, s, ptn);
    ok("OPEN", m >= 0 && strncmp(name, "/dev/pts/", 9) == 0 && s_locked < 0 && lockerr == EIO
               && g == 0 && u == 0 && s >= 0 && st_ok && isatty(s) && isatty(m));
    if (s >= 0) close(s);
    if (m >= 0) close(m);
}

static void t_session(void) {
    char name[32];
    int m;
    struct winsize ws0 = { 24, 80, 0, 0 };
    int pid = forkpty(&m, name, 0, &ws0);
    if (pid == 0) child_main(name);
    if (pid < 0) { ok("SESSION", 0); return; }
    set_nonblock(m);
    out_reset();
    int s1 = collect(m, "C-SESSION", 3000, 0) && collect(m, "\n", 500, 0);
    ok("SESSION", s1 && strstr(g_out, "C-SESSION ok") != 0);

    out_reset();
    mwrite(m, "hello\r");
    int c1 = collect(m, "GOT:hello\r\n", 3000, 0);
    ok("CANON", c1 && strstr(g_out, "hello\r\n") != 0);

    out_reset();
    sleep_ticks(100);                       /* the child is blocked in read() */
    mwrite(m, "\003");
    int i1 = collect(m, "C-INT ", 3000, 0) && collect(m, "\n", 500, 0);
    ok("SIGINT", i1 && strstr(g_out, "C-INT ok") && strstr(g_out, "^C"));

    out_reset();
    struct winsize ws = { 40, 100, 0, 0 };
    ioctl(m, TIOCSWINSZ, &ws);
    sleep_ticks(50);
    ioctl(m, TIOCSWINSZ, &ws);              /* unchanged: no second SIGWINCH */
    struct winsize back; ioctl(m, TIOCGWINSZ, &back);
    int w1 = collect(m, "C-WINCH ", 5000, 0) && collect(m, "\n", 500, 0);
    ok("WINCH", w1 && strstr(g_out, "C-WINCH ok") && back.ws_row == 40 && back.ws_col == 100);

    out_reset();
    collect(m, "C-RAW-READY", 3000, 0);
    out_reset();
    mwrite(m, "x\003y");
    int r1 = collect(m, "C-RAW ", 3000, 0) && collect(m, "\n", 500, 0);
    ok("RAW", r1 && strstr(g_out, "C-RAW ok") && !strstr(g_out, "x\003y"));

    collect(m, "C-HUP-READY", 3000, 0);
    sleep_ticks(50);
    close(m);                               /* hang the child up */
    int st = waitpid(pid);
    printf("PT-hup status=%d exit=%d\n", st, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    ok("HUP", WIFEXITED(st) && WEXITSTATUS(st) == 43);
}

static void t_eof(void) {
    int m;
    int pid = forkpty(&m, 0, 0, 0);
    if (pid == 0) { say("bye\n"); _exit(5); }
    set_nonblock(m);
    out_reset();
    int eio = 0;
    collect(m, 0, 5000, &eio);              /* until EIO */
    int st = waitpid(pid);
    ok("EOF", eio && strstr(g_out, "bye\r\n") && WIFEXITED(st) && WEXITSTATUS(st) == 5);
    close(m);
}

static void t_cycle(void) {
    char first[32] = "", last[32] = "";
    int good = 1;
    for (int i = 0; i < 100; i++) {
        int m, s; char nm[32];
        if (openpty(&m, &s, nm, 0, 0) != 0) { good = 0; printf("PT-cycle openpty failed at %d\n", i); break; }
        if (i == 0) strcpy(first, nm);
        strcpy(last, nm);
        write(s, "z", 1);
        char b[4]; set_nonblock(m);
        if (read(m, b, 4) != 1) good = 0;
        close(s); close(m);
    }
    for (int i = 0; i < 20 && good; i++) {
        int m;
        int pid = forkpty(&m, 0, 0, 0);
        if (pid == 0) _exit(0);
        if (pid < 0) { good = 0; break; }
        waitpid(pid);
        close(m);
    }
    printf("PT-cycle first=%s last=%s\n", first, last);
    /* Every earlier pty (incl. the session test's, held as a controlling
     * terminal) was freed, so slot 0 is handed out again. */
    ok("CYCLE", good && strcmp(first, "/dev/pts/0") == 0 && strcmp(first, last) == 0);
}

static void t_bash(void) {
    int m;
    int pid = forkpty(&m, 0, 0, 0);
    if (pid == 0) {
        char* av[] = { "bash", "-c", "tty; echo BASHPTY-$((6*7))", 0 };
        execv("/bin/bash", av);
        _exit(127);
    }
    set_nonblock(m);
    out_reset();
    int eio = 0;
    int f = collect(m, "BASHPTY-42", 15000, &eio);
    waitpid(pid);
    printf("PT-bash %s\n", strstr(g_out, "/dev/pts/") ? "tty=/dev/pts/N" : "tty=?");
    ok("BASH", f && strstr(g_out, "/dev/pts/") != 0);
    close(m);
}

static void t_ibash(void) {
    int m;
    struct winsize ws = { 30, 90, 0, 0 };
    int pid = forkpty(&m, 0, 0, &ws);
    if (pid == 0) {
        char* av[] = { "bash", "--norc", "-i", 0 };
        execv("/bin/bash", av);
        _exit(127);
    }
    set_nonblock(m);
    out_reset();
    sleep_ticks(3000);                       /* bash starts (readline, job control) */
    mwrite(m, "echo IB1-$((6*7))\r");
    int a = collect(m, "IB1-42", 10000, 0);
    out_reset();
    mwrite(m, "sleep 100\r");
    sleep_ticks(1500);
    mwrite(m, "\003");
    sleep_ticks(500);
    mwrite(m, "echo IB2-$((2+3))\r");
    int b = collect(m, "IB2-5", 10000, 0);
    out_reset();
    mwrite(m, "exit\r");
    int eio = 0;
    collect(m, 0, 10000, &eio);
    int st = waitpid(pid);
    printf("PT-ibash line=%d intr=%d exit=%d st=%d\n", a, b, eio, st);
    ok("IBASH", a && b && eio);
    close(m);
}

/* ---- script(1)-style relay --------------------------------------------------------- */
static int script_main(void) {
    int m;
    struct winsize ws;
    int have_ws = ioctl(0, TIOCGWINSZ, &ws) == 0;
    int pid = forkpty(&m, 0, 0, have_ws ? &ws : 0);
    if (pid < 0) { perror("script: forkpty"); return 1; }
    if (pid == 0) {
        char* av[] = { "bash", "-i", 0 };
        execv("/bin/bash", av);
        _exit(127);
    }
    struct termios orig, raw;
    int tty = tcgetattr(0, &orig) == 0;
    if (tty) {
        raw = orig; cfmakeraw(&raw);
        raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;        /* poll the console */
        tcsetattr(0, TCSAFLUSH, &raw);
    }
    set_nonblock(m);
    printf("Script started on a pty, the shell runs as pid %d.\r\n", pid);
    fflush(stdout);
    for (;;) {
        char b[512];
        int busy = 0;
        long n = read(m, b, sizeof(b));
        if (n > 0) { write(1, b, (size_t)n); busy = 1; }
        else if (n < 0 && errno == EIO) break;            /* the shell exited */
        n = read(0, b, sizeof(b));
        if (n > 0) { write(m, b, (size_t)n); busy = 1; }
        if (!busy) sleep_ticks(5);
    }
    if (tty) tcsetattr(0, TCSANOW, &orig);
    int st = waitpid(pid);
    printf("Script done (shell status %d).\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    close(m);
    return 0;
}

int main(int argc, char** argv) {
    const char* base = argv[0];
    for (const char* p = argv[0]; p && *p; p++) if (*p == '/') base = p + 1;
    if ((base && strcmp(base, "script") == 0) || (argc > 1 && strcmp(argv[1], "script") == 0))
        return script_main();
    int only_fast = argc > 1 && strcmp(argv[1], "fast") == 0;
    t_open();
    t_session();
    t_eof();
    t_cycle();
    if (!only_fast) { t_bash(); t_ibash(); }
    printf("PT-DONE %d/%d\n", pass, total);
    return pass == total ? 0 : 1;
}
