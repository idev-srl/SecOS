/* SecOS - built-in termcap for the console (tgetent & friends).
 * SPDX-License-Identifier: MIT
 *
 * SecOS ships no termcap/terminfo database. Programs that use the termcap API
 * (GNU readline, less, ...) get this compiled-in entry instead: the
 * capabilities the console emulator (drivers/vt.c, Linux-console/xterm subset)
 * implements and the sequences the PS/2/USB keyboards send. Any TERM except
 * "dumb" (and unset, which readline maps to "dumb") resolves to it, so a host
 * xterm on the serial line works too.
 */
#include <string.h>

/* Globals the termcap API exports (readline's tcap.h declares them extern). */
char PC = 0;
char* BC = 0;
char* UP = 0;
short ospeed = 15;   /* B38400 */

struct tc_str { const char* id; const char* val; };
static const struct tc_str tc_strs[] = {
    { "cl", "\033[H\033[J" },   /* clear screen */
    { "cd", "\033[J" },         /* clear to end of screen */
    { "ce", "\033[K" },         /* clear to end of line */
    { "cm", "\033[%i%d;%dH" },  /* cursor motion (row;col) */
    { "ch", "\033[%i%dG" },     /* column absolute */
    { "cv", "\033[%i%dd" },     /* row absolute */
    { "ho", "\033[H" },         /* home */
    { "cr", "\r" },
    { "up", "\033[A" },         /* cursor up */
    { "do", "\n" },             /* cursor down */
    { "nd", "\033[C" },         /* cursor right */
    { "le", "\b" },             /* cursor left */
    { "UP", "\033[%dA" }, { "DO", "\033[%dB" }, { "RI", "\033[%dC" }, { "LE", "\033[%dD" },
    { "al", "\033[L" },  { "AL", "\033[%dL" }, /* insert line(s) */
    { "dl", "\033[M" },  { "DL", "\033[%dM" }, /* delete line(s) */
    { "dc", "\033[P" },  { "DC", "\033[%dP" }, /* delete char(s) */
    { "IC", "\033[%d@" },                      /* insert chars */
    { "ec", "\033[%dX" },                      /* erase chars */
    { "cs", "\033[%i%d;%dr" },                 /* scroll region */
    { "sf", "\n" }, { "sr", "\033M" },         /* scroll forward / reverse */
    { "sc", "\0337" }, { "rc", "\0338" },      /* save / restore cursor */
    { "vi", "\033[?25l" }, { "ve", "\033[?25h" }, { "vs", "\033[?25h" },
    { "ti", "\033[?1049h" }, { "te", "\033[?1049l" },  /* alternate screen */
    { "so", "\033[7m" }, { "se", "\033[27m" }, /* standout */
    { "mr", "\033[7m" }, { "md", "\033[1m" }, { "me", "\033[0m" },
    { "us", "\033[4m" }, { "ue", "\033[24m" },
    { "AF", "\033[3%dm" }, { "AB", "\033[4%dm" },
    { "bl", "\007" },
    { "ta", "\t" },
    /* keys (what the console keyboards send; xterm-compatible) */
    { "ku", "\033[A" }, { "kd", "\033[B" }, { "kr", "\033[C" }, { "kl", "\033[D" },
    { "kh", "\033[H" }, { "@7", "\033[F" },
    { "kD", "\033[3~" }, { "kI", "\033[2~" }, { "kP", "\033[5~" }, { "kN", "\033[6~" },
    { "kb", "\177" },  { "kB", "\033[Z" },
    { "k1", "\033OP" }, { "k2", "\033OQ" }, { "k3", "\033OR" }, { "k4", "\033OS" },
    { "k5", "\033[15~" }, { "k6", "\033[17~" }, { "k7", "\033[18~" }, { "k8", "\033[19~" },
    { "k9", "\033[20~" }, { "k;", "\033[21~" }, { "F1", "\033[23~" }, { "F2", "\033[24~" },
    { 0, 0 }
};
/* Boolean capabilities: auto margins, the Linux "newline glitch" (pending
 * wrap), a meta key, safe to move in insert/standout mode, backspace works. */
static const char* const tc_flags[] = { "am", "xn", "km", "mi", "ms", "bs", 0 };

static int tc_lines = 25, tc_cols = 80;

static int tc_dumb(const char* name) { return !name || !*name || strcmp(name, "dumb") == 0; }

int tgetent(char* bp, const char* name) {
    if (tc_dumb(name)) return 0;
    if (bp) strcpy(bp, "secos-console");
    return 1;
}

int tgetflag(const char* id) {
    for (int i = 0; tc_flags[i]; i++) if (strncmp(id, tc_flags[i], 2) == 0) return 1;
    return 0;
}

int tgetnum(const char* id) {
    if (strncmp(id, "co", 2) == 0) return tc_cols;
    if (strncmp(id, "li", 2) == 0) return tc_lines;
    if (strncmp(id, "it", 2) == 0) return 8;
    if (strncmp(id, "Co", 2) == 0) return 16;
    return -1;
}

char* tgetstr(const char* id, char** area) {
    for (int i = 0; tc_strs[i].id; i++) {
        if (strncmp(id, tc_strs[i].id, 2) != 0) continue;
        const char* v = tc_strs[i].val;
        if (area && *area) {
            char* p = *area;
            strcpy(p, v);
            *area += strlen(v) + 1;
            return p;
        }
        return (char*)v;
    }
    return 0;
}

/* Expand %d %2 %3 %. %+c %i %r %% (the classic termcap subset; row is the
 * first parameter for cm). */
char* tgoto(const char* cap, int col, int row) {
    static char out[64];
    if (!cap) return (char*)"OOPS";
    int args[2] = { row, col }, ai = 0, k = 0;
    for (const char* p = cap; *p && k < (int)sizeof(out) - 8; p++) {
        if (*p != '%') { out[k++] = *p; continue; }
        p++;
        if (!*p) break;
        int v = args[ai < 2 ? ai : 1];
        switch (*p) {
        case 'd': case '2': case '3': {
            char t[12]; int n = 0, w = (*p == '2') ? 2 : (*p == '3') ? 3 : 0;
            if (v < 0) v = 0;
            do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
            while (n < w) t[n++] = '0';
            while (n) out[k++] = t[--n];
            ai++;
            break; }
        case '.': out[k++] = (char)v; ai++; break;
        case '+': p++; if (!*p) goto done; out[k++] = (char)(v + *p); ai++; break;
        case 'i': args[0]++; args[1]++; break;
        case 'r': { int t = args[0]; args[0] = args[1]; args[1] = t; break; }
        case '%': out[k++] = '%'; break;
        default: break;
        }
    }
done:
    out[k] = 0;
    return out;
}

/* Output a capability, skipping termcap padding ("$<5>" and leading digits). */
int tputs(const char* str, int affcnt, int (*putc_fn)(int)) {
    (void)affcnt;
    if (!str) return -1;
    while (*str >= '0' && *str <= '9') str++;
    if (*str == '.') { str++; while (*str >= '0' && *str <= '9') str++; }
    if (*str == '*') str++;
    for (; *str; str++) {
        if (str[0] == '$' && str[1] == '<') {
            while (*str && *str != '>') str++;
            if (!*str) break;
            continue;
        }
        putc_fn((unsigned char)*str);
    }
    return 0;
}
