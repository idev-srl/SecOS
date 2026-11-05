/* SecOS - select the console keyboard layout (/bin/loadkeys).
 * SPDX-License-Identifier: MIT
 *
 *   loadkeys          print the active layout
 *   loadkeys it|us    switch layout (console tty ioctl KDSKBLAYOUT)
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

int main(int argc, char** argv) {
    int fd = open("/dev/console", O_RDWR);
    if (fd < 0) fd = 0;
    char cur[16] = { 0 };
    if (argc < 2) {
        if (ioctl(fd, KDGKBLAYOUT, cur) != 0) { printf("loadkeys: %s\n", strerror(errno)); return 1; }
        printf("%s\n", cur);
        return 0;
    }
    const char* name = argv[1];
    if (!strcmp(name, "-l") || !strcmp(name, "--list")) { printf("us it\n"); return 0; }
    /* Accept common spellings: "it", "it.map", "it-latin1", "us", "en-us". */
    char want[16] = { 0 };
    if (!strncmp(name, "it", 2)) strcpy(want, "it");
    else if (!strncmp(name, "us", 2) || !strcmp(name, "en") || !strncmp(name, "en-us", 5)) strcpy(want, "us");
    else { printf("loadkeys: unknown layout '%s' (available: us it)\n", name); return 1; }
    if (ioctl(fd, KDSKBLAYOUT, want) != 0) { printf("loadkeys: %s\n", strerror(errno)); return 1; }
    ioctl(fd, KDGKBLAYOUT, cur);
    printf("loadkeys: keyboard layout %s\n", cur);
    return 0;
}
