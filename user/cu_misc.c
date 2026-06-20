/* SecOS coreutils — system / misc applets (cu_misc.c).
 * Part of the busybox-style multi-call coreutils binary; each applet is
 * `int applet_NAME(int argc, char** argv)` (argv[0] = applet name) returning an
 * exit code. Errors go to stderr (fd 2). Freestanding ring-3, no FP, no nested
 * functions (W^X). SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "libsecos.h"
#include "coreutils.h"

/* --- small file-scope helpers (no nested functions / statement-exprs) --- */

static const char HEX[] = "0123456789abcdef";

/* Render an unsigned value as a fixed-width, zero-padded lowercase hex string.
 * `width` is the field width (truncates high nibbles beyond it). Always NUL. */
static void hex_fixed(char* out, unsigned long val, int width)
{
    int i;
    for (i = width - 1; i >= 0; i--) {
        out[i] = HEX[val & 0xF];
        val >>= 4;
    }
    out[width] = '\0';
}

/* One classic hexdump row: 8-hex offset, up to 16 bytes as 2-hex pairs grouped
 * in two halves of 8, then a "|ascii|" gutter. `n` (1..16) is bytes in `row`. */
static void hexdump_row(unsigned long off, const unsigned char* row, int n)
{
    char obuf[9];
    char hb[3];
    int i;

    hex_fixed(obuf, off, 8);
    fputs(obuf, stdout);
    putchar(' ');

    for (i = 0; i < 16; i++) {
        if (i == 8)
            putchar(' '); /* split the two 8-byte groups */
        if (i < n) {
            hex_fixed(hb, row[i], 2);
            fputs(hb, stdout);
            putchar(' ');
        } else {
            fputs("   ", stdout); /* pad missing byte: 2 hex + 1 space */
        }
    }

    fputs(" |", stdout);
    for (i = 0; i < n; i++) {
        unsigned char c = row[i];
        if (c >= 0x20 && c < 0x7f)
            putchar((int)c);
        else
            putchar('.');
    }
    fputs("|\n", stdout);
}

/* --- applets --- */

int applet_true(int argc, char** argv)
{
    (void)argc; (void)argv;
    return 0;
}

int applet_false(int argc, char** argv)
{
    (void)argc; (void)argv;
    return 1;
}

int applet_uname(int argc, char** argv)
{
    const char* sysname  = "SecOS";
    const char* nodename = "secos";
    const char* release  = "0.1.0-dev";
    const char* machine  = "x86_64";

    if (argc < 2) {
        printf("%s\n", sysname);
        return 0;
    }

    if (strcmp(argv[1], "-a") == 0) {
        printf("%s %s %s %s\n", sysname, nodename, release, machine);
        return 0;
    }
    if (strcmp(argv[1], "-s") == 0) {
        printf("%s\n", sysname);
        return 0;
    }
    if (strcmp(argv[1], "-r") == 0) {
        printf("%s\n", release);
        return 0;
    }
    if (strcmp(argv[1], "-m") == 0) {
        printf("%s\n", machine);
        return 0;
    }

    /* Unknown option: behave like bare uname. */
    printf("%s\n", sysname);
    return 0;
}

int applet_sleep(int argc, char** argv)
{
    int secs;

    if (argc < 2) {
        fprintf(stderr, "usage: sleep SECONDS\n");
        return 1;
    }
    secs = atoi(argv[1]);
    if (secs < 0)
        secs = 0;
    sleep((unsigned)secs);
    return 0;
}

int applet_clear(int argc, char** argv)
{
    (void)argc; (void)argv;
    /* ANSI: erase entire screen + move cursor to home (1,1). */
    write(1, "\033[2J\033[H", 7);
    return 0;
}

int applet_hexdump(int argc, char** argv)
{
    int fd = 0;            /* default: stdin */
    int opened = 0;
    unsigned long off = 0;
    unsigned char buf[16];
    int filled;

    if (argc >= 2) {
        fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "hexdump: cannot open %s\n", argv[1]);
            return 1;
        }
        opened = 1;
    }

    for (;;) {
        /* Fill a full 16-byte row, tolerating short reads. */
        filled = 0;
        while (filled < 16) {
            ssize_t r = read(fd, buf + filled, (size_t)(16 - filled));
            if (r <= 0)
                break;     /* EOF or error */
            filled += (int)r;
        }
        if (filled == 0)
            break;         /* nothing left */
        hexdump_row(off, buf, filled);
        off += (unsigned long)filled;
        if (filled < 16)
            break;         /* short row => EOF reached */
    }

    if (opened)
        close(fd);
    return 0;
}

int applet_uptime(int argc, char** argv)
{
    unsigned long ticks;
    unsigned long secs;

    (void)argc; (void)argv;
    ticks = getticks();    /* ms-ticks, 1000/sec */
    secs  = ticks / 1000;
    printf("up %lu seconds (%lu ticks)\n", secs, ticks);
    return 0;
}
