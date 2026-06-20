/* SecOS coreutils — text-processing applets (cu_text.c).
 *
 * Part of the busybox-style multi-call coreutils binary. Each applet is
 * `int applet_NAME(int argc, char** argv)` with argv[0] = applet name and
 * returns an exit code (0 = success). Errors go to stderr, prefixed with the
 * applet name.
 *
 * Freestanding ring-3 code: NO GCC nested functions and NO statement-
 * expressions (they need an executable-stack trampoline, which violates the
 * kernel's W^X gate and would crash). All helpers are file-scope statics.
 * No floating point.
 *
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include "libsecos.h"
#include "coreutils.h"

/* ------------------------------------------------------------------ */
/* shared helpers                                                     */
/* ------------------------------------------------------------------ */

/* Open a file for reading, or return stdin when name is NULL ("-" also means
 * stdin by convention). On failure prints "<app>: <name>: cannot open" to
 * stderr and returns NULL. *is_stdin is set when stdin was selected so the
 * caller knows not to fclose() it. */
static FILE* open_input(const char* app, const char* name, int* is_stdin) {
    if (!name || (name[0] == '-' && name[1] == '\0')) {
        *is_stdin = 1;
        return stdin;
    }
    *is_stdin = 0;
    FILE* f = fopen(name, "r");
    if (!f)
        fprintf(stderr, "%s: %s: cannot open\n", app, name);
    return f;
}

/* Parse a count argument for head/tail. Supports "-n N" and "-N" forms.
 * On entry *idx points at the option token (argv[*idx]). Returns the parsed
 * count (>=0); advances *idx past the consumed option token(s). On a malformed
 * option leaves the default in place and does not advance. */
static long parse_count_opt(int argc, char** argv, int* idx, long deflt) {
    const char* a = argv[*idx];
    if (a[0] != '-')
        return deflt;
    if (a[1] == 'n' && a[2] == '\0') {
        /* "-n N" : value is the next token */
        if (*idx + 1 < argc) {
            long n = strtol(argv[*idx + 1], (char**)0, 10);
            *idx += 2;
            return n < 0 ? 0 : n;
        }
        *idx += 1;
        return deflt;
    }
    if (a[1] == 'n') {
        /* "-nN" : value glued to -n */
        long n = strtol(a + 2, (char**)0, 10);
        *idx += 1;
        return n < 0 ? 0 : n;
    }
    if (isdigit((unsigned char)a[1])) {
        /* "-N" */
        long n = strtol(a + 1, (char**)0, 10);
        *idx += 1;
        return n < 0 ? 0 : n;
    }
    /* unknown option: skip it, keep default */
    *idx += 1;
    return deflt;
}

/* ------------------------------------------------------------------ */
/* echo                                                               */
/* ------------------------------------------------------------------ */

int applet_echo(int argc, char** argv) {
    int i = 1;
    int newline = 1;
    if (i < argc && strcmp(argv[i], "-n") == 0) {
        newline = 0;
        i++;
    }
    for (; i < argc; i++) {
        fputs(argv[i], stdout);
        if (i + 1 < argc)
            fputc(' ', stdout);
    }
    if (newline)
        fputc('\n', stdout);
    return 0;
}

/* ------------------------------------------------------------------ */
/* cat                                                                */
/* ------------------------------------------------------------------ */

static int cat_stream(FILE* f) {
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        fwrite(buf, 1, n, stdout);
    return 0;
}

int applet_cat(int argc, char** argv) {
    if (argc < 2)
        return cat_stream(stdin);

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int is_stdin;
        FILE* f = open_input("cat", argv[i], &is_stdin);
        if (!f) {
            rc = 1;
            continue;
        }
        cat_stream(f);
        if (!is_stdin)
            fclose(f);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* wc                                                                 */
/* ------------------------------------------------------------------ */

static void wc_count(FILE* f, long* lines, long* words, long* bytes) {
    long l = 0, w = 0, b = 0;
    int in_word = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        b++;
        if (c == '\n')
            l++;
        if (isspace(c)) {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            w++;
        }
    }
    *lines = l;
    *words = w;
    *bytes = b;
}

int applet_wc(int argc, char** argv) {
    if (argc < 2) {
        long l, w, b;
        wc_count(stdin, &l, &w, &b);
        printf("%8ld%8ld%8ld\n", l, w, b);
        return 0;
    }

    int rc = 0;
    long tl = 0, tw = 0, tb = 0;
    int files = 0;
    for (int i = 1; i < argc; i++) {
        int is_stdin;
        FILE* f = open_input("wc", argv[i], &is_stdin);
        if (!f) {
            rc = 1;
            continue;
        }
        long l, w, b;
        wc_count(f, &l, &w, &b);
        printf("%8ld%8ld%8ld %s\n", l, w, b, argv[i]);
        tl += l;
        tw += w;
        tb += b;
        files++;
        if (!is_stdin)
            fclose(f);
    }
    if (files > 1)
        printf("%8ld%8ld%8ld total\n", tl, tw, tb);
    return rc;
}

/* ------------------------------------------------------------------ */
/* head                                                               */
/* ------------------------------------------------------------------ */

/* Print the first n lines of f. A "line" ends at '\n'; trailing partial line
 * (no final newline) counts as a line too. */
static void head_stream(FILE* f, long n) {
    long printed = 0;
    int c;
    int had_char = 0;
    while (printed < n && (c = fgetc(f)) != EOF) {
        had_char = 1;
        fputc(c, stdout);
        if (c == '\n') {
            printed++;
            had_char = 0;
        }
    }
    (void)had_char;
}

int applet_head(int argc, char** argv) {
    long n = 10;
    int i = 1;
    /* leading options */
    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0')
        n = parse_count_opt(argc, argv, &i, n);

    int nfiles = argc - i;
    if (nfiles <= 0) {
        head_stream(stdin, n);
        return 0;
    }

    int rc = 0;
    int first = 1;
    for (; i < argc; i++) {
        int is_stdin;
        FILE* f = open_input("head", argv[i], &is_stdin);
        if (!f) {
            rc = 1;
            continue;
        }
        if (nfiles > 1) {
            if (!first)
                fputc('\n', stdout);
            printf("==> %s <==\n", argv[i]);
        }
        first = 0;
        head_stream(f, n);
        if (!is_stdin)
            fclose(f);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* tail                                                               */
/* ------------------------------------------------------------------ */

/* Read the whole stream into a growable buffer, then print the last n lines.
 * Lines are counted by '\n'; a trailing partial line is its own line. */
static void tail_stream(FILE* f, long n) {
    if (n <= 0)
        return;

    size_t cap = 4096, len = 0;
    char* data = (char*)malloc(cap);
    if (!data) {
        fprintf(stderr, "tail: out of memory\n");
        return;
    }
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (len + 1 > cap) {
            size_t ncap = cap * 2;
            char* nd = (char*)realloc(data, ncap);
            if (!nd) {
                fprintf(stderr, "tail: out of memory\n");
                free(data);
                return;
            }
            data = nd;
            cap = ncap;
        }
        data[len++] = (char)c;
    }

    /* Find the start offset of the last n lines. Count newlines from the end;
     * we want to skip everything before the (total_lines - n)-th line break. */
    long want = n;
    size_t start = 0;
    long seen = 0;
    /* Walk backwards. We need the position just after the (want)-th newline
     * counted from the end, ignoring a trailing newline on the very last
     * byte (that one terminates the final line, it doesn't start a new one). */
    if (len > 0) {
        size_t idx = len;
        /* skip a single trailing newline so it isn't counted as an extra line */
        if (data[idx - 1] == '\n')
            idx--;
        while (idx > 0) {
            if (data[idx - 1] == '\n') {
                seen++;
                if (seen >= want) {
                    start = idx; /* byte after this newline */
                    break;
                }
            }
            idx--;
        }
    }
    if (start < len)
        fwrite(data + start, 1, len - start, stdout);
    free(data);
}

int applet_tail(int argc, char** argv) {
    long n = 10;
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0')
        n = parse_count_opt(argc, argv, &i, n);

    int nfiles = argc - i;
    if (nfiles <= 0) {
        tail_stream(stdin, n);
        return 0;
    }

    int rc = 0;
    int first = 1;
    for (; i < argc; i++) {
        int is_stdin;
        FILE* f = open_input("tail", argv[i], &is_stdin);
        if (!f) {
            rc = 1;
            continue;
        }
        if (nfiles > 1) {
            if (!first)
                fputc('\n', stdout);
            printf("==> %s <==\n", argv[i]);
        }
        first = 0;
        tail_stream(f, n);
        if (!is_stdin)
            fclose(f);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* rev                                                                */
/* ------------------------------------------------------------------ */

/* Reverse the characters of each line, preserving a trailing newline. */
static void rev_stream(FILE* f) {
    size_t cap = 256, len = 0;
    char* line = (char*)malloc(cap);
    if (!line) {
        fprintf(stderr, "rev: out of memory\n");
        return;
    }
    int c;
    for (;;) {
        c = fgetc(f);
        if (c == '\n' || c == EOF) {
            /* emit the line reversed */
            for (size_t k = len; k > 0; k--)
                fputc(line[k - 1], stdout);
            if (c == '\n')
                fputc('\n', stdout);
            len = 0;
            if (c == EOF)
                break;
            continue;
        }
        if (len + 1 > cap) {
            size_t ncap = cap * 2;
            char* nl = (char*)realloc(line, ncap);
            if (!nl) {
                fprintf(stderr, "rev: out of memory\n");
                free(line);
                return;
            }
            line = nl;
            cap = ncap;
        }
        line[len++] = (char)c;
    }
    free(line);
}

int applet_rev(int argc, char** argv) {
    if (argc < 2) {
        rev_stream(stdin);
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int is_stdin;
        FILE* f = open_input("rev", argv[i], &is_stdin);
        if (!f) {
            rc = 1;
            continue;
        }
        rev_stream(f);
        if (!is_stdin)
            fclose(f);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* nl                                                                 */
/* ------------------------------------------------------------------ */

/* Number non-empty lines (an empty line is just "\n"), starting at 1, with a
 * right-aligned 6-wide number followed by a tab. */
static void nl_stream(FILE* f, long* counter) {
    int c;
    int at_line_start = 1;
    int line_has_content = 0;
    /* We buffer nothing: decide numbering at the first char of each line. */
    for (;;) {
        c = fgetc(f);
        if (c == EOF) {
            if (!at_line_start) {
                /* final line without newline: already emitted prefix below */
            }
            break;
        }
        if (at_line_start) {
            if (c == '\n') {
                /* empty line: not numbered */
                fputc('\n', stdout);
                line_has_content = 0;
                continue; /* still at line start */
            }
            /* non-empty line: emit number prefix */
            (*counter)++;
            printf("%6ld\t", *counter);
            at_line_start = 0;
            line_has_content = 1;
        }
        fputc(c, stdout);
        if (c == '\n') {
            at_line_start = 1;
            line_has_content = 0;
        }
    }
    (void)line_has_content;
}

int applet_nl(int argc, char** argv) {
    long counter = 0;
    if (argc < 2) {
        nl_stream(stdin, &counter);
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int is_stdin;
        FILE* f = open_input("nl", argv[i], &is_stdin);
        if (!f) {
            rc = 1;
            continue;
        }
        nl_stream(f, &counter);
        if (!is_stdin)
            fclose(f);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* yes                                                                */
/* ------------------------------------------------------------------ */

int applet_yes(int argc, char** argv) {
    const char* s = (argc > 1) ? argv[1] : "y";
    /* BOUND: this OS has no Ctrl-C yet, so an unbounded loop would hang the
     * shell forever. Cap the output at 1000 iterations. */
    for (int i = 0; i < 1000; i++) {
        fputs(s, stdout);
        fputc('\n', stdout);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* seq                                                                */
/* ------------------------------------------------------------------ */

/* Forms:
 *   seq LAST              -> 1 .. LAST   (step 1)
 *   seq FIRST LAST        -> FIRST .. LAST (step 1)
 *   seq FIRST STEP LAST   -> FIRST .. LAST (step STEP)
 * Integer only. */
int applet_seq(int argc, char** argv) {
    long first = 1, step = 1, last = 0;

    if (argc == 2) {
        last = strtol(argv[1], (char**)0, 10);
    } else if (argc == 3) {
        first = strtol(argv[1], (char**)0, 10);
        last = strtol(argv[2], (char**)0, 10);
    } else if (argc == 4) {
        first = strtol(argv[1], (char**)0, 10);
        step = strtol(argv[2], (char**)0, 10);
        last = strtol(argv[3], (char**)0, 10);
    } else {
        fprintf(stderr, "seq: usage: seq LAST | seq FIRST LAST | seq FIRST STEP LAST\n");
        return 1;
    }

    if (step == 0) {
        fprintf(stderr, "seq: step must be non-zero\n");
        return 1;
    }

    if (step > 0) {
        for (long v = first; v <= last; v += step)
            printf("%ld\n", v);
    } else {
        for (long v = first; v >= last; v += step)
            printf("%ld\n", v);
    }
    return 0;
}
