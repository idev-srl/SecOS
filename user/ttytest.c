/* SecOS - console TTY / termios regression test (selftest line-editing gate).
 * SPDX-License-Identifier: MIT
 *
 * Driven over the serial console by tools/selftest.sh. Each step prints a
 * TT-<step> marker, then (where it needs input) waits for the bytes the
 * driver sends after seeing the marker:
 *   TT-WINSZ    TIOCGWINSZ reports the console size (rows/cols > 0)
 *   TT-RAW      ICANON/ECHO off, VMIN=1: "abc" arrives as three 1-byte reads
 *   TT-ESC      ESC [ A arrives whole; FIONREAD sees the rest after the ESC
 *   TT-ISIG     ISIG off: Ctrl-C (0x03) is read as a byte, no signal
 *   TT-VTIME    VMIN=0 VTIME=3: read() returns 0 after ~0.3 s without input
 *   TT-RESTORE  the saved settings come back (ICANON|ECHO|ISIG set)
 *   TT-CANON    canonical read returns one edited line ("xy<BS>z" -> "xz\n")
 * and finally TT-DONE <pass>/<total>.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/time.h>

static int pass, total;
static void ok(const char* what, int cond) {
    total++;
    if (cond) pass++;
    printf("TT-%s-%s\n", what, cond ? "OK" : "FAIL");
    fflush(stdout);
}
static void prompt(const char* step) { printf("TT-%s-READY\n", step); fflush(stdout); }

int main(void) {
    struct termios orig, raw, now;
    if (tcgetattr(0, &orig) != 0) { printf("TT-DONE 0/1 (stdin is not a tty)\n"); return 1; }

    struct winsize ws;
    int wr = ioctl(0, TIOCGWINSZ, &ws);
    printf("TT-winsz rows=%d cols=%d\n", (int)ws.ws_row, (int)ws.ws_col);
    ok("WINSZ", wr == 0 && ws.ws_row >= 24 && ws.ws_col >= 80);

    raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    ok("SETRAW", tcsetattr(0, TCSAFLUSH, &raw) == 0);

    /* 1. single bytes */
    prompt("RAW");
    unsigned char c[8]; int good = 1;
    for (int i = 0; i < 3; i++) {
        int n = (int)read(0, c + i, 1);
        if (n != 1) good = 0;
    }
    printf("TT-raw got %02x %02x %02x\n", c[0], c[1], c[2]);
    ok("RAW", good && c[0] == 'a' && c[1] == 'b' && c[2] == 'c');

    /* 2. escape sequence: ESC first, then the rest is already queued */
    prompt("ESC");
    int n = (int)read(0, c, 1);
    int avail = -1;
    ioctl(0, FIONREAD, &avail);
    int m = (int)read(0, c + 1, 2);
    printf("TT-esc n=%d avail=%d m=%d %02x %02x %02x\n", n, avail, m, c[0], c[1], c[2]);
    ok("ESC", n == 1 && c[0] == 0x1b && avail >= 2 && m == 2 && c[1] == '[' && c[2] == 'A');

    /* 3. ISIG off: Ctrl-C is data */
    raw.c_lflag &= ~ISIG;
    tcsetattr(0, TCSANOW, &raw);
    prompt("ISIG");
    n = (int)read(0, c, 1);
    ok("ISIG", n == 1 && c[0] == 0x03);

    /* 4. VMIN=0 VTIME=3: timed read with no input -> 0 */
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 3;
    tcsetattr(0, TCSANOW, &raw);
    struct timeval t0, t1;
    gettimeofday(&t0, 0);
    n = (int)read(0, c, 1);
    gettimeofday(&t1, 0);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;
    printf("TT-vtime n=%d ms=%ld\n", n, ms);
    ok("VTIME", n == 0 && ms >= 200 && ms < 3000);

    /* 5. restore */
    tcsetattr(0, TCSANOW, &orig);
    tcgetattr(0, &now);
    ok("RESTORE", (now.c_lflag & (ICANON | ECHO | ISIG)) == (ICANON | ECHO | ISIG) &&
                  now.c_cc[VMIN] == orig.c_cc[VMIN]);

    /* 6. canonical line with an erase */
    prompt("CANON");
    char line[64];
    n = (int)read(0, line, sizeof line - 1);
    if (n < 0) n = 0;
    line[n] = 0;
    printf("TT-canon n=%d\n", n);
    ok("CANON", n == 3 && line[0] == 'x' && line[1] == 'z' && line[2] == '\n');

    printf("TT-DONE %d/%d\n", pass, total);
    return pass == total ? 0 : 1;
}
