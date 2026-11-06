/* SecOS - select the console keyboard layout (/bin/loadkeys).
 * SPDX-License-Identifier: MIT
 *
 *   loadkeys          print the active layout
 *   loadkeys it|us    switch layout (console tty ioctl KDSKBLAYOUT) and save it
 *                     as KEYMAP= in /etc/vconsole.conf, which the kernel reads
 *                     at boot (on a persistent root the choice survives reboot)
 *   loadkeys -n it    switch for this boot only (no save)
 *   loadkeys -l       list the built-in layouts
 * The layouts are compiled into the kernel (drivers/keymap.c); this is not
 * the kbd-package loadkeys and reads no keymap files.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>    /* ioctl() */
#include <linux/kd.h>
#include "libsecos.h"

#define VCONSOLE     "/etc/vconsole.conf"
#define VCONSOLE_NEW "/etc/vconsole.conf.new"

/* Rewrite /etc/vconsole.conf with KEYMAP=<name>, keeping its other lines
 * (FONT=, comments). New file + rename: a crash leaves the old or the new. */
static int save_keymap(const char* name) {
    static char in[4096], out[4200];
    int n = 0, o = 0, done = 0;
    int fd = open(VCONSOLE, O_RDONLY);
    if (fd >= 0) { n = (int)read(fd, in, sizeof(in) - 1); close(fd); if (n < 0) n = 0; }
    in[n] = 0;
    for (char* p = in; *p; ) {
        char* line = p; while (*p && *p != '\n') p++;
        int len = (int)(p - line); if (*p) p++;
        if (len >= 7 && !strncmp(line, "KEYMAP=", 7)) {
            if (done) continue;
            o += snprintf(out + o, sizeof(out) - o, "KEYMAP=%s\n", name); done = 1;
        } else if (o + len + 1 < (int)sizeof(out)) {
            memcpy(out + o, line, (size_t)len); o += len; out[o++] = '\n';
        }
    }
    if (!done) o += snprintf(out + o, sizeof(out) - o, "KEYMAP=%s\n", name);
    fd = open(VCONSOLE_NEW, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int w = (int)write(fd, out, (size_t)o);
    close(fd);
    if (w != o) { unlink(VCONSOLE_NEW); return -1; }
    return rename(VCONSOLE_NEW, VCONSOLE);
}

int main(int argc, char** argv) {
    int fd = open("/dev/console", O_RDWR);
    if (fd < 0) fd = 0;
    char cur[16] = { 0 };
    int save = 1, ai = 1;
    if (argc > 1 && (!strcmp(argv[1], "-n") || !strcmp(argv[1], "--no-save"))) { save = 0; ai = 2; }
    if (argc <= ai) {
        if (ioctl(fd, KDGKBLAYOUT, cur) != 0) { printf("loadkeys: %s\n", strerror(errno)); return 1; }
        printf("%s\n", cur);
        return 0;
    }
    const char* name = argv[ai];
    if (!strcmp(name, "-l") || !strcmp(name, "--list")) { printf("us it\n"); return 0; }
    /* Accept common spellings: "it", "it.map", "it-latin1", "us", "en-us". */
    char want[16] = { 0 };
    if (!strncmp(name, "it", 2)) strcpy(want, "it");
    else if (!strncmp(name, "us", 2) || !strcmp(name, "en") || !strncmp(name, "en-us", 5)) strcpy(want, "us");
    else { printf("loadkeys: unknown layout '%s' (available: us it)\n", name); return 1; }
    if (ioctl(fd, KDSKBLAYOUT, want) != 0) { printf("loadkeys: %s\n", strerror(errno)); return 1; }
    ioctl(fd, KDGKBLAYOUT, cur);
    if (save && save_keymap(cur) != 0)
        printf("loadkeys: keyboard layout %s (not saved: %s)\n", cur, strerror(errno));
    else
        printf("loadkeys: keyboard layout %s%s\n", cur, save ? " (saved in " VCONSOLE ")" : "");
    return 0;
}
